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

/*
  duckq: runs SQL statements on a DuckDB database with the extensions the
  plugin uses (JSON, ICU, core functions; DuckLake and Parquet for lakes of
  versions before 2026-11-25) registered, to inspect the lake file while the
  server is stopped (DuckDB locks lake.duckdb).

  Build (release or debug build directory B):
    L=$B/extra/duckdb/install/lib
    clang++-21 -x c duckq.c -x none -I$B/extra/duckdb/install/include -o duckq \
      -Wl,--start-group $(ls $L/lib*.a) -Wl,--end-group -lpthread -ldl

  Example (read only):
    ./duckq :memory: \
      "ATTACH '<lake>/lake.duckdb' AS lake (READ_ONLY)" \
      "SELECT column_name FROM duckdb_columns() WHERE database_name = 'lake' AND table_name = 'dim'"
*/
#include <stdio.h>
#include "duckdb.h"
#include "duckdb_static_extension.h"
int32_t duckdb_extension_core_functions_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_parquet_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_json_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_icu_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_ducklake_describe(duckdb_extension_descriptor *);
int main(int argc, char **argv) {
  duckdb_database db; duckdb_connection con;
  duckdb_register_static_extension(duckdb_extension_core_functions_describe);
  duckdb_register_static_extension(duckdb_extension_parquet_describe);
  duckdb_register_static_extension(duckdb_extension_json_describe);
  duckdb_register_static_extension(duckdb_extension_icu_describe);
  duckdb_register_static_extension(duckdb_extension_ducklake_describe);
  if (argc < 3 || duckdb_open(argv[1], &db) == DuckDBError) { fprintf(stderr, "cannot open\n"); return 1; }
  duckdb_connect(db, &con);
  for (int a = 2; a < argc; a++) {
    duckdb_result r;
    if (duckdb_query(con, argv[a], &r) == DuckDBError) { fprintf(stderr, "ERROR: %s\n", duckdb_result_error(&r)); duckdb_destroy_result(&r); return 1; }
    for (idx_t i = 0; i < duckdb_row_count(&r); i++) {
      for (idx_t c = 0; c < duckdb_column_count(&r); c++) { char *v = duckdb_value_varchar(&r, c, i); printf("%s%s", c ? "\t" : "", v ? v : "NULL"); duckdb_free(v); }
      printf("\n");
    }
    duckdb_destroy_result(&r);
  }
  duckdb_disconnect(&con); duckdb_close(&db); return 0;
}
