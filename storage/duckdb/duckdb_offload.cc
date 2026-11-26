/* Copyright (c) 2026-present, Marco Tusa. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

// Query offload, DWL#6 steps 6a and 6b: one loaded table; column references,
// literals, comparisons, BETWEEN, IN lists, LIKE, IS [NOT] NULL, AND, OR, NOT;
// + - * MOD DIV, DOUBLE division, ABS, ROUND of exact numbers, CASE, IF,
// IFNULL, COALESCE, NULLIF; date parts of DATE/DATETIME; COUNT, COUNT
// DISTINCT, SUM, AVG, MIN, MAX; DISTINCT, GROUP BY, HAVING, ORDER BY, LIMIT,
// OFFSET; PAD SPACE binary collations; TIMESTAMP constants. Anything else is
// refused with a reason (the query then runs on InnoDB).

#include "storage/duckdb/duckdb_offload.h"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb.h"
#include "sql-common/my_decimal.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysqld_error.h"
#include "sql/binlog.h"  // mysql_bin_log, Log_info
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/item_subselect.h"
#include "include/mem_root_deque.h"
#include "sql/opt_trace.h"
#include "sql/protocol.h"  // Protocol::store_short (YEAR holder)
#include "sql/query_result.h"
#include "sql/query_term.h"
#include "sql/replication.h"  // Trans_observer
#include "scope_guard.h"
#include "storage/duckdb/duckdb_progress.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"  // QEP_TAB
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "sql/item_timefunc.h"  // Item_extract, Item_func_add_interval
#include "sql/sql_locale.h"     // MY_LOCALE (lc_time_names)
#include "sql/tztime.h"
#include "storage/duckdb/duckdb_binlog_reader.h"
#include "storage/duckdb/duckdb_collation.h"
#include "storage/duckdb/duckdb_guard.h"
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_load.h"  // history_scope_full
#include "storage/duckdb/duckdb_registry.h"
#include "storage/duckdb/duckdb_types.h"  // quote_ident

namespace duckdb_se {

namespace {

// Read your own writes (duckdb_offload_wait_applied): the binlog position of
// the last commit of each connection that has sent a query to DuckDB. The
// server resets THD's own position at every statement, so we record it at
// commit with a transaction observer (as semi-sync replication does).
std::mutex g_commit_mutex;
std::unordered_map<my_thread_id, BinlogPos> g_last_commit;
const handlerton *g_hton = nullptr;
char g_session_marker;  // our per-connection data: close_connection is called

// The transaction observer, after the InnoDB commit of each transaction.
// It also wakes the binlog reader: with binlog_order_commits=ON the server
// commits a group in InnoDB, then calls after_commit for each transaction in
// binary log order (sql/binlog.cc, ordered_commit), so the reader never reads
// a transaction InnoDB has not committed yet. after_sync (earlier) runs
// before the InnoDB commit: DuckDB could show a transaction before InnoDB
// does, for seconds with semi-sync AFTER_SYNC waiting for a replica.
int after_commit(Trans_param *param) {
  try {
    if (param->log_file == nullptr || *param->log_file == '\0') return 0;
    BinlogReader::instance().notify_committed(param->log_file, param->log_pos);
    std::lock_guard<std::mutex> guard(g_commit_mutex);
    auto it = g_last_commit.find(param->thread_id);
    if (it != g_last_commit.end())
      it->second = BinlogPos{binlog_base_name(param->log_file), param->log_pos};
  } catch (...) {
    // Never affect a commit.
  }
  return 0;
}

Trans_observer g_trans_observer = {sizeof(Trans_observer), nullptr, nullptr,
                                   nullptr, after_commit, nullptr, nullptr};
bool g_observer_registered = false;

int close_connection(handlerton *, THD *thd) {
  guard_forget(thd);
  try {
    std::lock_guard<std::mutex> guard(g_commit_mutex);
    g_last_commit.erase(thd->thread_id());
  } catch (...) {
  }
  return 0;
}

// The position this session's next offloaded query must see in DuckDB. The
// first time, the session is registered and we do not know its earlier
// commits: we use the current end of the binary log (all sessions).
bool freshness_target(THD *thd, BinlogPos *target) {
  {
    std::lock_guard<std::mutex> guard(g_commit_mutex);
    auto it = g_last_commit.find(thd->thread_id());
    if (it != g_last_commit.end()) {
      *target = it->second;
      return false;
    }
    g_last_commit.emplace(thd->thread_id(), BinlogPos());
  }
  thd_set_ha_data(thd, g_hton, &g_session_marker);
  Log_info info;
  if (!mysql_bin_log.is_open() || mysql_bin_log.get_current_log(&info) != 0)
    return true;
  *target = BinlogPos{binlog_base_name(info.log_file_name), info.pos};
  return false;
}

std::atomic<unsigned long long> g_offloaded{0};
std::atomic<unsigned long long> g_refused{0};
std::atomic<unsigned long long> g_explained{0};
std::atomic<unsigned long long> g_waits{0};
std::atomic<unsigned long long> g_wait_timeouts{0};
std::atomic<unsigned long long> g_decimal_reruns{0};
// Time in DuckDB of the statements counted in g_offloaded (execution and
// fetch, plus a first run during optimization), in microseconds.
std::atomic<unsigned long long> g_offload_time_us{0};

// The error a grouped string check raises in DuckDB (grouped_value).
constexpr char kGroupedAmbiguous[] = "duckdb_offload_grouped_value_ambiguous";

unsigned long long micros_since(std::chrono::steady_clock::time_point t) {
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t)
          .count());
}
std::mutex g_reason_mutex;
std::string g_last_refusal;

// How a select-list value comes back from DuckDB (the SQL casts it).
enum class Kind {
  INT,        // BIGINT
  UINT,       // UBIGINT
  DOUBLE,     // DOUBLE
  DECIMAL,    // DECIMAL(p,s) as VARCHAR text
  STRING,     // VARCHAR (UTF-8), converted to the item's character set
  BLOB,       // BLOB (bytes)
  DATE,       // DATE
  DATETIME,   // TIMESTAMP holding a DATETIME (no time zone)
  TIMESTAMP,  // TIMESTAMP holding a UTC instant (MySQL TIMESTAMP)
  BIT,        // UBIGINT holding a BIT(n) value, sent as ceil(n/8) bytes
  TIME,       // BIGINT holding a TIME as signed microseconds
};

// Per statement: the generated SQL, how to read each column, and the DuckDB
// statement prepared at optimization (a DuckDB bind error is then a refusal,
// before the server has sent anything to the client).
class OffloadContext : public Secondary_engine_execution_context {
 public:
  ~OffloadContext() override {
    restore_optimizer_switch();
    if (!con) return;
    std::string ignored;
    try {
      con->execute_cleanup(
          [this](duckdb_connection, std::string *) {
            if (first_chunk != nullptr) duckdb_destroy_data_chunk(&first_chunk);
            if (have_stream) duckdb_destroy_result(&stream_result);
            have_stream = false;
            if (stmt != nullptr) duckdb_destroy_prepare(&stmt);
            if (have_pre_result) duckdb_destroy_result(&pre_result);
            have_pre_result = false;
            return false;
          },
          &ignored);
    } catch (...) {
    }
  }
  std::string sql;
  std::vector<Kind> kinds;
  std::string reason;
  bool executed{false};
  std::unique_ptr<Lake::Connection> con;
  duckdb_prepared_statement stmt{nullptr};
  // A statement with DECIMAL values declared above 38 digits ran during
  // optimization (duckdb_decimal_overflow): its materialized result, sent by
  // execute_offloaded().
  duckdb_result pre_result;
  unsigned long long pre_run_us{0};  // first runs during optimization
  bool have_pre_result{false};
  // T2: every other statement starts in DuckDB during optimization and its
  // first chunk is fetched there (streaming); execute_offloaded() sends it
  // and fetches the rest.
  duckdb_result stream_result;
  bool have_stream{false};
  duckdb_data_chunk first_chunk{nullptr};
  bool first_fetched{false};

  // Semi-join materialization is turned off while the server optimizes a
  // statement for us: that strategy rewrites WHERE to read its own
  // temporary table (<subqueryN>), which DuckDB does not have. The other
  // strategies keep the conditions on the real tables. sql_lex.cc
  // (update_semijoin_strategies) says secondary engines choose their
  // strategies through thd->variables.optimizer_switch.
  void disable_semijoin_materialization(THD *t) {
    thd = t;
    had_materialization = (thd->variables.optimizer_switch &
                           OPTIMIZER_SWITCH_MATERIALIZATION) != 0;
    thd->variables.optimizer_switch &= ~OPTIMIZER_SWITCH_MATERIALIZATION;
    switch_changed = true;
  }
  void restore_optimizer_switch() {
    if (!switch_changed) return;
    if (had_materialization)
      thd->variables.optimizer_switch |= OPTIMIZER_SWITCH_MATERIALIZATION;
    switch_changed = false;
  }

 private:
  THD *thd{nullptr};
  bool had_materialization{false};
  bool switch_changed{false};
};

OffloadContext *context(THD *thd) {
  return static_cast<OffloadContext *>(
      thd->lex->secondary_engine_execution_context());
}

// Our own error for a scalar subquery with more than one row (the one-row
// check of a derived table made from a scalar subquery); DuckDB's own
// message for a scalar subquery is matched too.
constexpr const char *kDuckDBMultipleRows =
    "More than one row returned by a subquery used as an expression";

bool is_string(const Item *item) {
  return item->result_type() == STRING_RESULT && !item->is_temporal();
}

// Constant while the statement runs: a constant of the statement, or a
// prepared statement parameter, a stored program variable, a user variable
// (T1): their value is printed at each execution (value_literal()).
bool is_constant(const Item *item) {
  if (item->const_item()) return true;
  const Item *r = const_cast<Item *>(item)->real_item();
  if (r->type() == Item::PARAM_ITEM || r->type() == Item::ROUTINE_FIELD_ITEM)
    return true;
  // The optimizer caches a value fixed for the statement (NOW() compared
  // with a column): constant when what it caches is.
  if (r->type() == Item::CACHE_ITEM) {
    const Item *example = down_cast<const Item_cache *>(r)->get_example();
    return example != nullptr && is_constant(example);
  }
  if (r->type() != Item::FUNC_ITEM) return false;
  const auto *fn = down_cast<const Item_func *>(r);
  if (fn->functype() == Item_func::GUSERVAR_FUNC) return true;
  // The clock functions fixed at the start of the statement (T3).
  const std::string name = fn->func_name();
  return name == "now" || name == "utc_timestamp" || name == "curdate" ||
         name == "utc_date" || name == "curtime" || name == "utc_time" ||
         (name == "unix_timestamp" && fn->argument_count() == 0);
}

// Columns whose lake value is not what MySQL uses in every context: TIME
// (signed microseconds; MySQL's string and number forms differ), ENUM and
// SET (value plus hidden index), YEAR and BIT (numbers that MySQL shows as
// '0000' or as bytes in string contexts).
// JSON and GEOMETRY (2026-11-23, OPAQUE): stored as MySQL prints them (JSON
// text, SRID + WKB bytes), so selecting the column, IS [NOT] NULL and COUNT
// give MySQL's result; comparisons, functions, grouping, ordering, DISTINCT
// and other aggregates follow JSON / spatial rules DuckDB does not share:
// refused.
enum class SpecialColumn { NONE, NUMBER, OTHER, OPAQUE };
SpecialColumn special_column(const Item *item) {
  if (item->type() != Item::FIELD_ITEM) return SpecialColumn::NONE;
  const Field *f = down_cast<const Item_field *>(item)->field;
  if (f == nullptr) return SpecialColumn::NONE;
  switch (f->real_type()) {
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_BIT:
      return SpecialColumn::NUMBER;
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return SpecialColumn::OTHER;
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_GEOMETRY:
      return SpecialColumn::OPAQUE;
    default:
      return SpecialColumn::NONE;
  }
}

bool is_unicode_charset(const CHARSET_INFO *cs) {
  return cs->csname != nullptr && (strcmp(cs->csname, "utf8mb4") == 0 ||
                                   strcmp(cs->csname, "utf8mb3") == 0);
}

// How strings of a column compare in DuckDB, where strings are UTF-8 bytes
// compared without padding:
// - RAW: binary strings and NO PAD binary collations (utf8mb4_0900_bin):
//   byte order is the collation order;
// - PAD: PAD SPACE binary collations of utf8mb3/utf8mb4 (utf8mb3_bin,
//   utf8mb4_bin): code point order with trailing spaces ignored, reproduced
//   with rtrim (equality) and a padded sort key (ordering);
// - WEIGHT: any other collation: the copy has MySQL's weight of each value
//   in "__w_<column>" (DWL#6 step A); constants are weighted when we print
//   the SQL; other expressions are refused;
// - REFUSE: not used (kept for a collation without weights).
enum class StrMode { RAW, PAD, WEIGHT, REFUSE };

StrMode str_mode_cs(const CHARSET_INFO *cs) {
  if (cs == &my_charset_bin) return StrMode::RAW;
  if ((cs->state & MY_CS_BINSORT) != 0 && is_unicode_charset(cs))
    return cs->pad_attribute == NO_PAD ? StrMode::RAW : StrMode::PAD;
  return needs_weight(cs) ? StrMode::WEIGHT : StrMode::REFUSE;
}

// A BLOB literal of any bytes.
std::string blob_literal(const std::string &bytes) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out = "'";
  for (unsigned char b : bytes) {
    out += "\\x";
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out + "'::BLOB";
}

std::vector<std::string> split_list(const std::string &joined) {
  std::vector<std::string> out;
  size_t start = 0;
  while (!joined.empty() && start <= joined.size()) {
    size_t end = joined.find('\x1f', start);
    if (end == std::string::npos) end = joined.size();
    out.push_back(joined.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

StrMode str_mode(const Item *item) {
  return str_mode_cs(item->collation.collation);
}

// DATE_FORMAT's format (T3, 2026-11-24), printed from DuckDB date parts of
// x (a TIMESTAMP expression) as MySQL prints each code; codes MySQL takes
// from default_week_format or the week rules (%U %u %V %v %X %x) and a
// trailing '%' are refused, non-ASCII formats too. *fixed_numeric: every
// code gives a fixed number of digits (the outputs of one format then
// order under any collation as their bytes do). x empty: only checks.
bool date_format_sql(const std::string &fmt, const std::string &x,
                     std::string *out, bool *fixed_numeric) {
  auto num = [&](const std::string &part, int width) {
    const std::string v = "CAST(" + part + " AS VARCHAR)";
    return width == 0 ? v
                      : "lpad(" + v + ", " + std::to_string(width) + ", '0')";
  };
  const std::string h12 = "((hour(" + x + ") + 11) % 12 + 1)";
  const std::string ampm =
      "CASE WHEN hour(" + x + ") < 12 THEN 'AM' ELSE 'PM' END";
  std::vector<std::string> parts;
  std::string text;  // literal characters waiting to be added
  bool fixed = true;
  auto flush = [&]() {
    if (text.empty()) return;
    parts.push_back("'" + text + "'");
    text.clear();
  };
  auto add = [&](const std::string &piece) {
    flush();
    parts.push_back(piece);
  };
  for (size_t i = 0; i < fmt.size(); i++) {
    const unsigned char c = fmt[i];
    if (c >= 0x80) return false;
    if (c != '%') {
      if (c == '\'') text += '\'';
      text += static_cast<char>(c);
      continue;
    }
    if (++i == fmt.size()) return false;
    switch (fmt[i]) {
      case 'Y': add(num("year(" + x + ")", 4)); break;
      case 'y': add(num("year(" + x + ") % 100", 2)); break;
      case 'm': add(num("month(" + x + ")", 2)); break;
      case 'c': add(num("month(" + x + ")", 0)); fixed = false; break;
      case 'd': add(num("day(" + x + ")", 2)); break;
      case 'e': add(num("day(" + x + ")", 0)); fixed = false; break;
      case 'j': add(num("dayofyear(" + x + ")", 3)); break;
      case 'H': add(num("hour(" + x + ")", 2)); break;
      case 'k': add(num("hour(" + x + ")", 0)); fixed = false; break;
      case 'h':
      case 'I': add(num(h12, 2)); break;
      case 'l': add(num(h12, 0)); fixed = false; break;
      case 'i': add(num("minute(" + x + ")", 2)); break;
      case 'S':
      case 's': add(num("second(" + x + ")", 2)); break;
      // DuckDB's microsecond() includes the seconds.
      case 'f': add(num("microsecond(" + x + ") % 1000000", 6)); break;
      case 'T':
        add(num("hour(" + x + ")", 2) + " || ':' || " +
            num("minute(" + x + ")", 2) + " || ':' || " +
            num("second(" + x + ")", 2));
        break;
      case 'r':
        add(num(h12, 2) + " || ':' || " + num("minute(" + x + ")", 2) +
            " || ':' || " + num("second(" + x + ")", 2) + " || ' ' || " +
            ampm);
        fixed = false;
        break;
      case 'p': add(ampm); fixed = false; break;
      case 'M': add("monthname(" + x + ")"); fixed = false; break;
      case 'b': add("left(monthname(" + x + "), 3)"); fixed = false; break;
      case 'W': add("dayname(" + x + ")"); fixed = false; break;
      case 'a': add("left(dayname(" + x + "), 3)"); fixed = false; break;
      case 'w': add(num("dayofweek(" + x + ")", 0)); break;  // 0 = Sunday
      case 'D':
        add(num("day(" + x + ")", 0) + " || CASE WHEN day(" + x +
            ") IN (11, 12, 13) THEN 'th' WHEN day(" + x +
            ") % 10 = 1 THEN 'st' WHEN day(" + x +
            ") % 10 = 2 THEN 'nd' WHEN day(" + x +
            ") % 10 = 3 THEN 'rd' ELSE 'th' END");
        fixed = false;
        break;
      case 'U':
      case 'u':
      case 'V':
      case 'v':
      case 'X':
      case 'x':
        return false;
      default:  // %% and any other character: the character itself
        if (fmt[i] == '\'') text += '\'';
        text += fmt[i];
        break;
    }
  }
  flush();
  if (parts.empty()) parts.push_back("''");
  if (fixed_numeric != nullptr) *fixed_numeric = fixed;
  if (out != nullptr) {
    *out = "(";
    for (size_t k = 0; k < parts.size(); k++)
      *out += (k == 0 ? "" : " || ") + parts[k];
    *out += ")";
  }
  return true;
}

// The constant format of a DATE_FORMAT call, as bytes; false if not one.
bool date_format_format(const Item_func *fn, std::string *fmt) {
  Item *f = fn->arguments()[1]->real_item();
  if (!is_constant(f) || f->is_null()) return false;
  String buf;
  const String *v = f->val_str(&buf);
  if (v == nullptr) return false;
  fmt->assign(v->ptr(), v->length());
  return true;
}

// A string expression whose values compare under its collation as their
// bytes: DATE_FORMAT with a format we translate (equality: one format gives
// one spelling of each value; ordering: fixed-width numeric formats only).
// GROUP BY, DISTINCT and ORDER BY use it directly instead of a weight.
bool exact_text(const Item *item, bool ordering) {
  const Item *r = const_cast<Item *>(item)->real_item();
  if (r->type() != Item::FUNC_ITEM) return false;
  const auto *fn = down_cast<const Item_func *>(r);
  if (strcmp(fn->func_name(), "date_format") != 0) return false;
  std::string fmt;
  bool fixed = false;
  if (!date_format_format(fn, &fmt) ||
      !date_format_sql(fmt, "x", nullptr, &fixed))
    return false;
  return !ordering || fixed;
}

bool is_date_or_datetime(enum_field_types t) {
  return t == MYSQL_TYPE_DATE || t == MYSQL_TYPE_NEWDATE ||
         t == MYSQL_TYPE_DATETIME;
}

// Comparing "as if padded with spaces": rtrim, then pad with at least as many
// spaces as the longest operand can have. This orders exactly like the PAD
// SPACE collation, including strings that continue with characters below a
// space ('a' > 'a\t').
std::string pad_sort_key(const std::string &e, size_t max_bytes) {
  return "(rtrim(" + e + ", ' ') || repeat(' ', " +
         std::to_string(max_bytes + 1) + "))";
}

// A CHAR column (not BINARY): stored without trailing spaces (the binary
// log strips them, the load too), so under PAD SPACE its equality key is the
// column itself; rtrim() would hide it from DuckDB's filter pushdown and
// statistics (TPC-H: PART 4M estimated rows instead of 20K, 2026-11-26).
bool is_char_column(const Item *item) {
  const Item *r = const_cast<Item *>(item)->real_item();
  if (r->type() != Item::FIELD_ITEM) return false;
  const auto *fi = down_cast<const Item_field *>(r);
  const Field *f = fi->field;
  // Base tables only: a derived table's or view's CHAR column is an
  // expression in DuckDB, which may keep trailing spaces.
  return f != nullptr && f->real_type() == MYSQL_TYPE_STRING &&
         f->charset() != &my_charset_bin && fi->m_table_ref != nullptr &&
         !fi->m_table_ref->is_view_or_derived() &&
         f->table != nullptr && f->table->s->tmp_table == NO_TMP_TABLE;
}

std::string pad_equal_key(const std::string &e) {
  return "rtrim(" + e + ", ' ')";
}

// Kinds a set operation may combine: numbers with numbers, DATE with
// DATETIME; otherwise the same kind.
int kind_class(Kind k) {
  switch (k) {
    case Kind::INT:
    case Kind::UINT:
    case Kind::DOUBLE:
    case Kind::DECIMAL:
      return 0;
    case Kind::STRING:
      return 1;
    case Kind::BLOB:
      return 2;
    case Kind::DATE:
    case Kind::DATETIME:
      return 3;
    case Kind::TIMESTAMP:
      return 4;
    case Kind::BIT:
      return 5;
    case Kind::TIME:
      return 6;
  }
  return -1;
}

bool same_class(Kind a, Kind b) { return kind_class(a) == kind_class(b); }

class Generator {
 public:
  Generator(THD *thd, std::string *reason) : m_thd(thd), m_reason(reason) {}

  bool generate(LEX *lex, std::string *sql, std::vector<Kind> *kinds);

  // DECIMAL values MySQL declares with more than 38 digits: computed in
  // DuckDB as DECIMAL(38, scale), exact while the values fit (OffloadContext
  // runs such statements first, duckdb_decimal_overflow).
  bool decimal_wide() const { return m_decimal_wide; }
  /** A grouped string is shown with an in-query check (grouped_value):
      the statement runs first, during optimization. */
  bool grouped_check() const { return m_grouped_check; }
  // duckdb_decimal_overflow=ROUND, after an overflow: DECIMAL results keep
  // at most this many decimals (-1: all).
  void set_decimal_scale_cap(int cap) { m_scale_cap = cap; }
  int decimal_max_scale() const { return m_max_scale; }

 private:
  bool refuse(const std::string &why) {
    *m_reason = why;
    return true;
  }
  // DECIMAL(38, scale) for a DECIMAL result, with the ROUND cap.
  std::string dec38(unsigned scale) {
    m_max_scale = std::max(m_max_scale, static_cast<int>(scale));
    const unsigned s = m_scale_cap >= 0
                           ? std::min(scale, static_cast<unsigned>(m_scale_cap))
                           : scale;
    return "DECIMAL(38," + std::to_string(s) + ")";
  }
  bool m_decimal_wide{false};
  bool m_grouped_check{false};
  int m_scale_cap{-1};
  int m_max_scale{0};
  bool refuse_collation(const Item *item) {
    const char *name = item->type() == Item::FIELD_ITEM
                           ? down_cast<const Item_field *>(item)->field_name
                           : "expression";
    return refuse(std::string("collation:") + name + " (" +
                  item->collation.collation->m_coll_name + ")");
  }
  bool expr(Item *item, std::string *out);
  bool function(Item_func *fn, std::string *out);
  bool aggregate(Item_sum *s, std::string *out);
  bool operands(Item_func *fn, bool ordering, unsigned from, unsigned to,
                std::vector<std::string> *ops);
  bool numeric(Item *item, std::string *out, bool in_decimal = false);
  bool unsigned_arith(Item *left, Item *right, char op, std::string *out);
  bool time_literal(Item *item, std::string *out);
  bool special_args(Item_func *fn);
  int division_frac(int f1, int f2) const;
  bool internal_frac(Item *item, int *frac);
  bool exact_division(const std::string &a, int f1, const std::string &b,
                      int f2, int round_to, int a_digits, std::string *out);
  bool temporal_arg(Item *item, std::string *out);
  bool cast(Item_func *fn, std::string *out);
  bool order_key(Item *item, std::string *out);
  bool enum_index(Item *item, std::string *out);
  bool weight_expr(Item *item, const CHARSET_INFO *cs, std::string *out);
  bool string_key(Item *item, const CHARSET_INFO *cs, bool ordering,
                  size_t max_bytes, std::string *out);
  bool literal(Item *item, std::string *out);
  bool typed_literal(Item *item, enum_field_types type, std::string *out);
  bool output_kind(Item *item, Kind *kind, std::string *cast);

  // An AND part of a condition and the tables it uses.
  struct Conj {
    std::string sql;
    table_map tables;
    // A pair of a multiple equality (equality propagation): implied by the
    // other pairs, so it may be left out where one of its columns is not
    // visible (another semi-join's table).
    bool implied{false};
    Item_multi_eq *eq{nullptr};  // the multiple equality of an implied pair
    // A plain column = column equality: its two columns.
    Item_field *eq_a{nullptr};
    Item_field *eq_b{nullptr};
  };
  bool table_ref(Table_ref *tr, std::string *out);
  bool from_list(mem_root_deque<Table_ref *> &list, bool under_outer,
                 bool in_semi, std::string *out);
  bool conjuncts(Item *cond, table_map avoid, std::vector<Conj> *out);
  bool multi_eq(Item_multi_eq *eq, table_map avoid, std::vector<Conj> *out);
  bool comparable(Item *a, Item *b);
  bool block_sql(Query_block *qb, const std::vector<std::string> *names,
                 std::vector<Kind> *kinds, std::string *sql,
                 const std::vector<Item *> *targets = nullptr,
                 const std::string *from_sql = nullptr,
                 std::set<std::string> *weights_out = nullptr,
                 bool weights_required = false);
  bool setop_sql(Query_term_set_op *t, const std::vector<std::string> *names,
                 const std::vector<Item *> *targets, std::vector<Kind> *kinds,
                 std::string *sql, std::set<std::string> *weights_out = nullptr,
                 bool weights_required = false);
  bool subquery_sql(Query_expression *unit, std::string *sql);
  bool subquery(Item_subselect *sub, std::string *out);
  std::string truth(const std::string &e, Item::Bool_test test, bool *ok);

  // A table of the query: its alias and primary key (base tables).
  struct TableInfo {
    std::string alias;
    std::string pk;          // the key, or row(...) of its columns
    std::string pk_parts;    // its columns
    std::string pk_negated;  // negated (empty unless all integers)
    bool derived{false};     // a materialized derived table (our SQL)
    bool positional{false};  // columns named c1, c2, ... (set operations)
    bool scalar{false};      // a scalar subquery the server made a table:
                             // exactly one row (or none)
    std::set<std::string> weights;  // columns with a weight column
    std::vector<const Field *> pk_fields;  // base tables
  };
  // What is being printed in one query block.
  struct Block {
    Block *parent{nullptr};    // the enclosing block of a subquery
    Query_block *qb{nullptr};  // the query block printed
    std::set<const TABLE *> tables;  // the tables its columns may use
    bool post_group{false};          // select list, HAVING, ORDER BY
    int agg_depth{0};
    // PAD SPACE columns in GROUP BY: grouped on rtrim(); outside an
    // aggregate shown as the value of the row with the smallest primary
    // key, the row InnoDB meets first (single table only).
    std::vector<const Field *> group_pad;
    // Grouped string expressions under PAD SPACE (grouped on rtrim()):
    // shown with the per-group check (grouped_expression()).
    std::vector<Item *> group_pad_exprs;
    std::set<const Field *> grouped;  // GROUP BY columns
    struct Semi {
      Table_ref *nest;
      bool anti;
      std::string from;
      table_map tables{0};  // the tables of the nest (or derived table)
      Item *cond{nullptr};  // an anti-join's own condition (ON)
    };
    std::vector<Semi> semi;  // semi- and anti-join nests at the top
    // WHERE parts printed elsewhere: the IS NULL of a derived anti-join
    // (derived_marker()).
    std::set<const Item *> skip_where;
    // [NOT] EXISTS printed in place of a derived table's IS [NOT] NULL
    // marker inside OR (derived_marker()).
    std::map<const Item *, std::string> marker_sql;
  };
  bool grouped_value(const Field *field, const TableInfo &ti,
                     const std::string &col, std::string *out);

  THD *m_thd;
  Item_field *source_column(Item_field *f);
  bool value_literal(Item *item, std::string *out);
  // While an anti-join's condition is printed: the tables of the other
  // semi-join nests (not visible there) and of this nest.
  table_map m_anti_others{0};
  table_map m_anti_mine{0};
  Item_field *outer_equal(Item_field *f);
  Item *derived_marker(Table_ref *e, bool *anti, bool *top);
  bool unique_match(Table_ref *e);
  bool lateral_keys(Table_ref *e,
                    std::vector<std::pair<Item *, std::string>> *keys);
  // Tables a derived table printed now may reference as LATERAL (the
  // tables before it in its FROM list; in an EXISTS also the outer
  // query's), and the correlation for the next derived block printed.
  table_map m_lateral_ok{0};
  table_map m_main_visible{0};
  std::vector<std::pair<Item *, std::string>> m_lateral_keys;
  // Set while the derived table of a derived anti-join is printed: its
  // block leaves DISTINCT out (NOT EXISTS does not need it).
  bool m_drop_distinct{false};
  bool grouped_expression(Item *item, std::string *out);
  std::string *m_reason;
  bool m_full{false};
  std::map<const TABLE *, TableInfo> m_info;
  int m_next_alias{0};
  Block *m_block{nullptr};
  // While printing the operands of a comparison with a DATE, DATETIME or
  // TIMESTAMP column: constants are values of that type, as MySQL compares.
  enum_field_types m_temporal{MYSQL_TYPE_NULL};
};

