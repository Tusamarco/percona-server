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

#ifndef STORAGE_DUCKDB_DUCKDB_COLLATION_H_
#define STORAGE_DUCKDB_DUCKDB_COLLATION_H_

#include <string>

struct CHARSET_INFO;

namespace duckdb_se {

/**
  Weight strings (DWL#6 step A): string columns whose collation DuckDB cannot
  compare natively get a hidden column "__w_<column>" in the copy with
  MySQL's sort weight of each value (the bytes filesort compares), so that
  comparisons, grouping and ordering in DuckDB follow the MySQL collation.
*/
constexpr const char *kWeightPrefix = "__w_";

/**
  Whether values of this collation need a weight column: non-binary
  collations except the binary-sort utf8mb3/utf8mb4 ones (utf8mb4_bin,
  utf8mb3_bin, utf8mb4_0900_bin), which DuckDB compares on the UTF-8 bytes.
*/
bool needs_weight(const CHARSET_INFO *cs);

/**
  MySQL's weight of a value given in the collation's own character set. For
  a PAD SPACE collation the trailing spaces are left out (equal values have
  equal weights; ordering appends the space weight at query time); a NO PAD
  collation weighs the whole value. Weights compare as bytes (memcmp).
*/
std::string collation_weight(const CHARSET_INFO *cs, const char *s,
                             size_t len);

/** The weight of one space: the padding of a PAD SPACE collation, appended
    to weights to order them as padded values. */
std::string space_weight(const CHARSET_INFO *cs);

/** The weight of a UTF-8 value in a column of collation cs (archived rows
    read back from DuckDB). true if it cannot be converted. */
bool collation_weight_utf8(const CHARSET_INFO *cs, const std::string &utf8,
                           std::string *weight);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_COLLATION_H_
