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

#include "storage/duckdb/duckdb_lake.h"

#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "duckdb_static_extension.h"
#include "sql/mysqld.h"  // mysql_real_data_home, opt_secure_file_priv
#include "sql/sql_class.h"  // THD::is_killed
#include "storage/duckdb/duckdb_applier.h"
#include "storage/duckdb/duckdb_executor.h"
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_types.h"  // quote_ident
#include "storage/duckdb/duckdb_registry.h"

extern "C" {
int32_t duckdb_extension_core_functions_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_parquet_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_json_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_icu_describe(duckdb_extension_descriptor *);
int32_t duckdb_extension_ducklake_describe(duckdb_extension_descriptor *);
}

namespace duckdb_se {

namespace fs = std::filesystem;

namespace {

constexpr const char *kDefaultDirName = "duckdb_data";
constexpr const char *kCatalogFile = "catalog.ducklake";
constexpr const char *kDataDir = "data";
constexpr const char *kTmpDir = "tmp";
// 0750: owner all, group read and execute.
constexpr fs::perms kDirPerms = fs::perms::owner_all | fs::perms::group_read |
                                fs::perms::group_exec;

bool is_inside(const fs::path &child, const fs::path &parent) {
  const fs::path rel = child.lexically_relative(parent);
  return !rel.empty() && rel.native() != "." && *rel.begin() != "..";
}

bool dir_is_empty(const fs::path &p) {
  std::error_code ec;
  return !fs::exists(p, ec) || fs::is_empty(p, ec);
}

fs::path canonical_datadir() {
  std::error_code ec;
  return fs::weakly_canonical(fs::path(mysql_real_data_home), ec);
}

// Empty value: <datadir>/duckdb_data. Relative value: against the datadir.
fs::path resolve_path(const char *value) {
  fs::path p = (value == nullptr || *value == '\0') ? fs::path(kDefaultDirName)
                                                    : fs::path(value);
  if (p.is_relative()) p = fs::path(mysql_real_data_home) / p;
  std::error_code ec;
  return fs::weakly_canonical(p, ec);
}

// Refusals that do not depend on permissions. We refuse only clear mistakes.
bool check_location(const fs::path &lake, std::string *err) {
  if (lake.native().find('\'') != std::string::npos) {
    *err = "duckdb_data_path must not contain a single quote";
    return true;
  }
  const fs::path datadir = canonical_datadir();
  if (lake == datadir) {
    *err = "duckdb_data_path must not be the datadir itself";
    return true;
  }
  if (is_inside(lake, datadir)) {
    const fs::path rel = lake.lexically_relative(datadir);
    if (std::distance(rel.begin(), rel.end()) > 1) {
      *err = "duckdb_data_path " + lake.string() +
             " is inside a directory of the datadir (a schema or engine "
             "directory)";
      return true;
    }
    std::error_code ec;
    if (fs::is_directory(lake, ec) && !dir_is_empty(lake) &&
        !fs::exists(lake / kCatalogFile, ec) &&
        !fs::exists(lake / kDataDir, ec)) {
      *err = "directory " + lake.string() +
             " exists in the datadir and is not a DuckDB lake (it may be a "
             "database directory)";
      return true;
    }
  }
  return false;
}

// We create a missing directory with mode 0750.
bool make_dir(const fs::path &p, std::string *err) {
  std::error_code ec;
  if (fs::exists(p, ec)) {
    if (fs::is_directory(p, ec)) return false;
    *err = p.string() + " exists and is not a directory";
    return true;
  }
  fs::create_directories(p, ec);
  if (ec) {
    *err = "cannot create " + p.string() + ": " + ec.message();
    return true;
  }
  fs::permissions(p, kDirPerms, fs::perm_options::replace, ec);
  return false;
}

// World-writable is refused; everything else only produces a warning.
bool check_permissions(const fs::path &lake, std::string *err) {
  struct stat st;
  if (stat(lake.c_str(), &st) != 0) {
    *err = "cannot read the attributes of " + lake.string();
    return true;
  }
  if (st.st_mode & S_IWOTH) {
    *err = "the DuckDB lake directory " + lake.string() + " is world-writable";
    return true;
  }
  if (st.st_mode & S_IROTH)
    DUCKDB_LOG_WARNING("DuckDB lake directory %s is world-readable",
                       lake.c_str());
  if (st.st_mode & S_IWGRP)
    DUCKDB_LOG_WARNING("DuckDB lake directory %s is group-writable",
                       lake.c_str());
  if (st.st_uid != geteuid())
    DUCKDB_LOG_WARNING(
        "DuckDB lake directory %s is not owned by the mysqld user",
        lake.c_str());

  // secure_file_priv: "NULL" disables file import and export, "" allows any
  // path, otherwise it is a directory.
  if (opt_secure_file_priv != nullptr &&
      strcasecmp(opt_secure_file_priv, "NULL") != 0) {
    if (*opt_secure_file_priv == '\0') {
      DUCKDB_LOG_WARNING(
          "secure_file_priv is empty: users with the FILE privilege can read "
          "and write the DuckDB lake files in %s",
          lake.c_str());
    } else {
      std::error_code ec;
      const fs::path sfp = fs::weakly_canonical(opt_secure_file_priv, ec);
      if (!ec && (lake == sfp || is_inside(lake, sfp)))
        DUCKDB_LOG_WARNING(
            "DuckDB lake directory %s is inside secure_file_priv (%s): users "
            "with the FILE privilege can read and write its files",
            lake.c_str(), sfp.c_str());
    }
  }
  return false;
}

// Static extensions are registered once per process, before the first open.
bool register_extensions(std::string *err) {
  static std::once_flag once;
  static bool failed = false;
  std::call_once(once, [] {
    for (auto describe : {duckdb_extension_core_functions_describe,
                          duckdb_extension_parquet_describe,
                          duckdb_extension_json_describe,
                          duckdb_extension_icu_describe,
                          duckdb_extension_ducklake_describe}) {
      if (duckdb_register_static_extension(describe) != 0) failed = true;
    }
  });
  if (failed) *err = "cannot register the DuckDB static extensions";
  return failed;
}

}  // namespace

Lake &Lake::instance() {
  static Lake lake;
  return lake;
}

bool run_sql(duckdb_connection con, const std::string &sql, std::string *err) {
  duckdb_result res;
  const bool failed = duckdb_query(con, sql.c_str(), &res) == DuckDBError;
  if (failed) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "unknown DuckDB error";
  }
  duckdb_destroy_result(&res);
  return failed;
}