// A constant compared with a DATE, DATETIME or TIMESTAMP: MySQL converts it
// to that type; we print the converted value as a typed DuckDB literal.
// TIMESTAMP columns hold UTC in DuckDB: the value, given in the session time
// zone, is converted to UTC.
bool Generator::typed_literal(Item *item, enum_field_types type,
                              std::string *out) {
  Datetime_val t;
  if (item->val_datetime(&t, 0) || item->null_value)
    return refuse("temporal_literal");
  char buf[80];
  if (type == MYSQL_TYPE_DATE || type == MYSQL_TYPE_NEWDATE) {
    if (t.hour != 0 || t.minute != 0 || t.second != 0 || t.second_part != 0)
      return refuse("temporal_literal_with_time");
    snprintf(buf, sizeof(buf), "DATE '%04u-%02u-%02u'", t.year, t.month,
             t.day);
  } else if (type == MYSQL_TYPE_TIMESTAMP) {
    bool in_gap = false;
    const my_time_t secs = m_thd->time_zone()->TIME_to_gmt_sec(&t, &in_gap);
    if (in_gap) return refuse("timestamp_literal_in_dst_gap");
    MYSQL_TIME u;
    my_tz_UTC->gmt_sec_to_TIME(&u, secs);
    snprintf(buf, sizeof(buf),
             "TIMESTAMP '%04u-%02u-%02u %02u:%02u:%02u.%06lu'", u.year,
             u.month, u.day, u.hour, u.minute, u.second,
             static_cast<unsigned long>(t.second_part));
  } else {
    snprintf(buf, sizeof(buf),
             "TIMESTAMP '%04u-%02u-%02u %02u:%02u:%02u.%06lu'", t.year,
             t.month, t.day, t.hour, t.minute, t.second,
             static_cast<unsigned long>(t.second_part));
  }
  *out = buf;
  return false;
}

// A constant compared with a TIME column: converted to TIME as MySQL does
// (Item::val_time), as signed microseconds.
bool Generator::time_literal(Item *item, std::string *out) {
  Time_val t;
  if (item->val_time(&t) || item->null_value) return refuse("time_literal");
  *out = "CAST(" + std::to_string(t.to_microseconds()) + " AS BIGINT)";
  return false;
}

// The value of an item that is constant while the statement runs but is not
// a literal (T1, 2026-11-23): a prepared statement's parameter (?), a stored
// program's variable or parameter, a user variable (@x). The SQL is
// generated again at every EXECUTE / CALL, so each run prints its own
// values. Typed by the value's kind, as MySQL compares it: integers as
// integers, DECIMAL exactly (more than 38 digits refused), DOUBLE as DOUBLE,
// strings converted to UTF-8 (their collation is the item's, as for a
// string literal).
bool Generator::value_literal(Item *item, std::string *out) {
  switch (item->result_type()) {
    case INT_RESULT: {
      const longlong v = item->val_int();
      if (item->null_value) {
        *out = "NULL";
        return false;
      }
      char buf[32];
      if (item->unsigned_flag)
        snprintf(buf, sizeof(buf), "%llu", static_cast<unsigned long long>(v));
      else
        snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
      *out = buf;
      return false;
    }
    case REAL_RESULT: {
      const double d = item->val_real();
      if (item->null_value) {
        *out = "NULL";
        return false;
      }
      if (!std::isfinite(d)) return refuse("value_not_finite");
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", d);
      *out = std::string("CAST(") + buf + " AS DOUBLE)";
      return false;
    }
    case DECIMAL_RESULT: {
      my_decimal dec;
      const my_decimal *d = item->val_decimal(&dec);
      if (item->null_value || d == nullptr) {
        *out = "NULL";
        return false;
      }
      if (my_decimal_length_to_precision(d->intg + d->frac, d->frac, false) >
          38)
        return refuse("decimal_value_over_38_digits");
      String text;
      if (my_decimal2string(E_DEC_FATAL_ERROR, d, &text) != E_DEC_OK)
        return refuse("decimal_value");
      *out = std::string(text.ptr(), text.length());
      return false;
    }
    case STRING_RESULT: {
      String buf;
      String *v = item->val_str(&buf);
      if (item->null_value || v == nullptr) {
        *out = "NULL";
        return false;
      }
      String utf8;
      unsigned errors = 0;
      utf8.copy(v->ptr(), v->length(), v->charset(), &my_charset_utf8mb4_bin,
                &errors);
      if (errors != 0) return refuse("value_charset");
      std::string text = "'";
      for (size_t i = 0; i < utf8.length(); i++) {
        const char c = utf8.ptr()[i];
        if (c == '\'') text += '\'';
        if (c == '\0') return refuse("value_nul");
        text += c;
      }
      *out = text + "'";
      return false;
    }
    default:
      return refuse("unsupported_value_type");
  }
}

bool Generator::literal(Item *item, std::string *out) {
  if (item->is_null()) {
    *out = "NULL";
    return false;
  }
  switch (item->type()) {
    case Item::INT_ITEM: {
      char buf[32];
      if (item->unsigned_flag)
        snprintf(buf, sizeof(buf), "%llu",
                 static_cast<unsigned long long>(item->val_int()));
      else
        snprintf(buf, sizeof(buf), "%lld",
                 static_cast<long long>(item->val_int()));
      *out = buf;
      return false;
    }
    case Item::DECIMAL_ITEM:
      // DuckDB reads a numeric literal of more than 38 digits as DOUBLE (or
      // HUGEINT): the comparison would no longer be exact.
      if (item->decimal_precision() > 38)
        return refuse("decimal_literal_over_38_digits");
      [[fallthrough]];
    case Item::REAL_ITEM: {
      String buf;
      String *s = item->val_str(&buf);
      if (s == nullptr) return refuse("literal");
      *out = std::string(s->ptr(), s->length());
      return false;
    }
    case Item::STRING_ITEM: {
      String buf;
      String *s = item->val_str(&buf);
      if (s == nullptr) return refuse("literal");
      String utf8;
      unsigned errors = 0;
      utf8.copy(s->ptr(), s->length(), s->charset(),
                &my_charset_utf8mb4_bin, &errors);
      if (errors != 0) return refuse("literal_charset");
      std::string text = "'";
      for (size_t i = 0; i < utf8.length(); i++) {
        const char c = utf8.ptr()[i];
        if (c == '\'') text += '\'';
        if (c == '\0') return refuse("literal_nul");
        text += c;
      }
      *out = text + "'";
      return false;
    }
    case Item::PARAM_ITEM:
    case Item::ROUTINE_FIELD_ITEM:
      return value_literal(item, out);
    default:
      return refuse(std::string("unsupported_literal:") +
                    std::to_string(static_cast<int>(item->type())));
  }
}

// The operands args[from, to) of a comparison, printed so that DuckDB
// compares them as MySQL does:
// - with a DATE/DATETIME/TIMESTAMP column, constants become typed literals;
// - strings follow their column's collation (RAW, PAD; others refused);
// - a string column with a number, or a TIMESTAMP with another temporal or
//   string column, is refused (MySQL converts in ways DuckDB does not).
// `ordering`: <, >, BETWEEN (PAD strings use the sort key) versus =, IN.
bool Generator::operands(Item_func *fn, bool ordering, unsigned from,
                         unsigned to, std::vector<std::string> *ops) {
  Item **args = fn->arguments();
  enum_field_types temporal = MYSQL_TYPE_NULL;
  bool timestamp = false, strings = false, numbers = false, pad = false;
  bool const_numbers = false, time = false, const_datetime = false;
  size_t max_bytes = 0;
  for (unsigned i = from; i < to; i++) {
    Item *a = args[i]->real_item();
    if (is_constant(a)) {
      if (is_string(a))
        max_bytes = std::max<size_t>(max_bytes, a->max_length);
      else if (a->data_type() == MYSQL_TYPE_DATETIME ||
               a->data_type() == MYSQL_TYPE_TIMESTAMP)
        const_datetime = true;  // NOW(): a DATE column compares as DATETIME
      else if (!a->is_temporal() && !a->is_null())
        const_numbers = true;
      continue;
    }
    const enum_field_types t = a->data_type();
    if (t == MYSQL_TYPE_TIME) {
      time = true;
    } else if (t == MYSQL_TYPE_TIMESTAMP) {
      timestamp = true;
    } else if (is_date_or_datetime(t)) {
      if (temporal == MYSQL_TYPE_NULL || t == MYSQL_TYPE_DATETIME)
        temporal = t;
    } else if (is_string(a)) {
      strings = true;
      if (str_mode(a) == StrMode::REFUSE) return refuse_collation(a);
      max_bytes = std::max<size_t>(max_bytes, a->max_length);
    } else {
      numbers = true;
    }
  }
  // TIME (signed microseconds in the lake) only with TIME columns and
  // constants, which MySQL compares as TIME (each converted to TIME).
  if (time && (timestamp || temporal != MYSQL_TYPE_NULL || strings || numbers))
    return refuse("mixed_time_comparison");
  if (timestamp && (temporal != MYSQL_TYPE_NULL || strings || numbers))
    return refuse("mixed_timestamp_comparison");
  if (temporal != MYSQL_TYPE_NULL && (strings || numbers))
    return refuse("mixed_temporal_comparison");
  if (temporal == MYSQL_TYPE_DATE && const_datetime)
    temporal = MYSQL_TYPE_DATETIME;
  // ENUM / SET columns compared with number constants: MySQL compares as
  // DOUBLE, the column giving its index / bitmask (val_real).
  if (strings && const_numbers && !numbers) {
    bool all_enum = true;
    for (unsigned i = from; i < to; i++) {
      Item *a = args[i]->real_item();
      if (is_constant(a)) {
        all_enum = all_enum && !is_string(a);
        continue;
      }
      all_enum = all_enum && special_column(a) == SpecialColumn::OTHER &&
                 !a->is_temporal();
    }
    if (all_enum) {
      for (unsigned i = from; i < to; i++) {
        Item *a = args[i]->real_item();
        std::string s;
        if (is_constant(a)) {
          if (a->is_null()) {
            s = "NULL";
          } else {
            char buf[64];
            snprintf(buf, sizeof(buf), "CAST(%.17g AS DOUBLE)", a->val_real());
            s = buf;
          }
        } else {
          std::string n;
          if (enum_index(a, &n)) return true;
          s = "CAST(" + n + " AS DOUBLE)";
        }
        ops->push_back(s);
      }
      return false;
    }
  }
  // MySQL compares a string with a number as DOUBLE; DuckDB would cast the
  // string to the number's type (and fail on 'ab').
  if (strings && (numbers || const_numbers))
    return refuse("string_number_comparison");
  // The comparison's collation decides (a COLLATE clause or a column of
  // another collation can change it).
  const CHARSET_INFO *cs = nullptr;
  if (strings) {
    cs = fn->compare_collation();
    for (unsigned i = from; i < to && cs == nullptr; i++)
      if (is_string(args[i]->real_item()) && !is_constant(args[i]->real_item()))
        cs = args[i]->real_item()->collation.collation;
    if (cs == nullptr || str_mode_cs(cs) == StrMode::REFUSE)
      return refuse(std::string("collation:comparison (") +
                    (cs != nullptr ? cs->m_coll_name : "unknown") + ")");
  }
  (void)pad;
  const enum_field_types saved = m_temporal;
  m_temporal = timestamp ? MYSQL_TYPE_TIMESTAMP : temporal;
  bool failed = false;
  for (unsigned i = from; i < to && !failed; i++) {
    Item *a = args[i]->real_item();
    std::string s;
    if (time && is_constant(a)) {
      failed = a->is_null() ? literal(a, &s) : time_literal(a, &s);
    } else if (m_temporal != MYSQL_TYPE_NULL && is_constant(a)) {
      failed = a->is_null() ? literal(a, &s) : typed_literal(a, m_temporal, &s);
    } else if (numbers && is_constant(a) && is_string(a)) {
      // A number compared with a string constant: MySQL compares as DOUBLE.
      char buf[64];
      const double v = a->val_real();
      snprintf(buf, sizeof(buf), "CAST(%.17g AS DOUBLE)", v);
      s = a->null_value ? "NULL" : buf;
    } else if (strings && is_string(a) && !ordering &&
               str_mode_cs(cs) == StrMode::WEIGHT &&
               (cs->state & MY_CS_BINSORT) != 0) {
      // =, <>, IN under a binary collation of another character set
      // (latin1_bin): equal means the same characters (apart from trailing
      // spaces under PAD SPACE), which the UTF-8 copy keeps one to one, so
      // no weight is needed and expressions (SUBSTR) work; ordering keeps
      // the weights (the character set's byte order is not UTF-8's).
      std::string e;
      failed = expr(a, &e);
      s = cs->pad_attribute == NO_PAD || is_char_column(a) ? e
                                                          : pad_equal_key(e);
    } else if (strings && is_string(a)) {
      // The comparison key of the collation: bytes, rtrim / padded key, or
      // MySQL's weight.
      failed = string_key(a, cs, ordering, max_bytes, &s);
    } else {
      failed = expr(a, &s);
    }
    ops->push_back(s);
  }
  m_temporal = saved;
  return failed;
}

// MySQL's decimal division (mysys/decimal.cc, do_div_mod): operand scales
// are rounded up to 9-digit words, div_precision_increment is reduced by
// that padding, and the quotient is truncated at a whole number of words:
// F fractional digits. That truncated value is what other expressions use;
// only the output rounds it (half up) to the item's scale.
int Generator::division_frac(int f1, int f2) const {
  auto words = [](int digits) { return (digits + 8) / 9; };
  const int frac1 = words(f1) * 9, frac2 = words(f2) * 9;
  int incr = static_cast<int>(m_thd->variables.div_precincrement) -
             (frac1 - f1) - (frac2 - f2);
  if (incr < 0) incr = 0;
  return words(frac1 + frac2 + incr) * 9;
}

// The scale of an operand's value as MySQL holds it: arithmetic functions
// (division included) round their value to their scale; AVG keeps the
// division's F digits.
bool Generator::internal_frac(Item *item, int *frac) {
  item = item->real_item();
  if (item->type() == Item::SUM_FUNC_ITEM) {
    auto *sum = down_cast<Item_sum *>(item);
    if ((sum->sum_func() == Item_sum::AVG_FUNC ||
         sum->sum_func() == Item_sum::AVG_DISTINCT_FUNC) &&
        sum->result_type() == DECIMAL_RESULT && sum->argument_count() == 1) {
      *frac = division_frac(static_cast<int>(sum->get_arg(0)->decimals), 0);
      return false;
    }
  }
  *frac = static_cast<int>(item->decimals);
  return false;
}

// a / b exactly as MySQL: with A, B the operands times 10^f1, 10^f2 (exact
// integers), R = |A| 10^(F - f1 + f2) // |B| (truncated), with the sign of
// A * B, as DECIMAL(38, F), then rounded half up to round_to digits (unless
// -1); NULL when b is 0. Computed in HUGEINT: a value
// too large for 128 bits is a DuckDB error, never a wrong value.
bool Generator::exact_division(const std::string &a, int f1,
                               const std::string &b, int f2, int round_to,
                               int a_digits, std::string *out) {
  const int F_mysql = division_frac(f1, f2);
  if (F_mysql > 36 || F_mysql - f1 + f2 < 0 || f1 > 18 || f2 > 18)
    return refuse("exact_division_precision");
  // duckdb_decimal_overflow=ROUND after an overflow: the quotient is
  // computed with one digit more than the kept decimals (then rounded), so
  // the scaled HUGEINT intermediate is smaller.
  const int kept_mysql = round_to >= 0 && round_to < F_mysql ? round_to : F_mysql;
  m_max_scale = std::max(m_max_scale, kept_mysql);
  int F = F_mysql;
  if (m_scale_cap >= 0 && m_scale_cap + 1 < F)
    F = std::max(m_scale_cap + 1, f1 - f2);
  const int shift = F - f1 + f2;
  // The dividend (a_digits digits) times 10^shift is a 128-bit integer:
  // above 38 digits a large value overflows, so the statement runs first
  // (duckdb_decimal_overflow), whatever precision MySQL declares.
  if (a_digits + F_mysql - f1 + f2 > 38) m_decimal_wide = true;
  auto pow10 = [](int n) {
    return "CAST('1" + std::string(static_cast<size_t>(n), '0') +
           "' AS HUGEINT)";
  };
  // The value times 10^scale as an exact integer: integer part and
  // fraction separately (x * 10^scale in DECIMAL overflows DuckDB's
  // 38-digit intermediate for values that fit).
  auto scaled = [&](const std::string &e, int scale) {
    if (scale == 0) return "CAST(" + e + " AS HUGEINT)";
    const std::string x =
        "CAST(" + e + " AS DECIMAL(38," + std::to_string(scale) + "))";
    return "(CAST(trunc(" + x + ") AS HUGEINT) * " + pow10(scale) +
           " + CAST((" + x + " - trunc(" + x + ")) * " + pow10(scale) +
           " AS HUGEINT))";
  };
  const std::string A = scaled(a, f1), B = scaled(b, f2);
  const std::string R =
      "((abs(" + A + ") * " + pow10(shift) + ") // abs(" + B + "))";
  const std::string signed_r = "(CASE WHEN (" + A + " < 0) <> (" + B +
                               " < 0) THEN -" + R + " ELSE " + R + " END)";
  const std::string unit =
      F == 0 ? std::string("CAST(1 AS DECIMAL(1,0))")
             : "CAST('0." + std::string(static_cast<size_t>(F - 1), '0') +
                   "1' AS DECIMAL(" + std::to_string(F + 1) + "," +
                   std::to_string(F) + "))";
  std::string value = "CAST(CAST(" + signed_r + " AS DECIMAL(38,0)) * " +
                      unit + " AS DECIMAL(38," + std::to_string(F) + "))";
  // DuckDB's DECIMAL cast rounds half away from zero, as MySQL's HALF_UP.
  if (round_to >= 0 && round_to < F)
    value = "CAST(" + value + " AS DECIMAL(38," + std::to_string(round_to) +
            "))";
  // duckdb_decimal_overflow=ROUND after an overflow: fewer decimals.
  if (m_scale_cap >= 0 && m_scale_cap < kept_mysql &&
      m_scale_cap < (round_to >= 0 && round_to < F ? round_to : F))
    value = "CAST(" + value + " AS DECIMAL(38," + std::to_string(m_scale_cap) +
            "))";
  *out = "(CASE WHEN " + B + " = 0 THEN NULL ELSE " + value + " END)";
  return false;
}

// An arithmetic operand: a number (no string or temporal conversion), with
// integers widened to BIGINT as MySQL computes them (DuckDB keeps INTEGER and
// would overflow at 32 bits). Unsigned integers are refused (MySQL's mixed
// signed/unsigned rules).
// The scale DuckDB gives the expression we print for item, when we know
// it: DECIMAL columns and literals keep MySQL's scale, integers 0, + and -
// the larger, * the sum, as MySQL (above 30 decimals MySQL caps: unknown).
// -1: unknown (the caller casts).
int decimal_scale(const Item *item) {
  const Item *a = const_cast<Item *>(item)->real_item();
  if (a->result_type() == INT_RESULT && !a->is_temporal() &&
      special_column(a) == SpecialColumn::NONE)
    return 0;
  if (a->result_type() != DECIMAL_RESULT) return -1;
  if (a->type() == Item::FIELD_ITEM || a->type() == Item::DECIMAL_ITEM)
    return a->decimals;
  if (a->type() != Item::FUNC_ITEM) return -1;
  const auto *fn = down_cast<const Item_func *>(a);
  if (fn->argument_count() != 2) return -1;
  const int l = decimal_scale(fn->arguments()[0]);
  const int r = decimal_scale(fn->arguments()[1]);
  if (l < 0 || r < 0) return -1;
  int sc = -1;
  switch (fn->functype()) {
    case Item_func::PLUS_FUNC:
    case Item_func::MINUS_FUNC:
      sc = std::max(l, r);
      break;
    case Item_func::MUL_FUNC:
      sc = l + r;
      break;
    default:
      return -1;
  }
  // Only when MySQL agrees (its own caps, a wide result printed with a
  // cast): otherwise the caller casts.
  if (sc != static_cast<int>(fn->decimals) || sc > 30 ||
      fn->decimal_precision() > 38)
    return -1;
  return sc;
}

bool Generator::numeric(Item *item, std::string *out, bool in_decimal) {
  Item *a = item->real_item();
  // An ENUM / SET column as a number is its index / bitmask (val_real).
  if (special_column(a) == SpecialColumn::OTHER && !a->is_temporal()) {
    std::string n;
    if (enum_index(a, &n)) return true;
    *out = "CAST(" + n + " AS DOUBLE)";
    return false;
  }
  if (a->is_temporal() || a->result_type() == STRING_RESULT)
    return refuse("arithmetic_on_non_number");
  if (a->result_type() == INT_RESULT && a->unsigned_flag && !is_constant(a))
    return refuse("arithmetic_on_unsigned");
  std::string e;
  if (expr(a, &e)) return true;
  *out = a->result_type() == INT_RESULT && !in_decimal
             ? "CAST(" + e + " AS BIGINT)"
             : e;
  return false;
}

// The largest value an unsigned integer column can hold (BIT(n), YEAR,
// unsigned TINYINT..BIGINT); 0 if it is not one.
unsigned __int128 unsigned_column_max(const Item *item) {
  if (item->type() != Item::FIELD_ITEM) return 0;
  const Field *f = down_cast<const Item_field *>(item)->field;
  if (f == nullptr) return 0;
  switch (f->real_type()) {
    case MYSQL_TYPE_BIT: {
      const uint32 bits = f->field_length;
      if (bits < 1 || bits > 64) return 0;
      return (static_cast<unsigned __int128>(1) << bits) - 1;
    }
    case MYSQL_TYPE_YEAR:
      return 2155;
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      if (!f->is_unsigned()) return 0;
      const uint32 bytes = f->pack_length();
      if (bytes < 1 || bytes > 8) return 0;
      return (static_cast<unsigned __int128>(1) << (8 * bytes)) - 1;
    }
    default:
      return 0;
  }
}

// Unsigned integer + or * (the result is unsigned when an operand is,
// Item_func_plus/mul): MySQL raises error 1690 when the result leaves
// 0..2^64-1, and which rows DuckDB evaluates may differ from MySQL, so we
// translate only what cannot leave the range: unsigned columns (BIT, YEAR,
// unsigned integers) and non-negative integer constants whose largest values
// keep the result within 64 bits (b + 0, y + 1, u * 2 of a small type).
bool Generator::unsigned_arith(Item *left, Item *right, char op,
                               std::string *out) {
  constexpr unsigned __int128 kMax = ~static_cast<uint64_t>(0);
  unsigned __int128 max[2];
  std::string sql[2];
  Item *ops[2] = {left->real_item(), right->real_item()};
  for (int i = 0; i < 2; i++) {
    Item *a = ops[i];
    if (is_constant(a)) {
      if (a->type() != Item::INT_ITEM || a->is_null())
        return refuse("arithmetic_on_unsigned");
      const longlong v = a->val_int();
      if (!a->unsigned_flag && v < 0) return refuse("arithmetic_on_unsigned");
      max[i] = static_cast<uint64_t>(v);
      sql[i] = std::to_string(static_cast<uint64_t>(v));
    } else {
      max[i] = unsigned_column_max(a);
      if (max[i] == 0) return refuse("arithmetic_on_unsigned");
      std::string e;
      if (expr(a, &e)) return true;
      sql[i] = e;
    }
  }
  // max[i] <= 2^64 - 1, so neither sum nor product overflows 128 bits.
  const unsigned __int128 bound =
      op == '+' ? max[0] + max[1] : max[0] * max[1];
  if (bound > kMax) return refuse("arithmetic_on_unsigned");
  *out = "(CAST(" + sql[0] + " AS UBIGINT) " + op + " CAST(" + sql[1] +
         " AS UBIGINT))";
  return false;
}

