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

#ifndef STORAGE_DUCKDB_DUCKDB_DDL_H_
#define STORAGE_DUCKDB_DUCKDB_DDL_H_

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace duckdb_se {

/** Column renames of a refresh: (old name, new name), in order. */
using ColumnRenames = std::vector<std::pair<std::string, std::string>>;

/** A table named by a statement, with the default database applied. */
struct TableName {
  std::string db;
  std::string table;
  bool operator==(const TableName &o) const {
    return db == o.db && table == o.table;
  }
};

/**
  What a DDL statement from the binary log does to the tables it names, as far
  as the DuckDB copies are concerned (DWL#4 step 4d). We do not need the full
  MySQL grammar: only which tables change shape or data, which are renamed,
  and which columns are renamed (to keep archived rows of history tables).
*/
struct DdlStatement {
  /** Tables whose copy must be rebuilt from InnoDB (after the renames). */
  std::vector<TableName> refresh;
  /** Table renames, in statement order: RENAME TABLE, ALTER ... RENAME TO. */
  std::vector<std::pair<TableName, TableName>> renames;
  /** Column renames of the refreshed table (old name, new name). */
  ColumnRenames column_renames;

  bool empty() const { return refresh.empty() && renames.empty(); }
};

/**
  Reads a query event's statement. Statements that do not concern table copies
  (DML, BEGIN, CREATE TABLE, DROP TABLE, which the server already sends to the
  plugin, SECONDARY_LOAD / SECONDARY_UNLOAD, index-only changes) give an empty
  result. Unknown ALTER TABLE clauses count as changes (a needless refresh is
  safe, a missed one is not). lower_case_names: lower_case_table_names is not
  0, names are compared in lower case.
*/
DdlStatement parse_ddl(std::string_view query, const std::string &default_db,
                       bool lower_case_names);

/**
  The renames of `first` followed by those of `then`, as one mapping from the
  names before `first` (a->b then b->c gives a->c). A refresh that waits for a
  later RENAME collects the column renames of every DDL until then.
*/
ColumnRenames compose_renames(const ColumnRenames &first,
                              const ColumnRenames &then);

/** Renames as text, for lake.__mysql.tables.pending_renames, and back. */
std::string encode_renames(const ColumnRenames &renames);
ColumnRenames decode_renames(std::string_view text);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_DDL_H_
