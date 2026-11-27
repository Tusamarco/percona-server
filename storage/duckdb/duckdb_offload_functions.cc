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

// Query offload, functions: comparisons, arithmetic, temporal and DATE_FORMAT
// helpers, aggregates, Generator::function and Generator::cast.

#include "storage/duckdb/duckdb_offload_internal.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "my_time.h"
#include "sql/item_timefunc.h"  // Item_extract, Item_func_add_interval
#include "sql/mysqld.h"         // system_time_zone_dst_on / _off
#include "sql/sql_class.h"
#include "sql/sql_locale.h"  // MY_LOCALE (lc_time_names)
#include "sql/tztime.h"

namespace duckdb_se {
namespace offload {

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

bool is_date_or_datetime(enum_field_types t) {
  return t == MYSQL_TYPE_DATE || t == MYSQL_TYPE_NEWDATE ||
         t == MYSQL_TYPE_DATETIME;
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

namespace {

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

}  // namespace

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

namespace {

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

}  // namespace

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

namespace {

// The session time zone as a fixed offset from UTC, in seconds: UTC, an
// offset ('+02:00'), or SYSTEM when the server's system zone is UTC.
// false for zones with daylight saving (named zones, other system zones).
bool session_fixed_offset(THD *thd, long *seconds) {
  const Time_zone *tz = thd->variables.time_zone;
  if (tz == nullptr) return false;
  switch (tz->get_timezone_type()) {
    case Time_zone::TZ_UTC:
      *seconds = 0;
      return true;
    case Time_zone::TZ_OFFSET:
      *seconds = tz->get_timezone_offset();
      return true;
    case Time_zone::TZ_SYSTEM: {
      // The name alone is not enough: TZ=GMT-3 is named "GMT" and is three
      // hours east of UTC (MTR runs with it). The system zone must also be
      // at offset 0 in January and July (no daylight saving).
      if ((strcmp(system_time_zone_dst_off, "UTC") != 0 &&
           strcmp(system_time_zone_dst_off, "GMT") != 0) ||
          strcmp(system_time_zone_dst_on, system_time_zone_dst_off) != 0)
        return false;
      for (const time_t t : {static_cast<time_t>(1704067200),    // 2024-01-01
                             static_cast<time_t>(1719792000)}) {  // 2024-07-01
        struct tm local;
        if (localtime_r(&t, &local) == nullptr || local.tm_gmtoff != 0)
          return false;
      }
      *seconds = 0;
      return true;
    }
    default:
      return false;
  }
}

}  // namespace

// A TIMESTAMP column or expression (stored in UTC in DuckDB) as the value
// in the session time zone, which MySQL's date functions use: the UTC
// value plus the zone's fixed offset (T3 step 4, 2026-11-26). Zones with
// daylight saving are refused (the offset changes with the date; MySQL's
// zone tables and DuckDB's ICU data could also differ).
bool Generator::local_timestamp(Item *a, std::string *out) {
  long offset = 0;
  if (!session_fixed_offset(m_thd, &offset))
    return refuse("timestamp_function_time_zone");
  std::string e;
  if (expr(a, &e)) return true;
  *out = offset == 0 ? e
                     : "(" + e + " + INTERVAL (" + std::to_string(offset) +
                           ") SECOND)";
  return false;
}

// The argument of a date function: a DATE or DATETIME column or expression,
// or a TIMESTAMP one in the session time zone (local_timestamp()). Strings
// are refused.
bool Generator::temporal_arg(Item *item, std::string *out) {
  Item *a = item->real_item();
  if (a->data_type() == MYSQL_TYPE_TIMESTAMP && !is_constant(a))
    return local_timestamp(a, out);
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
      (a->data_type() == MYSQL_TYPE_TIMESTAMP && is_constant(a)) ||
      a->data_type() == MYSQL_TYPE_TIME || a->data_type() == MYSQL_TYPE_JSON)
    return refuse("cast_source:" + name);
  // A TIMESTAMP column casts as its value in the session time zone
  // (temporal_arg(), as a DATETIME of the same precision).
  const bool ts = a->data_type() == MYSQL_TYPE_TIMESTAMP;
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
        !((t == MYSQL_TYPE_DATETIME || ts) && a->decimals <= fn->decimals))
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
             ((t == MYSQL_TYPE_DATETIME || ts) && a->decimals == 0)) {
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

}  // namespace offload
}  // namespace duckdb_se
