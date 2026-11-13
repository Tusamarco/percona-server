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

#ifndef STORAGE_DUCKDB_DUCKDB_LOAD_H_
#define STORAGE_DUCKDB_DUCKDB_LOAD_H_

#include <string>
#include <utility>
#include <vector>

#include "storage/duckdb/duckdb_ddl.h"  // ColumnRenames
#include "storage/duckdb/duckdb_lake.h"

class THD;
struct TABLE;

namespace duckdb_se {

/**
  Makes the next SECONDARY_LOAD of a table already in the lake a refresh
  (DWL#4 step 4d): the copy is rebuilt from InnoDB in one DuckLake
  transaction, archived rows of history tables are kept (converted to the new
  column types, renamed columns followed), and the registry gets the new load
  position. Set by the binlog reader before it runs the internal
  SECONDARY_LOAD; the load consumes it.
*/
void request_refresh(const std::string &db, const std::string &table,
                     const ColumnRenames &renames);
/** Drops a request the load did not consume (the load failed early). */
void cancel_refresh(const std::string &db, const std::string &table);

/**
  RENAME TABLE of a loaded table (DWL#4 step 4d): renames the lake table (or
  copies it to another schema) and its registry row, under apply_mutex.
  true on error.
*/
bool rename_in_lake(Lake::Connection &con, const std::string &db,
                    const std::string &table, const std::string &new_db,
                    const std::string &new_table, std::string *err);

/** Marks a copy stale in the lake and in the registry, under apply_mutex. */
bool mark_stale_in_lake(Lake::Connection &con, const std::string &db,
                        const std::string &table, std::string *err);

/**
  Marks a copy as waiting for a refresh after a later RENAME (state
  'refresh_pending', kept across restarts, with the column renames to apply
  then), under apply_mutex.
*/
bool mark_refresh_pending_in_lake(Lake::Connection &con, const std::string &db,
                                  const std::string &table,
                                  const ColumnRenames &renames,
                                  std::string *err);

/** duckdb_history_check_only and duckdb_unload_history_confirm of the
    session (ha_duckdb.cc, DWL#3 step 3b). */
bool history_check_only(THD *thd);
bool unload_history_confirmed(THD *thd);
/** duckdb_history_scope=FULL in this session (DWL#5; read by DWL#6). */
bool history_scope_full(THD *thd);

/**
  SET GLOBAL duckdb_discard_stale_copy = 'db.table' (DWL#3 step 3b): drops a
  stale copy whose MySQL table no longer exists. History copies require
  duckdb_unload_history_confirm=ON. Reports errors with my_error. true on
  error.
*/
bool discard_stale_copy(THD *thd, const std::string &spec);

/**
  Loads a table, opened and locked by the server for SECONDARY_LOAD, into the
  lake in one DuckLake transaction. Reports errors with my_error.
  @retval true on error
*/
bool load_table_into_lake(THD *thd, const TABLE &table);

/**
  Unloads a table from the lake. explicit_unload is true for SECONDARY_UNLOAD
  and false when DDL (ALTER, DROP, partition operations) unloads the table.
  @retval true on error
*/
bool unload_table_from_lake(THD *thd, const char *db, const char *table,
                            bool explicit_unload);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_LOAD_H_