bool query_value_sql(duckdb_connection con, const std::string &sql,
                     std::string *value, std::string *err) {
  duckdb_result res;
  if (duckdb_query(con, sql.c_str(), &res) == DuckDBError) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "unknown DuckDB error";
    duckdb_destroy_result(&res);
    return true;
  }
  value->clear();
  if (duckdb_row_count(&res) > 0) {
    char *v = duckdb_value_varchar(&res, 0, 0);
    if (v != nullptr) {
      *value = v;
      duckdb_free(v);
    }
  }
  duckdb_destroy_result(&res);
  return false;
}

bool Lake::run_all(const char *const *statements, std::string *err) {
  for (; *statements != nullptr; ++statements)
    if (run(*statements, err)) return true;
  return false;
}

bool Lake::open(const LakeSettings &s, std::string *err) {
  std::unique_lock<std::shared_mutex> usage(m_usage);
  if (is_open()) return false;

  // Filesystem work stays on the caller's thread: it is not DuckDB code.
  const fs::path lake = resolve_path(s.data_path);
  if (check_location(lake, err)) return true;

  std::error_code ec;
  const bool catalog_existed = fs::exists(lake / kCatalogFile, ec);
  if (make_dir(lake, err) || make_dir(lake / kDataDir, err) ||
      make_dir(lake / kTmpDir, err))
    return true;
  if (check_permissions(lake, err)) return true;

  // A missing catalog with existing data means the catalog was lost. A new
  // empty catalog would orphan the only copy of archived rows.
  if (!catalog_existed && !dir_is_empty(lake / kDataDir)) {
    *err = "the DuckDB lake catalog " + (lake / kCatalogFile).string() +
           " is missing but " + (lake / kDataDir).string() +
           " contains files: restore the catalog before starting";
    return true;
  }

  // Every DuckDB call runs on an executor thread (see Executor).
  Executor &executor = Executor::instance();
  if (executor.start(s.executor_threads,
                     static_cast<size_t>(s.executor_stack_size), err)) {
    executor.stop();
    return true;
  }
  bool failed = true;
  if (executor.run([&] { failed = open_duckdb(lake.string(), s, err); })) {
    *err = "the DuckDB executor did not run the open task";
    failed = true;
  }
  if (failed) {
    executor.stop();
    return true;
  }

  m_path = lake.string();
  DUCKDB_LOG_INFO(
      "DuckDB lake: catalog=%s, data=%s/, created=%s, executor threads=%u",
      (lake / kCatalogFile).c_str(), (lake / kDataDir).c_str(),
      catalog_existed ? "no" : "yes", s.executor_threads);
  return false;
}

