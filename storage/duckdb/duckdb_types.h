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

#include <string>

#include "duckdb.h"

class Field;

namespace duckdb_se {

/** Identifier in double quotes, safe for DuckDB SQL ("date", "time", ...). */
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