// The argument of a date function: a DATE or DATETIME column or expression.
// TIMESTAMP (UTC in DuckDB) and strings are refused.
bool Generator::temporal_arg(Item *item, std::string *out) {
  Item *a = item->real_item();
  if (!is_date_or_datetime(a->data_type()))
    return refuse("date_function_argument");
  // A constant of DATE / DATETIME type (NOW(), CURDATE(), a parameter): its
  // value, typed as it is.
  if (is_constant(a))
    return typed_literal(a,
                         a->data_type() == MYSQL_TYPE_DATE
                             ? MYSQL_TYPE_DATE
                             : MYSQL_TYPE_DATETIME,
                         out);
  return expr(a, out);
}

// The hidden index column of an ENUM / SET column of this block's base
// tables (ENUM index, SET bitmask): what MySQL sorts on and uses as a
// number. In the grouped part of a block the column is grouped on its value,
// which has one index (ENUM / SET elements are distinct under the
// collation): min() picks it. Columns of derived tables and set operations
// carry no index: refused.
bool Generator::enum_index(Item *item, std::string *out) {
  Item *e = item->real_item();
  if (special_column(e) != SpecialColumn::OTHER ||
      down_cast<Item_field *>(e)->field->real_type() == MYSQL_TYPE_TIME2)
    return refuse("enum_index");
  const Field *f = down_cast<Item_field *>(e)->field;
  const auto info = m_info.find(f->table);
  if (info == m_info.end() || info->second.derived ||
      info->second.positional || info->second.scalar ||
      m_block->tables.count(f->table) == 0)
    return refuse("enum_order_outside_base_table");
  const std::string col = info->second.alias + "." +
                          quote_ident(std::string(kIndexPrefix) + f->field_name);
  *out = m_block->post_group && m_block->agg_depth == 0 ? "min(" + col + ")"
                                                        : col;
  return false;
}

// An ORDER BY expression or MIN/MAX argument: PAD SPACE strings use the
// padded sort key.
bool Generator::order_key(Item *item, std::string *out) {
  Item *e = item->real_item();
  // DATE_FORMAT with a fixed-width numeric format: its bytes order as its
  // collation does (exact_text()).
  if (is_string(e) && exact_text(e, true)) return expr(e, out);
  if (is_string(e))
    return string_key(e, e->collation.collation, true, e->max_length, out);
  return expr(e, out);
}

// The weight of a string under collation cs: the weight column of a column
// (the column's own collation must be cs), or the weight of a constant,
// computed now with MySQL's collation code. Other expressions are refused.
bool Generator::weight_expr(Item *item, const CHARSET_INFO *cs,
                            std::string *out) {
  item = item->real_item();
  if (item->type() == Item::CACHE_ITEM &&
      down_cast<Item_cache *>(item)->get_example() != nullptr)
    item = down_cast<Item_cache *>(item)->get_example()->real_item();
  if (is_constant(item)) {
    if (item->is_null()) {
      *out = "NULL";
      return false;
    }
    String buf;
    const String *v = item->val_str(&buf);
    if (v == nullptr) {
      *out = "NULL";
      return false;
    }
    String conv;
    uint errors = 0;
    if (conv.copy(v->ptr(), v->length(), v->charset(), cs, &errors) ||
        errors != 0)
      return refuse("weighted_constant_charset");
    *out = blob_literal(collation_weight(cs, conv.ptr(), conv.length()));
    return false;
  }
  if (item->type() != Item::FIELD_ITEM)
    return refuse(std::string("collation:expression (") + cs->m_coll_name +
                  ")");
  auto *f = down_cast<Item_field *>(item);
  if (f->field == nullptr) return refuse("weight_without_field");
  if (f->field->charset() != cs)
    return refuse(std::string("collation:") + f->field_name + " compared as " +
                  cs->m_coll_name);
  const Block *owner = m_block;
  while (owner != nullptr && owner->tables.count(f->field->table) == 0)
    owner = owner->parent;
  if (owner == nullptr) return refuse("outer_or_other_table_column");
  const TableInfo &ti = m_info[f->field->table];
  const std::string name =
      ti.positional ? "c" + std::to_string(f->field->field_index() -
                                           f->field->table->hidden_field_count +
                                           1)
                    : std::string(f->field->field_name);
  if (ti.weights.count(name) == 0)
    return refuse(std::string("collation:") + f->field_name + " (" +
                  cs->m_coll_name + ", copy without weights: reload it)");
  *out = ti.alias + "." + quote_ident(std::string(kWeightPrefix) + name);
  return false;
}

// The key a string compares by under collation cs: its bytes (binary
// collations), rtrim / padded key (PAD SPACE binary), or MySQL's weight;
// for ordering a PAD SPACE weight gets the space weight appended up to the
// longest value (max_bytes), so 'a' sorts as 'a   ' (after 'a\t').
bool Generator::string_key(Item *item, const CHARSET_INFO *cs, bool ordering,
                           size_t max_bytes, std::string *out) {
  switch (str_mode_cs(cs)) {
    case StrMode::RAW:
      return expr(item, out);
    case StrMode::PAD: {
      std::string e;
      if (expr(item, &e)) return true;
      *out = ordering                ? pad_sort_key(e, max_bytes)
             : is_char_column(item) ? e
                                    : pad_equal_key(e);
      return false;
    }
    case StrMode::WEIGHT: {
      // An expression (no weight column) under a binary PAD SPACE collation
      // of latin1 or ascii: equality is the bytes without trailing spaces,
      // and the order of the bytes is the order of the code points, which
      // is the order of the UTF-8 text in the lake (2026-11-22, TPC-H Q22).
      if (item->real_item()->type() != Item::FIELD_ITEM &&
          (cs->state & MY_CS_BINSORT) != 0 && cs->pad_attribute != NO_PAD &&
          (strcmp(cs->csname, "latin1") == 0 ||
           strcmp(cs->csname, "ascii") == 0)) {
        std::string e;
        if (expr(item, &e)) return true;
        *out = ordering ? pad_sort_key(e, max_bytes) : pad_equal_key(e);
        return false;
      }
      if (weight_expr(item, cs, out)) return true;
      if (ordering && cs->pad_attribute != NO_PAD) {
        const size_t chars = max_bytes / std::max<uint>(cs->mbmaxlen, 1) + 1;
        if (chars > 4096) return refuse("padded_order_too_long");
        std::string pad;
        const std::string space = space_weight(cs);
        for (size_t i = 0; i < chars; i++) pad += space;
        *out = "(" + *out + " || " + blob_literal(pad) + ")";
      }
      return false;
    }
    default:
      return refuse(std::string("collation:") + cs->m_coll_name);
  }
}

// A grouped string column (PAD or weight collation) outside an aggregate:
// the group holds values equal under the collation that may differ ('a',
// 'A '). MySQL shows the first row it met. On one table that is the row
// with the smallest primary key; when GROUP BY contains the column's
// table's primary key every row of the group has the same value; otherwise
// (a join) the first row depends on the join order: refused (decided
// 2026-11-14: exactly as InnoDB). Exception (2026-11-17): a CHAR column
// under a binary collation: MySQL reads CHAR without trailing spaces (the
// copy stores it so, from the load and from the binary log, where
// Field_string::pack strips them), so values equal under the PAD SPACE
// binary comparison are byte-identical: every row of a group shows the
// same value, whichever InnoDB meets first.
bool Generator::grouped_value(const Field *field, const TableInfo &ti,
                              const std::string &col, std::string *out) {
  if (field != nullptr && field->real_type() == MYSQL_TYPE_STRING &&
      (field->charset()->state & MY_CS_BINSORT) != 0) {
    *out = "any_value(" + col + ")";
    return false;
  }
  bool pk_grouped = !ti.pk_fields.empty();
  for (const Field *k : ti.pk_fields)
    pk_grouped = pk_grouped && m_block->grouped.count(k) > 0;
  if (pk_grouped) {
    *out = "any_value(" + col + ")";
    return false;
  }
  if (m_block->tables.size() == 1 && !ti.pk.empty()) {
    *out = "arg_min(" + col + ", " + ti.pk + ")";
    return false;
  }
  // A join: InnoDB shows the value of the first row of the group in its
  // join order, which we cannot reproduce. When the group holds a single
  // stored value (no 'done' next to 'done ' or 'Done'), there is no choice
  // and any row's value is InnoDB's. We check that per group, in the query
  // itself (same snapshot as the result): otherwise the marker error
  // stops it. Such statements run first, during optimization; the marker
  // becomes a refusal and InnoDB answers (2026-11-22, next step 2).
  m_grouped_check = true;
  *out = "CASE WHEN count(DISTINCT " + col + ") > 1 THEN error('" +
         kGroupedAmbiguous + "') ELSE any_value(" + col + ") END";
  return false;
}

// With a join, GROUP BY and a DISTINCT aggregate the server's plan
// materializes the join into a temporary table first, and create_tmp_table()
// replaces the aggregate's column arguments with columns of that table
// (sum_item->set_arg(Item_field(<temporary>.col)); not recorded for later
// rollback in a regular statement). We translate the statement, not that
// plan: a temporary column made from a table column keeps that column's
// database, table and name (Field::new_field), and we find the column again
// among the tables of this block and the enclosing ones. Ambiguous (the same
// table twice in the query) or not a plain column: nullptr, and the query
// is refused as before.
Item_field *Generator::source_column(Item_field *f) {
  const Field *tf = f->field;
  if (tf == nullptr || tf->table == nullptr ||
      tf->table->s->tmp_table == NO_TMP_TABLE || tf->orig_table_name == nullptr ||
      tf->orig_db_name == nullptr || tf->field_name == nullptr)
    return nullptr;
  Field *found = nullptr;
  for (const Block *b = m_block; b != nullptr; b = b->parent) {
    for (const TABLE *t : b->tables) {
      const Table_ref *tr = t->pos_in_table_list;
      if (tr == nullptr || tr->db == nullptr || tr->table_name == nullptr ||
          strcmp(tr->db, tf->orig_db_name) != 0 ||
          strcmp(tr->table_name, tf->orig_table_name) != 0)
        continue;
      for (Field **fp = t->field; *fp != nullptr; fp++) {
        if (my_strcasecmp(system_charset_info, (*fp)->field_name,
                          tf->field_name) != 0)
          continue;
        if (found != nullptr) return nullptr;  // the table twice: ambiguous
        found = *fp;
      }
    }
  }
  if (found == nullptr) return nullptr;
  return new (m_thd->mem_root) Item_field(found);
}

// TPC-H Q21: EXISTS (... l2 ... l2.k = l1.k) AND NOT EXISTS (... l3 ...
// l3.k = l1.k). The optimizer propagates l1.k = l2.k = l3.k and rewrites the
// anti-join's condition with the "best" equal field: l3.k = l2.k, a column
// of the other nest, not visible in our NOT EXISTS. That multiple equality
// holds for every row that passes WHERE (the semi-join's condition is part
// of it), so an equal column of the outer tables can stand for it. The
// Item_field keeps its multiple equalities (the one of its nest, and the
// one across nests MySQL keeps for hash join to undo this same
// substitution). Never a column of this nest (l3.k = l3.k) or of another
// nest; nullptr when there is no outer one.
Item_field *Generator::outer_equal(Item_field *f) {
  const table_map hidden = m_anti_others | m_anti_mine;
  for (Item_multi_eq *eq : {f->multi_equality(), f->item_equal_all_join_nests}) {
    if (eq == nullptr) continue;
    for (Item_field &m : eq->get_fields()) {
      if (m.field == nullptr || (m.used_tables() & hidden) != 0) continue;
      const Block *owner = m_block;
      while (owner != nullptr && owner->tables.count(m.field->table) == 0)
        owner = owner->parent;
      if (owner != nullptr && comparable(f, &m)) return &m;
    }
  }
  return nullptr;
}

bool Generator::aggregate(Item_sum *s, std::string *out) {
  if (s->m_is_window_function) return refuse("window_function");
  if (m_block->agg_depth > 0) return refuse("nested_aggregate");
  if (s->aggr_query_block != nullptr && m_block->qb != nullptr &&
      s->aggr_query_block != m_block->qb)
    return refuse("aggregate_of_outer_query");
  const Item_sum::Sumfunctype f = s->sum_func();
  if (s->argument_count() != 1) return refuse("aggregate_arguments");
  Item *arg = s->get_arg(0)->real_item();
  if (special_column(arg) == SpecialColumn::OPAQUE &&
      f != Item_sum::COUNT_FUNC)
    return refuse(std::string("json_or_geometry_in_aggregate:") +
                  s->func_name());
  m_block->agg_depth++;
  struct Leave {
    int &d;
    ~Leave() { d--; }
  } leave{m_block->agg_depth};
  std::string a;
  switch (f) {
    case Item_sum::COUNT_FUNC:
      if (is_constant(arg) && !arg->is_null()) {
        *out = "count(*)";
        return false;
      }
      if (expr(arg, &a)) return true;
      *out = "count(" + a + ")";
      return false;
    case Item_sum::COUNT_DISTINCT_FUNC:
      if (is_string(arg)) {
        if (string_key(arg, arg->collation.collation, false, arg->max_length,
                       &a))
          return true;
      } else if (expr(arg, &a)) {
        return true;
      }
      *out = "count(DISTINCT " + a + ")";
      return false;
    case Item_sum::SUM_FUNC:
    case Item_sum::SUM_DISTINCT_FUNC:
    case Item_sum::AVG_FUNC:
    case Item_sum::AVG_DISTINCT_FUNC: {
      if (is_string(arg) || arg->is_temporal())
        return refuse("sum_or_avg_of_non_number");
      // DuckDB adds FLOAT / DOUBLE values in parallel, in no fixed order:
      // the last digits differ from InnoDB's (which adds in row order) and
      // from one run to the next. Refused only when the user asks for it
      // (duckdb_offload_float_aggregates=REFUSE; the default accepts it).
      if (arg->result_type() == REAL_RESULT &&
          float_aggregates(m_thd) == FloatAggregates::REFUSE)
        return refuse(
            "sum_or_avg_of_float: DuckDB adds FLOAT / DOUBLE values in no "
            "fixed order, the last digits can differ from InnoDB "
            "(duckdb_offload_float_aggregates=REFUSE)");
      if (expr(arg, &a)) return true;
      const bool distinct = f == Item_sum::SUM_DISTINCT_FUNC ||
                            f == Item_sum::AVG_DISTINCT_FUNC;
      const std::string d = distinct ? "DISTINCT " : "";
      if (f == Item_sum::SUM_FUNC || f == Item_sum::SUM_DISTINCT_FUNC) {
        *out = "sum(" + d + a + ")";
        return false;
      }
      // MySQL's AVG of integers and decimals is an exact decimal; DuckDB's
      // AVG is DOUBLE: exact sum divided by the count.
      if (arg->result_type() == REAL_RESULT) {
        *out = "avg(" + d + a + ")";
        return false;
      }
      // MySQL divides the exact sum by the count with its decimal division
      // (Item_sum_avg): the same exact division as `/`.
      // AVG keeps the truncated digits (Item_sum_avg::val_decimal does not
      // round; the output does).
      // The sum has at most the argument's digits plus 19 (row count).
      return exact_division("sum(" + d + a + ")",
                            static_cast<int>(arg->decimals),
                            "count(" + d + a + ")", 0, -1,
                            static_cast<int>(arg->decimal_precision()) + 19,
                            out);
    }
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC: {
      const char *fn = f == Item_sum::MIN_FUNC ? "min" : "max";
      if (is_string(arg) && (str_mode(arg) == StrMode::PAD ||
                             str_mode(arg) == StrMode::WEIGHT)) {
        // Values equal under PAD SPACE ('' and ' ') tie: MySQL keeps the
        // first one met, the smallest primary key in a scan. The tie is
        // broken on the primary key (negated for MAX).
        // In a join the first row met depends on the join order: refused.
        if (m_block->tables.size() > 1) return refuse("padded_min_max_in_join");
        if (arg->type() != Item::FIELD_ITEM)
          return refuse("padded_min_max_of_expression");
        const auto info =
            m_info.find(down_cast<Item_field *>(arg)->field->table);
        if (info == m_info.end() || info->second.pk_negated.empty())
          return refuse("padded_min_max_key");
        std::string key;
        if (expr(arg, &a) || order_key(arg, &key)) return true;
        const bool min = f == Item_sum::MIN_FUNC;
        *out = std::string(min ? "arg_min(" : "arg_max(") + a + ", row(" +
               key + ", " +
               (min ? info->second.pk_parts : info->second.pk_negated) +
               ")) FILTER (WHERE " + a + " IS NOT NULL)";
        return false;
      }
      if (is_string(arg) && str_mode(arg) == StrMode::REFUSE)
        return refuse_collation(arg);
      if (expr(arg, &a)) return true;
      *out = std::string(fn) + "(" + a + ")";
      return false;
    }
    default:
      return refuse(std::string("unsupported_aggregate:") + s->func_name());
  }
}

// Such columns are translated only by the functions that know them:
// comparisons and IN/BETWEEN (operands(): constants converted as MySQL
// does), arithmetic (numeric(), unsigned_arith()), IS [NOT] NULL. Elsewhere
// (LIKE, CASE, IF, IFNULL, COALESCE, string and date functions) they are
// refused, except YEAR and BIT where the function's result is an integer.
bool Generator::special_args(Item_func *fn) {
  // JSON / GEOMETRY: only IS [NOT] NULL.
  if (fn->functype() != Item_func::ISNULL_FUNC &&
      fn->functype() != Item_func::ISNOTNULL_FUNC) {
    Item **a = fn->arguments();
    for (unsigned i = 0; i < fn->argument_count(); i++)
      if (special_column(a[i]->real_item()) == SpecialColumn::OPAQUE)
        return refuse(std::string("json_or_geometry_in_function:") +
                      fn->func_name());
  }
  switch (fn->functype()) {
    case Item_func::EQ_FUNC:
    case Item_func::NE_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::GE_FUNC:
    case Item_func::BETWEEN:
    case Item_func::IN_FUNC:
    case Item_func::ISNULL_FUNC:
    case Item_func::ISNOTNULL_FUNC:
    case Item_func::PLUS_FUNC:
    case Item_func::MINUS_FUNC:
    case Item_func::MUL_FUNC:
    case Item_func::DIV_FUNC:
    case Item_func::MOD_FUNC:
    case Item_func::NEG_FUNC:
      return false;
    default:
      break;
  }
  Item **args = fn->arguments();
  for (unsigned i = 0; i < fn->argument_count(); i++) {
    const SpecialColumn k = special_column(args[i]->real_item());
    // LIKE on ENUM / SET matches the string value, as stored.
    if (k == SpecialColumn::OTHER && fn->functype() == Item_func::LIKE_FUNC &&
        !args[i]->real_item()->is_temporal())
      continue;
    if (k == SpecialColumn::OTHER ||
        (k == SpecialColumn::NUMBER && fn->result_type() != INT_RESULT))
      return refuse(std::string("column_type_in_function:") +
                    fn->func_name());
  }
  return false;
}

