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
#include <mutex>
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
#include "include/mem_root_deque.h"
#include "sql/opt_trace.h"
#include "sql/query_result.h"
#include "sql/replication.h"  // Trans_observer
#include "sql/sql_class.h"
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "sql/tztime.h"
#include "storage/duckdb/duckdb_binlog_reader.h"
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

int after_commit(Trans_param *param) {
  try {
    if (param->log_file == nullptr || *param->log_file == '\0') return 0;
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
  double best_cost{DBL_MAX};
  bool executed{false};
  std::unique_ptr<Lake::Connection> con;
  duckdb_prepared_statement stmt{nullptr};
};

OffloadContext *context(THD *thd) {
  return static_cast<OffloadContext *>(
      thd->lex->secondary_engine_execution_context());
}

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
// - REFUSE: any other collation (weight strings, a later step).
enum class StrMode { RAW, PAD, REFUSE };

StrMode str_mode(const Item *item) {
  const CHARSET_INFO *cs = item->collation.collation;
  if (cs == &my_charset_bin) return StrMode::RAW;
  if ((cs->state & MY_CS_BINSORT) != 0 && is_unicode_charset(cs))
    return cs->pad_attribute == NO_PAD ? StrMode::RAW : StrMode::PAD;
  return StrMode::REFUSE;
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
  bool temporal_arg(Item *item, std::string *out);
  bool order_key(Item *item, std::string *out);
  bool literal(Item *item, std::string *out);
  bool typed_literal(Item *item, enum_field_types type, std::string *out);
  bool output_kind(Item *item, Kind *kind, std::string *cast);

  THD *m_thd;
  std::string *m_reason;
  TABLE *m_table{nullptr};
  // While printing the operands of a comparison with a DATE, DATETIME or
  // TIMESTAMP column: constants are values of that type, as MySQL compares.
  enum_field_types m_temporal{MYSQL_TYPE_NULL};
  // Printing the select list, HAVING or ORDER BY of a grouped query.
  bool m_post_group{false};
  int m_agg_depth{0};
  // PAD SPACE string columns in GROUP BY: grouped on rtrim(); outside an
  // aggregate they are shown as the value of the row with the smallest
  // primary key, the row InnoDB meets first in the group.
  std::vector<const Field *> m_group_pad;
  std::string m_pk;
  // The primary key columns, and negated (empty if one is not a number).
  std::string m_pk_parts;
  std::string m_pk_negated;
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
      const StrMode mode = str_mode(a);
      if (mode == StrMode::REFUSE) return refuse_collation(a);
      if (mode == StrMode::PAD) pad = true;
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
    } else {
      failed = expr(a, &s);
    }
    if (!failed && pad && is_string(a))
      s = ordering ? pad_sort_key(s, max_bytes) : pad_equal_key(s);
    ops->push_back(s);
  }
  m_temporal = saved;
  return failed;
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
  if (is_string(e)) {
    const StrMode mode = str_mode(e);
    if (mode == StrMode::REFUSE) return refuse_collation(e);
    if (expr(e, &s)) return true;
    *out = mode == StrMode::PAD ? pad_sort_key(s, e->max_length) : s;
    return false;
  }
  return expr(e, out);
}

