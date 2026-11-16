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

#include "storage/duckdb/duckdb_types.h"

#include <cstring>
#include <string>

#include "my_temporal.h"
#include "my_time_t.h"
#include "mysql/strings/m_ctype.h"
#include "sql/field.h"
#include "sql_string.h"
#include "template_utils.h"

namespace duckdb_se {

namespace {

// Bytes of these character sets are already valid UTF-8.
bool is_utf8_compatible(const CHARSET_INFO *cs) {
  return strncmp(cs->csname, "utf8mb", 6) == 0 ||
         strcmp(cs->csname, "ascii") == 0;
}

std::string column_error(const Field &field, const std::string &what) {
  return std::string("column ") + field.field_name + ": " + what;
}

}  // namespace

std::string quote_ident(const std::string &name) {
  std::string out = "\"";
  for (char c : name) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

std::string quote_literal(const std::string &value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') out += '\'';
    out += c;
  }
  return out + "'";
}

std::string index_column_type(const Field &field) {
  switch (field.real_type()) {
    case MYSQL_TYPE_ENUM:
      return "USMALLINT";
    case MYSQL_TYPE_SET:
      return "UBIGINT";
    default:
      return "";
  }
}

bool index_from_value_sql(const Field &field, const std::string &value_sql,
                          std::string *out, std::string *err) {
  const bool is_set = field.real_type() == MYSQL_TYPE_SET;
  if (!is_set && field.real_type() != MYSQL_TYPE_ENUM) {
    *err = column_error(field, "not an ENUM or SET column");
    return true;
  }
  const TYPELIB *lib = down_cast<const Field_enum &>(field).typelib;
  std::string sum;
  for (unsigned i = 0; i < lib->count; i++) {
    // The element in UTF-8, as values are stored.
    String utf8;
    uint errors = 0;
    if (utf8.copy(lib->type_names[i], lib->type_lengths[i], field.charset(),
                  &my_charset_utf8mb4_bin, &errors) ||
        errors != 0) {
      *err = column_error(field, "element cannot be converted to UTF-8");
      return true;
    }
    const std::string elem =
        quote_literal(std::string(utf8.ptr(), utf8.length()));
    if (is_set)
      sum += (sum.empty() ? "" : " + ") + std::string("CASE WHEN list_contains("
             "string_split(") + value_sql + ", ','), " + elem + ") THEN " +
             std::to_string(1ULL << i) + "::UBIGINT ELSE 0::UBIGINT END";
    else
      sum += " WHEN " + elem + " THEN " + std::to_string(i + 1);
  }
  if (is_set)
    *out = "CASE WHEN " + value_sql + " IS NULL THEN NULL ELSE " +
           (sum.empty() ? std::string("0::UBIGINT") : "(" + sum + ")") +
           " END";
  else
    *out = "CASE WHEN " + value_sql + " IS NULL THEN NULL ELSE CAST(CASE " +
           value_sql + sum + " ELSE 0 END AS USMALLINT) END";
  return false;
}

bool set_value_from_value_sql(const Field &field, const std::string &value_sql,
                              std::string *out, std::string *err) {
  if (field.real_type() != MYSQL_TYPE_SET) {
    *err = column_error(field, "not a SET column");
    return true;
  }
  const TYPELIB *lib = down_cast<const Field_enum &>(field).typelib;
  std::string parts;
  for (unsigned i = 0; i < lib->count; i++) {
    String utf8;
    uint errors = 0;
    if (utf8.copy(lib->type_names[i], lib->type_lengths[i], field.charset(),
                  &my_charset_utf8mb4_bin, &errors) ||
        errors != 0) {
      *err = column_error(field, "element cannot be converted to UTF-8");
      return true;
    }
    const std::string elem =
        quote_literal(std::string(utf8.ptr(), utf8.length()));
    parts += ", CASE WHEN list_contains(string_split(" + value_sql +
             ", ','), " + elem + ") THEN " + elem + " END";
  }
  *out = "CASE WHEN " + value_sql + " IS NULL THEN NULL ELSE " +
         (parts.empty() ? std::string("''") : "concat_ws(','" + parts + ")") +
         " END";
  return false;
}

std::string duckdb_column_type(const Field &field, std::string *err) {
  const bool uns = field.is_unsigned();
  switch (field.real_type()) {
    case MYSQL_TYPE_TINY:
      return uns ? "UTINYINT" : "TINYINT";
    case MYSQL_TYPE_SHORT:
      return uns ? "USMALLINT" : "SMALLINT";
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
      return uns ? "UINTEGER" : "INTEGER";
    case MYSQL_TYPE_LONGLONG:
      return uns ? "UBIGINT" : "BIGINT";
    case MYSQL_TYPE_YEAR:
      // The year number: 0 (0000) or 1901..2155.
      return "USMALLINT";
    case MYSQL_TYPE_BIT:
      // BIT(1..64) as its unsigned value.
      return "UBIGINT";
    case MYSQL_TYPE_TIME2:
      // Signed microseconds: MySQL TIME spans -838:59:59..838:59:59, DuckDB
      // TIME only 00:00:00..24:00:00.
      return "BIGINT";
    case MYSQL_TYPE_NEWDECIMAL: {
      const auto &d = down_cast<const Field_new_decimal &>(field);
      if (d.precision > 38) {
        *err =
            column_error(field, "DECIMAL precision above 38 is not supported");
        return "";
      }
      return "DECIMAL(" + std::to_string(d.precision) + "," +
             std::to_string(field.decimals()) + ")";
    }
    case MYSQL_TYPE_FLOAT:
      return "FLOAT";
    case MYSQL_TYPE_DOUBLE:
      return "DOUBLE";
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      // The value; the index is in the hidden column (index_column_type).
      return "VARCHAR";
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BLOB:
      // Binary strings keep their bytes; text in any character set is stored
      // as UTF-8. Collation semantics are enforced at query time (DWL#6).
      return field.charset() == &my_charset_bin ? "BLOB" : "VARCHAR";
    case MYSQL_TYPE_NEWDATE:
      return "DATE";
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2:
      return "TIMESTAMP";
    default:
      *err = column_error(field,
                          "type not supported by the DuckDB secondary engine "
                          "in this version");
      return "";
  }
}

duckdb_type chunk_type(const Field &field) {
  switch (field.real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      return field.is_unsigned() ? DUCKDB_TYPE_UBIGINT : DUCKDB_TYPE_BIGINT;
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_BIT:
      return DUCKDB_TYPE_UBIGINT;
    case MYSQL_TYPE_TIME2:
      return DUCKDB_TYPE_BIGINT;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      return DUCKDB_TYPE_DOUBLE;
    case MYSQL_TYPE_NEWDATE:
      return DUCKDB_TYPE_DATE;
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2:
      return DUCKDB_TYPE_TIMESTAMP;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
      return DUCKDB_TYPE_VARCHAR;
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BLOB:
      return field.charset() == &my_charset_bin ? DUCKDB_TYPE_BLOB
                                                : DUCKDB_TYPE_VARCHAR;
    default:
      // DECIMAL goes as text and the appender casts it to DECIMAL(p,s).
      return DUCKDB_TYPE_VARCHAR;
  }
}

void store_null(duckdb_vector vec, idx_t row) {
  duckdb_vector_ensure_validity_writable(vec);
  duckdb_validity_set_row_invalid(duckdb_vector_get_validity(vec), row);
}

bool store_field(duckdb_vector vec, idx_t row, Field &field,
                 std::string *err) {
  if (field.is_null()) {
    store_null(vec, row);
    return false;
  }
  void *data = duckdb_vector_get_data(vec);

  switch (field.real_type()) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG: {
      const longlong v = field.val_int();
      if (field.is_unsigned())
        static_cast<uint64_t *>(data)[row] = static_cast<uint64_t>(v);
      else
        static_cast<int64_t *>(data)[row] = v;
      return false;
    }
    case MYSQL_TYPE_YEAR:
      // Field_year::val_int(): 0 for 0000, else the year.
      static_cast<uint64_t *>(data)[row] =
          static_cast<uint64_t>(field.val_int());
      return false;
    case MYSQL_TYPE_BIT:
      // Field_bit::val_int(): the bits as an integer (up to 64).
      static_cast<uint64_t *>(data)[row] =
          static_cast<uint64_t>(field.val_int());
      return false;
    case MYSQL_TYPE_TIME2: {
      Time_val t;
      if (field.val_time(&t)) {
        *err = column_error(field, "invalid TIME value");
        return true;
      }
      static_cast<int64_t *>(data)[row] = t.to_microseconds();
      return false;
    }
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      static_cast<double *>(data)[row] = field.val_real();
      return false;
    case MYSQL_TYPE_NEWDECIMAL: {
      String buf;
      const String *s = field.val_str(&buf);
      duckdb_vector_assign_string_element_len(vec, row, s->ptr(), s->length());
      return false;
    }
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_BLOB: {
      // CHAR values come without trailing spaces, as MySQL returns them;
      // ENUM / SET give their string value (Field_enum::val_str).
      String buf;
      const String *s = field.val_str(&buf);
      if (field.charset() == &my_charset_bin ||
          is_utf8_compatible(field.charset())) {
        duckdb_vector_assign_string_element_len(vec, row, s->ptr(),
                                                s->length());
        return false;
      }
      // Other character sets: we convert to UTF-8 with the server's own
      // conversion; values convert back exactly when results are sent.
      String utf8;
      uint errors = 0;
      if (utf8.copy(s->ptr(), s->length(), s->charset(),
                    &my_charset_utf8mb4_bin, &errors) ||
          errors != 0) {
        *err = column_error(field, "value cannot be converted to UTF-8");
        return true;
      }
      duckdb_vector_assign_string_element_len(vec, row, utf8.ptr(),
                                              utf8.length());
      return false;
    }
    case MYSQL_TYPE_NEWDATE: {
      Date_val d;
      if (field.val_date(&d, 0) || d.is_zero_date() || d.month() == 0 ||
          d.day() == 0) {
        *err = column_error(field, "zero dates cannot be loaded into DuckDB");
        return true;
      }
      duckdb_date_struct ds{static_cast<int32_t>(d.year()),
                            static_cast<int8_t>(d.month()),
                            static_cast<int8_t>(d.day())};
      static_cast<duckdb_date *>(data)[row] = duckdb_to_date(ds);
      return false;
    }
    case MYSQL_TYPE_DATETIME2: {
      Datetime_val dt;
      if (field.val_datetime(&dt, 0) || dt.month == 0 || dt.day == 0) {
        *err = column_error(field, "zero dates cannot be loaded into DuckDB");
        return true;
      }
      duckdb_timestamp_struct ts{
          {static_cast<int32_t>(dt.year), static_cast<int8_t>(dt.month),
           static_cast<int8_t>(dt.day)},
          {static_cast<int8_t>(dt.hour), static_cast<int8_t>(dt.minute),
           static_cast<int8_t>(dt.second),
           static_cast<int32_t>(dt.second_part)}};
      static_cast<duckdb_timestamp *>(data)[row] = duckdb_to_timestamp(ts);
      return false;
    }
    case MYSQL_TYPE_TIMESTAMP2: {
      // TIMESTAMP is stored as UTC seconds; we keep it in UTC in DuckDB.
      my_timeval tv;
      int warnings = 0;
      if (field.get_timestamp(&tv, &warnings) ||
          (tv.m_tv_sec == 0 && tv.m_tv_usec == 0)) {
        *err =
            column_error(field, "zero timestamps cannot be loaded into DuckDB");
        return true;
      }
      static_cast<duckdb_timestamp *>(data)[row] =
          duckdb_timestamp{tv.m_tv_sec * 1000000 + tv.m_tv_usec};
      return false;
    }
    default:
      *err = column_error(field, "type not supported");
      return true;
  }
}

}  // namespace duckdb_se