bool Generator::function(Item_func *fn, std::string *out) {
  Item **args = fn->arguments();
  const unsigned n = fn->argument_count();
  const std::string name = fn->func_name();
  // NOW() / CURRENT_TIMESTAMP / LOCALTIME, CURDATE() / CURRENT_DATE,
  // UTC_TIMESTAMP, UTC_DATE, CURTIME, UTC_TIME, UNIX_TIMESTAMP(): MySQL fixes
  // their value when the statement starts (thd->query_start), the same for
  // every row: we print that value (T3, 2026-11-24). SYSDATE() changes during
  // the statement: not translated.
  if (name == "now" || name == "utc_timestamp" || name == "curdate" ||
      name == "utc_date") {
    const bool date = name == "curdate" || name == "utc_date";
    enum_field_types t = date ? MYSQL_TYPE_DATE : MYSQL_TYPE_DATETIME;
    // Compared with a TIMESTAMP column: in UTC, as the column is stored.
    if (m_temporal == MYSQL_TYPE_TIMESTAMP) t = MYSQL_TYPE_TIMESTAMP;
    return typed_literal(fn, t, out);
  }
  if (name == "curtime" || name == "utc_time") return time_literal(fn, out);
  if (name == "unix_timestamp" && n == 0) return value_literal(fn, out);
  // A user variable (@x): its value, constant in a statement that does not
  // assign it (@x := ... is not translated).
  if (fn->functype() == Item_func::GUSERVAR_FUNC) {
    if (m_temporal != MYSQL_TYPE_NULL && !fn->is_null())
      return typed_literal(fn, m_temporal, out);
    return value_literal(fn, out);
  }
  if (special_args(fn)) return true;
  auto compare = [&](const char *op) {
    std::vector<std::string> ops;
    const bool ordering = op[0] == '<' || op[0] == '>';
    const bool not_equal = std::string(op) == "<>";
    if (operands(fn, ordering && !not_equal, 0, 2, &ops)) return true;
    *out = "(" + ops[0] + " " + op + " " + ops[1] + ")";
    return false;
  };
  auto arith = [&](const char *op) {
    const bool wide =
        fn->result_type() == DECIMAL_RESULT && fn->decimal_precision() > 38;
    if (wide) m_decimal_wide = true;
    if (fn->result_type() == INT_RESULT && fn->unsigned_flag &&
        (op[0] == '+' || op[0] == '*'))
      return unsigned_arith(args[0], args[1], op[0], out);
    std::string l, r;
    // In DECIMAL arithmetic an integer operand stays as it is (DuckDB
    // turns it into DECIMAL(w, 0) exactly); CAST(.. AS BIGINT) would make
    // the operation 128-bit (2026-11-25, about 30% of Q1's time).
    const bool dec = fn->result_type() == DECIMAL_RESULT;
    if (numeric(args[0], &l, dec) || numeric(args[1], &r, dec)) return true;
    *out = "(" + l + " " + op + " " + r + ")";
    // MySQL rounds a DECIMAL result to the item's scale
    // (Item_func_numhybrid::val_decimal): it matters when an operand has
    // more digits (AVG). When DuckDB's own result scale is already MySQL's
    // (decimal_scale()), the CAST changes nothing and is left out: it cost
    // about 30% of Q1's time (2026-11-25).
    if (dec && (wide || decimal_scale(fn) != static_cast<int>(fn->decimals) ||
                m_scale_cap >= 0))
      *out = "CAST(" + *out + " AS " + dec38(fn->decimals) + ")";
    else if (dec)
      m_max_scale = std::max(m_max_scale, static_cast<int>(fn->decimals));
    return false;
  };
  switch (fn->functype()) {
    case Item_func::EQ_FUNC: return compare("=");
    case Item_func::NE_FUNC: return compare("<>");
    case Item_func::LT_FUNC: return compare("<");
    case Item_func::LE_FUNC: return compare("<=");
    case Item_func::GT_FUNC: return compare(">");
    case Item_func::GE_FUNC: return compare(">=");
    case Item_func::ISNULL_FUNC:
    case Item_func::ISNOTNULL_FUNC: {
      std::string a;
      if (expr(args[0], &a)) return true;
      *out = "(" + a +
             (fn->functype() == Item_func::ISNULL_FUNC ? " IS NULL)"
                                                       : " IS NOT NULL)");
      return false;
    }
    case Item_func::NOT_FUNC: {
      std::string a;
      if (expr(args[0], &a)) return true;
      *out = "(NOT " + a + ")";
      return false;
    }
    case Item_func::BETWEEN: {
      std::vector<std::string> ops;
      if (operands(fn, true, 0, 3, &ops)) return true;
      const bool neg = down_cast<Item_func_opt_neg *>(fn)->negated;
      *out = "(" + ops[0] + (neg ? " NOT BETWEEN " : " BETWEEN ") + ops[1] +
             " AND " + ops[2] + ")";
      return false;
    }
    case Item_func::IN_FUNC: {
      for (unsigned i = 1; i < n; i++)
        if (!is_constant(args[i])) return refuse("in_list_not_constant");
      std::vector<std::string> ops;
      if (operands(fn, false, 0, n, &ops)) return true;
      std::string list;
      for (size_t i = 1; i < ops.size(); i++)
        list += (i > 1 ? ", " : "") + ops[i];
      const bool neg = down_cast<Item_func_opt_neg *>(fn)->negated;
      *out = "(" + ops[0] + (neg ? " NOT IN (" : " IN (") + list + "))";
      return false;
    }
    case Item_func::COND_AND_FUNC:
    case Item_func::COND_OR_FUNC: {
      const char *op =
          fn->functype() == Item_func::COND_AND_FUNC ? " AND " : " OR ";
      std::string all;
      for (Item &arg : *down_cast<Item_cond *>(fn)->argument_list()) {
        std::string a;
        if (expr(&arg, &a)) return true;
        all += (all.empty() ? "" : op) + a;
      }
      *out = "(" + all + ")";
      return false;
    }
    case Item_func::MULTI_EQ_FUNC: {
      // The optimizer's equality propagation: f1 = f2 = ... [= const].
      std::vector<Conj> parts;
      if (multi_eq(down_cast<Item_multi_eq *>(fn), 0, &parts)) return true;
      std::string all;
      for (const Conj &c : parts) all += (all.empty() ? "" : " AND ") + c.sql;
      *out = all.empty() ? "TRUE" : "(" + all + ")";
      return false;
    }
    case Item_func::LIKE_FUNC: {
      Item *a = args[0]->real_item();
      if (!is_string(a) || is_constant(a)) return refuse("like_argument");
      if (a->collation.collation == &my_charset_bin)
        return refuse("like_on_binary_string");
      // LIKE compares characters, not weights, and never pads ('a ' LIKE
      // 'a' is false also under PAD SPACE). Under a binary collation of any
      // character set (latin1_bin, utf8mb4_bin, gbk_bin ...) a character
      // matches only itself, and the copy holds each value converted to
      // UTF-8 character by character: DuckDB's LIKE on it is MySQL's. The
      // collation is the LIKE's own (a COLLATE clause can change it).
      const CHARSET_INFO *like_cs = fn->compare_collation();
      if (like_cs == nullptr) like_cs = a->collation.collation;
      const bool binary_like = (like_cs->state & MY_CS_BINSORT) != 0;
      if (!binary_like && str_mode(a) != StrMode::RAW &&
          str_mode(a) != StrMode::PAD)
        return refuse_collation(a);
      if (!is_constant(args[1]) || !is_string(args[1]->real_item()))
        return refuse("like_pattern_not_constant");
      // Trailing spaces are significant in LIKE, PAD SPACE or not.
      std::string s, p;
      if (expr(a, &s) || literal(args[1]->real_item(), &p)) return true;
      // The escape character MySQL uses: the ESCAPE clause, else '\\'
      // (none under NO_BACKSLASH_ESCAPES), Item_func_like::eval_escape_clause.
      // DuckDB's LIKE has no default escape: we always name it.
      auto *like = down_cast<Item_func_like *>(fn);
      if (n > 2 && !is_constant(args[2]))
        return refuse("like_escape_not_constant");
      if (!like->escape_is_evaluated()) return refuse("like_escape");
      const int esc = like->escape();
      std::string escape;
      if (esc != 0) {
        if (esc < 0 || esc > 127) return refuse("like_escape");
        const char c = static_cast<char>(esc);
        escape = c == '\'' ? " ESCAPE ''''" : std::string(" ESCAPE '") + c +
                                                  "'";
      }
      *out = "(" + s + " LIKE " + p + escape + ")";
      return false;
    }
    case Item_func::PLUS_FUNC: return arith("+");
    case Item_func::MINUS_FUNC: return arith("-");
    case Item_func::MUL_FUNC: return arith("*");
    case Item_func::DIV_FUNC: {
      if (name == "DIV") break;  // integer division, below
      if (fn->result_type() == DECIMAL_RESULT) {
        Item *l = args[0]->real_item(), *rt = args[1]->real_item();
        if (l->result_type() == REAL_RESULT || rt->result_type() == REAL_RESULT)
          return refuse("exact_division");
        int f1, f2;
        if (internal_frac(l, &f1) || internal_frac(rt, &f2)) return true;
        std::string a, b;
        if (numeric(l, &a) || numeric(rt, &b)) return true;
        // The value is rounded to the item's scale (as every arithmetic
        // function's, Item_func_numhybrid::val_decimal).
        return exact_division(a, f1, b, f2, static_cast<int>(fn->decimals),
                              static_cast<int>(l->decimal_precision()), out);
      }
      if (fn->result_type() != REAL_RESULT) return refuse("exact_division");
      std::string l, r;
      if (numeric(args[0], &l) || numeric(args[1], &r)) return true;
      *out = "(CAST(" + l + " AS DOUBLE) / NULLIF(CAST(" + r +
             " AS DOUBLE), 0))";
      return false;
    }
    case Item_func::MOD_FUNC: {
      if (fn->result_type() == DECIMAL_RESULT && fn->decimal_precision() > 38)
        m_decimal_wide = true;
      std::string l, r;
      if (numeric(args[0], &l) || numeric(args[1], &r)) return true;
      // Sign of the dividend in both; MySQL gives NULL for a zero divisor.
      *out = "(" + l + " % NULLIF(" + r + ", 0))";
      return false;
    }
    case Item_func::NEG_FUNC: {
      std::string a;
      if (numeric(args[0], &a)) return true;
      // A space: "--" would start a comment when `a` is a negative literal.
      *out = "(- " + a + ")";
      return false;
    }
    case Item_func::ABS_FUNC: {
      std::string a;
      if (numeric(args[0], &a)) return true;
      *out = "abs(" + a + ")";
      return false;
    }
    case Item_func::ROUND_FUNC: {
      // ROUND of exact numbers rounds half away from zero in both; of DOUBLE
      // MySQL rounds half to even. TRUNCATE is not translated.
      if (name != "round") return refuse("truncate");
      if (fn->result_type() == REAL_RESULT ||
          args[0]->result_type() == REAL_RESULT)
        return refuse("round_of_double");
      std::string a;
      if (numeric(args[0], &a)) return true;
      if (n == 1) {
        *out = "round(" + a + ")";
        return false;
      }
      if (!is_constant(args[1]) || args[1]->result_type() != INT_RESULT)
        return refuse("round_digits_not_constant");
      *out = "round(" + a + ", " + std::to_string(args[1]->val_int()) + ")";
      return false;
    }
    case Item_func::CASE_FUNC: {
      auto *c = down_cast<Item_func_case *>(fn);
      const int first = c->get_first_expr_num();
      const int els = c->get_else_expr_num();
      const unsigned pairs = (n - (first >= 0) - (els >= 0)) / 2;
      std::string text = "(CASE";
      if (first >= 0) {
        // Simple CASE compares the operand with each WHEN value: only
        // numbers and RAW strings (no conversion rules to reproduce).
        Item *op = args[first]->real_item();
        if (op->is_temporal()) return refuse("simple_case_on_temporal");
        if (is_string(op) && str_mode(op) != StrMode::RAW)
          return refuse_collation(op);
        for (unsigned i = 0; i < pairs; i++)
          if (is_string(op) != is_string(args[2 * i]->real_item()))
            return refuse("simple_case_types");
        std::string o;
        if (expr(op, &o)) return true;
        text += " " + o;
      }
      for (unsigned i = 0; i < pairs; i++) {
        std::string w, t;
        if (expr(args[2 * i], &w) || expr(args[2 * i + 1], &t)) return true;
        text += " WHEN " + w + " THEN " + t;
      }
      if (els >= 0) {
        std::string e;
        if (expr(args[els], &e)) return true;
        text += " ELSE " + e;
      }
      *out = text + " END)";
      return false;
    }
    case Item_func::ISTRUTH_FUNC: {
      std::string a;
      if (expr(args[0], &a)) return true;
      // The test is only visible through its name (bool_transform_names).
      static const std::pair<const char *, Item::Bool_test> tests[] = {
          {"is true", Item::BOOL_IS_TRUE},
          {"is false", Item::BOOL_IS_FALSE},
          {"is not true", Item::BOOL_NOT_TRUE},
          {"is not false", Item::BOOL_NOT_FALSE}};
      for (const auto &t : tests)
        if (name == t.first) {
          bool ok = true;
          *out = truth(a, t.second, &ok);
          return ok ? false : refuse("truth_test");
        }
      return refuse("truth_test:" + name);
    }
    case Item_func::BOOL_IF_FUNC:  // internal IF with constant branches
    case Item_func::IF_FUNC: {
      std::string c, a, b;
      if (expr(args[0], &c) || expr(args[1], &a) || expr(args[2], &b))
        return true;
      *out = "(CASE WHEN " + c + " THEN " + a + " ELSE " + b + " END)";
      return false;
    }
    case Item_func::NULLIF_FUNC: {
      std::vector<std::string> ops;
      if (operands(fn, false, 0, 2, &ops)) return true;
      // NULLIF returns its first argument, not the trimmed key.
      std::string a;
      if (expr(args[0], &a)) return true;
      *out = "(CASE WHEN " + ops[0] + " = " + ops[1] + " THEN NULL ELSE " + a +
             " END)";
      return false;
    }
    case Item_func::YEAR_FUNC: {
      std::string a;
      if (temporal_arg(args[0], &a)) return true;
      *out = "year(" + a + ")";
      return false;
    }
    default:
      break;
  }
  // Functions without their own functype, by name.
  if (name == "<in_optimizer>") {
    // Wraps an IN subquery predicate (created with it as its only argument;
    // the left operand is added later): we look for it.
    for (unsigned i = 0; i < n; i++) {
      Item *a = args[i] != nullptr ? args[i]->real_item() : nullptr;
      if (a != nullptr && a->type() == Item::SUBQUERY_ITEM)
        return subquery(down_cast<Item_subselect *>(a), out);
    }
    return refuse("in_optimizer");
  }
  if (name == "any_value")
    // The server's rewrite of a correlated scalar subquery without
    // aggregate: MySQL raises an error when a key has several rows, which
    // any_value hides.
    return refuse("correlated_scalar_subquery_without_aggregate");
  if (name == "true" || name == "false") {
    *out = name == "true" ? "TRUE" : "FALSE";
    return false;
  }
  if (name == "coalesce" || name == "ifnull") {
    std::string all;
    for (unsigned i = 0; i < n; i++) {
      std::string a;
      if (expr(args[i], &a)) return true;
      all += (i ? ", " : "") + a;
    }
    *out = "coalesce(" + all + ")";
    return false;
  }
  if (name == "DIV") {
    // Integer division, truncated toward zero in both; NULL for zero.
    if (args[0]->result_type() != INT_RESULT ||
        args[1]->result_type() != INT_RESULT)
      return refuse("div_of_non_integer");
    std::string l, r;
    if (numeric(args[0], &l) || numeric(args[1], &r)) return true;
    *out = "(" + l + " // NULLIF(" + r + ", 0))";
    return false;
  }
  // ---- Functions reviewed against DuckDB (2026-11-17) -------------------
  // Each is translated only where DuckDB gives MySQL's result; the guards
  // below cover the differences found (tests: project_duckdb/offload_queries/duckdb_offload_fn_*.sql).
  // A string argument: character data (not binary strings); its value is
  // stored as UTF-8 character by character, so character positions match.
  // The arguments of these functions are not the comparison's operands: a
  // constant among them is not a date of the enclosing comparison
  // (m_temporal applies to the comparison's own constants only).
  struct OwnArgs {
    enum_field_types &t;
    enum_field_types saved;
    explicit OwnArgs(enum_field_types &x) : t(x), saved(x) {
      t = MYSQL_TYPE_NULL;
    }
    ~OwnArgs() { t = saved; }
  };
  auto text_arg = [&](unsigned i, std::string *o) {
    Item *a = args[i]->real_item();
    if (!is_string(a) || a->collation.collation == &my_charset_bin)
      return refuse("string_function_argument:" + name);
    OwnArgs own(m_temporal);
    return expr(a, o);
  };
  auto int_arg = [&](unsigned i, std::string *o) {
    Item *a = args[i]->real_item();
    if (a->result_type() != INT_RESULT) return refuse("integer_argument:" + name);
    OwnArgs own(m_temporal);
    return numeric(a, o);
  };
  if (fn->functype() == Item_func::EXTRACT_FUNC) {
    // MySQL's EXTRACT of a DATE / DATETIME (Item_extract::val_int); WEEK
    // follows default_week_format (DuckDB: ISO weeks): not translated.
    const interval_type unit = down_cast<Item_extract *>(fn)->int_type;
    std::string a;
    if (temporal_arg(args[0], &a)) return true;
    const std::string t = "CAST(" + a + " AS TIMESTAMP)";
    auto part = [&](const char *p) {
      return "CAST(date_part('" + std::string(p) + "', " + t + ") AS BIGINT)";
    };
    const std::string us = "(" + part("microseconds") + " % 1000000)";
    const std::string hms = "(" + part("hour") + " * 10000 + " +
                            part("minute") + " * 100 + " + part("second") + ")";
    switch (unit) {
      case INTERVAL_YEAR: *out = part("year"); return false;  // 0: year 0
      case INTERVAL_QUARTER: *out = part("quarter"); return false;
      case INTERVAL_MONTH: *out = part("month"); return false;
      case INTERVAL_DAY: *out = part("day"); return false;
      case INTERVAL_HOUR: *out = part("hour"); return false;
      case INTERVAL_MINUTE: *out = part("minute"); return false;
      case INTERVAL_SECOND: *out = part("second"); return false;
      case INTERVAL_MICROSECOND: *out = us; return false;
      case INTERVAL_YEAR_MONTH:
        *out = "(" + part("year") + " * 100 + " + part("month") + ")";
        return false;
      case INTERVAL_DAY_HOUR:
        *out = "(" + part("day") + " * 100 + " + part("hour") + ")";
        return false;
      case INTERVAL_DAY_MINUTE:
        *out = "(" + part("day") + " * 10000 + " + part("hour") + " * 100 + " +
               part("minute") + ")";
        return false;
      case INTERVAL_DAY_SECOND:
        *out = "(" + part("day") + " * 1000000 + " + hms + ")";
        return false;
      case INTERVAL_HOUR_MINUTE:
        *out = "(" + part("hour") + " * 100 + " + part("minute") + ")";
        return false;
      case INTERVAL_HOUR_SECOND: *out = hms; return false;
      case INTERVAL_MINUTE_SECOND:
        *out = "(" + part("minute") + " * 100 + " + part("second") + ")";
        return false;
      case INTERVAL_DAY_MICROSECOND:
        *out = "((" + part("day") + " * 1000000 + " + hms + ") * 1000000 + " +
               us + ")";
        return false;
      case INTERVAL_HOUR_MICROSECOND:
        *out = "(" + hms + " * 1000000 + " + us + ")";
        return false;
      case INTERVAL_MINUTE_MICROSECOND:
        *out = "((" + part("minute") + " * 100 + " + part("second") +
               ") * 1000000 + " + us + ")";
        return false;
      case INTERVAL_SECOND_MICROSECOND:
        *out = "(" + part("second") + " * 1000000 + " + us + ")";
        return false;
      default:
        return refuse("extract_unit");
    }
  }
  if (fn->functype() == Item_func::DATEADD_FUNC) {
    // DATE_ADD / DATE_SUB / ADDDATE / SUBDATE of a DATE or DATETIME with an
    // integer count of one unit. Months clamp to the month's end in both;
    // a result outside 1000-01-01 .. 9999-12-31 is NULL in MySQL.
    auto *ai = down_cast<Item_func_add_interval *>(fn);
    static const char *units[] = {"YEAR",   "QUARTER", "MONTH",  "WEEK",
                                  "DAY",    "HOUR",    "MINUTE", "SECOND",
                                  "MICROSECOND"};
    const interval_type unit = ai->get_interval_type();
    if (unit > INTERVAL_MICROSECOND) return refuse("date_add_unit");
    if (fn->data_type() != MYSQL_TYPE_DATE &&
        fn->data_type() != MYSQL_TYPE_DATETIME)
      return refuse("date_add_result_type");
    std::string a, n;
    if (temporal_arg(args[0], &a) || int_arg(1, &n)) return true;
    const std::string v = "(CAST(" + a + " AS TIMESTAMP) " +
                          (ai->is_subtract() ? "-" : "+") + " to_" +
                          [&] {
                            std::string u = units[unit];
                            for (char &c : u) c = static_cast<char>(tolower(c));
                            if (u == "quarter") return std::string("months");
                            return u + "s";
                          }() +
                          "(" + (unit == INTERVAL_QUARTER ? "3 * " : "") +
                          "CAST(" + n + " AS INTEGER)))";
    const std::string typed =
        fn->data_type() == MYSQL_TYPE_DATE ? "CAST(" + v + " AS DATE)" : v;
    // MySQL computes down to year 0 (0000-01-01) and gives NULL before it
    // or from year 10000 (DuckDB's year 0 is 1 BC).
    *out = "(CASE WHEN date_part('year', " + v + ") < 0 OR date_part('year', " +
           v + ") > 9999 THEN NULL ELSE " + typed + " END)";
    return false;
  }
  if (name == "to_days") {
    // Day number from year 0 (DATEDIFF is TO_DAYS(a) - TO_DAYS(b)); of a
    // constant: MySQL's own value.
    if (is_constant(args[0])) {
      const longlong v = fn->val_int();
      *out = fn->null_value ? "NULL" : "CAST(" + std::to_string(v) + " AS BIGINT)";
      return false;
    }
    std::string a;
    if (temporal_arg(args[0], &a)) return true;
    *out = "(date_diff('day', DATE '0001-01-01', CAST(" + a +
           " AS DATE)) + 366)";
    return false;
  }
  if (name == "last_day") {
    std::string a;
    if (temporal_arg(args[0], &a)) return true;
    *out = "last_day(CAST(" + a + " AS DATE))";
    return false;
  }
  if (name == "date_format") {
    // English names only (lc_time_names = en_US), as DAYNAME.
    if (strcmp(m_thd->variables.lc_time_names->name, "en_US") != 0)
      return refuse("lc_time_names");
    std::string fmt, a;
    if (!date_format_format(fn, &fmt)) return refuse("date_format_format");
    if (temporal_arg(args[0], &a)) return true;
    if (!date_format_sql(fmt, "CAST(" + a + " AS TIMESTAMP)", out, nullptr))
      return refuse("date_format_code");
    return false;
  }
  if (name == "dayname" || name == "monthname") {
    // English names only (lc_time_names = en_US).
    if (strcmp(m_thd->variables.lc_time_names->name, "en_US") != 0)
      return refuse("lc_time_names");
    std::string a;
    if (temporal_arg(args[0], &a)) return true;
    *out = name + "(" + a + ")";
    return false;
  }
  // Numbers.
  if (name == "floor" || name == "ceiling" || name == "sign") {
    std::string a;
    if (numeric(args[0], &a)) return true;
    *out = std::string(name == "ceiling" ? "ceil" : name.c_str()) + "(" + a +
           ")";
    return false;
  }
  if (fn->functype() == Item_func::TRUNCATE_FUNC) {
    // Exact numbers only, constant digits.
    if (args[0]->result_type() == REAL_RESULT)
      return refuse("truncate_of_double");
    if (!is_constant(args[1]) || args[1]->result_type() != INT_RESULT)
      return refuse("truncate_digits_not_constant");
    std::string a;
    if (numeric(args[0], &a)) return true;
    *out = "trunc(" + a + ", " + std::to_string(args[1]->val_int()) + ")";
    if (fn->result_type() == DECIMAL_RESULT)
      *out = "CAST(" + *out + " AS DECIMAL(38," +
             std::to_string(fn->decimals) + "))";
    return false;
  }
  if (name == "sqrt" || name == "ln" || name == "log2" || name == "log10" ||
      (name == "log" && n == 1)) {
    // MySQL gives NULL outside the domain; DuckDB NaN or -inf.
    std::string a;
    if (numeric(args[0], &a)) return true;
    const std::string x = "CAST(" + a + " AS DOUBLE)";
    const std::string f = name == "sqrt"    ? "sqrt"
                          : name == "log2"  ? "log2"
                          : name == "log10" ? "log10"
                                            : "ln";
    *out = "(CASE WHEN " + x + (name == "sqrt" ? " < 0" : " <= 0") +
           " THEN NULL ELSE " + f + "(" + x + ") END)";
    return false;
  }
  if (name == "greatest" || name == "least") {
    // Numbers, compared in the function's type (MySQL's aggregated type);
    // NULL if any argument is NULL (DuckDB skips NULLs).
    std::string type;
    switch (fn->result_type()) {
      case INT_RESULT:
        if (fn->unsigned_flag) return refuse("greatest_least_unsigned");
        type = "BIGINT";
        break;
      case DECIMAL_RESULT:
        type = "DECIMAL(38," + std::to_string(fn->decimals) + ")";
        break;
      case REAL_RESULT:
        type = "DOUBLE";
        break;
      default:
        return refuse("greatest_least_arguments");
    }
    std::string all, nulls;
    for (unsigned i = 0; i < n; i++) {
      Item *a = args[i]->real_item();
      std::string e;
      OwnArgs own(m_temporal);
      if (a->type() == Item::NULL_ITEM) {
        e = "NULL";
      } else if (a->result_type() != INT_RESULT &&
                 a->result_type() != DECIMAL_RESULT &&
                 a->result_type() != REAL_RESULT) {
        return refuse("greatest_least_arguments");
      } else if (numeric(a, &e)) {
        return true;
      }
      e = "CAST(" + e + " AS " + type + ")";
      all += (i ? ", " : "") + e;
      nulls += std::string(i ? " OR " : "") + e + " IS NULL";
    }
    *out = "(CASE WHEN " + nulls + " THEN NULL ELSE " + name + "(" + all +
           ") END)";
    return false;
  }
  // Strings (character data; positions and lengths in characters).
  if (name == "char_length") {
    std::string a;
    if (text_arg(0, &a)) return true;
    *out = "CAST(length(" + a + ") AS BIGINT)";
    return false;
  }
  if (name == "length") {
    // Bytes in the column's character set: the UTF-8 copy has MySQL's
    // bytes only for utf8mb4 / utf8mb3 / ascii.
    Item *a0 = args[0]->real_item();
    if (!is_string(a0) || a0->collation.collation == &my_charset_bin)
      return refuse("string_function_argument:length");
    const char *csname = a0->collation.collation->csname;
    if (strncmp(csname, "utf8mb", 6) != 0 && strcmp(csname, "ascii") != 0)
      return refuse("length_of_charset:" + std::string(csname));
    std::string a;
    if (expr(a0, &a)) return true;
    *out = "CAST(strlen(" + a + ") AS BIGINT)";
    return false;
  }
  if (name == "substr" || name == "left" || name == "right") {
    // MySQL: position 0 or a length below 1 give ''; a negative position
    // counts from the end, beyond the start gives ''. DuckDB returns
    // characters in those cases.
    std::string s0, p, l;
    if (text_arg(0, &s0) || int_arg(1, &p)) return true;
    if (name == "substr" && n == 3 && int_arg(2, &l)) return true;
    const std::string len = "length(" + s0 + ")";
    // Constant position and length (SUBSTRING(c, 1, 2), TPC-H Q22): the
    // edge cases are decided now (2026-11-26).
    auto const_int = [&](unsigned i, long long *v) {
      Item *a = args[i]->real_item();
      if (!is_constant(a) || a->result_type() != INT_RESULT) return false;
      *v = a->val_int();
      return !a->null_value;
    };
    long long pv = 0, lv = 1;
    const std::string empty =
        "(CASE WHEN " + s0 + " IS NULL THEN NULL ELSE '' END)";
    if (const_int(1, &pv) && (name != "substr" || n < 3 || const_int(2, &lv))) {
      if (name == "left" || name == "right") {
        *out = pv < 1 ? empty
                      : name + "(" + s0 + ", " + std::to_string(pv) + ")";
        return false;
      }
      if (pv == 0 || (n == 3 && lv < 1)) {
        *out = empty;
        return false;
      }
      if (pv > 0) {
        *out = "substring(" + s0 + ", " + std::to_string(pv) +
               (n == 3 ? ", " + std::to_string(lv) : std::string()) + ")";
        return false;
      }
    }
    if (name == "left" || name == "right") {
      *out = "(CASE WHEN " + p + " IS NULL OR " + s0 +
             " IS NULL THEN NULL WHEN " + p + " < 1 THEN '' ELSE " + name +
             "(" + s0 + ", " + p + ") END)";
      return false;
    }
    std::string guard = p + " = 0 OR (" + p + " < 0 AND -" + p + " > " + len +
                        ")";
    if (n == 3) guard += " OR " + l + " < 1";
    *out = "(CASE WHEN " + s0 + " IS NULL OR " + p + " IS NULL" +
           (n == 3 ? " OR " + l + " IS NULL" : std::string()) +
           " THEN NULL WHEN " + guard + " THEN '' ELSE substring(" + s0 +
           ", " + p + (n == 3 ? ", " + l : std::string()) + ") END)";
    return false;
  }
  if (name == "concat" || name == "concat_ws") {
    // Strings, integers, DECIMAL, DATE and DATETIME(0) print as in MySQL.
    // CONCAT is NULL with any NULL argument ('||' too); CONCAT_WS skips
    // NULL values (DuckDB's concat_ws too).
    std::vector<std::string> parts;
    for (unsigned i = 0; i < n; i++) {
      Item *a = args[i]->real_item();
      std::string e;
      OwnArgs own(m_temporal);
      if (a->type() == Item::NULL_ITEM) {
        e = "NULL";
      } else if (is_string(a)) {
        if (a->collation.collation == &my_charset_bin)
          return refuse("concat_of_binary");
        if (expr(a, &e)) return true;
      } else if (a->result_type() == INT_RESULT ||
                 a->result_type() == DECIMAL_RESULT) {
        if (a->data_type() == MYSQL_TYPE_YEAR || a->data_type() == MYSQL_TYPE_BIT)
          return refuse("concat_argument");
        if (expr(a, &e)) return true;
        e = "CAST(" + e + " AS VARCHAR)";
      } else if (a->data_type() == MYSQL_TYPE_DATE ||
                 (a->data_type() == MYSQL_TYPE_DATETIME && a->decimals == 0)) {
        if (is_constant(a)) return refuse("concat_argument");
        if (expr(a, &e)) return true;
        e = "CAST(" + e + " AS VARCHAR)";
      } else {
        return refuse("concat_argument");
      }
      parts.push_back(e);
    }
    if (name == "concat") {
      std::string all;
      for (const std::string &e : parts) all += (all.empty() ? "" : " || ") + e;
      *out = "(" + all + ")";
    } else {
      std::string all;
      for (const std::string &e : parts) all += (all.empty() ? "" : ", ") + e;
      *out = "concat_ws(" + all + ")";
    }
    return false;
  }
  if (name == "trim" || name == "ltrim" || name == "rtrim") {
    // Spaces, or one character removed repeatedly (MySQL removes a string,
    // DuckDB any of a set of characters: equal for one character).
    std::string a, r;
    if (text_arg(0, &a)) return true;
    if (n == 2) {
      if (!is_constant(args[1]) || args[1]->is_null())
        return refuse("trim_string");
      String buf;
      String *rs = args[1]->val_str(&buf);
      if (rs == nullptr || rs->numchars() != 1) return refuse("trim_string");
      if (literal(args[1]->real_item(), &r)) return true;
      *out = name + "(" + a + ", " + r + ")";
    } else {
      *out = name + "(" + a + ", ' ')";
    }
    return false;
  }
  if (name == "replace" || name == "reverse") {
    // REPLACE matches bytes in MySQL (case-sensitive under any collation).
    std::string a, b, c;
    if (text_arg(0, &a)) return true;
    if (name == "reverse") {
      // MySQL reverses code points; DuckDB's reverse() keeps grapheme
      // clusters together ('e' + U+0301 stays one). We split into code
      // points and join them reversed ('' and NULL stay as they are).
      *out = "array_to_string(list_reverse(string_split(" + a + ", '')), '')";
      return false;
    }
    if (text_arg(1, &b) || text_arg(2, &c)) return true;
    *out = "replace(" + a + ", " + b + ", " + c + ")";
    return false;
  }
  if (name == "lpad" || name == "rpad") {
    // MySQL: NULL for a negative length, or an empty pad that must grow
    // the value (DuckDB: an error); shorter lengths truncate in both.
    std::string a, l, pad;
    if (text_arg(0, &a) || int_arg(1, &l) || text_arg(2, &pad)) return true;
    // MySQL: an empty pad cannot grow the value, which is returned as is
    // (truncated when longer); DuckDB raises an error.
    *out = "(CASE WHEN " + l + " < 0 THEN NULL WHEN " + pad +
           " = '' THEN left(" + a + ", CAST(" + l + " AS INTEGER)) ELSE " +
           name + "(" + a + ", CAST(" + l + " AS INTEGER), " + pad + ") END)";
    return false;
  }
  if (name == "instr" || (name == "locate" && n == 2)) {
    // A search under the arguments' collation: every non-constant string
    // argument binary (a constant takes the column's collation).
    for (unsigned i = 0; i < 2; i++) {
      Item *a = args[i]->real_item();
      if (!is_constant(a) && (a->collation.collation->state & MY_CS_BINSORT) == 0)
        return refuse_collation(a);
    }
    // The parser builds LOCATE(substr, str) with str first, as INSTR.
    std::string hay, needle;
    if (text_arg(0, &hay) || text_arg(1, &needle)) return true;
    *out = "CAST(instr(" + hay + ", " + needle + ") AS BIGINT)";
    return false;
  }
  if (name == "cast_as_signed" || name == "cast_as_unsigned" ||
      name == "cast_as_decimal" || name == "cast_as_real" ||
      name == "cast_as_char" || name == "cast_as_datetime")
    return cast(fn, out);
  struct DateFn {
    const char *mysql;
    const char *prefix;
    const char *suffix;
    bool time_part;
  };
  static const DateFn date_fns[] = {
      {"month", "month(", ")", false},
      {"dayofmonth", "day(", ")", false},
      {"dayofyear", "dayofyear(", ")", false},
      {"quarter", "quarter(", ")", false},
      {"dayofweek", "(dayofweek(", ") + 1)", false},  // MySQL: 1 = Sunday
      {"weekday", "(isodow(", ") - 1)", false},       // MySQL: 0 = Monday
      {"hour", "hour(", ")", true},
      {"minute", "minute(", ")", true},
      {"second", "second(", ")", true},
      {"cast_as_date", "CAST(", " AS DATE)", false},
  };
  for (const DateFn &d : date_fns) {
    if (name != d.mysql) continue;
    std::string a;
    if (temporal_arg(args[0], &a)) return true;
    if (d.time_part && args[0]->real_item()->data_type() != MYSQL_TYPE_DATETIME)
      a = "CAST(" + a + " AS TIMESTAMP)";
    *out = std::string(d.prefix) + a + d.suffix;
    return false;
  }
  return refuse("unsupported_function:" + name);
}

