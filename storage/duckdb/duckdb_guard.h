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

#ifndef STORAGE_DUCKDB_DUCKDB_GUARD_H_
#define STORAGE_DUCKDB_DUCKDB_GUARD_H_

#include "mysql/plugin.h"

class THD;
struct st_mysql_audit;

namespace duckdb_se {

/**
  DWL#7: the duckdb_guard audit plugin, in the same library as the engine.
  A statement that reads a history table under duckdb_history_scope=FULL
  must not run on InnoDB (which has only the live rows): the guard marks it
  secondary-only, or stops it with an error.
  It also lets the plugin's internal statements (refresh after DDL) pass
  read_only and super_read_only (pre-parse event, is_internal_session()).
*/
extern st_mysql_audit guard_descriptor;
int guard_init(MYSQL_PLUGIN plugin);
int guard_deinit(MYSQL_PLUGIN plugin);

/** The guard plugin is active: FULL scope is allowed only then. */
bool guard_active();

/** Forgets a connection's state (close_connection). */
void guard_forget(THD *thd);

struct GuardStatus {
  unsigned long long forced{0};    // statements sent to DuckDB only
  unsigned long long rejected{0};  // statements stopped with an error
};
GuardStatus guard_status();

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_GUARD_H_