bool Generator::aggregate(Item_sum *s, std::string *out) {
  if (s->m_is_window_function) return refuse("window_function");
  if (m_agg_depth > 0) return refuse("nested_aggregate");
  const Item_sum::Sumfunctype f = s->sum_func();
  if (s->argument_count() != 1) return refuse("aggregate_arguments");
  Item *arg = s->get_arg(0)->real_item();
  m_agg_depth++;
  struct Leave {
    int &d;
    ~Leave() { d--; }
  } leave{m_agg_depth};
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
        const StrMode mode = str_mode(arg);
        if (mode == StrMode::REFUSE) return refuse_collation(arg);
        if (expr(arg, &a)) return true;
        if (mode == StrMode::PAD) a = pad_equal_key(a);
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
      *out = "(CAST(sum(" + d + a + ") AS DECIMAL(38," +
             std::to_string(s->decimals) + ")) / NULLIF(count(" + d + a +
             "), 0))";
      return false;
    }
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC: {
      const char *fn = f == Item_sum::MIN_FUNC ? "min" : "max";
      if (is_string(arg) && str_mode(arg) == StrMode::PAD) {
        // Values equal under PAD SPACE ('' and ' ') tie: MySQL keeps the
        // first one met, the smallest primary key in a scan. The tie is
        // broken on the primary key (negated for MAX).
        if (m_pk_negated.empty()) return refuse("padded_min_max_key");
        std::string key;
        if (expr(arg, &a) || order_key(arg, &key)) return true;
        const bool min = f == Item_sum::MIN_FUNC;
        *out = std::string(min ? "arg_min(" : "arg_max(") + a + ", row(" +
               key + ", " + (min ? m_pk_parts : m_pk_negated) +
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
      auto *eq = down_cast<Item_multi_eq *>(fn);
      std::vector<Item *> fields;
      for (Item_field &f : eq->get_fields()) fields.push_back(&f);
      if (fields.empty()) return refuse("empty_multiple_equality");
      Item *first = fields[0];
      const bool str = is_string(first);
      if (str && str_mode(first) == StrMode::REFUSE)
        return refuse_collation(first);
      const bool pad = str && str_mode(first) == StrMode::PAD;
      const enum_field_types t = first->data_type();
      auto print = [&](Item *it, std::string *s) {
        if (expr(it, s)) return true;
        if (pad) *s = pad_equal_key(*s);
        return false;
      };
      std::string f0, all;
      if (print(first, &f0)) return true;
      for (size_t i = 1; i < fields.size(); i++) {
        if (fields[i]->data_type() != t ||
            (is_string(fields[i]) && str_mode(fields[i]) != str_mode(first)))
          return refuse("multiple_equality_types");
        std::string a;
        if (print(fields[i], &a)) return true;
        all += (all.empty() ? "" : " AND ") + ("(" + f0 + " = " + a + ")");
      }
      if (Item *c = eq->const_arg(); c != nullptr) {
        std::string cs;
        if (t == MYSQL_TYPE_TIMESTAMP || is_date_or_datetime(t)) {
          if (typed_literal(c, t, &cs)) return true;
        } else if (str != is_string(c)) {
          return refuse("multiple_equality_types");
        } else if (expr(c, &cs)) {
          return true;
        }
        if (pad) cs = pad_equal_key(cs);
        all += (all.empty() ? "" : " AND ") + ("(" + f0 + " = " + cs + ")");
      }
      *out = all.empty() ? "TRUE" : "(" + all + ")";
      return false;
    }
    case Item_func::LIKE_FUNC: {
      Item *a = args[0]->real_item();
      if (!is_string(a) || a->const_item()) return refuse("like_argument");
      if (a->collation.collation == &my_charset_bin)
        return refuse("like_on_binary_string");
      if (str_mode(a) == StrMode::REFUSE) return refuse_collation(a);
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
      // MySQL divides integers and decimals exactly (DECIMAL result); DuckDB
      // returns DOUBLE: only the DOUBLE case is the same.
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
  if (item->has_subquery()) return refuse("subquery");
  switch (item->type()) {
    case Item::FIELD_ITEM: {
      auto *f = down_cast<Item_field *>(item);
      if (f->field == nullptr || f->field->table != m_table)
        return refuse("outer_or_other_table_column");
      if (f->field->is_flag_set(NOT_SECONDARY_FLAG))
        return refuse(std::string("not_secondary_column:") + f->field_name);
      const std::string col = "t." + quote_ident(f->field->field_name);
      bool group_pad = false;
      for (const Field *g : m_group_pad) group_pad = group_pad || g == f->field;
      *out = m_post_group && m_agg_depth == 0 && group_pad
                 ? "arg_min(" + col + ", " + m_pk + ")"
                 : col;
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
      // YEAR() of a date is an integer; a YEAR column is not translated.
      if (item->type() != Item::FUNC_ITEM) return refuse("unsupported_result_type");
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

bool Generator::generate(LEX *lex, std::string *sql,
                         std::vector<Kind> *kinds) {
  if (lex->sql_command != SQLCOM_SELECT)
    return refuse("unsupported_statement");
  Query_expression *unit = lex->unit;
  if (!unit->is_simple()) return refuse("set_operation");
  Query_block *qb = unit->first_query_block();
  if (qb->leaf_table_count != 1 || qb->leaf_tables == nullptr)
    return refuse("not_single_table");
  Table_ref *tr = qb->leaf_tables;
  if (!tr->is_base_table() || tr->table == nullptr)
    return refuse("not_a_base_table");
  m_table = tr->table;
  const std::string db = m_table->s->db.str;
  const std::string name = m_table->s->table_name.str;
  TableState state;
  if (!Registry::instance().snapshot(db, name, &state) || !state.usable())
    return refuse("not_loaded:" + db + "." + name);
  if (qb->active_options() & OPTION_FOUND_ROWS)
    return refuse("sql_calc_found_rows");
  if (qb->olap != UNSPECIFIED_OLAP_TYPE) return refuse("rollup");
  if (qb->has_windows()) return refuse("window");
  if (m_table->s->primary_key == MAX_KEY) return refuse("no_primary_key");
  {
    const KEY &pk = m_table->key_info[m_table->s->primary_key];
    std::string cols, negated;
    bool numeric_key = true;
    for (unsigned i = 0; i < pk.user_defined_key_parts; i++) {
      const Field *f = pk.key_part[i].field;
      const std::string c = "t." + quote_ident(f->field_name);
      cols += (i ? ", " : "") + c;
      // HUGEINT: negating an unsigned BIGINT stays unsigned in DuckDB.
      negated += (i ? ", " : "") + ("-CAST(" + c + " AS HUGEINT)");
      numeric_key = numeric_key && f->result_type() == INT_RESULT;
    }
    m_pk = pk.user_defined_key_parts == 1 ? cols : "row(" + cols + ")";
    m_pk_parts = cols;
    if (numeric_key) m_pk_negated = negated;
  }

  // History scope (DWL#5): LIVE hides archived rows; FULL shows them and
  // needs DUCKDB_HISTORY_READER at query time too (a session can inherit
  // FULL from the global value).
  const bool full = history_scope_full(m_thd);
  if (full && !m_thd->security_context()
                   ->has_global_grant(STRING_WITH_LEN("DUCKDB_HISTORY_READER"))
                   .first)
    return refuse("history_scope_full_without_privilege");

  // WHERE, on rows before grouping.
  std::string where = state.retain_history && !full
                          ? std::string("t.\"__archived_at\" IS NULL")
                          : std::string();
  if (qb->where_cond() != nullptr) {
    std::string w;
    if (expr(qb->where_cond(), &w)) return true;
    where += (where.empty() ? "" : " AND ") + w;
  }

  // GROUP BY. PAD SPACE strings group on rtrim(); a plain column of the
  // select list must be grouped (DuckDB has no ONLY_FULL_GROUP_BY off mode).
  std::string group;
  for (ORDER *o = qb->group_list.first; o != nullptr; o = o->next) {
    Item *g = (*o->item)->real_item();
    std::string e;
    if (expr(g, &e)) return true;
    if (is_string(g)) {
      const StrMode mode = str_mode(g);
      if (mode == StrMode::REFUSE) return refuse_collation(g);
      if (mode == StrMode::PAD) {
        if (g->type() != Item::FIELD_ITEM)
          return refuse("group_by_padded_expression");
        m_group_pad.push_back(down_cast<Item_field *>(g)->field);
        e = pad_equal_key(e);
      }
    }
    group += (group.empty() ? "" : ", ") + e;
  }
  m_post_group = !group.empty();

  // Select list, each value cast to the type we read back.
  const bool distinct = qb->is_distinct();
  std::string select;
  for (Item *item : qb->visible_fields()) {
    if (distinct && is_string(item) && str_mode(item) != StrMode::RAW)
      return str_mode(item) == StrMode::REFUSE
                 ? refuse_collation(item)
                 : refuse("distinct_padded_string");
    std::string e, cast;
    Kind kind;
    if (output_kind(item, &kind, &cast) || expr(item, &e)) return true;
    select += (select.empty() ? "" : ", ") +
              (kind == Kind::DECIMAL ? "CAST(CAST(" + e + " AS " + cast + ")"
                                     : "CAST(" + e + " AS " + cast + ")");
    kinds->push_back(kind);
  }

  std::string having;
  if (qb->having_cond() != nullptr && expr(qb->having_cond(), &having))
    return true;

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
         " FROM lake." + quote_ident(db) + "." + quote_ident(name) + " AS t";
  if (!where.empty()) *sql += " WHERE " + where;
  if (!group.empty()) *sql += " GROUP BY " + group;
  if (!having.empty()) *sql += " HAVING " + having;
  if (!order.empty()) *sql += " ORDER BY " + order;
  if (qb->select_limit != nullptr) {
    if (!qb->select_limit->const_item()) return refuse("limit_not_constant");
    *sql += " LIMIT " + std::to_string(qb->select_limit->val_uint());
  }
  if (qb->offset_limit != nullptr) {
    if (!qb->offset_limit->const_item()) return refuse("offset_not_constant");
    *sql += " OFFSET " + std::to_string(qb->offset_limit->val_uint());
  }
  return false;
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

// JOIN::override_executor_func: runs the whole statement in DuckDB and sends
// the rows. Called once per query block; single-table queries have one.
bool execute_offloaded(JOIN *join, Query_result *result) {
  THD *thd = join->thd;
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr || ctx->executed) return false;
  ctx->executed = true;

  mem_root_deque<Item *> holders(thd->mem_root);
  std::vector<Item_cache *> caches;
  // Typed like the items the server sent as metadata (join->fields: with
  // GROUP BY these can be temporary table columns, e.g. BIGINT for an INT
  // expression), so each value is sent as the client was told.
  size_t i = 0;
  for (Item *item : *join->fields) {
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
    if (!thd->is_error())
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               ("DuckDB: " + msg).c_str());
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
bool pre_prepare(THD *thd) {
  if (!Lake::instance().is_open()) return false;
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
  lex->unit->first_query_block()->join->override_executor_func =
      execute_offloaded;
  g_offloaded++;
  Opt_trace_context *const trace = &thd->opt_trace;
  if (trace->is_started()) {
    const Opt_trace_object wrapper(trace);
    Opt_trace_object oto(trace, "duckdb_offload");
    oto.add_utf8("sql", ctx->sql.c_str());
  }
  return false;
}

bool compare_cost(THD *thd, const JOIN &, double optimizer_cost,
                  bool *use_best_so_far, bool *cheaper,
                  double *secondary_engine_cost) {
  *use_best_so_far = false;
  *secondary_engine_cost = optimizer_cost;
  OffloadContext *ctx = context(thd);
  *cheaper = ctx == nullptr || optimizer_cost < ctx->best_cost;
  if (ctx != nullptr && *cheaper) ctx->best_cost = optimizer_cost;
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