// CAST / CONVERT (T3, 2026-11-24): the conversions DuckDB does as MySQL.
// Sources: integers, DECIMAL, DATE, DATETIME (not TIMESTAMP: session time
// zone). Strings and DOUBLE as sources are refused: MySQL's string to
// number rules (prefix, warnings) and its DOUBLE printing differ from
// DuckDB's. CAST AS DATE goes through the date functions (function()).
// - SIGNED: integers; DECIMAL rounded half away from zero (both), when
//   its integer part fits in BIGINT (MySQL clips with a warning, DuckDB
//   errors); BIGINT UNSIGNED refused (MySQL reinterprets the bits).
// - UNSIGNED: integers; a negative value wraps to 2^64 + x as in MySQL.
// - DECIMAL(M, D): integers and DECIMAL whose integer part fits (else
//   MySQL clips to 99.9 with a warning); a smaller scale rounds half away
//   from zero in both.
// - DOUBLE / FLOAT: integers (both round to the nearest double).
// - CHAR [(N)]: integers, DECIMAL (printed with MySQL's scale), DATE,
//   DATETIME(0); N truncates (MySQL also warns). BINARY refused.
// - DATETIME(n): DATE, or DATETIME(m) with m <= n (fewer digits round in
//   MySQL).
bool Generator::cast(Item_func *fn, std::string *out) {
  const std::string name = fn->func_name();
  Item *a = fn->arguments()[0]->real_item();
  if (special_column(a) != SpecialColumn::NONE || is_string(a) ||
      a->data_type() == MYSQL_TYPE_TIMESTAMP ||
      a->data_type() == MYSQL_TYPE_TIME || a->data_type() == MYSQL_TYPE_JSON)
    return refuse("cast_source:" + name);
  const bool integer = a->result_type() == INT_RESULT && !a->is_temporal();
  const bool decimal = a->result_type() == DECIMAL_RESULT && !a->is_temporal();
  const enum_field_types t = a->data_type();
  std::string e;
  // A DECIMAL source with the precision and scale MySQL gave it (an
  // expression's DuckDB scale can differ; the cast fixes it).
  auto source = [&](std::string *s) -> bool {
    if (expr(a, s)) return true;
    if (decimal) {
      const unsigned p = a->decimal_precision();
      if (p > 38) return refuse("cast_decimal_over_38_digits");
      *s = "CAST(" + *s + " AS DECIMAL(" + std::to_string(std::max(p, 1u)) +
           ", " + std::to_string(a->decimals) + "))";
    }
    return false;
  };
  if (name == "cast_as_signed") {
    if (integer && !(a->unsigned_flag && t == MYSQL_TYPE_LONGLONG)) {
      if (source(&e)) return true;
      *out = "CAST(" + e + " AS BIGINT)";
      return false;
    }
    if (decimal && a->decimal_int_part() <= 18) {
      if (source(&e)) return true;
      *out = "CAST(" + e + " AS BIGINT)";
      return false;
    }
    return refuse("cast_source:" + name);
  }
  if (name == "cast_as_unsigned") {
    if (!integer) return refuse("cast_source:" + name);
    if (source(&e)) return true;
    if (a->unsigned_flag)
      *out = "CAST(" + e + " AS UBIGINT)";
    else
      *out = "CAST(CASE WHEN " + e + " < 0 THEN CAST(" + e +
             " AS HUGEINT) + 18446744073709551616 ELSE " + e +
             " END AS UBIGINT)";
    return false;
  }
  if (name == "cast_as_decimal") {
    const unsigned p = fn->decimal_precision();
    const unsigned d = fn->decimals;
    if (p > 38) return refuse("cast_decimal_over_38_digits");
    if (!(integer || decimal) ||
        static_cast<unsigned>(a->decimal_int_part()) > p - d)
      return refuse("cast_source:" + name);
    if (source(&e)) return true;
    *out = "CAST(" + e + " AS DECIMAL(" + std::to_string(p) + ", " +
           std::to_string(d) + "))";
    return false;
  }
  if (name == "cast_as_real") {
    if (!integer) return refuse("cast_source:" + name);
    if (source(&e)) return true;
    *out = "CAST(" + e + " AS " +
           (fn->data_type() == MYSQL_TYPE_FLOAT ? "FLOAT" : "DOUBLE") + ")";
    return false;
  }
  if (name == "cast_as_datetime") {
    if (t != MYSQL_TYPE_DATE &&
        !(t == MYSQL_TYPE_DATETIME && a->decimals <= fn->decimals))
      return refuse("cast_source:" + name);
    if (temporal_arg(a, &e)) return true;
    *out = "CAST(" + e + " AS TIMESTAMP)";
    return false;
  }
  // cast_as_char
  if (fn->collation.collation == &my_charset_bin)
    return refuse("cast_as_binary");
  if (integer || decimal) {
    if (source(&e)) return true;
  } else if (t == MYSQL_TYPE_DATE ||
             (t == MYSQL_TYPE_DATETIME && a->decimals == 0)) {
    if (temporal_arg(a, &e)) return true;
  } else {
    return refuse("cast_source:" + name);
  }
  e = "CAST(" + e + " AS VARCHAR)";
  if (a->max_char_length() > fn->max_char_length())
    e = "left(" + e + ", " + std::to_string(fn->max_char_length()) + ")";
  *out = e;
  return false;
}

// A grouped PAD SPACE string expression shown after grouping: the group
// holds values equal under the collation ('13' and '13 '); MySQL shows the
// first row's. As for a grouped column in a join (grouped_value()), we
// check per group that only one stored value is present: the statement
// runs first and a group with two becomes a refusal.
bool Generator::grouped_expression(Item *item, std::string *out) {
  m_block->agg_depth++;
  struct Leave {
    int &d;
    ~Leave() { d--; }
  } leave{m_block->agg_depth};
  std::string e;
  if (expr(item, &e)) return true;
  m_grouped_check = true;
  *out = "CASE WHEN count(DISTINCT " + e + ") > 1 THEN error('" +
         kGroupedAmbiguous + "') ELSE any_value(" + e + ") END";
  return false;
}

bool Generator::expr(Item *item, std::string *out) {
  item = item->real_item();
  if (!m_block->marker_sql.empty()) {
    const auto m = m_block->marker_sql.find(item);
    if (m != m_block->marker_sql.end()) {
      *out = m->second;
      return false;
    }
  }
  if (m_block->post_group && m_block->agg_depth == 0 &&
      item->type() != Item::FIELD_ITEM) {
    for (Item *g : m_block->group_pad_exprs)
      if (item == g || item->eq(g)) return grouped_expression(item, out);
  }
  switch (item->type()) {
    case Item::SUBQUERY_ITEM:
      return subquery(down_cast<Item_subselect *>(item), out);
    case Item::CACHE_ITEM: {
      // A cached copy of an expression (the server's rewrites use them).
      Item *example = down_cast<Item_cache *>(item)->get_example();
      if (example == nullptr) return refuse("cache_without_expression");
      return expr(example, out);
    }
    case Item::FIELD_ITEM: {
      auto *f = down_cast<Item_field *>(item);
      // In an anti-join's condition, a column of another semi-join nest
      // (MySQL's best-equal-field substitution, TPC-H Q21): an equal outer
      // column instead (outer_equal()).
      if (m_anti_others != 0 && f->field != nullptr &&
          (f->used_tables() & m_anti_others) != 0) {
        Item_field *sub = outer_equal(f);
        if (sub == nullptr) return refuse("condition_across_semijoins");
        return expr(sub, out);
      }
      // A column of this block or, in a subquery, of an enclosing block (a
      // correlated reference: aliases are unique in the statement).
      const Block *owner = m_block;
      while (owner != nullptr && f->field != nullptr &&
             owner->tables.count(f->field->table) == 0)
        owner = owner->parent;
      if (f->field != nullptr && owner == nullptr) {
        // A column of a temporary table of the server's plan, made from a
        // column of this statement: that column.
        if (Item_field *src = source_column(f); src != nullptr)
          return expr(src, out);
      }
      if (f->field == nullptr || owner == nullptr)
        return refuse(std::string("outer_or_other_table_column:") +
                      (f->table_name != nullptr ? f->table_name : "") + "." +
                      (f->field_name != nullptr ? f->field_name : ""));
      if (f->field->is_flag_set(NOT_SECONDARY_FLAG))
        return refuse(std::string("not_secondary_column:") + f->field_name);
      const TableInfo &ti = m_info[f->field->table];
      const std::string col =
          ti.positional
              ? ti.alias + ".c" +
                    std::to_string(f->field->field_index() -
                                   f->field->table->hidden_field_count + 1)
              : ti.alias + "." + quote_ident(f->field->field_name);
      if (owner != m_block) {
        *out = col;
        return false;
      }
      // A table made from a scalar subquery has one row: its columns are
      // the same for the whole group (MySQL allows them in HAVING and the
      // select list; DuckDB wants an aggregate).
      if (ti.scalar && m_block->post_group && m_block->agg_depth == 0) {
        *out = "any_value(" + col + ")";
        return false;
      }
      bool group_pad = false;
      for (const Field *g : m_block->group_pad)
        group_pad = group_pad || g == f->field;
      if (m_block->post_group && m_block->agg_depth == 0 && group_pad)
        return grouped_value(f->field, ti, col, out);
      *out = col;
      return false;
    }
    case Item::PARAM_ITEM:
    case Item::ROUTINE_FIELD_ITEM:
    case Item::INT_ITEM:
    case Item::DECIMAL_ITEM:
    case Item::REAL_ITEM:
    case Item::STRING_ITEM:
    case Item::NULL_ITEM:
      if (m_temporal != MYSQL_TYPE_NULL && !item->is_null())
        return typed_literal(item, m_temporal, out);
      return literal(item, out);
    case Item::SUM_FUNC_ITEM:
      return aggregate(down_cast<Item_sum *>(item), out);
    case Item::FUNC_ITEM:
    case Item::COND_ITEM:
      // A constant date or datetime (the optimizer turns string literals
      // compared with temporal columns into date_literal / datetime_literal):
      // MySQL's own value, printed as a typed literal.
      if (item->type() == Item::FUNC_ITEM && item->const_item() &&
          is_date_or_datetime(item->data_type()))
        return typed_literal(item, m_temporal == MYSQL_TYPE_TIMESTAMP
                                       ? MYSQL_TYPE_TIMESTAMP
                                       : item->data_type(),
                             out);
      return function(down_cast<Item_func *>(item), out);
    default:
      return refuse("unsupported_item:" +
                    std::to_string(static_cast<int>(item->type())));
  }
}

