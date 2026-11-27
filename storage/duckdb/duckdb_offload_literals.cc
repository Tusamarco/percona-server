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

// Query offload, literals: constants of the statement printed as DuckDB
// literals (Generator::literal, typed_literal, value_literal, time_literal).

#include "storage/duckdb/duckdb_offload_internal.h"

#include <cstdio>
#include <string>

#include "my_time.h"
#include "sql-common/my_decimal.h"
#include "sql/sql_class.h"
#include "sql/tztime.h"

namespace duckdb_se {
namespace offload {

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

}  // namespace offload
}  // namespace duckdb_se