bool Lake::open_duckdb(const std::string &lake, const LakeSettings &s,
                       std::string *err) {
  if (register_extensions(err)) return true;

  duckdb_config cfg;
  if (duckdb_create_config(&cfg) == DuckDBError) {
    *err = "cannot create the DuckDB configuration";
    return true;
  }
  const std::string mem = std::to_string(s.memory_limit) + " bytes";
  const std::string thr = std::to_string(s.threads);
  const bool cfg_failed =
      duckdb_set_config(cfg, "autoload_known_extensions", "false") ==
          DuckDBError ||
      duckdb_set_config(cfg, "autoinstall_known_extensions", "false") ==
          DuckDBError ||
      duckdb_set_config(cfg, "memory_limit", mem.c_str()) == DuckDBError ||
      duckdb_set_config(cfg, "threads", thr.c_str()) == DuckDBError;
  char *open_error = nullptr;
  bool open_failed = cfg_failed;
  if (!cfg_failed)
    open_failed =
        duckdb_open_ext(nullptr, &m_db, cfg, &open_error) == DuckDBError;
  duckdb_destroy_config(&cfg);
  if (open_failed) {
    *err = std::string("cannot open DuckDB: ") +
           (open_error != nullptr ? open_error : "invalid configuration");
    if (open_error != nullptr) duckdb_free(open_error);
    m_db = nullptr;
    return true;
  }
  if (duckdb_connect(m_db, &m_admin) == DuckDBError) {
    *err = "cannot connect to DuckDB";
    close_duckdb();
    return true;
  }

  const std::string catalog = lake + "/" + kCatalogFile;
  const std::string data = lake + "/" + kDataDir + "/";
  const std::string tmp = lake + "/" + kTmpDir;
  std::string msg;
  if (run("ATTACH 'ducklake:" + catalog + "' AS lake (DATA_PATH '" + data +
              "')",
          &msg)) {
    // DuckLake keeps the data path in the catalog and refuses another one:
    // the lake was moved or restored elsewhere.
    const bool moved = msg.find("does not match existing data path") !=
                       std::string::npos;
    if (!moved || !s.relocate) {
      *err = "cannot attach the DuckDB lake " + catalog + ": " + msg +
             (moved ? " (with duckdb_lake_relocate=ON the plugin accepts a "
                      "moved lake whose files are all in duckdb_data_path)"
                    : "");
      close_duckdb();
      return true;
    }
    if (relocate(catalog, data, err)) {
      close_duckdb();
      return true;
    }
  }
  if (run("SET temp_directory = '" + tmp + "'", &msg) ||
      run("CREATE SCHEMA IF NOT EXISTS lake.__mysql", &msg) ||
      run(Registry::create_table_sql(), &msg) ||
      run_all(Registry::migration_sql(), &msg) ||
      run(applied_position_create_sql(), &msg) ||
      // From now on DuckDB can read and write files only inside the lake
      // directory and cannot reach the network. The lake directory holds
      // everything DuckDB needs: the catalog and its WAL, data/ (Parquet
      // files) and tmp/ (spills). Extensions are linked statically, so no
      // extension directory is needed. The allowed directory must be set
      // before external access is disabled; DuckDB refuses it afterwards.
      run("SET allowed_directories = ['" + lake + "/']", &msg) ||
      run("SET enable_external_access = false", &msg) ||
      Registry::instance().reload(m_admin, &msg)) {
    *err = "cannot prepare the DuckDB lake: " + msg;
    close_duckdb();
    return true;
  }
  drop_build_leftovers();
  return false;
}