// The DuckDB type each select-list value is cast to, matching the MySQL type
// of the item, so the conversion back is one rule per kind.
bool Generator::output_kind(Item *item, Kind *kind, std::string *cast) {
  switch (item->data_type()) {
    case MYSQL_TYPE_TIMESTAMP:
      *kind = Kind::TIMESTAMP;
      *cast = "TIMESTAMP";
      return false;
    case MYSQL_TYPE_DATETIME:
      *kind = Kind::DATETIME;
      *cast = "TIMESTAMP";
      return false;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      *kind = Kind::DATE;
      *cast = "DATE";
      return false;
    case MYSQL_TYPE_YEAR: {
      // The year number (0 for 0000). A YEAR column (base table, derived
      // table, view) is sent zero-filled, "0000" (Field_year::
      // send_to_protocol), a YEAR expression (MAX(y), YEAR(d)) as a plain
      // integer, "0" (Item::send): make_holder() picks the holder.
      const Item::Type t = item->real_item()->type();
      if (t != Item::FIELD_ITEM && t != Item::FUNC_ITEM &&
          t != Item::SUM_FUNC_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::INT;
      *cast = "BIGINT";
      return false;
    }
    case MYSQL_TYPE_BIT: {
      // A BIT column or MIN/MAX of one: MySQL sends the value's bytes,
      // big-endian, ceil(n/8) of them (max_length is n).
      const Item::Type t = item->real_item()->type();
      if ((t != Item::FIELD_ITEM && t != Item::SUM_FUNC_ITEM) ||
          item->max_length < 1 || item->max_length > 64)
        return refuse("unsupported_result_type");
      *kind = Kind::BIT;
      *cast = "UBIGINT";
      return false;
    }
    case MYSQL_TYPE_TIME: {
      // A TIME column or MIN/MAX of one (signed microseconds in the lake).
      // Other TIME expressions (IF, CASE, functions) are not translated.
      const Item::Type t = item->real_item()->type();
      if (t != Item::FIELD_ITEM && t != Item::SUM_FUNC_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::TIME;
      *cast = "BIGINT";
      return false;
    }
    case MYSQL_TYPE_JSON:
      // A JSON column: the text MySQL prints, as stored (sent with the
      // JSON type of the metadata). JSON expressions are not translated.
      if (item->real_item()->type() != Item::FIELD_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::STRING;
      *cast = "VARCHAR";
      return false;
    case MYSQL_TYPE_GEOMETRY:
      // A GEOMETRY column: SRID + WKB, MySQL's bytes.
      if (item->real_item()->type() != Item::FIELD_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::BLOB;
      *cast = "BLOB";
      return false;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_VECTOR:
      return refuse("unsupported_result_type");
    default:
      break;
  }
  switch (item->result_type()) {
    case INT_RESULT:
      *kind = item->unsigned_flag ? Kind::UINT : Kind::INT;
      *cast = item->unsigned_flag ? "UBIGINT" : "BIGINT";
      return false;
    case REAL_RESULT:
      *kind = Kind::DOUBLE;
      *cast = "DOUBLE";
      return false;
    case DECIMAL_RESULT: {
      // MySQL widens SUM and arithmetic results up to 65 digits; DuckDB
      // computes up to 38 and raises an error on overflow (never a wrong
      // value): the result is read as at most DECIMAL(38,s).
      unsigned scale = item->decimals;
      if (scale > 37) return refuse("decimal_scale_above_37");
      if (item->decimal_precision() > 38) m_decimal_wide = true;
      m_max_scale = std::max(m_max_scale, static_cast<int>(scale));
      if (m_scale_cap >= 0)
        scale = std::min(scale, static_cast<unsigned>(m_scale_cap));
      unsigned precision = std::min(item->decimal_precision(), 38U);
      if (precision < scale) precision = scale;
      if (precision == 0) precision = 1;
      *kind = Kind::DECIMAL;
      // Read back as exact text: CAST(CAST(e AS DECIMAL(p,s)) AS VARCHAR).
      *cast = "DECIMAL(" + std::to_string(precision) + "," +
              std::to_string(scale) + ")) AS VARCHAR";
      return false;
    }
    case STRING_RESULT:
      if (item->collation.collation == &my_charset_bin) {
        *kind = Kind::BLOB;
        *cast = "BLOB";
      } else {
        *kind = Kind::STRING;
        *cast = "VARCHAR";
      }
      return false;
    default:
      return refuse("unsupported_result_type");
  }
}

// A base or materialized derived table of the FROM clause: its alias, and
// its primary key columns (base tables) for the PAD SPACE tie-break.
bool Generator::table_ref(Table_ref *tr, std::string *out) {
  if (tr->is_table_function()) return refuse("table_function");
  if (tr->is_recursive_reference()) return refuse("recursive_cte");
  TABLE *table = tr->table;
  if (table == nullptr) return refuse("table_not_open");
  TableInfo ti;
  ti.alias = "t" + std::to_string(m_next_alias++);
  if (tr->is_view_or_derived()) {
    // Merged views and derived tables are join nests (from_list); this one
    // is materialized: a subquery in FROM, columns named as MySQL names them.
    Query_expression *unit = tr->derived_query_expression();
    if (unit == nullptr) return refuse("derived_without_query");
    if (unit->is_recursive()) return refuse("recursive_cte");
    if (unit->m_lateral_deps != 0) return refuse("lateral_derived_table");
    std::vector<std::string> names;
    for (uint i = 0; i < table->visible_field_count(); i++)
      names.emplace_back(table->visible_field_ptr()[i]->field_name);
    // A grouped derived table joined on its GROUP BY columns (MySQL's
    // rewrite of correlated scalar / ALL / ANY subqueries for secondary
    // engines): LATERAL with the correlation put back (lateral_keys()).
    std::vector<std::pair<Item *, std::string>> keys;
    if (lateral_keys(tr, &keys)) return true;
    Block inner;
    Block *outer = m_block;
    const enum_field_types temporal = m_temporal;
    m_block = &inner;
    m_temporal = MYSQL_TYPE_NULL;
    m_lateral_keys = keys;
    std::string sql;
    // Columns of weight collations carry their weights when they can.
    const bool failed =
        unit->is_simple()
            ? block_sql(unit->first_query_block(), &names, nullptr, &sql,
                        nullptr, nullptr, &ti.weights, false)
            : setop_sql(unit->set_operation(), &names, nullptr, nullptr, &sql,
                        &ti.weights, false);
    m_block = outer;
    m_temporal = temporal;
    if (failed) return true;
    if (unit->m_reject_multiple_rows)
      // A scalar subquery the server made a table: MySQL raises
      // ER_SUBQUERY_NO_1_ROW when it has more than one row (the executor
      // maps DuckDB's error back to it).
      sql = "SELECT * EXCLUDE (\"__duckdb_rows\") FROM (SELECT *, count(*) "
            "OVER () AS \"__duckdb_rows\" FROM (" +
            sql +
            ") AS r) AS r WHERE CASE WHEN \"__duckdb_rows\" > 1 THEN "
            "error('" +
            std::string(kDuckDBMultipleRows) + "') ELSE TRUE END";
    *out = std::string(keys.empty() ? "" : "LATERAL ") + "(" + sql + ") AS " +
           ti.alias;
    ti.derived = true;
    ti.scalar = tr->m_was_scalar_subquery;
  } else {
    const std::string db = table->s->db.str;
    const std::string name = table->s->table_name.str;
    TableState state;
    if (!Registry::instance().snapshot(db, name, &state) || !state.usable())
      return refuse("not_loaded:" + db + "." + name);
    if (table->s->primary_key == MAX_KEY) return refuse("no_primary_key");
    const KEY &pk = table->key_info[table->s->primary_key];
    std::string negated;
    bool integer_key = true;
    for (const std::string &w : split_list(state.weight_columns))
      ti.weights.insert(w);
    for (unsigned i = 0; i < pk.user_defined_key_parts; i++) {
      const Field *f = pk.key_part[i].field;
      ti.pk_fields.push_back(f);
      const std::string c = ti.alias + "." + quote_ident(f->field_name);
      ti.pk_parts += (i ? ", " : "") + c;
      // HUGEINT: negating an unsigned BIGINT stays unsigned in DuckDB.
      negated += (i ? ", " : "") + ("-CAST(" + c + " AS HUGEINT)");
      integer_key = integer_key && f->result_type() == INT_RESULT;
    }
    ti.pk = pk.user_defined_key_parts == 1 ? ti.pk_parts
                                           : "row(" + ti.pk_parts + ")";
    if (integer_key) ti.pk_negated = negated;
    const std::string lake =
        "lake." + quote_ident(db) + "." + quote_ident(name);
    // LIVE scope hides archived rows. Filtering in a subquery keeps the
    // filter on the table itself, wherever it is in the join (in WHERE it
    // would turn a LEFT JOIN into an inner join).
    *out = state.retain_history && !m_full
               ? "(SELECT * FROM " + lake +
                     " WHERE \"__archived_at\" IS NULL) AS " + ti.alias
               : lake + " AS " + ti.alias;
  }
  m_info[table] = ti;
  m_block->tables.insert(table);
  return false;
}

// MySQL plans a correlated scalar subquery (and ALL / ANY) for a secondary
// engine as a derived table grouped on the correlation columns, joined on
// them (Query_block::transform_scalar_subqueries_to_join_with_derived):
//   FROM ot JOIN (SELECT agg(..), it.k FROM it GROUP BY it.k) AS d
//   ON / WHERE d.k = ot.k
// DuckDB then aggregates the whole inner table (TPC-H Q17: 600M rows into
// 20M groups at SF100) before the join can reduce it. Filtering the
// derived block on it.k = ot.k removes whole groups that the join drops
// anyway, so the result is the same; as LATERAL, DuckDB decorrelates it
// and aggregates only the keys it needs (2026-11-25). The equalities come
// from d's ON condition and, for an inner join, the block's WHERE (also
// multiple equalities); the outer column must be of a table d may
// reference (m_lateral_ok), the types such that DuckDB's = is MySQL's.
// keys: (d's select-list item for the key, the outer column's SQL).
bool Generator::lateral_keys(
    Table_ref *e, std::vector<std::pair<Item *, std::string>> *keys) {
  if (m_lateral_ok == 0 || e->table == nullptr) return false;
  Query_expression *unit = e->derived_query_expression();
  if (unit == nullptr || !unit->is_simple() || unit->m_reject_multiple_rows ||
      unit->m_lateral_deps != 0)
    return false;
  // Only what was a correlated subquery in the statement, as AliSQL and
  // MariaDB send it: a scalar one (m_was_scalar_subquery), or ALL / ANY
  // (a derived table MySQL made, "derived_N_M", whose select list has the
  // aggregates, grouped on the correlation columns). A grouped view, or an
  // uncorrelated IN (SELECT k ... GROUP BY k HAVING ...) whose select list
  // is the key (TPC-H Q15, Q18), is better as it is: LATERAL makes DuckDB
  // first collect all outer keys. (LATERAL is exact either way.)
  if (!e->m_was_scalar_subquery) {
    if (e->alias == nullptr || strncmp(e->alias, "derived_", 8) != 0)
      return false;
    bool aggregates = false;
    for (Item *v : unit->first_query_block()->visible_fields())
      aggregates = aggregates || v->has_aggregation();
    if (!aggregates) return false;
  }
  Query_block *dq = unit->first_query_block();
  if (dq->group_list.elements == 0 || dq->olap != UNSPECIFIED_OLAP_TYPE ||
      dq->has_windows() || dq->select_limit != nullptr ||
      dq->offset_limit != nullptr)
    return false;
  // d's column -> its select-list item, if it is a GROUP BY item.
  auto group_item = [&](const Item_field *f) -> Item * {
    if (f->field == nullptr || f->field->table != e->table) return nullptr;
    const uint idx = f->field->field_index();
    uint k = 0;
    Item *src = nullptr;
    for (Item *v : dq->visible_fields())
      if (k++ == idx) {
        src = v;
        break;
      }
    if (src == nullptr) return nullptr;
    for (ORDER *o = dq->group_list.first; o != nullptr; o = o->next)
      if ((*o->item)->real_item() == src->real_item() ||
          (*o->item)->real_item()->eq(src->real_item()))
        return src;
    return nullptr;
  };
  auto usable = [&](Item *x, Item *y) {
    const bool strings = is_string(x) || is_string(y);
    if (strings)
      return is_string(x) && is_string(y) && str_mode(x) == StrMode::RAW &&
             str_mode(y) == StrMode::RAW;
    return x->result_type() == y->result_type() &&
           x->result_type() != REAL_RESULT && !x->is_temporal() &&
           !y->is_temporal();
  };
  std::set<const Item *> done;
  auto consider = [&](Item_field *a, Item_field *b) -> bool {
    Item *src = group_item(a);
    if (src == nullptr || done.count(src->real_item()) != 0) return false;
    if (b->field == nullptr || b->field->table == e->table) return false;
    const table_map bm = b->used_tables() & ~PSEUDO_TABLE_BITS;
    if (bm == 0 || (bm & ~m_lateral_ok) != 0) return false;
    if (!usable(src->real_item(), b)) return false;
    std::string o;
    if (expr(b, &o)) return true;
    done.insert(src->real_item());
    keys->emplace_back(src, o);
    return false;
  };
  std::vector<Item *> parts;
  auto split = [&](Item *c) {
    if (c == nullptr) return;
    c = c->real_item();
    if (c->type() == Item::COND_ITEM &&
        down_cast<Item_func *>(c)->functype() == Item_func::COND_AND_FUNC) {
      for (Item &a : *down_cast<Item_cond *>(c)->argument_list())
        parts.push_back(a.real_item());
    } else {
      parts.push_back(c);
    }
  };
  split(e->join_cond());
  if (!e->outer_join && m_block->qb != nullptr)
    split(m_block->qb->where_cond());
  for (Item *c : parts) {
    if (c->type() != Item::FUNC_ITEM) continue;
    auto *fn = down_cast<Item_func *>(c);
    if (fn->functype() == Item_func::EQ_FUNC) {
      Item *l = fn->arguments()[0]->real_item();
      Item *r = fn->arguments()[1]->real_item();
      if (l->type() != Item::FIELD_ITEM || r->type() != Item::FIELD_ITEM)
        continue;
      auto *lf = down_cast<Item_field *>(l), *rf = down_cast<Item_field *>(r);
      if (consider(lf, rf) || consider(rf, lf)) return true;
    } else if (fn->functype() == Item_func::MULTI_EQ_FUNC) {
      std::vector<Item_field *> fields;
      for (Item_field &f : down_cast<Item_multi_eq *>(fn)->get_fields())
        fields.push_back(&f);
      for (Item_field *a : fields)
        for (Item_field *b : fields)
          if (a != b && consider(a, b)) return true;
    }
  }
  return false;
}

// MySQL plans NOT EXISTS / NOT IN for a secondary engine as a derived
// table (Item_exists_subselect::is_derived_candidate, cannot_do_antijoin):
//   FROM ot LEFT JOIN (SELECT DISTINCT 1 AS c, ie ...) AS d ON <cond>
//   WHERE d.c IS NULL
// which is exactly NOT EXISTS (SELECT 1 FROM (SELECT ...) AS d WHERE
// <cond>) when d.c is never NULL in d and d is used nowhere else. DuckDB
// plans that as an anti join; the LEFT JOIN made it compute the DISTINCT
// derived table first (TPC-H Q21 at SF100: hundreds of millions of rows,
// spilled to disk). Returns the IS NULL part of WHERE, or nullptr when e
// is not such a table.
Item *Generator::derived_marker(Table_ref *e, bool *anti, bool *top) {
  if (!e->outer_join || !e->is_view_or_derived() || e->table == nullptr ||
      e->m_was_scalar_subquery)
    return nullptr;
  Query_expression *unit = e->derived_query_expression();
  Query_block *qb = m_block->qb;
  if (unit == nullptr || !unit->is_simple() || unit->is_recursive() ||
      unit->m_reject_multiple_rows || unit->m_lateral_deps != 0 ||
      qb == nullptr || qb->where_cond() == nullptr)
    return nullptr;
  Query_block *dq = unit->first_query_block();
  if (dq->select_limit != nullptr || dq->offset_limit != nullptr)
    return nullptr;
  auto uses_d = [&](Item *item) {
    return item != nullptr &&
           WalkItem(item, enum_walk::PREFIX, [&](Item *i) {
             const Item *r = i->real_item();
             return r->type() == Item::FIELD_ITEM &&
                    down_cast<const Item_field *>(r)->field != nullptr &&
                    down_cast<const Item_field *>(r)->field->table == e->table;
           });
  };
  // d's column idx is in an equality (EQ or multiple equality) of ON.
  auto on_equated = [&](uint idx) {
    if (e->join_cond() == nullptr) return false;
    bool found = false;
    WalkItem(e->join_cond(), enum_walk::PREFIX, [&](Item *i) {
      if (i->type() != Item::FUNC_ITEM) return false;
      auto *fn = down_cast<Item_func *>(i);
      auto is_d = [&](Item *x) {
        x = x->real_item();
        return x->type() == Item::FIELD_ITEM &&
               down_cast<Item_field *>(x)->field != nullptr &&
               down_cast<Item_field *>(x)->field->table == e->table &&
               down_cast<Item_field *>(x)->field->field_index() == idx;
      };
      if (fn->functype() == Item_func::EQ_FUNC &&
          (is_d(fn->arguments()[0]) || is_d(fn->arguments()[1])))
        found = true;
      if (fn->functype() == Item_func::MULTI_EQ_FUNC)
        for (Item_field &f : down_cast<Item_multi_eq *>(fn)->get_fields())
          if (is_d(&f)) found = true;
      return found;
    });
    return found;
  };
  // IS [NOT] NULL on a column of d that is never NULL in d, or that ON
  // equates.
  auto is_marker = [&](Item *p, bool *is_anti) {
    if (p->type() != Item::FUNC_ITEM) return false;
    const auto ft = down_cast<Item_func *>(p)->functype();
    if (ft != Item_func::ISNULL_FUNC && ft != Item_func::ISNOTNULL_FUNC)
      return false;
    Item *a = down_cast<Item_func *>(p)->arguments()[0]->real_item();
    if (a->type() != Item::FIELD_ITEM ||
        down_cast<Item_field *>(a)->field == nullptr ||
        down_cast<Item_field *>(a)->field->table != e->table)
      return false;
    const uint idx = down_cast<Item_field *>(a)->field->field_index();
    uint k = 0;
    Item *src = nullptr;
    for (Item *f : dq->visible_fields())
      if (k++ == idx) {
        src = f;
        break;
      }
    if (src == nullptr) return false;
    // A nullable column (the key of IN) is a marker too when ON equates
    // it: a matched row then never has NULL there.
    if (src->is_nullable() && !on_equated(idx)) return false;
    *is_anti = ft == Item_func::ISNULL_FUNC;
    return true;
  };
  // Walk AND / OR: d may appear only in one marker.
  Item *marker = nullptr;
  bool bad = false;
  Item *where = qb->where_cond()->real_item();
  std::function<void(Item *, bool)> walk = [&](Item *c, bool at_top) {
    c = c->real_item();
    if (c->type() == Item::COND_ITEM) {
      const bool is_and = down_cast<Item_func *>(c)->functype() ==
                          Item_func::COND_AND_FUNC;
      for (Item &a : *down_cast<Item_cond *>(c)->argument_list())
        walk(&a, at_top && is_and);
      return;
    }
    if (!uses_d(c)) return;
    bool a = false;
    if (marker == nullptr && is_marker(c, &a)) {
      marker = c;
      *anti = a;
      *top = at_top;
      return;
    }
    bad = true;
  };
  walk(where, true);
  if (bad || marker == nullptr) return nullptr;
  // d nowhere else: select list, GROUP BY, HAVING, ORDER BY, other joins.
  // (used_tables() is not enough: COUNT(*) reports every table.)
  for (Item *f : qb->fields)
    if (uses_d(f)) return nullptr;
  if (uses_d(qb->having_cond())) return nullptr;
  for (ORDER *o = qb->group_list.first; o != nullptr; o = o->next)
    if (uses_d(*o->item)) return nullptr;
  for (ORDER *o = qb->order_list.first; o != nullptr; o = o->next)
    if (uses_d(*o->item)) return nullptr;
  std::function<bool(mem_root_deque<Table_ref *> &)> other_use =
      [&](mem_root_deque<Table_ref *> &list) {
        for (Table_ref *t : list) {
          if (t != e && uses_d(t->join_cond())) return true;
          if (t->nested_join != nullptr &&
              other_use(t->nested_join->m_tables))
            return true;
        }
        return false;
      };
  if (other_use(qb->m_table_nest)) return nullptr;
  return marker;
}

// d (a DISTINCT derived table) matches each outer row at most once: its ON
// condition is only equalities, together fixing every column of d that is
// not a constant (the EXISTS marker). Then LEFT JOIN d ... d.c IS [NOT] NULL
// never duplicated a row and [NOT] EXISTS gives the same rows anywhere in
// WHERE.
bool Generator::unique_match(Table_ref *e) {
  Query_expression *unit = e->derived_query_expression();
  if (unit == nullptr || e->join_cond() == nullptr) return false;
  Query_block *dq = unit->first_query_block();
  if (!dq->is_distinct()) return false;
  std::set<uint> fixed;
  auto d_field = [&](Item *i) -> const Item_field * {
    i = i->real_item();
    if (i->type() != Item::FIELD_ITEM) return nullptr;
    const auto *f = down_cast<const Item_field *>(i);
    return f->field != nullptr && f->field->table == e->table ? f : nullptr;
  };
  auto uses_d = [&](Item *item) {
    return WalkItem(item, enum_walk::PREFIX,
                    [&](Item *i) { return d_field(i) != nullptr; });
  };
  std::vector<Item *> parts;
  Item *c = e->join_cond()->real_item();
  if (c->type() == Item::COND_ITEM &&
      down_cast<Item_func *>(c)->functype() == Item_func::COND_AND_FUNC) {
    for (Item &a : *down_cast<Item_cond *>(c)->argument_list())
      parts.push_back(a.real_item());
  } else {
    parts.push_back(c);
  }
  for (Item *p : parts) {
    if (p->type() != Item::FUNC_ITEM) return false;
    auto *fn = down_cast<Item_func *>(p);
    if (fn->functype() == Item_func::EQ_FUNC) {
      Item *l = fn->arguments()[0], *r = fn->arguments()[1];
      const Item_field *dl = d_field(l), *dr = d_field(r);
      if (dl != nullptr && !uses_d(r))
        fixed.insert(dl->field->field_index());
      else if (dr != nullptr && !uses_d(l))
        fixed.insert(dr->field->field_index());
      else
        return false;
    } else if (fn->functype() == Item_func::MULTI_EQ_FUNC) {
      bool outer = false;
      std::vector<uint> mine;
      for (Item_field &f : down_cast<Item_multi_eq *>(fn)->get_fields()) {
        if (d_field(&f) != nullptr)
          mine.push_back(f.field->field_index());
        else
          outer = true;
      }
      if (!outer) return false;
      fixed.insert(mine.begin(), mine.end());
    } else {
      return false;
    }
  }
  uint k = 0;
  for (Item *f : dq->visible_fields()) {
    if (fixed.count(k) == 0 && !f->const_item()) return false;
    k++;
  }
  return true;
}

// A join list (stored in reverse order by the parser). Inner joins, CROSS
// JOIN and LEFT JOIN (RIGHT JOIN is already a LEFT JOIN) are printed as
// joins; semi-join nests (IN / EXISTS subqueries the optimizer flattened)
// become EXISTS and anti-join nests (NOT EXISTS, NOT IN) NOT EXISTS in the
// block's WHERE, so only at the top of the block.
bool Generator::from_list(mem_root_deque<Table_ref *> &list, bool under_outer,
                          bool in_semi, std::string *out) {
  std::string from;
  table_map seen = 0;  // tables of this list printed so far
  // The tables of this list's semi- and anti-join nests (printed as EXISTS
  // / NOT EXISTS in WHERE, not visible in FROM).
  table_map semi_all = 0;
  if (!in_semi)
    for (Table_ref *e : list)
      if (e->is_sj_or_aj_nest()) semi_all |= e->nested_join->used_tables;
  // The tables an EXISTS of this block can see: the block's tables outside
  // its semi- and anti-join nests.
  if (!in_semi && !under_outer && m_block->qb != nullptr &&
      &list == &m_block->qb->m_table_nest)
    m_main_visible = m_block->qb->all_tables_map() & ~semi_all;
  for (auto it = list.rbegin(); it != list.rend(); ++it) {
    Table_ref *e = *it;
    if (e->is_sj_or_aj_nest()) {
      if (under_outer || in_semi) return refuse("semijoin_not_at_top_level");
      Block::Semi s;
      s.nest = e;
      s.anti = e->is_aj_nest();
      s.tables = e->nested_join->used_tables;
      s.cond = e->join_cond();
      if (from_list(e->nested_join->m_tables, false, true, &s.from))
        return true;
      if (s.from.empty()) return refuse("empty_semijoin_nest");
      m_block->semi.push_back(s);
      continue;
    }
    // NOT EXISTS / NOT IN made a derived table by MySQL (2026-11-24):
    // printed back as NOT EXISTS (derived_marker()).
    bool anti = false, top = false;
    Item *marker = !in_semi && !under_outer && !from.empty() &&
                           e->nested_join == nullptr
                       ? derived_marker(e, &anti, &top)
                       : nullptr;
    if (marker != nullptr && !(anti && top) && unique_match(e)) {
      // Inside OR (or IS NOT NULL): [NOT] EXISTS in place of the marker;
      // each outer row matches at most one row of d (unique_match()), so
      // the LEFT JOIN never duplicated rows (2026-11-26).
      std::string sub;
      m_drop_distinct = true;
      bool failed = table_ref(e, &sub);
      m_drop_distinct = false;
      if (failed) return true;
      std::string on = "TRUE";
      if (e->join_cond() != nullptr) {
        const table_map saved_others = m_anti_others;
        const table_map saved_mine = m_anti_mine;
        m_anti_others = semi_all;
        m_anti_mine = e->map();
        failed = expr(e->join_cond(), &on);
        m_anti_others = saved_others;
        m_anti_mine = saved_mine;
        if (failed) return true;
      }
      m_block->marker_sql[marker] = std::string(anti ? "(NOT EXISTS (" : "(EXISTS (") +
                                    "SELECT 1 FROM " + sub + " WHERE " + on +
                                    "))";
      continue;
    }
    if (marker != nullptr && anti && top) {
      {
        Block::Semi s;
        s.nest = e;
        s.anti = true;
        s.tables = e->map();
        s.cond = e->join_cond();
        m_drop_distinct = true;
        const bool failed = table_ref(e, &s.from);
        m_drop_distinct = false;
        if (failed) return true;
        m_block->skip_where.insert(marker);
        m_block->semi.push_back(s);
        continue;
      }
    }
    std::string item;
    if (e->nested_join != nullptr) {
      std::string nest;
      if (from_list(e->nested_join->m_tables, under_outer || e->outer_join,
                    in_semi, &nest))
        return true;
      if (nest.empty()) return refuse("empty_join_nest");
      item = "(" + nest + ")";
    } else {
      // LATERAL only at the top of a block or of an EXISTS (not inside a
      // parenthesized join).
      m_lateral_ok =
          under_outer ? 0 : (seen | (in_semi ? m_main_visible : 0));
      const bool failed = table_ref(e, &item);
      m_lateral_ok = 0;
      if (failed) return true;
    }
    seen |= e->nested_join != nullptr ? e->nested_join->used_tables
                                      : (e->table != nullptr ? e->map() : 0);
    std::string on;
    if (e->join_cond() != nullptr) {
      // A join condition may name a column of a semi-join nest of this
      // block (TPC-H Q21: MySQL turns NOT EXISTS into LEFT JOIN derived ON
      // derived.k = l2.k, l2 in the EXISTS we print in WHERE, not visible
      // here): an equal outer column stands for it (outer_equal()).
      const table_map saved_others = m_anti_others;
      const table_map saved_mine = m_anti_mine;
      if (semi_all != 0) {
        m_anti_others = semi_all;
        m_anti_mine = e->nested_join != nullptr
                          ? e->nested_join->used_tables
                          : (e->table != nullptr ? e->map() : 0);
      }
      const bool failed = expr(e->join_cond(), &on);
      m_anti_others = saved_others;
      m_anti_mine = saved_mine;
      if (failed) return true;
    }
    if (from.empty()) {
      if (e->outer_join || !on.empty())
        return refuse("join_condition_on_first_table");
      from = item;
    } else if (e->outer_join) {
      from += " LEFT JOIN " + item + " ON " + (on.empty() ? "TRUE" : on);
    } else if (!on.empty()) {
      from += " JOIN " + item + " ON " + on;
    } else {
      from += " CROSS JOIN " + item;
    }
  }
  *out = from;
  return false;
}

// The top-level AND parts of a condition, each with the tables it uses;
// multiple equalities are split into pairs anchored on a column outside
// `avoid` (the semi-join tables) when there is one.
bool Generator::conjuncts(Item *cond, table_map avoid,
                          std::vector<Conj> *out) {
  // Conditions MySQL added to an IN subquery for its in-to-exists strategy:
  // we print the IN predicate itself (MySQL removes them the same way when
  // it changes strategy).
  if (cond->created_by_in2exists()) return false;
  cond = cond->real_item();
  if (cond->created_by_in2exists()) return false;
  if (m_block->skip_where.count(cond) != 0) return false;
  if (cond->type() == Item::COND_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::COND_AND_FUNC) {
    for (Item &arg : *down_cast<Item_cond *>(cond)->argument_list())
      if (conjuncts(&arg, avoid, out)) return true;
    return false;
  }
  if (cond->type() == Item::FUNC_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::MULTI_EQ_FUNC)
    return multi_eq(down_cast<Item_multi_eq *>(cond), avoid, out);
  std::string s;
  if (expr(cond, &s)) return true;
  Conj c{s, cond->used_tables() & ~PSEUDO_TABLE_BITS};
  if (cond->type() == Item::FUNC_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::EQ_FUNC) {
    Item *l = down_cast<Item_func *>(cond)->arguments()[0]->real_item();
    Item *r = down_cast<Item_func *>(cond)->arguments()[1]->real_item();
    if (l->type() == Item::FIELD_ITEM && r->type() == Item::FIELD_ITEM) {
      c.eq_a = down_cast<Item_field *>(l);
      c.eq_b = down_cast<Item_field *>(r);
    }
  }
  out->push_back(c);
  return false;
}

// Two columns MySQL compares the same way DuckDB does.
bool Generator::comparable(Item *a, Item *b) {
  if (is_string(a) || is_string(b))
    return is_string(a) && is_string(b) && str_mode(a) == str_mode(b) &&
           str_mode(a) != StrMode::REFUSE &&
           (str_mode(a) != StrMode::WEIGHT ||
            a->collation.collation == b->collation.collation);
  if (a->is_temporal() || b->is_temporal()) {
    const bool da = is_date_or_datetime(a->data_type());
    const bool db = is_date_or_datetime(b->data_type());
    return (da && db) || a->data_type() == b->data_type();
  }
  return true;
}

// The optimizer's equality propagation: f1 = f2 = ... [= const], as pairs.
bool Generator::multi_eq(Item_multi_eq *eq, table_map avoid,
                         std::vector<Conj> *out) {
  std::vector<Item *> fields;
  for (Item_field &f : eq->get_fields()) fields.push_back(&f);
  if (fields.empty()) return refuse("empty_multiple_equality");
  size_t anchor = 0;
  for (size_t i = 0; i < fields.size(); i++)
    if ((fields[i]->used_tables() & avoid) == 0) {
      anchor = i;
      break;
    }
  Item *first = fields[anchor];
  for (Item *f : fields)
    if (special_column(f) == SpecialColumn::OPAQUE)
      return refuse("json_or_geometry_in_function:=");
  const bool str = is_string(first);
  if (str && str_mode(first) == StrMode::REFUSE)
    return refuse_collation(first);
  const enum_field_types t = first->data_type();
  const CHARSET_INFO *fcs = first->collation.collation;
  auto print = [&](Item *it, std::string *s) {
    return str ? string_key(it, fcs, false, it->max_length, s) : expr(it, s);
  };
  std::string f0;
  if (print(first, &f0)) return true;
  const table_map t0 = first->used_tables() & ~PSEUDO_TABLE_BITS;
  for (size_t i = 0; i < fields.size(); i++) {
    if (i == anchor) continue;
    if (!comparable(first, fields[i]))
      return refuse("multiple_equality_types");
    std::string a;
    if (print(fields[i], &a)) return true;
    out->push_back({"(" + f0 + " = " + a + ")",
                    t0 | (fields[i]->used_tables() & ~PSEUDO_TABLE_BITS),
                    true, eq});
  }
  if (Item *c = eq->const_arg(); c != nullptr) {
    std::string cs;
    if (t == MYSQL_TYPE_TIME) {
      if (c->is_null() ? literal(c, &cs) : time_literal(c, &cs)) return true;
    } else if (t == MYSQL_TYPE_TIMESTAMP || is_date_or_datetime(t)) {
      if (typed_literal(c, t, &cs)) return true;
    } else if (str != is_string(c)) {
      return refuse("multiple_equality_types");
    } else if (print(c, &cs)) {
      return true;
    }
    out->push_back({"(" + f0 + " = " + cs + ")", t0});
  }
  return false;
}

// One query block: FROM (joins), WHERE (with EXISTS / NOT EXISTS for the
// semi- and anti-join nests), GROUP BY, select list, HAVING, ORDER BY,
// LIMIT. `names`: the columns of a derived table (select values named, kept
// in their DuckDB type); otherwise the top block (values cast for reading
// back, kinds filled).
bool Generator::block_sql(Query_block *qb, const std::vector<std::string> *names,
                          std::vector<Kind> *kinds, std::string *sql,
                          const std::vector<Item *> *targets,
                          const std::string *from_sql,
                          std::set<std::string> *weights_out,
                          bool weights_required) {
  if (qb->active_options() & OPTION_FOUND_ROWS)
    return refuse("sql_calc_found_rows");
  if (qb->olap != UNSPECIFIED_OLAP_TYPE) return refuse("rollup");
  if (qb->has_windows()) return refuse("window");
  m_block->qb = qb;
  const bool drop_distinct = m_drop_distinct;  // this block only
  m_drop_distinct = false;
  const std::vector<std::pair<Item *, std::string>> lateral =
      std::move(m_lateral_keys);
  m_lateral_keys.clear();
  const table_map saved_main_visible = m_main_visible;

  std::string from;
  if (from_sql != nullptr)
    from = *from_sql;
  else if (from_list(qb->m_table_nest, false, false, &from))
    return true;
  // A block without tables (SELECT 1, as a set operation operand) has no
  // FROM.
  if (from.empty() && qb->leaf_table_count != 0) return refuse("no_table");
  m_main_visible = saved_main_visible;

  // WHERE, on rows before grouping. A semi-join's condition was merged into
  // WHERE: each AND part that uses a semi-join's tables goes into its EXISTS
  // (for a semi-join, a part on the outer tables alone is the same there or
  // in WHERE).
  table_map semi_tables = 0;
  for (const Block::Semi &s : m_block->semi)
    if (!s.anti) semi_tables |= s.tables;
  std::vector<std::string> where;
  std::vector<std::vector<std::string>> semi_conds(m_block->semi.size());
  if (qb->where_cond() != nullptr) {
    // Split in AND parts (in-to-exists conditions are left out).
    std::vector<Conj> parts;
    if (conjuncts(qb->where_cond(), semi_tables, &parts)) return true;
    // For each semi-join nest, the outer columns equal to each of its
    // columns (plain equalities: MySQL's best-equal-field substitution
    // anchors o.k = l2.k and l1.k = l2.k on l2, a table of the EXISTS).
    std::vector<std::map<const Field *, std::vector<Item_field *>>> links(
        m_block->semi.size());
    for (const Conj &c : parts) {
      int hit = -1;
      for (size_t i = 0; i < m_block->semi.size(); i++) {
        const Block::Semi &s = m_block->semi[i];
        if (s.anti || (c.tables & s.tables) == 0)
          continue;
        if (hit >= 0) return refuse("condition_across_semijoins");
        hit = static_cast<int>(i);
      }
      if (hit < 0) {
        where.push_back(c.sql);
        continue;
      }
      semi_conds[hit].push_back(c.sql);
      if (c.eq_a != nullptr && c.eq_a->field != nullptr &&
          c.eq_b->field != nullptr) {
        const table_map nest = m_block->semi[hit].tables;
        const table_map a = c.eq_a->used_tables(), b = c.eq_b->used_tables();
        if ((a & nest) != 0 && (b & semi_tables) == 0)
          links[hit][c.eq_a->field].push_back(c.eq_b);
        else if ((b & nest) != 0 && (a & semi_tables) == 0)
          links[hit][c.eq_b->field].push_back(c.eq_a);
      }
    }
    // Outer columns equal to the same column of a nest are equal to each
    // other whenever the EXISTS holds: we also say so in the outer WHERE,
    // so DuckDB joins them (without it, TPC-H Q21 at SF100 made DuckDB
    // build ORDERS x LINEITEM and fill memory). The result cannot change.
    for (const auto &nest_links : links)
      for (const auto &link : nest_links) {
        const std::vector<Item_field *> &outs = link.second;
        for (size_t k = 1; k < outs.size(); k++) {
          // Only where DuckDB's = is MySQL's: binary strings, or the same
          // exact type (no DOUBLE, no collation keys).
          Item *x = outs[0], *y = outs[k];
          const bool strings = is_string(x) || is_string(y);
          if (strings ? !(is_string(x) && is_string(y) &&
                          str_mode(x) == StrMode::RAW &&
                          str_mode(y) == StrMode::RAW)
                      : (x->result_type() != y->result_type() ||
                         x->result_type() == REAL_RESULT ||
                         !comparable(x, y)))
            continue;
          std::string a, b;
          if (expr(outs[0], &a) || expr(outs[k], &b)) return true;
          where.push_back("(" + a + " = " + b + ")");
        }
      }
  }
  for (size_t i = 0; i < m_block->semi.size(); i++) {
    const Block::Semi &s = m_block->semi[i];
    std::vector<std::string> conds = semi_conds[i];
    if (s.anti && s.cond != nullptr) {
      // An anti-join keeps its condition, as a LEFT JOIN's ON, split in AND
      // parts. MySQL's equality propagation may put a column of another
      // semi-join nest into it (TPC-H Q21: l1.k = l2.k = l3.k, l2 in the
      // EXISTS, l3 in this NOT EXISTS), and that table is not visible in
      // this subquery. We anchor each multiple equality on a visible column
      // (outer or ours); a pair that still ties in another nest's column
      // only repeats an equality that nest's own EXISTS enforces, and is
      // left out. Any other condition on another nest's table: refused.
      table_map others = 0;
      for (size_t j = 0; j < m_block->semi.size(); j++)
        if (j != i) others |= m_block->semi[j].tables;
      const table_map mine = s.tables;
      if (s.nest->nested_join == nullptr) {
        // A derived anti-join (derived_marker()): its ON condition,
        // where a column of another nest is printed as an equal outer
        // column (outer_equal()), as when it was a LEFT JOIN.
        std::string c;
        m_anti_others = others;
        m_anti_mine = mine;
        const bool failed = expr(s.cond, &c);
        m_anti_others = 0;
        m_anti_mine = 0;
        if (failed) return true;
        conds.push_back(c);
      }
      std::vector<Conj> parts;
      m_anti_others = others;
      m_anti_mine = mine;
      const bool failed = s.nest->nested_join != nullptr &&
                          conjuncts(s.cond, others, &parts);
      m_anti_others = 0;
      m_anti_mine = 0;
      if (failed) return true;
      for (const Conj &c : parts) {
        if ((c.tables & others) != 0) {
          // Left out only when the equality also holds an outer column:
          // then this nest's pair with it is printed here, and the other
          // nest's pair with it in that nest's EXISTS.
          bool outer_member = false;
          if (c.implied && c.eq != nullptr)
            for (Item_field &f : c.eq->get_fields())
              outer_member = outer_member ||
                             (f.used_tables() & (others | mine)) == 0;
          if (outer_member) continue;
          return refuse("condition_across_semijoins");
        }
        conds.push_back(c.sql);
      }
    }
    std::string body = "SELECT 1 FROM " + s.from;
    for (size_t j = 0; j < conds.size(); j++)
      body += (j == 0 ? " WHERE " : " AND ") + conds[j];
    where.push_back(std::string(s.anti ? "NOT EXISTS (" : "EXISTS (") + body +
                    ")");
  }
  // A LATERAL derived block: its GROUP BY columns equal to the outer
  // columns the join compares them with (lateral_keys()).
  for (const auto &k : lateral) {
    std::string inner;
    if (expr(k.first, &inner)) return true;
    where.push_back("(" + inner + " = " + k.second + ")");
  }

  // GROUP BY. PAD SPACE strings group on rtrim(); a plain column of the
  // select list must be grouped (DuckDB has no ONLY_FULL_GROUP_BY off mode).
  std::string group;
  for (ORDER *o = qb->group_list.first; o != nullptr; o = o->next) {
    Item *g = (*o->item)->real_item();
    if (special_column(g) == SpecialColumn::OPAQUE)
      return refuse("json_or_geometry_in_group_by");
    if (g->type() == Item::FIELD_ITEM)
      m_block->grouped.insert(down_cast<Item_field *>(g)->field);
    std::string e;
    if (is_string(g) && (str_mode(g) == StrMode::PAD ||
                         str_mode(g) == StrMode::WEIGHT)) {
      // Grouped on the equality key (rtrim / weight); the value shown is
      // decided by grouped_value().
      if (g->type() != Item::FIELD_ITEM && exact_text(g, false)) {
        if (expr(g, &e)) return true;
        group += (group.empty() ? "" : ", ") + e;
        continue;
      }
      if (g->type() != Item::FIELD_ITEM) {
        // An expression (Q22: SUBSTR(c_phone, 1, 2)) has no weight column:
        // only PAD SPACE, grouped on rtrim() (2026-11-22).
        // Also a binary PAD SPACE collation of latin1 / ascii (weights for
        // columns; string_key() keys an expression on rtrim()).
        const CHARSET_INFO *gcs = g->collation.collation;
        const bool binary_pad = (gcs->state & MY_CS_BINSORT) != 0 &&
                                gcs->pad_attribute != NO_PAD &&
                                (strcmp(gcs->csname, "latin1") == 0 ||
                                 strcmp(gcs->csname, "ascii") == 0);
        if (str_mode(g) != StrMode::PAD && !binary_pad)
          return refuse(std::string("group_by_expression (") +
                        gcs->m_coll_name + ")");
        if (string_key(g, gcs, false, g->max_length, &e)) return true;
        m_block->group_pad_exprs.push_back(g);
        group += (group.empty() ? "" : ", ") + e;
        continue;
      }
      m_block->group_pad.push_back(down_cast<Item_field *>(g)->field);
      if (string_key(g, g->collation.collation, false, g->max_length, &e))
        return true;
    } else {
      if (is_string(g) && str_mode(g) == StrMode::REFUSE)
        return refuse_collation(g);
      if (expr(g, &e)) return true;
    }
    group += (group.empty() ? "" : ", ") + e;
  }
  m_block->post_group = !group.empty();

  // Select list: the top block casts each value to the type we read back; a
  // derived table keeps DuckDB types and names its columns.
  const bool distinct = qb->is_distinct() && !drop_distinct;
  std::string select;
  size_t col = 0;
  for (Item *item : qb->visible_fields()) {
    if (special_column(item->real_item()) == SpecialColumn::OPAQUE &&
        (distinct || targets != nullptr))
      return refuse(distinct ? "json_or_geometry_in_distinct"
                             : "json_or_geometry_in_set_operation");
    if (distinct && is_string(item) && str_mode(item) != StrMode::RAW &&
        !exact_text(item, false))
      return str_mode(item) == StrMode::PAD
                 ? refuse("distinct_padded_string")
                 : refuse_collation(item);
    std::string e, cast;
    Kind kind;
    if (output_kind(item, &kind, &cast) || expr(item, &e)) return true;
    if (targets != nullptr) {
      // An operand of a set operation: cast to the column type MySQL
      // computed for the result. Mixing kinds (number and string, temporal
      // and other) is refused: MySQL's conversions differ (decided
      // 2026-11-13).
      if (col >= targets->size()) return refuse("set_operation_columns");
      Kind target;
      std::string target_cast;
      if (output_kind((*targets)[col], &target, &target_cast)) return true;
      // A NULL literal takes any type (is_null() of a column would read
      // the current row: only the item type is checked).
      if (item->real_item()->type() != Item::NULL_ITEM &&
          !same_class(kind, target))
        return refuse("set_operation_mixed_types");
      kind = target;
      cast = target_cast;
    }
    std::string value;
    if (names == nullptr) {
      value = kind == Kind::DECIMAL ? "CAST(CAST(" + e + " AS " + cast + ")"
                                    : "CAST(" + e + " AS " + cast + ")";
      kinds->push_back(kind);
    } else {
      if (col >= names->size()) return refuse("derived_column_count");
      if (kind == Kind::DECIMAL) cast = cast.substr(0, cast.find(')') + 1);
      value = "CAST(" + e + " AS " + cast + ") AS " + quote_ident((*names)[col]);
      // A string of a weight collation carries its weight, for grouping and
      // ordering on this column outside (derived table, set operation).
      Item *type_item = targets != nullptr ? (*targets)[col] : item;
      if (weights_out != nullptr && is_string(type_item) &&
          str_mode(type_item) == StrMode::WEIGHT) {
        std::string w;
        const std::string saved_reason = *m_reason;
        if (!weight_expr(item, type_item->collation.collation, &w)) {
          value += ", " + w + " AS " +
                   quote_ident(std::string(kWeightPrefix) + (*names)[col]);
          weights_out->insert((*names)[col]);
        } else if (weights_required) {
          return true;
        } else {
          *m_reason = saved_reason;  // best effort: no weight for it
        }
      }
    }
    col++;
    select += (select.empty() ? "" : ", ") + value;
  }

  std::string having;
  if (qb->having_cond() != nullptr) {
    std::vector<Conj> parts;
    if (conjuncts(qb->having_cond(), 0, &parts)) return true;
    for (const Conj &c : parts)
      having += (having.empty() ? "" : " AND ") + c.sql;
  }

  // MySQL sorts NULL first ascending and last descending; DuckDB sorts NULL
  // last by default.
  std::string order;
  for (ORDER *o = qb->order_list.first; o != nullptr; o = o->next) {
    std::string s;
    if (special_column((*o->item)->real_item()) == SpecialColumn::OPAQUE)
      return refuse("json_or_geometry_in_order_by");
    // MySQL sorts ENUM / SET on the index, not the string.
    const bool by_index =
        special_column((*o->item)->real_item()) == SpecialColumn::OTHER &&
        down_cast<Item_field *>((*o->item)->real_item())->field->real_type() !=
            MYSQL_TYPE_TIME2;
    if (by_index ? enum_index(*o->item, &s) : order_key(*o->item, &s))
      return true;
    const bool desc = o->direction == ORDER_DESC;
    order += (order.empty() ? "" : ", ") + s +
             (desc ? " DESC NULLS LAST" : " ASC NULLS FIRST");
  }

  *sql = std::string("SELECT ") + (distinct ? "DISTINCT " : "") + select +
         (from.empty() ? "" : " FROM " + from);
  for (size_t i = 0; i < where.size(); i++)
    *sql += (i == 0 ? " WHERE " : " AND ") + where[i];
  if (!group.empty()) *sql += " GROUP BY " + group;
  if (!having.empty()) *sql += " HAVING " + having;
  if (!order.empty()) *sql += " ORDER BY " + order;
  // A LIMIT 1 MySQL added to a subquery predicate (m_internal_limit, for its
  // EXISTS strategy) is not the query's: it would cut an IN list to one
  // value.
  if (qb->select_limit != nullptr && !qb->m_internal_limit) {
    if (!is_constant(qb->select_limit)) return refuse("limit_not_constant");
    *sql += " LIMIT " + std::to_string(qb->select_limit->val_uint());
  }
  if (qb->offset_limit != nullptr) {
    if (!is_constant(qb->offset_limit)) return refuse("offset_not_constant");
    *sql += " OFFSET " + std::to_string(qb->offset_limit->val_uint());
  }
  return false;
}

// IS [NOT] TRUE / FALSE and NOT, as MySQL's Bool_test (IS TRUE: NULL is
// false).
std::string Generator::truth(const std::string &e, Item::Bool_test test,
                             bool *ok) {
  *ok = true;
  switch (test) {
    case Item::BOOL_IDENTITY:
      return e;
    case Item::BOOL_NEGATED:
      return "(NOT " + e + ")";
    case Item::BOOL_IS_TRUE:
      return "coalesce(" + e + ", FALSE)";
    case Item::BOOL_IS_FALSE:
      return "coalesce(NOT " + e + ", FALSE)";
    case Item::BOOL_NOT_TRUE:
      return "(NOT coalesce(" + e + ", FALSE))";
    case Item::BOOL_NOT_FALSE:
      return "coalesce(" + e + ", TRUE)";
    default:
      *ok = false;
      return e;
  }
}

// The query of a subquery predicate or scalar subquery, inside the current
// block: it may refer to the enclosing blocks' tables (correlated).
bool Generator::subquery_sql(Query_expression *unit, std::string *sql) {
  if (unit == nullptr) return refuse("subquery_without_query");
  if (unit->is_recursive()) return refuse("recursive_cte");
  size_t columns = 0;
  if (unit->is_simple()) {
    for (Item *item : unit->first_query_block()->visible_fields()) {
      (void)item;
      columns++;
    }
  } else {
    for (Item *type : *unit->set_operation()->types_array())
      if (!type->hidden) columns++;
  }
  std::vector<std::string> names;
  for (size_t i = 0; i < columns; i++)
    names.push_back("c" + std::to_string(i + 1));
  Block inner;
  inner.parent = m_block;
  Block *outer = m_block;
  const enum_field_types temporal = m_temporal;
  m_block = &inner;
  m_temporal = MYSQL_TYPE_NULL;
  const bool failed =
      unit->is_simple()
          ? block_sql(unit->first_query_block(), &names, nullptr, sql)
          : setop_sql(unit->set_operation(), &names, nullptr, nullptr, sql);
  m_block = outer;
  m_temporal = temporal;
  return failed;
}

// Subquery items the server left as expressions: scalar subqueries,
// [NOT] EXISTS, [NOT] IN, with MySQL's truth test. DuckDB evaluates IN and
// NOT IN with the same three-valued logic (NULL when there is no match and
// the subquery or the operand has a NULL; FALSE for an empty subquery).
bool Generator::subquery(Item_subselect *sub, std::string *out) {
  std::string q;
  switch (sub->subquery_type()) {
    case Item_subselect::SCALAR_SUBQUERY: {
      auto *s = down_cast<Item_singlerow_subselect *>(sub);
      if (s->is_maxmin()) return refuse("quantified_subquery");
      if (s->cols() != 1) return refuse("row_subquery");
      if (subquery_sql(sub->query_expr(), &q)) return true;
      // More than one row: DuckDB raises an error, mapped to MySQL's.
      *out = "(" + q + ")";
      return false;
    }
    case Item_subselect::EXISTS_SUBQUERY: {
      if (subquery_sql(sub->query_expr(), &q)) return true;
      bool ok = true;
      *out = truth("EXISTS (" + q + ")",
                   down_cast<Item_exists_subselect *>(sub)->value_transform,
                   &ok);
      return ok ? false : refuse("truth_test");
    }
    case Item_subselect::IN_SUBQUERY: {
      auto *in = down_cast<Item_in_subselect *>(sub);
      Item *left = in->left_expr != nullptr ? in->left_expr->real_item()
                                            : nullptr;
      if (left == nullptr) return refuse("in_subquery_operand");
      if (left->type() == Item::CACHE_ITEM &&
          down_cast<Item_cache *>(left)->get_example() != nullptr)
        left = down_cast<Item_cache *>(left)->get_example()->real_item();
      if (left->type() == Item::ROW_ITEM) return refuse("row_in_subquery");
      Query_expression *unit = sub->query_expr();
      if (!unit->is_simple()) return refuse("in_subquery_set_operation");
      Item *inner = nullptr;
      for (Item *item : unit->first_query_block()->visible_fields())
        if (inner == nullptr) inner = item->real_item();
      if (inner == nullptr) return refuse("in_subquery_columns");
      // Compared as MySQL compares them: byte strings, same kinds.
      if (is_string(left) && is_string(inner)) {
        if (str_mode(left) == StrMode::REFUSE) return refuse_collation(left);
        if (str_mode(left) != StrMode::RAW || str_mode(inner) != StrMode::RAW)
          return refuse("in_subquery_collation");
      }
      if (!comparable(left, inner)) return refuse("in_subquery_types");
      std::string l;
      if (expr(left, &l) || subquery_sql(unit, &q)) return true;
      bool ok = true;
      *out = truth("(" + l + " IN (" + q + "))", in->value_transform, &ok);
      return ok ? false : refuse("truth_test");
    }
    default:
      // ALL / ANY: the server rewrites the usual cases into joins.
      return refuse("quantified_subquery");
  }
}

// A set operation (UNION, INTERSECT, EXCEPT, or a parenthesized query with
// its own ORDER BY / LIMIT): `SELECT ... FROM ((c0) OP (c1) ...) AS tN
// [ORDER BY] [LIMIT]`, printed by the post-processing block whose only
// table is the result. The operands' columns are cast to the result types
// and named c1, c2, ...; operators are left-associative in both:
// - UNION: DISTINCT up to the last DISTINCT operand, ALL after it;
// - INTERSECT: DISTINCT if any operator is (the parser folds it so);
// - EXCEPT: ALL before the first DISTINCT operand, DISTINCT from it on.
bool Generator::setop_sql(Query_term_set_op *t,
                          const std::vector<std::string> *names,
                          const std::vector<Item *> *targets,
                          std::vector<Kind> *kinds, std::string *sql,
                          std::set<std::string> *weights_out,
                          bool weights_required) {
  Query_block *post = t->query_block();
  if (post == nullptr || post->get_table_list() == nullptr ||
      post->get_table_list()->table == nullptr)
    return refuse("set_operation_without_result_table");
  TABLE *result = post->get_table_list()->table;
  std::vector<Item *> types;
  for (Item *type : *t->types_array())
    if (!type->hidden) types.push_back(type);
  std::vector<std::string> cols;
  for (size_t i = 0; i < types.size(); i++)
    cols.push_back("c" + std::to_string(i + 1));

  const Query_term_type op = t->term_type();
  const size_t n = t->child_count();
  // Comparing rows: strings must compare as bytes.
  const bool compares = op == QT_INTERSECT || op == QT_EXCEPT ||
                        (op == QT_UNION && t->last_distinct() > 0);
  if (compares)
    for (Item *type : types)
      if (is_string(type) && str_mode(type) != StrMode::RAW)
        return str_mode(type) == StrMode::PAD
                   ? refuse("set_operation_padded_string")
                   : refuse_collation(type);

  std::string chain;
  for (size_t i = 0; i < n; i++) {
    Query_term *c = t->child(i);
    std::string csql;
    Block inner;
    Block *outer = m_block;
    m_block = &inner;
    // Every operand carries the weights of the result's weight-collation
    // string columns (the same columns in each, for ORDER BY on them).
    std::set<std::string> child_weights;
    const bool failed =
        c->term_type() == QT_QUERY_BLOCK
            ? block_sql(down_cast<Query_block *>(c), &cols, nullptr, &csql,
                        &types, nullptr, &child_weights, true)
            : setop_sql(down_cast<Query_term_set_op *>(c), &cols, &types,
                        nullptr, &csql, &child_weights, true);
    m_block = outer;
    if (failed) return true;
    if (i > 0) {
      const int64_t idx = static_cast<int64_t>(i);
      switch (op) {
        case QT_UNION:
          chain += idx <= t->last_distinct() ? " UNION " : " UNION ALL ";
          break;
        case QT_INTERSECT:
          chain += t->last_distinct() > 0 ? " INTERSECT " : " INTERSECT ALL ";
          break;
        case QT_EXCEPT:
          chain += idx >= t->first_distinct() ? " EXCEPT " : " EXCEPT ALL ";
          break;
        default:
          return refuse("set_operation_type");
      }
    }
    chain += "(" + csql + ")";
  }

  TableInfo ti;
  ti.alias = "t" + std::to_string(m_next_alias++);
  ti.derived = true;
  ti.positional = true;
  for (size_t i = 0; i < types.size(); i++)
    if (is_string(types[i]) && str_mode(types[i]) == StrMode::WEIGHT)
      ti.weights.insert(cols[i]);
  m_info[result] = ti;
  Block block;
  block.tables.insert(result);
  Block *outer = m_block;
  m_block = &block;
  const std::string from = "(" + chain + ") AS " + ti.alias;
  const bool failed = block_sql(post, names, kinds, sql, targets, &from,
                                weights_out, weights_required);
  m_block = outer;
  return failed;
}

bool Generator::generate(LEX *lex, std::string *sql,
                         std::vector<Kind> *kinds) {
  if (lex->sql_command != SQLCOM_SELECT)
    return refuse("unsupported_statement");
  // PAD_CHAR_TO_FULL_LENGTH (deprecated): MySQL returns CHAR values padded
  // to their length; the copy holds them without trailing spaces.
  if (m_thd->variables.sql_mode & MODE_PAD_CHAR_TO_FULL_LENGTH)
    return refuse("sql_mode:PAD_CHAR_TO_FULL_LENGTH");
  Query_expression *unit = lex->unit;

  // History scope (DWL#5): LIVE hides archived rows; FULL shows them and
  // needs DUCKDB_HISTORY_READER at query time too (a session can inherit
  // FULL from the global value).
  m_full = history_scope_full(m_thd);
  if (m_full && !m_thd->security_context()
                     ->has_global_grant(STRING_WITH_LEN("DUCKDB_HISTORY_READER"))
                     .first)
    return refuse("history_scope_full_without_privilege");

  Block top;
  m_block = &top;
  const bool failed =
      unit->is_simple()
          ? block_sql(unit->first_query_block(), nullptr, kinds, sql)
          : setop_sql(unit->set_operation(), nullptr, nullptr, kinds, sql);
  m_block = nullptr;
  return failed;
}

void note_refusal(THD *thd, OffloadContext *ctx, const std::string &reason) {
  ctx->reason = "the DuckDB secondary engine cannot run this query: " + reason;
  g_refused++;
  {
    std::lock_guard<std::mutex> guard(g_reason_mutex);
    g_last_refusal = reason;
  }
  Opt_trace_context *const trace = &thd->opt_trace;
  if (trace->is_started()) {
    const Opt_trace_object wrapper(trace);
    Opt_trace_object oto(trace, "duckdb_offload");
    oto.add_alnum("refused", reason.c_str());
  }
}

// ---- execution --------------------------------------------------------

// A YEAR column's holder: sent zero-filled to 4 digits, as
// Field_year::send_to_protocol does ("0000"); Item::send would send "0".
class Year_column_holder : public Item_cache_int {
 public:
  Year_column_holder() : Item_cache_int(MYSQL_TYPE_YEAR) {}
  bool send(Protocol *protocol, String *) override {
    const longlong nr = val_int();
    if (null_value) return protocol->store_null();
    return protocol->store_short(nr, 4);
  }
};

// A value holder per select-list item, typed like the item, filled per row.
Item_cache *make_holder(Item *item, Kind kind) {
  Item_cache *h = nullptr;
  switch (kind) {
    case Kind::INT:
    case Kind::UINT:
      if (item->data_type() == MYSQL_TYPE_YEAR &&
          item->real_item()->type() == Item::FIELD_ITEM)
        h = new Year_column_holder();
      else
        h = new Item_cache_int(item->data_type());
      break;
    case Kind::BIT:
      h = new Item_cache_str(item);
      break;
    case Kind::TIME: {
      // As the DATE holder: attributes copied by hand.
      auto *t = new Item_cache_time();
      if (t == nullptr) return nullptr;
      t->max_length = item->max_length;
      t->decimals = item->decimals;
      t->collation.set(item->collation);
      return t;
    }
    case Kind::DOUBLE:
      h = new Item_cache_real();
      // A FLOAT is sent as FLOAT (the metadata says so): MySQL prints its
      // shortest float digits, 0.1 and not 0.10000000149011612.
      if (h != nullptr && item->data_type() == MYSQL_TYPE_FLOAT)
        h->set_data_type(MYSQL_TYPE_FLOAT);
      break;
    case Kind::DECIMAL:
      h = new Item_cache_decimal();
      break;
    case Kind::DATE: {
      // store_value(Date_val) requires no example item: we copy the
      // attributes by hand.
      auto *d = new Item_cache_date();
      if (d == nullptr) return nullptr;
      d->max_length = item->max_length;
      d->decimals = item->decimals;
      d->collation.set(item->collation);
      return d;
    }
    case Kind::DATETIME:
    case Kind::TIMESTAMP:
      h = new Item_cache_datetime(MYSQL_TYPE_DATETIME);
      break;
    case Kind::STRING:
    case Kind::BLOB:
      h = new Item_cache_str(item);
      break;
  }
  if (h == nullptr || h->setup(item)) return nullptr;
  return h;
}

// The DuckDB type each kind is read as: checked before reading a chunk, so a
// mismatch is a query error, never a wrong read.
duckdb_type expected_type(Kind kind) {
  switch (kind) {
    case Kind::INT:
    case Kind::TIME: return DUCKDB_TYPE_BIGINT;
    case Kind::UINT:
    case Kind::BIT: return DUCKDB_TYPE_UBIGINT;
    case Kind::DOUBLE: return DUCKDB_TYPE_DOUBLE;
    case Kind::DECIMAL: return DUCKDB_TYPE_VARCHAR;
    case Kind::STRING: return DUCKDB_TYPE_VARCHAR;
    case Kind::BLOB: return DUCKDB_TYPE_BLOB;
    case Kind::DATE: return DUCKDB_TYPE_DATE;
    case Kind::DATETIME:
    case Kind::TIMESTAMP: return DUCKDB_TYPE_TIMESTAMP;
  }
  return DUCKDB_TYPE_INVALID;
}

bool vector_type_is(duckdb_vector v, Kind kind) {
  duckdb_logical_type t = duckdb_vector_get_column_type(v);
  const bool ok = duckdb_get_type_id(t) == expected_type(kind);
  duckdb_destroy_logical_type(&t);
  return ok;
}

std::string duckdb_text(duckdb_vector v, idx_t row) {
  auto *s = static_cast<duckdb_string_t *>(duckdb_vector_get_data(v)) + row;
  return std::string(duckdb_string_t_data(s), duckdb_string_t_length(*s));
}

// Converts one value; false on success.
bool store_cell(THD *thd, Item_cache *h, Item *not_null, Kind kind,
                duckdb_vector v, idx_t row, String *buf) {
  uint64_t *validity = duckdb_vector_get_validity(v);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    h->store_null();
    return false;
  }
  void *data = duckdb_vector_get_data(v);
  switch (kind) {
    case Kind::INT:
      down_cast<Item_cache_int *>(h)->store_value(
          not_null, static_cast<int64_t *>(data)[row]);
      return false;
    case Kind::UINT:
      down_cast<Item_cache_int *>(h)->store_value(
          not_null, static_cast<longlong>(static_cast<uint64_t *>(data)[row]));
      // store_value() copies unsigned_flag from not_null (signed): values
      // from 2^63 would be sent as negative numbers.
      h->unsigned_flag = true;
      return false;
    case Kind::DOUBLE:
      down_cast<Item_cache_real *>(h)->store_value(
          not_null, static_cast<double *>(data)[row]);
      return false;
    case Kind::TIME: {
      const int64_t micros = static_cast<int64_t *>(data)[row];
      const uint64_t abs_us = micros < 0 ? 0 - static_cast<uint64_t>(micros)
                                         : static_cast<uint64_t>(micros);
      Time_val t;
      if (Time_val::make_time(micros < 0,
                              static_cast<uint32_t>(abs_us / 3600000000ULL),
                              static_cast<uint32_t>(abs_us / 60000000ULL % 60),
                              static_cast<uint32_t>(abs_us / 1000000ULL % 60),
                              static_cast<uint32_t>(abs_us % 1000000ULL), &t))
        return true;
      down_cast<Item_cache_time *>(h)->store_value(t);
      return false;
    }
    case Kind::BIT: {
      // Field_bit::val_str: the value big-endian over ceil(n/8) bytes.
      uint64_t bits = static_cast<uint64_t *>(data)[row];
      const size_t n = (h->max_length + 7) / 8;
      if (n < 1 || n > 8) return true;
      char bytes[8];
      for (size_t k = n; k-- > 0;) {
        bytes[k] = static_cast<char>(bits & 0xFF);
        bits >>= 8;
      }
      buf->copy(bytes, n, &my_charset_bin);
      down_cast<Item_cache_str *>(h)->store_value(not_null, *buf);
      return false;
    }
    case Kind::DECIMAL: {
      const std::string text = duckdb_text(v, row);
      my_decimal d;
      const char *end = text.data() + text.size();
      if (str2my_decimal(E_DEC_FATAL_ERROR, text.data(), &d, &end) != 0)
        return true;
      down_cast<Item_cache_decimal *>(h)->store_value(not_null, &d);
      return false;
    }
    case Kind::STRING:
    case Kind::BLOB: {
      const std::string text = duckdb_text(v, row);
      unsigned errors = 0;
      if (kind == Kind::BLOB)
        buf->copy(text.data(), text.size(), &my_charset_bin);
      else
        buf->copy(text.data(), text.size(), &my_charset_utf8mb4_bin,
                  h->collation.collation, &errors);
      down_cast<Item_cache_str *>(h)->store_value(not_null, *buf);
      return false;
    }
    case Kind::DATE: {
      const duckdb_date_struct d =
          duckdb_from_date(static_cast<duckdb_date *>(data)[row]);
      down_cast<Item_cache_date *>(h)->store_value(
          Date_val(d.year, d.month, d.day));
      return false;
    }
    case Kind::DATETIME:
    case Kind::TIMESTAMP: {
      const duckdb_timestamp ts = static_cast<duckdb_timestamp *>(data)[row];
      MYSQL_TIME t{};
      if (kind == Kind::TIMESTAMP) {
        // UTC instant: shown in the session time zone, as MySQL does.
        const int64_t micros = ts.micros;
        int64_t secs = micros / 1000000;
        int64_t frac = micros % 1000000;
        if (frac < 0) {
          frac += 1000000;
          secs -= 1;
        }
        thd->time_zone()->gmt_sec_to_TIME(&t, static_cast<my_time_t>(secs));
        t.second_part = static_cast<unsigned long>(frac);
      } else {
        const duckdb_timestamp_struct s = duckdb_from_timestamp(ts);
        t.year = s.date.year;
        t.month = s.date.month;
        t.day = s.date.day;
        t.hour = s.time.hour;
        t.minute = s.time.min;
        t.second = s.time.sec;
        t.second_part = s.time.micros;
      }
      t.time_type = MYSQL_TIMESTAMP_DATETIME;
      down_cast<Item_cache_datetime *>(h)->store_value(
          not_null, TIME_to_longlong_datetime_packed(t));
      return false;
    }
  }
  return true;
}

// EXPLAIN (any format): the DuckDB SQL and DuckDB's own plan as notes. MySQL
// prints its plan for the query; DuckDB plans the SQL itself. SHOW WARNINGS
// shows at most 512 bytes of a message, so long text is split in numbered
// notes (at line ends when possible, never inside a UTF-8 character).
void push_notes(THD *thd, const std::string &title, const std::string &text) {
  constexpr size_t kChunk = 480;  // + the title, under 512 bytes
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = std::min(text.size(), pos + kChunk);
    if (end < text.size()) {
      const size_t nl = text.rfind('\n', end);
      if (nl != std::string::npos && nl > pos + kChunk / 2) {
        end = nl + 1;
      } else {
        while (end > pos && (static_cast<unsigned char>(text[end]) & 0xC0) ==
                                0x80)
          end--;
      }
    }
    parts.push_back(text.substr(pos, end - pos));
    pos = end;
  }
  for (size_t i = 0; i < parts.size(); i++) {
    std::string msg = title;
    if (parts.size() > 1)
      msg += " (" + std::to_string(i + 1) + "/" +
             std::to_string(parts.size()) + ")";
    msg += ":\n" + parts[i];
    push_warning(thd, Sql_condition::SL_NOTE, ER_SECONDARY_ENGINE_PLUGIN,
                 msg.c_str());
  }
}

void explain_notes(THD *thd, OffloadContext *ctx) {
  push_notes(thd, "DuckDB SQL", ctx->sql);
  std::string plan, err;
  const bool failed = ctx->con->execute(
      [&](duckdb_connection c, std::string *e) {
        duckdb_result res;
        if (duckdb_query(c, ("EXPLAIN " + ctx->sql).c_str(), &res) ==
            DuckDBError) {
          const char *m = duckdb_result_error(&res);
          *e = m != nullptr ? m : "EXPLAIN failed";
          duckdb_destroy_result(&res);
          return true;
        }
        const idx_t columns = duckdb_column_count(&res);
        for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
          char *v = duckdb_value_varchar(&res, columns - 1, r);
          if (v != nullptr) plan += v;
          duckdb_free(v);
        }
        duckdb_destroy_result(&res);
        return false;
      },
      &err);
  push_notes(thd, "DuckDB plan", failed ? "not available: " + err : plan);
}

// A DuckDB error during execution. When MySQL has the same error, the client
// gets MySQL's code with both messages (decided 2026-11-13); otherwise
// ER_SECONDARY_ENGINE_PLUGIN with DuckDB's message.
void report_duckdb_error(const std::string &msg) {
  const std::string text = msg.substr(0, 400);
  if (msg.find(kDuckDBMultipleRows) != std::string::npos ||
      msg.find("More than one row returned by a subquery") !=
          std::string::npos) {
    my_printf_error(ER_SUBQUERY_NO_1_ROW,
                    "Subquery returns more than 1 row (DuckDB error: %s)",
                    MYF(0), text.c_str());
    return;
  }
  if (msg.find("INTERRUPT Error") != std::string::npos) {
    // max_execution_time or KILL QUERY interrupted DuckDB.
    THD *thd = current_thd;
    if (thd != nullptr && thd->killed == THD::KILL_TIMEOUT) {
      my_printf_error(ER_QUERY_TIMEOUT,
                      "Query execution was interrupted, maximum statement "
                      "execution time exceeded (DuckDB error: %s)",
                      MYF(0), text.c_str());
      return;
    }
    my_printf_error(ER_QUERY_INTERRUPTED,
                    "Query execution was interrupted (DuckDB error: %s)",
                    MYF(0), text.c_str());
    return;
  }
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
           ("DuckDB error: " + text).c_str());
}

