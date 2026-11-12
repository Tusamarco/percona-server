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
  Build smoke test for the DuckDB secondary engine dependencies.

  We check that the libraries built by cmake/duckdb.cmake link into a program
  compiled with the server's C++ standard and work: static extension
  registration, ATTACH of a DuckLake catalog with a local data path, the
  Appender on a DuckLake table, an archive-mark UPDATE and a read back.
  We use only the DuckDB C API, as the plugin will.
*/

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "duckdb.h"
#include "duckdb_static_extension.h"

extern "C" {
int32_t duckdb_extension_core_functions_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_parquet_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_json_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_icu_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_ducklake_describe(duckdb_extension_descriptor *);
}

namespace {

int fail(const std::string &what, const char *detail) {
  std::fprintf(stderr, "FAIL: %s: %s\n", what.c_str(),
               detail != nullptr ? detail : "");
  return 1;
}

// We run one statement and, when asked, return its first value as text.
bool query_value(duckdb_connection con, const std::string &sql,
                 std::string *value) {
  duckdb_result res;
  if (duckdb_query(con, sql.c_str(), &res) == DuckDBError) {
    std::fprintf(stderr, "FAIL: %s\n  -> %s\n", sql.c_str(),
                 duckdb_result_error(&res));
    duckdb_destroy_result(&res);
    return false;
  }
  if (value != nullptr && duckdb_row_count(&res) > 0) {
    char *v = duckdb_value_varchar(&res, 0, 0);
    *value = v != nullptr ? v : "NULL";
    duckdb_free(v);
  }
  duckdb_destroy_result(&res);
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  namespace fs = std::filesystem;
  const fs::path lake =
      argc > 1 ? fs::path(argv[1]) : fs::current_path() / "duckdb_build_smoke";
  constexpr int64_t kRows = 100000;

  // We start from an empty lake. DuckLake does not create the parent
  // directory, so we create it, as the plugin will at startup.
  std::error_code ec;
  fs::remove_all(lake, ec);
  fs::create_directories(lake / "data", ec);
  if (ec) return fail("create directories", ec.message().c_str());

  for (auto describe : {duckdb_extension_core_functions_describe,
                        duckdb_extension_parquet_describe,
                        duckdb_extension_json_describe,
                        duckdb_extension_icu_describe,
                        duckdb_extension_ducklake_describe}) {
    if (duckdb_register_static_extension(describe) != 0)
      return fail("register static extension", nullptr);
  }

  duckdb_config cfg;
  duckdb_create_config(&cfg);
  duckdb_set_config(cfg, "autoload_known_extensions", "false");
  duckdb_set_config(cfg, "autoinstall_known_extensions", "false");
  duckdb_set_config(cfg, "threads", "2");
  duckdb_set_config(cfg, "memory_limit", "1GB");

  duckdb_database db;
  char *open_error = nullptr;
  const bool opened =
      duckdb_open_ext(nullptr, &db, cfg, &open_error) != DuckDBError;
  duckdb_destroy_config(&cfg);
  if (!opened) return fail("open", open_error);

  duckdb_connection con;
  if (duckdb_connect(db, &con) == DuckDBError) return fail("connect", nullptr);

  const std::string lake_str = lake.string();
  std::string version;
  bool ok =
      query_value(con,
                  "ATTACH 'ducklake:" + lake_str +
                      "/catalog.ducklake' AS lake "
                      "(DATA_PATH '" +
                      lake_str + "/data/')",
                  nullptr) &&
      query_value(con, "CREATE SCHEMA lake.test", nullptr) &&
      query_value(con,
                  "CREATE TABLE lake.test.orders (id BIGINT, amount DOUBLE, "
                  "created_at TIMESTAMP, __archived_at TIMESTAMP)",
                  nullptr);
  if (!ok) return 1;

  // We append through the Appender directly into the DuckLake table.
  duckdb_appender app;
  if (duckdb_appender_create_ext(con, "lake", "test", "orders", &app) ==
      DuckDBError)
    return fail("appender create", duckdb_appender_error(app));
  for (int64_t i = 1; i <= kRows; i++) {
    duckdb_append_int64(app, i);
    duckdb_append_double(app, static_cast<double>(i % 1000) / 10.0);
    duckdb_append_varchar(app, i <= kRows / 2 ? "2020-01-01 00:00:00"
                                              : "2026-11-01 00:00:00");
    duckdb_append_null(app);
    duckdb_appender_end_row(app);
  }
  if (duckdb_appender_close(app) == DuckDBError)
    return fail("appender close", duckdb_appender_error(app));
  duckdb_appender_destroy(&app);

  // We archive half of the rows, as the applier will for purge transactions.
  ok = query_value(con,
                   "UPDATE lake.test.orders SET __archived_at = now() "
                   "WHERE created_at < '2021-01-01'",
                   nullptr);
  if (!ok) return 1;

  std::string live, full, archived;
  ok = query_value(con,
                   "SELECT count(*) FROM lake.test.orders "
                   "WHERE __archived_at IS NULL",
                   &live) &&
       query_value(con, "SELECT count(*) FROM lake.test.orders", &full) &&
       query_value(con, "SELECT count(__archived_at) FROM lake.test.orders",
                   &archived) &&
       query_value(con, "SELECT version()", &version);
  if (!ok) return 1;

  duckdb_disconnect(&con);
  duckdb_close(&db);

  std::printf("DuckDB %s: live=%s full=%s archived=%s\n", version.c_str(),
              live.c_str(), full.c_str(), archived.c_str());
  const std::string half = std::to_string(kRows / 2);
  if (live != half || full != std::to_string(kRows) || archived != half)
    return fail("unexpected counts", nullptr);

  fs::remove_all(lake, ec);
  std::printf("OK\n");
  return 0;
}