// A moved lake (duckdb_lake_relocate=ON): attach with the configured data
// path, check that every file the current snapshot uses is there (data and
// delete files; paths relative to the data path, schema and table, or
// absolute), then store the new data path in the catalog so the next start
// attaches normally. A missing file refuses the lake: the configured
// directory is not this catalog's data.
bool Lake::relocate(const std::string &catalog, const std::string &data,
                    std::string *err) {
  std::string msg;
  if (run("ATTACH 'ducklake:" + catalog + "' AS lake (DATA_PATH '" + data +
              "', OVERRIDE_DATA_PATH true)",
          &msg)) {
    *err = "cannot attach the moved DuckDB lake " + catalog + ": " + msg;
    return true;
  }
  const std::string m = "__ducklake_metadata_lake.";
  const std::string dir =
      "CASE WHEN t.path_is_relative THEN (CASE WHEN s.path_is_relative THEN "
      "'" + data + "' || s.path ELSE s.path END) || t.path ELSE t.path END";
  const std::string files =
      "SELECT CASE WHEN f.path_is_relative THEN " + dir +
      " || f.path ELSE f.path END FROM " + m + "ducklake_data_file f JOIN " +
      m + "ducklake_table t ON t.table_id = f.table_id AND t.end_snapshot IS "
      "NULL JOIN " + m + "ducklake_schema s ON s.schema_id = t.schema_id AND "
      "s.end_snapshot IS NULL WHERE f.end_snapshot IS NULL UNION ALL SELECT "
      "CASE WHEN f.path_is_relative THEN " + dir +
      " || f.path ELSE f.path END FROM " + m + "ducklake_delete_file f JOIN " +
      m + "ducklake_table t ON t.table_id = f.table_id AND t.end_snapshot IS "
      "NULL JOIN " + m + "ducklake_schema s ON s.schema_id = t.schema_id AND "
      "s.end_snapshot IS NULL WHERE f.end_snapshot IS NULL";
  duckdb_result res;
  if (duckdb_query(m_admin, files.c_str(), &res) == DuckDBError) {
    const char *e = duckdb_result_error(&res);
    *err = std::string("cannot list the files of the moved DuckDB lake: ") +
           (e != nullptr ? e : "query failed");
    duckdb_destroy_result(&res);
    return true;
  }
  size_t total = 0, missing = 0;
  std::string first_missing;
  for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
    char *p = duckdb_value_varchar(&res, 0, r);
    total++;
    std::error_code ec;
    if (p == nullptr || !fs::exists(fs::path(p), ec)) {
      if (missing++ == 0) first_missing = p != nullptr ? p : "(null)";
    }
    duckdb_free(p);
  }
  duckdb_destroy_result(&res);
  if (missing > 0) {
    *err = "the DuckDB lake catalog " + catalog + " stores another data path "
           "and " + std::to_string(missing) + " of its " +
           std::to_string(total) + " files are not under " + data +
           " (first: " + first_missing + "): not relocated";
    return true;
  }
  std::string old_path;
  if (duckdb_query(m_admin,
                   ("SELECT value FROM " + m +
                    "ducklake_metadata WHERE key = 'data_path'")
                       .c_str(),
                   &res) == DuckDBSuccess &&
      duckdb_row_count(&res) > 0) {
    char *v = duckdb_value_varchar(&res, 0, 0);
    old_path = v != nullptr ? v : "";
    duckdb_free(v);
  }
  duckdb_destroy_result(&res);
  if (run("UPDATE " + m + "ducklake_metadata SET value = '" + data +
              "' WHERE key = 'data_path'",
          &msg)) {
    *err = "cannot store the new data path of the moved DuckDB lake: " + msg;
    return true;
  }
  DUCKDB_LOG_WARNING(
      "DuckDB lake moved: the catalog's data path %s is now %s (%zu files "
      "checked; duckdb_lake_relocate=ON)",
      old_path.c_str(), data.c_str(), total);
  return false;
}

// A load builds its copy as <table>__duckdb_build and renames it when it
// writes the registry (DWL#3 step 3b). A crash in between leaves the build
// table: the registry never refers to one, so we drop them all. A failure
// here is logged, not fatal: the next load of the table drops its own.
void Lake::drop_build_leftovers() {
  duckdb_result res;
  if (duckdb_query(m_admin,
                   "SELECT schema_name, table_name FROM duckdb_tables() WHERE "
                   "database_name = 'lake' AND schema_name <> '__mysql' AND "
                   "(suffix(table_name, '__duckdb_build') OR "
                   "suffix(table_name, '__duckdb_refresh'))",
                   &res) == DuckDBError) {
    duckdb_destroy_result(&res);
    return;
  }
  std::vector<std::pair<std::string, std::string>> leftovers;
  for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
    char *s = duckdb_value_varchar(&res, 0, r);
    char *t = duckdb_value_varchar(&res, 1, r);
    leftovers.emplace_back(s != nullptr ? s : "", t != nullptr ? t : "");
    duckdb_free(s);
    duckdb_free(t);
  }
  duckdb_destroy_result(&res);
  for (const auto &[schema, table] : leftovers) {
    std::string msg;
    if (run("DROP TABLE IF EXISTS lake." + quote_ident(schema) + "." +
                quote_ident(table),
            &msg))
      DUCKDB_LOG_WARNING("cannot drop the leftover build table %s.%s: %s",
                         schema.c_str(), table.c_str(), msg.c_str());
    else
      DUCKDB_LOG_WARNING(
          "dropped %s.%s: the build table of a load interrupted before its "
          "commit",
          schema.c_str(), table.c_str());
  }
}