// JOIN::override_executor_func: runs the whole statement in DuckDB and sends
// the rows. Called once per query block; single-table queries have one.
bool execute_offloaded(JOIN *join, Query_result *result) {
  THD *thd = join->thd;
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr || ctx->executed) return false;
  ctx->executed = true;
  const auto started = std::chrono::steady_clock::now();

  mem_root_deque<Item *> holders(thd->mem_root);
  std::vector<Item_cache *> caches;
  // Typed like the items the server sent as metadata (the query
  // expression's field list: with GROUP BY these can be temporary table
  // columns, e.g. BIGINT for an INT expression; for a set operation the
  // result columns), so each value is sent as the client was told.
  size_t i = 0;
  for (Item *item :
       *join->query_block->master_query_expression()->get_field_list()) {
    if (item->hidden) continue;
    if (i >= ctx->kinds.size()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "select list changed after offload planning");
      return true;
    }
    Item_cache *h = make_holder(item, ctx->kinds[i++]);
    if (h == nullptr) return true;
    holders.push_back(h);
    caches.push_back(h);
  }
  Item *not_null = new Item_int(0);
  if (not_null == nullptr) return true;
  not_null->null_value = false;

  std::string err;
  Lake::Connection *con = ctx->con.get();
  if (con == nullptr || ctx->stmt == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "the DuckDB statement was not prepared");
    return true;
  }
  duckdb_result res;
  bool have_result = false;
  duckdb_data_chunk chunk = nullptr;
  auto cleanup = [&] {
    std::string ignored;
    con->execute_cleanup(
        [&](duckdb_connection, std::string *) {
          if (chunk != nullptr) duckdb_destroy_data_chunk(&chunk);
          if (have_result) duckdb_destroy_result(&res);
          return false;
        },
        &ignored);
  };
  auto fail = [&](const std::string &msg) {
    cleanup();
    if (!thd->is_error()) report_duckdb_error(msg);
    return true;
  };

  bool use_first = false;
  if (ctx->have_pre_result) {
    // Already run during optimization: the result is ours now.
    res = ctx->pre_result;
    ctx->have_pre_result = false;
    have_result = true;
  } else if (ctx->have_stream) {
    // Started during optimization (T2): the stream and its first chunk are
    // ours now.
    res = ctx->stream_result;
    ctx->have_stream = false;
    have_result = true;
    chunk = ctx->first_chunk;
    ctx->first_chunk = nullptr;
    use_first = ctx->first_fetched;
    ctx->first_fetched = false;
  } else if (con->execute(
          [&](duckdb_connection, std::string *e) {
            if (duckdb_execute_prepared_streaming(ctx->stmt, &res) ==
                DuckDBError) {
              const char *m = duckdb_result_error(&res);
              *e = m != nullptr ? m : "execution failed";
              duckdb_destroy_result(&res);
              return true;
            }
            have_result = true;
            return false;
          },
          &err))
    return fail(err);

  String buf;
  for (;;) {
    if (use_first) {
      use_first = false;  // the chunk fetched during optimization
    } else if (con->execute(
            [&](duckdb_connection, std::string *e) {
              chunk = duckdb_fetch_chunk(res);
              if (chunk == nullptr) {
                const char *m = duckdb_result_error(&res);
                if (m != nullptr && *m != '\0') {
                  *e = m;
                  return true;
                }
              }
              return false;
            },
            &err))
      return fail(err);
    if (chunk == nullptr) break;
    const idx_t rows = duckdb_data_chunk_get_size(chunk);
    if (duckdb_data_chunk_get_column_count(chunk) != caches.size())
      return fail("unexpected column count");
    for (size_t c = 0; c < caches.size(); c++)
      if (!vector_type_is(duckdb_data_chunk_get_vector(chunk, c),
                          ctx->kinds[c]))
        return fail("unexpected type of column " + std::to_string(c + 1));
    for (idx_t r = 0; r < rows; r++) {
      if (thd->killed) return fail("interrupted");
      for (size_t c = 0; c < caches.size(); c++) {
        if (store_cell(thd, caches[c], not_null, ctx->kinds[c],
                       duckdb_data_chunk_get_vector(chunk, c), r, &buf))
          return fail("cannot convert a value of column " +
                      std::to_string(c + 1));
      }
      if (result->send_data(thd, holders)) {
        cleanup();
        return true;
      }
      join->send_records++;
    }
    std::string ignored;
    con->execute_cleanup(
        [&](duckdb_connection, std::string *) {
          duckdb_destroy_data_chunk(&chunk);
          chunk = nullptr;
          return false;
        },
        &ignored);
  }
  cleanup();
  g_offloaded++;  // run in DuckDB to the end
  g_offload_time_us += micros_since(started) + ctx->pre_run_us;
  return false;
}

