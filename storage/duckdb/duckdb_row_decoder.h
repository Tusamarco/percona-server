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

#ifndef STORAGE_DUCKDB_DUCKDB_ROW_DECODER_H_
#define STORAGE_DUCKDB_DUCKDB_ROW_DECODER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace duckdb_se {

/**
  A decoded column value, in the form we hand to DuckDB (same as the load):
  DATE as days since 1970-01-01, DATETIME and TIMESTAMP as microseconds since
  the epoch (TIMESTAMP in UTC), DECIMAL as text, text as UTF-8.
*/
struct Value {
  enum class Kind { NUL, INT, UINT, DOUBLE, TEXT, BLOB, DATE, TIMESTAMP };
  Kind kind{Kind::NUL};
  int64_t i{0};   // INT, DATE, TIMESTAMP
  uint64_t u{0};  // UINT
  double d{0};    // DOUBLE
  std::string s;  // TEXT, BLOB
  bool operator==(const Value &o) const;
  /** Readable form, for the debug log and the collapse key. */
  std::string to_string() const;
};

/** One column of a Table_map event (binlog_row_metadata=FULL). */
struct ColumnInfo {
  std::string name;
  unsigned int type{0};      // binlog column type (MYSQL_TYPE_*)
  unsigned int metadata{0};  // type metadata from the Table_map
  bool is_unsigned{false};
  unsigned int charset{0};  // collation id of string columns, 0 otherwise
};

struct TableInfo {
  std::string db;
  std::string table;
  std::vector<ColumnInfo> columns;
  std::vector<unsigned int> primary_key;  // column indexes
};

/** Binlog types that carry signedness information (numeric columns). */
bool type_has_signedness(unsigned int type);

/**
  Whether a column is a character column for the charset metadata: CHAR (not
  ENUM/SET), VARCHAR and BLOB/TEXT types.
*/
bool type_is_character(unsigned int type, unsigned int metadata);

/**
  Reads one type's metadata from the packed Table_map metadata at *pos and
  advances *pos. true on error (type not supported).
*/
bool read_column_metadata(unsigned int type, const unsigned char **pos,
                          const unsigned char *end, unsigned int *metadata,
                          std::string *err);

/**
  Decodes one row image at *pos. `present` is the image's column bitmap (with
  binlog_row_image=FULL every column is present). On success *pos points past
  the image. true on error.
*/
bool decode_row(const TableInfo &table, const std::vector<uint8_t> &present,
                const unsigned char **pos, const unsigned char *end,
                std::vector<Value> *row, std::string *err);

/** Days since 1970-01-01 for a civil date (proleptic Gregorian). */
int64_t days_from_civil(int64_t y, unsigned m, unsigned d);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_ROW_DECODER_H_
