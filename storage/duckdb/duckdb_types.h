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

#ifndef STORAGE_DUCKDB_DUCKDB_TYPES_H_
#define STORAGE_DUCKDB_DUCKDB_TYPES_H_

#include <cstdint>
#include <string>

#include "duckdb.h"
#include "mysql_time.h"

class Field;

namespace duckdb_se {

/** Identifier in double quotes, safe for DuckDB SQL ("date", "time", ...). */
/**
  A MySQL TIME as signed microseconds, as the lake stores it (TIME spans
  -838:59:59.999999..838:59:59.999999; DuckDB's TIME only one day).
*/
inline int64_t time_to_micros(const MYSQL_TIME &t) {
  const int64_t v =
      ((static_cast<int64_t>(t.hour) * 60 + t.minute) * 60 + t.second) *
          1000000 +
      static_cast<int64_t>(t.second_part);
  return t.neg ? -v : v;
}

/** The reverse of time_to_micros (a MYSQL_TIMESTAMP_TIME value). */
inline void micros_to_time(int64_t micros, MYSQL_TIME *t) {
  *t = MYSQL_TIME{};
  t->time_type = MYSQL_TIMESTAMP_TIME;
  t->neg = micros < 0;
  uint64_t v = t->neg ? static_cast<uint64_t>(-micros)
                      : static_cast<uint64_t>(micros);
  t->second_part = static_cast<unsigned long>(v % 1000000);
  v /= 1000000;
  t->second = static_cast<unsigned>(v % 60);
  v /= 60;
  t->minute = static_cast<unsigned>(v % 60);
  t->hour = static_cast<unsigned>(v / 60);
}

/**
  ENUM and SET columns are stored as their string value (VARCHAR, with a
  weight column for collations DuckDB cannot compare) plus a hidden column
  "__n_<column>": the ENUM index (USMALLINT, 0 for the invalid value '') or
  the SET bitmask (UBIGINT). MySQL uses the string in comparisons, MIN/MAX,
  LIKE, and the number in ORDER BY and numeric contexts.
*/
constexpr const char *kIndexPrefix = "__n_";

/** The ENUM or SET field's hidden index column type; "" for other fields. */
std::string index_column_type(const Field &field);

/**
  SQL computing the index column of an ENUM / SET field from its stored
  value (value_sql, UTF-8), with the field's current definition: used for
  archived rows when a history copy is rebuilt (the definition may have
  changed). A value not in the definition gives 0. true on error.
*/
bool index_from_value_sql(const Field &field, const std::string &value_sql,
                          std::string *out, std::string *err);

/**
  SQL rebuilding a SET value (value_sql, UTF-8) in the field's current
  element order, keeping only current elements, as MySQL writes it
  ('x,z' becomes 'z,x' after SET('z','y','x')). For archived rows of a
  rebuilt history copy. true on error.
*/
bool set_value_from_value_sql(const Field &field, const std::string &value_sql,
                              std::string *out, std::string *err);

std::string quote_ident(const std::string &name);
/** String literal in single quotes for DuckDB SQL. */
std::string quote_literal(const std::string &value);

/**
  DuckDB column type for a MySQL column (DWL#3, Type mapping). Returns an empty
  string and sets *err when the column type is not supported.
*/
std::string duckdb_column_type(const Field &field, std::string *err);

/*
  Data chunks. The functions below only write memory (no DuckDB planning), so
  they are safe on a MySQL connection thread; the chunk is appended on an
  executor thread.
*/

/** Vector type we fill for a column; the appender casts it to the column type. */
duckdb_type chunk_type(const Field &field);

/** Writes the field's current value into row `row` of `vec`. true on error. */
bool store_field(duckdb_vector vec, idx_t row, Field &field, std::string *err);

/** Marks row `row` of `vec` as NULL. */
void store_null(duckdb_vector vec, idx_t row);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_TYPES_H_
