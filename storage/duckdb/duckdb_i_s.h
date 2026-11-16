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

#ifndef STORAGE_DUCKDB_DUCKDB_I_S_H_
#define STORAGE_DUCKDB_DUCKDB_I_S_H_

#include "mysql/plugin.h"

struct st_mysql_information_schema;

namespace duckdb_se {

/**
  INFORMATION_SCHEMA.DUCKDB_TABLES: the tables in the lake, from the
  engine's registry (lake.__mysql.tables), one row per copy: loaded, stale
  (e.g. the history copy of a dropped table) or waiting for a refresh.
  Empty while the lake is closed (duckdb_enable=OFF). Requires the PROCESS
  privilege, as InnoDB's INFORMATION_SCHEMA tables.
*/
extern st_mysql_information_schema i_s_tables_descriptor;
int i_s_tables_init(void *p);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_I_S_H_