// ---- hooks ------------------------------------------------------------

// Before preparing for the secondary engine: the engine must be open and the
// query expensive enough (as the MOCK engine does).
// MySQL's plan for InnoDB reads a big loaded table where its row estimate
// comes from sampled index statistics (full scan, full index scan,
// non-unique lookup): the cost can be far too low (DWL#6 6d-3: 382 instead
// of 52,560 rows per key). Primary key / unique lookups and index ranges
// (index dives) keep MySQL's estimate. The table size is InnoDB's table
// estimate, accurate enough at this scale and free during planning.
bool big_table_scan(THD *thd, std::string *which) {
  const unsigned long long min_rows = offload_min_rows(thd);
  if (min_rows == 0) return false;
  for (const Query_block *qb = thd->lex->all_query_blocks_list; qb != nullptr;
       qb = qb->next_select_in_list()) {
    const JOIN *join = qb->join;
    if (join == nullptr || join->qep_tab == nullptr) continue;
    for (uint i = 0; i < join->primary_tables; i++) {
      const QEP_TAB &tab = join->qep_tab[i];
      const TABLE *table = tab.table();
      if (table == nullptr || table->file == nullptr ||
          !table->s->has_secondary_engine())
        continue;
      const join_type type = tab.type();
      if (type != JT_ALL && type != JT_INDEX_SCAN && type != JT_REF &&
          type != JT_REF_OR_NULL)
        continue;
      if (table->file->stats.records < min_rows) continue;
      *which = std::string(table->s->db.str) + "." + table->s->table_name.str;
      return true;
    }
  }
  return false;
}

bool pre_prepare(THD *thd) {
  if (!Lake::instance().is_open()) return false;
  std::string big;
  if (thd->m_current_query_cost <=
          static_cast<double>(thd->variables.secondary_engine_cost_threshold) &&
      big_table_scan(thd, &big)) {
    Opt_trace_context *const trace = &thd->opt_trace;
    if (trace->is_started()) {
      const Opt_trace_object wrapper(trace);
      Opt_trace_object oto(trace, "duckdb_offload_candidate");
      oto.add_alnum("reason",
                    "scan of a large loaded table (duckdb_offload_min_rows)");
      oto.add_utf8("table", big.c_str());
      oto.add("cost", thd->m_current_query_cost);
    }
    return true;
  }
  if (thd->m_current_query_cost <=
      static_cast<double>(thd->variables.secondary_engine_cost_threshold)) {
    Opt_trace_context *const trace = &thd->opt_trace;
    if (trace->is_started()) {
      const Opt_trace_object wrapper(trace);
      Opt_trace_object oto(trace, "secondary_engine_not_used");
      oto.add_alnum("reason",
                    "The estimated query cost does not exceed "
                    "secondary_engine_cost_threshold.");
      oto.add("cost", thd->m_current_query_cost);
      oto.add("threshold", thd->variables.secondary_engine_cost_threshold);
    }
    return false;
  }
  return true;
}

bool prepare(THD *thd, LEX *lex) {
  auto *ctx = new (thd->mem_root) OffloadContext;
  if (ctx == nullptr) return true;
  lex->set_secondary_engine_execution_context(ctx);
  ctx->disable_semijoin_materialization(thd);
  // The handler cannot read rows: no constant tables, no subquery evaluated
  // during optimization (as the MOCK engine).
  lex->add_statement_options(OPTION_NO_CONST_TABLES |
                             OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);
  return false;
}

// A session that wrote in its open transaction would not see its own
// uncommitted rows in DuckDB.
bool wrote_in_transaction(THD *thd) {
  if (!thd->in_active_multi_stmt_transaction()) return false;
  Transaction_ctx *trn = thd->get_transaction();
  for (auto &info : trn->ha_trx_info(Transaction_ctx::SESSION))
    if (info.is_trx_read_write()) return true;
  return false;
}

// DuckDB errors that mean a DECIMAL value did not fit in 38 digits: by
// error type (an overflow in arithmetic is OUT_OF_RANGE, in a cast to
// DECIMAL(38, s) CONVERSION), and the value is a DECIMAL (its 128-bit
// integer). Other errors are never retried with fewer decimals.
bool is_decimal_overflow(duckdb_error_type type, const std::string &msg) {
  if (type != DUCKDB_ERROR_OUT_OF_RANGE && type != DUCKDB_ERROR_CONVERSION)
    return false;
  return msg.find("DECIMAL") != std::string::npos ||
         msg.find("HUGEINT") != std::string::npos ||
         msg.find("INT128") != std::string::npos;
}

bool optimize(THD *thd, LEX *lex) {
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr) return true;
  ctx->restore_optimizer_switch();  // the server's optimization is done
  std::string reason;
  bool wide = false;  // DECIMAL values declared above 38 digits
  bool grouped_check = false;  // grouped strings checked in the query
  int max_scale = 0;
  auto build = [&](int scale_cap) {
    Generator gen(thd, &reason);
    gen.set_decimal_scale_cap(scale_cap);
    ctx->kinds.clear();
    ctx->sql.clear();
    gen.generate(lex, &ctx->sql, &ctx->kinds);
    wide = gen.decimal_wide();
    max_scale = gen.decimal_max_scale();
    grouped_check = gen.grouped_check();
  };
  if (wrote_in_transaction(thd)) {
    reason = "written_in_transaction";
  } else {
    build(-1);
  }
  // Freshness (duckdb_offload_wait_applied): the session's last commit must
  // be in DuckDB before we read it there.
  if (reason.empty() && offload_wait_applied(thd)) {
    BinlogPos target;
    if (freshness_target(thd, &target)) {
      reason = "binary_log_position_unknown";
    } else if (!target.empty()) {
      g_waits++;
      // A statement that can only run in DuckDB (history table under FULL:
      // InnoDB has no archived rows) waits without a limit; KILL QUERY and
      // max_execution_time stop it. Others wait duckdb_offload_wait_timeout_ms
      // and then run on InnoDB. The wait shows its progress (SHOW
      // PROCESSLIST, Performance Schema).
      const bool only_duckdb = lex->can_execute_only_in_secondary_engine();
      const unsigned long long limit = offload_wait_timeout_ms(thd);
      const WaitResult w = wait_for_applier(
          thd, target, only_duckdb ? 0 : (limit == 0 ? 1 : limit),
          "the session's last commit", nullptr, nullptr, nullptr);
      if (w == WaitResult::TIMEOUT) {
        g_wait_timeouts++;
        reason = "applier_behind";
      } else if (w == WaitResult::KILLED) {
        reason = "applier_behind";
      } else if (w == WaitResult::NOT_RUNNING) {
        reason = "applier_not_running";
      }
    }
  }
  // Prepared now: a DuckDB bind error (types MySQL accepts but DuckDB does
  // not) is a refusal, and the query runs on InnoDB.
  auto prepare_sql = [&](std::string *err) {
    return ctx->con->execute(
        [ctx](duckdb_connection c, std::string *e) {
          if (ctx->stmt != nullptr) duckdb_destroy_prepare(&ctx->stmt);
          ctx->stmt = nullptr;
          if (duckdb_prepare(c, ctx->sql.c_str(), &ctx->stmt) == DuckDBError) {
            const char *m = duckdb_prepare_error(ctx->stmt);
            *e = m != nullptr ? m : "prepare failed";
            duckdb_destroy_prepare(&ctx->stmt);
            ctx->stmt = nullptr;
            return true;
          }
          return false;
        },
        err);
  };
  // Run now, materialized (a DECIMAL overflow is then a refusal, not an
  // error in the middle of sending rows).
  duckdb_error_type run_error = DUCKDB_ERROR_INVALID;
  auto run_first = [&](std::string *err) {
    const auto started = std::chrono::steady_clock::now();
    auto timed = create_scope_guard(
        [&] { ctx->pre_run_us += micros_since(started); });
    return ctx->con->execute(
        [ctx, &run_error](duckdb_connection, std::string *e) {
          if (duckdb_execute_prepared(ctx->stmt, &ctx->pre_result) ==
              DuckDBError) {
            const char *m = duckdb_result_error(&ctx->pre_result);
            *e = m != nullptr ? m : "execution failed";
            run_error = duckdb_result_error_type(&ctx->pre_result);
            duckdb_destroy_result(&ctx->pre_result);
            return true;
          }
          ctx->have_pre_result = true;
          return false;
        },
        err);
  };
  if (reason.empty()) {
    std::string err;
    ctx->con = Lake::instance().connect(thd, &err);
    if (!ctx->con)
      reason = "duckdb_connection: " + err;
    else if (prepare_sql(&err))
      reason = "duckdb_prepare: " + err.substr(0, 200);
  }
  // DECIMAL values MySQL declares above 38 digits (duckdb_decimal_overflow):
  // DuckDB computes them as DECIMAL(38, scale), exact while they fit. With
  // FALLBACK and ROUND the statement runs here first; ERROR streams as
  // usual and an overflow is the statement's error.
  const DecimalOverflow mode = decimal_overflow(thd);
  auto ambiguous = [](const std::string &err) {
    return err.find(kGroupedAmbiguous) != std::string::npos;
  };
  const char *const kAmbiguousReason =
      "grouped_string_value_in_join: a group holds values that are equal "
      "under the collation but stored differently ('a' and 'a ', or 'A' and "
      "'a'); InnoDB shows one of them by its join order";
  if (reason.empty() && (grouped_check || (wide && mode != DecimalOverflow::ERROR)) &&
      !lex->is_explain()) {
    std::string err;
    if (run_first(&err)) {
      if (ambiguous(err)) {
        reason = kAmbiguousReason;
      } else if (!wide || mode == DecimalOverflow::ERROR) {
        reason = "duckdb_execution: " + err.substr(0, 200);
      } else if (!is_decimal_overflow(run_error, err)) {
        reason = "duckdb_execution: " + err.substr(0, 200);
      } else if (mode == DecimalOverflow::FALLBACK) {
        reason = "decimal_overflow: a DECIMAL value exceeds DuckDB's 38 "
                 "digits (duckdb_decimal_overflow=FALLBACK): " +
                 err.substr(0, 160);
      } else {
        // ROUND: fewer decimals until the values fit, with a warning.
        // Fewer decimals each time: 1, 2, 4, 8 ... fewer, then none.
        bool done = false;
        int cap = max_scale;
        int step = 1;
        while (!done && reason.empty() && cap > 0) {
          cap = cap > step ? cap - step : 0;
          step *= 2;
          build(cap);
          if (!reason.empty()) break;
          g_decimal_reruns++;
          if (prepare_sql(&err)) {
            reason = "duckdb_prepare: " + err.substr(0, 200);
          } else if (!run_first(&err)) {
            done = true;
            push_warning_printf(
                thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE_PLUGIN,
                "DuckDB: DECIMAL results rounded to %d decimals: a value "
                "exceeds DuckDB's 38 digits with %d decimals "
                "(duckdb_decimal_overflow=ROUND)",
                cap, max_scale);
          } else if (ambiguous(err)) {
            reason = kAmbiguousReason;
          } else if (!is_decimal_overflow(run_error, err)) {
            reason = "duckdb_execution: " + err.substr(0, 200);
          }
        }
        if (!done && reason.empty())
          reason = "decimal_overflow: a DECIMAL value exceeds DuckDB's 38 "
                   "digits, also with fewer decimals (the overflow is in "
                   "DuckDB's own arithmetic; duckdb_decimal_overflow=ROUND)";
      }
    }
  }
  // T2 (2026-11-23): we start the statement here and fetch its first chunk.
  // DuckDB runs the blocking parts of a plan (hash join builds, aggregates,
  // sorts) before it returns a first chunk, which is where out of memory or
  // a full temporary disk happen. An error here is a refusal: MySQL then runs
  // the statement on InnoDB (use_secondary_engine = ON). It cannot once our
  // executor has started (Sql_cmd_dml::execute_inner marks the statement
  // executed before it), so a later error still reaches the client. The rest
  // is streamed by execute_offloaded(), never materialized.
  // Not with duckdb_decimal_overflow = ERROR on a statement with DECIMAL
  // values above 38 digits: there an overflow must be the statement's error
  // (also with ON), and any error here would make MySQL run it on InnoDB.
  if (reason.empty() && !lex->is_explain() && !ctx->have_pre_result &&
      !(wide && mode == DecimalOverflow::ERROR)) {
    bool simulate = false;
    DBUG_EXECUTE_IF("duckdb_offload_fail_first_chunk", simulate = true;);
    std::string err;
    const auto started = std::chrono::steady_clock::now();
    const bool failed = ctx->con->execute(
        [ctx, simulate](duckdb_connection, std::string *e) {
          if (duckdb_execute_prepared_streaming(ctx->stmt,
                                                &ctx->stream_result) ==
              DuckDBError) {
            const char *m = duckdb_result_error(&ctx->stream_result);
            *e = m != nullptr ? m : "execution failed";
            duckdb_destroy_result(&ctx->stream_result);
            return true;
          }
          ctx->have_stream = true;
          if (simulate) {
            *e = "Out of Memory Error: simulated (duckdb_offload_fail_first_chunk)";
            return true;
          }
          ctx->first_chunk = duckdb_fetch_chunk(ctx->stream_result);
          if (ctx->first_chunk == nullptr) {
            const char *m = duckdb_result_error(&ctx->stream_result);
            if (m != nullptr && *m != '\0') {
              *e = m;
              return true;
            }
          }
          ctx->first_fetched = true;
          return false;
        },
        &err);
    ctx->pre_run_us += micros_since(started);
    if (failed) {
      // KILL QUERY / max_execution_time, and a scalar subquery with more
      // than one row: MySQL's own error (InnoDB gives the same; a killed
      // statement is never retried).
      if (thd->is_killed() ||
          err.find("More than one row returned by a subquery") !=
              std::string::npos ||
          err.find(kDuckDBMultipleRows) != std::string::npos) {
        report_duckdb_error(err);
        return true;
      }
      reason = "duckdb_execution: " + err.substr(0, 200);
    }
  }
  if (!reason.empty()) {
    note_refusal(thd, ctx, reason);
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ctx->reason.c_str());
    return true;
  }
  if (lex->is_explain()) explain_notes(thd, ctx);
  // The server calls the executor of each query block of a set operation;
  // the first call runs the whole statement.
  for (Query_block *qb = lex->unit->first_query_block(); qb != nullptr;
       qb = qb->next_query_block())
    if (qb->join != nullptr) qb->join->override_executor_func = execute_offloaded;
  // Counted when it has run (execute_offloaded); EXPLAIN separately.
  if (lex->is_explain()) g_explained++;
  Opt_trace_context *const trace = &thd->opt_trace;
  if (trace->is_started()) {
    const Opt_trace_object wrapper(trace);
    Opt_trace_object oto(trace, "duckdb_offload");
    oto.add_utf8("sql", ctx->sql.c_str());
  }
  return false;
}

// DuckDB makes its own plan from the SQL: the server's join order is not
// used, so its own comparison stands (`cheaper` is already set, per JOIN; a
// statement-wide best cost made every plan of a second JOIN "not cheaper"
// and the planner found none).
bool compare_cost(THD *, const JOIN &, double optimizer_cost,
                  bool *use_best_so_far, bool *,
                  double *secondary_engine_cost) {
  *use_best_so_far = false;
  *secondary_engine_cost = optimizer_cost;
  return false;
}

// The server asks for a reason whenever a FORCED query did not run in the
// secondary engine, also when it never asked us (sql_select.cc,
// find_and_set_offload_fail_reason, asserts a non-empty reason): with
// autocommit=0 the statement is part of a multi-statement transaction and
// the server does not offload it, before any of our hooks runs.
std::string_view fail_reason(THD *thd) {
  OffloadContext *ctx = context(thd);
  if (ctx != nullptr && !ctx->reason.empty()) return ctx->reason;
  if (thd != nullptr && thd->in_multi_stmt_transaction_mode())
    return "autocommit=0 or an open transaction: MySQL does not run queries "
           "of multi-statement transactions in a secondary engine (use "
           "autocommit=1)";
  return "the query was not sent to the DuckDB secondary engine";
}

}  // namespace

bool register_offload_observer(void *plugin) {
  if (register_trans_observer(&g_trans_observer, plugin) != 0) return true;
  g_observer_registered = true;
  return false;
}

void unregister_offload_observer(void *plugin) {
  if (!g_observer_registered) return;
  unregister_trans_observer(&g_trans_observer, plugin);
  g_observer_registered = false;
}

void install_offload_hooks(handlerton *hton) {
  g_hton = hton;
  hton->close_connection = close_connection;
  hton->secondary_engine_pre_prepare_hook = pre_prepare;
  hton->prepare_secondary_engine = prepare;
  hton->optimize_secondary_engine = optimize;
  hton->compare_secondary_engine_cost = compare_cost;
  hton->find_secondary_engine_offload_fail_reason = fail_reason;
  hton->secondary_engine_flags = MakeSecondaryEngineFlags(
      SecondaryEngineFlag::SUPPORTS_HASH_JOIN,
      SecondaryEngineFlag::SUPPORTS_NESTED_LOOP_JOIN,
      SecondaryEngineFlag::USE_EXTERNAL_EXECUTOR);
}

bool session_commit_position(THD *thd, BinlogPos *pos) {
  return freshness_target(thd, pos);
}

OffloadStatus offload_status() {
  OffloadStatus s;
  s.offloaded = g_offloaded;
  s.refused = g_refused;
  s.explained = g_explained;
  s.waits = g_waits;
  s.wait_timeouts = g_wait_timeouts;
  s.decimal_reruns = g_decimal_reruns;
  s.time_us = g_offload_time_us;
  std::lock_guard<std::mutex> guard(g_reason_mutex);
  s.last_refusal = g_last_refusal;
  return s;
}

}  // namespace duckdb_se
