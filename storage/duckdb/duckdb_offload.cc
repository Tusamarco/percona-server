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
#include "sql/query_result.h"
#include "sql/query_term.h"
#include "sql/replication.h"  // Trans_observer
#include "sql/sql_class.h"
#include "sql/sql_executor.h"  // QEP_TAB
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
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
            if (stmt != nullptr) duckdb_destroy_prepare(&stmt);
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
  }
  return -1;
}

bool same_class(Kind a, Kind b) { return kind_class(a) == kind_class(b); }

class Generator {
 public:
  Generator(THD *thd, std::string *reason) : m_thd(thd), m_reason(reason) {}

  bool generate(LEX *lex, std::string *sql, std::vector<Kind> *kinds);

 private:
  bool refuse(const std::string &why) {
    *m_reason = why;
    return true;
  }
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
  bool numeric(Item *item, std::string *out);
  int division_frac(int f1, int f2) const;
  bool internal_frac(Item *item, int *frac);
  bool exact_division(const std::string &a, int f1, const std::string &b,
                      int f2, int round_to, std::string *out);
  bool temporal_arg(Item *item, std::string *out);
  bool order_key(Item *item, std::string *out);
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
    std::set<const Field *> grouped;  // GROUP BY columns
    struct Semi {
      Table_ref *nest;
      bool anti;
      std::string from;
    };
    std::vector<Semi> semi;  // semi- and anti-join nests at the top
  };
  bool grouped_value(const Field *field, const TableInfo &ti,
                     const std::string &col, std::string *out);

  THD *m_thd;
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
  bool const_numbers = false;
  size_t max_bytes = 0;
  for (unsigned i = from; i < to; i++) {
    Item *a = args[i]->real_item();
    if (a->const_item()) {
      if (is_string(a))
        max_bytes = std::max<size_t>(max_bytes, a->max_length);
      else if (!a->is_temporal() && !a->is_null())
        const_numbers = true;
      continue;
    }
    const enum_field_types t = a->data_type();
    if (t == MYSQL_TYPE_TIMESTAMP) {
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
  if (timestamp && (temporal != MYSQL_TYPE_NULL || strings || numbers))
    return refuse("mixed_timestamp_comparison");
  if (temporal != MYSQL_TYPE_NULL && (strings || numbers))
    return refuse("mixed_temporal_comparison");
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
      if (is_string(args[i]->real_item()) && !args[i]->real_item()->const_item())
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
    if (m_temporal != MYSQL_TYPE_NULL && a->const_item()) {
      failed = a->is_null() ? literal(a, &s) : typed_literal(a, m_temporal, &s);
    } else if (numbers && a->const_item() && is_string(a)) {
      // A number compared with a string constant: MySQL compares as DOUBLE.
      char buf[64];
      const double v = a->val_real();
      snprintf(buf, sizeof(buf), "CAST(%.17g AS DOUBLE)", v);
      s = a->null_value ? "NULL" : buf;
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
                               std::string *out) {
  const int F = division_frac(f1, f2);
  const int shift = F - f1 + f2;
  if (F > 36 || shift < 0 || f1 > 18 || f2 > 18)
    return refuse("exact_division_precision");
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
  *out = "(CASE WHEN " + B + " = 0 THEN NULL ELSE " + value + " END)";
  return false;
}

// An arithmetic operand: a number (no string or temporal conversion), with
// integers widened to BIGINT as MySQL computes them (DuckDB keeps INTEGER and
// would overflow at 32 bits). Unsigned integers are refused (MySQL's mixed
// signed/unsigned rules).
bool Generator::numeric(Item *item, std::string *out) {
  Item *a = item->real_item();
  if (a->is_temporal() || a->result_type() == STRING_RESULT)
    return refuse("arithmetic_on_non_number");
  if (a->result_type() == INT_RESULT && a->unsigned_flag && !a->const_item())
    return refuse("arithmetic_on_unsigned");
  std::string e;
  if (expr(a, &e)) return true;
  *out = a->result_type() == INT_RESULT ? "CAST(" + e + " AS BIGINT)" : e;
  return false;
}

// The argument of a date function: a DATE or DATETIME column or expression.
// TIMESTAMP (UTC in DuckDB) and strings are refused.
bool Generator::temporal_arg(Item *item, std::string *out) {
  Item *a = item->real_item();
  if (!is_date_or_datetime(a->data_type()) || a->const_item())
    return refuse("date_function_argument");
  return expr(a, out);
}

// An ORDER BY expression or MIN/MAX argument: PAD SPACE strings use the
// padded sort key.
bool Generator::order_key(Item *item, std::string *out) {
  Item *e = item->real_item();
  std::string s;
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
  if (item->const_item()) {
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
      *out = ordering ? pad_sort_key(e, max_bytes) : pad_equal_key(e);
      return false;
    }
    case StrMode::WEIGHT: {
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
// 2026-11-14: exactly as InnoDB).
bool Generator::grouped_value(const Field *field, const TableInfo &ti,
                              const std::string &col, std::string *out) {
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
  (void)field;
  return refuse("grouped_string_value_in_join");
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
  m_block->agg_depth++;
  struct Leave {
    int &d;
    ~Leave() { d--; }
  } leave{m_block->agg_depth};
  std::string a;
  switch (f) {
    case Item_sum::COUNT_FUNC:
      if (arg->const_item() && !arg->is_null()) {
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
      return exact_division("sum(" + d + a + ")",
                            static_cast<int>(arg->decimals),
                            "count(" + d + a + ")", 0, -1, out);
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

bool Generator::function(Item_func *fn, std::string *out) {
  Item **args = fn->arguments();
  const unsigned n = fn->argument_count();
  const std::string name = fn->func_name();
  auto compare = [&](const char *op) {
    std::vector<std::string> ops;
    const bool ordering = op[0] == '<' || op[0] == '>';
    const bool not_equal = std::string(op) == "<>";
    if (operands(fn, ordering && !not_equal, 0, 2, &ops)) return true;
    *out = "(" + ops[0] + " " + op + " " + ops[1] + ")";
    return false;
  };
  auto arith = [&](const char *op) {
    if (fn->result_type() == DECIMAL_RESULT && fn->decimal_precision() > 38)
      return refuse("decimal_precision_above_38");
    std::string l, r;
    if (numeric(args[0], &l) || numeric(args[1], &r)) return true;
    *out = "(" + l + " " + op + " " + r + ")";
    // MySQL rounds a DECIMAL result to the item's scale
    // (Item_func_numhybrid::val_decimal): it matters when an operand has
    // more digits (AVG).
    if (fn->result_type() == DECIMAL_RESULT)
      *out = "CAST(" + *out + " AS DECIMAL(38," +
             std::to_string(fn->decimals) + "))";
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
        if (!args[i]->const_item()) return refuse("in_list_not_constant");
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
      if (!is_string(a) || a->const_item()) return refuse("like_argument");
      if (a->collation.collation == &my_charset_bin)
        return refuse("like_on_binary_string");
      // LIKE compares characters: not with weights.
      if (str_mode(a) != StrMode::RAW && str_mode(a) != StrMode::PAD)
        return refuse_collation(a);
      if (!args[1]->const_item() || !is_string(args[1]->real_item()))
        return refuse("like_pattern_not_constant");
      // Trailing spaces are significant in LIKE, PAD SPACE or not.
      std::string s, p;
      if (expr(a, &s) || literal(args[1]->real_item(), &p)) return true;
      std::string escape;
      if (n > 2) {
        if (!args[2]->const_item()) return refuse("like_escape_not_constant");
        String buf;
        String *e = args[2]->val_str(&buf);
        if (e != nullptr && e->length() > 0) {
          if (e->length() != 1) return refuse("like_escape");
          const char c = e->ptr()[0];
          escape = c == '\'' ? " ESCAPE ''''" : std::string(" ESCAPE '") + c +
                                                    "'";
        }
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
                              out);
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
        return refuse("decimal_precision_above_38");
      std::string l, r;
      if (numeric(args[0], &l) || numeric(args[1], &r)) return true;
      // Sign of the dividend in both; MySQL gives NULL for a zero divisor.
      *out = "(" + l + " % NULLIF(" + r + ", 0))";
      return false;
    }
    case Item_func::NEG_FUNC: {
      std::string a;
      if (numeric(args[0], &a)) return true;
      *out = "(-" + a + ")";
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
      if (!args[1]->const_item() || args[1]->result_type() != INT_RESULT)
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

bool Generator::expr(Item *item, std::string *out) {
  item = item->real_item();
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
      // A column of this block or, in a subquery, of an enclosing block (a
      // correlated reference: aliases are unique in the statement).
      const Block *owner = m_block;
      while (owner != nullptr && f->field != nullptr &&
             owner->tables.count(f->field->table) == 0)
        owner = owner->parent;
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
    case MYSQL_TYPE_YEAR:
      // YEAR() of a date is an integer, also as a column of a derived table
      // (cast to BIGINT by our SQL); a YEAR column is not translated.
      if (Item *r = item->real_item(); r->type() == Item::FIELD_ITEM) {
        const auto info = m_info.find(down_cast<Item_field *>(r)->field->table);
        if (info == m_info.end() || !info->second.derived)
          return refuse("unsupported_result_type");
      } else if (r->type() != Item::FUNC_ITEM) {
        return refuse("unsupported_result_type");
      }
      *kind = Kind::INT;
      *cast = "BIGINT";
      return false;
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_BIT:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_GEOMETRY:
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
      const unsigned scale = item->decimals;
      if (scale > 37) return refuse("decimal_scale_above_37");
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
    Block inner;
    Block *outer = m_block;
    const enum_field_types temporal = m_temporal;
    m_block = &inner;
    m_temporal = MYSQL_TYPE_NULL;
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
    *out = "(" + sql + ") AS " + ti.alias;
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

// A join list (stored in reverse order by the parser). Inner joins, CROSS
// JOIN and LEFT JOIN (RIGHT JOIN is already a LEFT JOIN) are printed as
// joins; semi-join nests (IN / EXISTS subqueries the optimizer flattened)
// become EXISTS and anti-join nests (NOT EXISTS, NOT IN) NOT EXISTS in the
// block's WHERE, so only at the top of the block.
bool Generator::from_list(mem_root_deque<Table_ref *> &list, bool under_outer,
                          bool in_semi, std::string *out) {
  std::string from;
  for (auto it = list.rbegin(); it != list.rend(); ++it) {
    Table_ref *e = *it;
    if (e->is_sj_or_aj_nest()) {
      if (under_outer || in_semi) return refuse("semijoin_not_at_top_level");
      Block::Semi s;
      s.nest = e;
      s.anti = e->is_aj_nest();
      if (from_list(e->nested_join->m_tables, false, true, &s.from))
        return true;
      if (s.from.empty()) return refuse("empty_semijoin_nest");
      m_block->semi.push_back(s);
      continue;
    }
    std::string item;
    if (e->nested_join != nullptr) {
      std::string nest;
      if (from_list(e->nested_join->m_tables, under_outer || e->outer_join,
                    in_semi, &nest))
        return true;
      if (nest.empty()) return refuse("empty_join_nest");
      item = "(" + nest + ")";
    } else if (table_ref(e, &item)) {
      return true;
    }
    std::string on;
    if (e->join_cond() != nullptr && expr(e->join_cond(), &on)) return true;
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
  out->push_back({s, cond->used_tables() & ~PSEUDO_TABLE_BITS});
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
                    t0 | (fields[i]->used_tables() & ~PSEUDO_TABLE_BITS)});
  }
  if (Item *c = eq->const_arg(); c != nullptr) {
    std::string cs;
    if (t == MYSQL_TYPE_TIMESTAMP || is_date_or_datetime(t)) {
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

  std::string from;
  if (from_sql != nullptr)
    from = *from_sql;
  else if (from_list(qb->m_table_nest, false, false, &from))
    return true;
  // A block without tables (SELECT 1, as a set operation operand) has no
  // FROM.
  if (from.empty() && qb->leaf_table_count != 0) return refuse("no_table");

  // WHERE, on rows before grouping. A semi-join's condition was merged into
  // WHERE: each AND part that uses a semi-join's tables goes into its EXISTS
  // (for a semi-join, a part on the outer tables alone is the same there or
  // in WHERE).
  table_map semi_tables = 0;
  for (const Block::Semi &s : m_block->semi)
    if (!s.anti) semi_tables |= s.nest->nested_join->used_tables;
  std::vector<std::string> where;
  std::vector<std::vector<std::string>> semi_conds(m_block->semi.size());
  if (qb->where_cond() != nullptr) {
    // Split in AND parts (in-to-exists conditions are left out).
    std::vector<Conj> parts;
    if (conjuncts(qb->where_cond(), semi_tables, &parts)) return true;
    for (const Conj &c : parts) {
      int hit = -1;
      for (size_t i = 0; i < m_block->semi.size(); i++) {
        const Block::Semi &s = m_block->semi[i];
        if (s.anti || (c.tables & s.nest->nested_join->used_tables) == 0)
          continue;
        if (hit >= 0) return refuse("condition_across_semijoins");
        hit = static_cast<int>(i);
      }
      if (hit < 0)
        where.push_back(c.sql);
      else
        semi_conds[hit].push_back(c.sql);
    }
  }
  for (size_t i = 0; i < m_block->semi.size(); i++) {
    const Block::Semi &s = m_block->semi[i];
    std::vector<std::string> conds = semi_conds[i];
    if (s.anti) {
      // An anti-join keeps its condition, as a LEFT JOIN's ON.
      std::string c;
      if (s.nest->join_cond() != nullptr && expr(s.nest->join_cond(), &c))
        return true;
      if (!c.empty()) conds.push_back(c);
    }
    std::string body = "SELECT 1 FROM " + s.from;
    for (size_t j = 0; j < conds.size(); j++)
      body += (j == 0 ? " WHERE " : " AND ") + conds[j];
    where.push_back(std::string(s.anti ? "NOT EXISTS (" : "EXISTS (") + body +
                    ")");
  }

  // GROUP BY. PAD SPACE strings group on rtrim(); a plain column of the
  // select list must be grouped (DuckDB has no ONLY_FULL_GROUP_BY off mode).
  std::string group;
  for (ORDER *o = qb->group_list.first; o != nullptr; o = o->next) {
    Item *g = (*o->item)->real_item();
    if (g->type() == Item::FIELD_ITEM)
      m_block->grouped.insert(down_cast<Item_field *>(g)->field);
    std::string e;
    if (is_string(g) && (str_mode(g) == StrMode::PAD ||
                         str_mode(g) == StrMode::WEIGHT)) {
      // Grouped on the equality key (rtrim / weight); the value shown is
      // decided by grouped_value().
      if (g->type() != Item::FIELD_ITEM)
        return refuse(std::string("group_by_expression (") +
                      g->collation.collation->m_coll_name + ")");
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
  const bool distinct = qb->is_distinct();
  std::string select;
  size_t col = 0;
  for (Item *item : qb->visible_fields()) {
    if (distinct && is_string(item) && str_mode(item) != StrMode::RAW)
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
    if (order_key(*o->item, &s)) return true;
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
    if (!qb->select_limit->const_item()) return refuse("limit_not_constant");
    *sql += " LIMIT " + std::to_string(qb->select_limit->val_uint());
  }
  if (qb->offset_limit != nullptr) {
    if (!qb->offset_limit->const_item()) return refuse("offset_not_constant");
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

// A value holder per select-list item, typed like the item, filled per row.
Item_cache *make_holder(Item *item, Kind kind) {
  Item_cache *h = nullptr;
  switch (kind) {
    case Kind::INT:
    case Kind::UINT:
      h = new Item_cache_int(item->data_type());
      break;
    case Kind::DOUBLE:
      h = new Item_cache_real();
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
    case Kind::INT: return DUCKDB_TYPE_BIGINT;
    case Kind::UINT: return DUCKDB_TYPE_UBIGINT;
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
      return false;
    case Kind::DOUBLE:
      down_cast<Item_cache_real *>(h)->store_value(
          not_null, static_cast<double *>(data)[row]);
      return false;
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

  if (con->execute(
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
    if (con->execute(
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

bool optimize(THD *thd, LEX *lex) {
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr) return true;
  ctx->restore_optimizer_switch();  // the server's optimization is done
  std::string reason;
  if (wrote_in_transaction(thd)) {
    reason = "written_in_transaction";
  } else {
    Generator gen(thd, &reason);
    ctx->kinds.clear();
    gen.generate(lex, &ctx->sql, &ctx->kinds);
  }
  // Freshness (duckdb_offload_wait_applied): the session's last commit must
  // be in DuckDB before we read it there.
  if (reason.empty() && offload_wait_applied(thd)) {
    BinlogPos target;
    if (freshness_target(thd, &target)) {
      reason = "binary_log_position_unknown";
    } else if (!target.empty()) {
      bool not_running = false;
      std::string err;
      if (BinlogReader::instance().wait_applied(target, thd, &not_running,
                                                &err, 5))
        reason = "applier_behind";
      else if (not_running)
        reason = "applier_not_running";
    }
  }
  // Prepared now: a DuckDB bind error (types MySQL accepts but DuckDB does
  // not) is a refusal, and the query runs on InnoDB.
  if (reason.empty()) {
    std::string err;
    ctx->con = Lake::instance().connect(thd, &err);
    if (!ctx->con) {
      reason = "duckdb_connection: " + err;
    } else if (ctx->con->execute(
                   [ctx](duckdb_connection c, std::string *e) {
                     if (duckdb_prepare(c, ctx->sql.c_str(), &ctx->stmt) ==
                         DuckDBError) {
                       const char *m = duckdb_prepare_error(ctx->stmt);
                       *e = m != nullptr ? m : "prepare failed";
                       duckdb_destroy_prepare(&ctx->stmt);
                       ctx->stmt = nullptr;
                       return true;
                     }
                     return false;
                   },
                   &err)) {
      reason = "duckdb_prepare: " + err.substr(0, 200);
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
  g_offloaded++;
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

std::string_view fail_reason(THD *thd) {
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr || ctx->reason.empty()) return {};
  return ctx->reason;
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

OffloadStatus offload_status() {
  OffloadStatus s;
  s.offloaded = g_offloaded;
  s.refused = g_refused;
  std::lock_guard<std::mutex> guard(g_reason_mutex);
  s.last_refusal = g_last_refusal;
  return s;
}

}  // namespace duckdb_se