void Lake::close() {
  std::unique_lock<std::shared_mutex> usage(m_usage);  // waits for operations
  if (m_db != nullptr) Executor::instance().run([this] { close_duckdb(); });
  Executor::instance().stop();
  m_path.clear();
}

void Lake::close_duckdb() {
  if (m_admin != nullptr) duckdb_disconnect(&m_admin);
  if (m_db != nullptr) duckdb_close(&m_db);
  m_admin = nullptr;
  m_db = nullptr;
}

Lake::Connection::Connection(std::shared_lock<std::shared_mutex> usage,
                             duckdb_connection con, THD *thd)
    : m_usage(std::move(usage)), m_con(con), m_thd(thd) {}

// We disconnect before m_usage is released (members are destroyed after the
// body), so close() never runs while this connection is still alive.
Lake::Connection::~Connection() {
  if (m_con != nullptr)
    Executor::instance().run([this] { duckdb_disconnect(&m_con); });
}

bool Lake::Connection::run_task(
    const std::function<bool(duckdb_connection, std::string *)> &fn,
    std::string *err, bool interruptible) {
  bool failed = true;
  std::string task_err;
  THD *thd = interruptible ? m_thd : nullptr;
  duckdb_connection con = m_con;
  const bool cancelled = Executor::instance().run(
      [&] { failed = fn(con, &task_err); },
      [thd] { return thd != nullptr && thd->is_killed(); },
      [con] { duckdb_interrupt(con); });
  if (cancelled) {
    *err = "interrupted";
    return true;
  }
  // A task that failed with an exception left no message (the executor logged
  // it).
  if (failed)
    *err = task_err.empty() ? "internal error (see the error log)" : task_err;
  return failed;
}

bool Lake::Connection::execute(
    const std::function<bool(duckdb_connection, std::string *)> &fn,
    std::string *err) {
  return run_task(fn, err, true);
}

bool Lake::Connection::execute_cleanup(
    const std::function<bool(duckdb_connection, std::string *)> &fn,
    std::string *err) {
  return run_task(fn, err, false);
}

bool Lake::Connection::run(const std::string &sql, std::string *err) {
  return execute(
      [&](duckdb_connection c, std::string *e) { return run_sql(c, sql, e); },
      err);
}

bool Lake::Connection::query_value(const std::string &sql, std::string *value,
                                   std::string *err) {
  return execute(
      [&](duckdb_connection c, std::string *e) {
        return query_value_sql(c, sql, value, e);
      },
      err);
}

std::unique_ptr<Lake::Connection> Lake::connect(THD *thd, std::string *err) {
  std::shared_lock<std::shared_mutex> usage(m_usage);
  if (m_db == nullptr) {
    *err = "the DuckDB secondary engine is disabled (duckdb_enable=OFF)";
    return nullptr;
  }
  duckdb_connection con = nullptr;
  bool failed = true;
  if (Executor::instance().run(
          [&] { failed = duckdb_connect(m_db, &con) == DuckDBError; }) ||
      failed) {
    *err = "cannot connect to DuckDB";
    return nullptr;
  }
  return std::unique_ptr<Connection>(
      new Connection(std::move(usage), con, thd));
}

bool Lake::set_memory_limit(unsigned long long bytes, std::string *err) {
  bool failed = true;
  if (Executor::instance().run([&] {
        failed =
            run("SET memory_limit = '" + std::to_string(bytes) + " bytes'", err);
      }))
    *err = "the DuckDB executor is not running";
  return failed;
}

bool Lake::set_threads(unsigned int threads, std::string *err) {
  bool failed = true;
  if (Executor::instance().run(
          [&] { failed = run("SET threads = " + std::to_string(threads), err); }))
    *err = "the DuckDB executor is not running";
  return failed;
}

}  // namespace duckdb_se
