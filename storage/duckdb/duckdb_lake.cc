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
// Native DuckDB storage (2026-11-25): the copies and the plugin's tables in
// one DuckDB database file (WAL next to it). DuckLake (catalog + Parquet
// files) was measured 2-3x slower on scans, and its own features
// (snapshots, open Parquet files) were not used.
// Named repository.duckdb since 2026-11-26 ("lake" read as DuckLake).
constexpr const char *kLakeFile = "repository.duckdb";
constexpr const char *kMigratingFile = "repository.duckdb.migrating";
// The file's name on 2026-11-25: renamed at startup.
constexpr const char *kOldLakeFile = "lake.duckdb";
// A DuckLake lake of an earlier version: migrated once at startup.
constexpr const char *kLegacyCatalog = "catalog.ducklake";
constexpr const char *kLegacyDataDir = "data";
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
        !fs::exists(lake / kLakeFile, ec) &&
        !fs::exists(lake / kOldLakeFile, ec) &&
        !fs::exists(lake / kLegacyCatalog, ec) &&
        !fs::exists(lake / kLegacyDataDir, ec)) {
      *err = "directory " + lake.string() +
             " exists in the datadir and is not a DuckDB repository (it may be a "
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
    *err = "the DuckDB repository directory " + lake.string() + " is world-writable";
    return true;
  }
  if (st.st_mode & S_IROTH)
    DUCKDB_LOG_WARNING("DuckDB repository directory %s is world-readable",
                       lake.c_str());
  if (st.st_mode & S_IWGRP)
    DUCKDB_LOG_WARNING("DuckDB repository directory %s is group-writable",
                       lake.c_str());
  if (st.st_uid != geteuid())
    DUCKDB_LOG_WARNING(
        "DuckDB repository directory %s is not owned by the mysqld user",
        lake.c_str());

  // secure_file_priv: "NULL" disables file import and export, "" allows any
  // path, otherwise it is a directory.
  if (opt_secure_file_priv != nullptr &&
      strcasecmp(opt_secure_file_priv, "NULL") != 0) {
    if (*opt_secure_file_priv == '\0') {
      DUCKDB_LOG_WARNING(
          "secure_file_priv is empty: users with the FILE privilege can read "
          "and write the DuckDB repository files in %s",
          lake.c_str());
    } else {
      std::error_code ec;
      const fs::path sfp = fs::weakly_canonical(opt_secure_file_priv, ec);
      if (!ec && (lake == sfp || is_inside(lake, sfp)))
        DUCKDB_LOG_WARNING(
            "DuckDB repository directory %s is inside secure_file_priv (%s): users "
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
  // lake.duckdb (the name on 2026-11-25) becomes repository.duckdb, with
  // its WAL.
  if (!fs::exists(lake / kLakeFile, ec) && fs::exists(lake / kOldLakeFile, ec)) {
    const fs::path old_wal = lake / (std::string(kOldLakeFile) + ".wal");
    if (fs::exists(old_wal, ec))
      fs::rename(old_wal, lake / (std::string(kLakeFile) + ".wal"), ec);
    if (!ec) fs::rename(lake / kOldLakeFile, lake / kLakeFile, ec);
    if (ec) {
      *err = "cannot rename " + (lake / kOldLakeFile).string() + " to " +
             kLakeFile + ": " + ec.message();
      return true;
    }
    DUCKDB_LOG_INFO("DuckDB repository: %s renamed to %s",
                    (lake / kOldLakeFile).c_str(), (lake / kLakeFile).c_str());
  }
  const bool lake_existed = fs::exists(lake / kLakeFile, ec);
  const bool migrate = !lake_existed && fs::exists(lake / kLegacyCatalog, ec);
  // Where DuckDB spills: duckdb_tmp_path, by default <lake>/tmp (2026-11-27:
  // spills on the device of the data slowed its reads under load).
  const fs::path tmp = (s.tmp_path != nullptr && *s.tmp_path != '\0')
                           ? resolve_path(s.tmp_path)
                           : lake / kTmpDir;
  if (tmp.native().find('\'') != std::string::npos) {
    *err = "duckdb_tmp_path must not contain a single quote";
    return true;
  }
  {
    const fs::path datadir = canonical_datadir();
    if (tmp == datadir ||
        (is_inside(tmp, datadir) && !is_inside(tmp, lake) &&
         std::distance(tmp.lexically_relative(datadir).begin(),
                       tmp.lexically_relative(datadir).end()) > 1)) {
      *err = "duckdb_tmp_path " + tmp.string() +
             " must not be the datadir or inside a directory of the datadir";
      return true;
    }
  }
  if (make_dir(lake, err) || make_dir(tmp, err)) return true;
  if (check_permissions(lake, err)) return true;
  if (tmp != lake / kTmpDir && check_permissions(tmp, err)) return true;
  // Spill files of an earlier run (a crash, a killed server): nothing uses
  // them now, and they can be large. Only DuckDB's own temporary files
  // (duckdb_temp_*) are removed.
  {
    unsigned long long freed = 0;
    unsigned files = 0;
    for (const auto &e : fs::directory_iterator(tmp, ec)) {
      const std::string name = e.path().filename().string();
      if (name.rfind("duckdb_temp_", 0) != 0 || !e.is_regular_file(ec))
        continue;
      const auto size = e.file_size(ec);
      if (fs::remove(e.path(), ec)) {
        files++;
        freed += ec ? 0 : size;
      }
    }
    if (files > 0)
      DUCKDB_LOG_INFO(
          "DuckDB repository: removed %u leftover spill file(s) in %s "
          "(%llu MB)",
          files, tmp.c_str(), freed >> 20);
  }

  // DuckLake data files without their catalog: the catalog was lost. A new
  // empty lake would hide the only copy of archived rows.
  if (!lake_existed && !migrate && !dir_is_empty(lake / kLegacyDataDir)) {
    *err = "the DuckDB repository " + (lake / kLakeFile).string() +
           " does not exist and the DuckLake catalog " +
           (lake / kLegacyCatalog).string() + " is missing, but " +
           (lake / kLegacyDataDir).string() +
           " contains files: restore the catalog before starting";
    return true;
  }
  // A migration interrupted by a crash: its file is incomplete.
  fs::remove(lake / kMigratingFile, ec);
  fs::remove(lake / (std::string(kMigratingFile) + ".wal"), ec);

  // Every DuckDB call runs on an executor thread (see Executor).
  Executor &executor = Executor::instance();
  if (executor.start(s.executor_threads,
                     static_cast<size_t>(s.executor_stack_size), err)) {
    executor.stop();
    return true;
  }
  bool failed = true;
  if (executor.run(
          [&] {
            failed = open_duckdb(lake.string(), tmp.string(), s, migrate, err);
          })) {
    *err = "the DuckDB executor did not run the open task";
    failed = true;
  }
  if (failed) {
    executor.stop();
    return true;
  }

  m_path = lake.string();
  DUCKDB_LOG_INFO(
      "DuckDB repository: file=%s, created=%s, executor threads=%u",
      (lake / kLakeFile).c_str(),
      lake_existed ? "no" : (migrate ? "migrated from DuckLake" : "yes"),
      s.executor_threads);
  return false;
}

bool Lake::open_duckdb(const std::string &lake, const std::string &tmp,
                       const LakeSettings &s, bool migrate, std::string *err) {
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

  const std::string file = lake + "/" + kLakeFile;
  std::string msg;
  if (migrate && migrate_from_ducklake(lake, s, err)) {
    close_duckdb();
    return true;
  }
  // A native DuckDB file stores no path: a moved lake opens as it is.
  if (run("ATTACH '" + file + "' AS lake", &msg)) {
    *err = "cannot attach the DuckDB repository " + file + ": " + msg;
    close_duckdb();
    return true;
  }
  if (run("SET temp_directory = '" + tmp + "'", &msg) ||
      run("CREATE SCHEMA IF NOT EXISTS lake.__mysql", &msg) ||
      run(Registry::create_table_sql(), &msg) ||
      run_all(Registry::migration_sql(), &msg) ||
      run(applied_position_create_sql(), &msg) ||
      // From now on DuckDB can read and write files only inside the lake
      // directory and cannot reach the network. The lake directory holds
      // everything DuckDB needs: repository.duckdb and its WAL, and tmp/
      // (spills). Extensions are linked statically, so no
      // extension directory is needed. The allowed directory must be set
      // before external access is disabled; DuckDB refuses it afterwards.
      run("SET allowed_directories = ['" + lake + "/'" +
              (tmp.rfind(lake + "/", 0) == 0 ? std::string()
                                             : ", '" + tmp + "/'") +
              "]",
          &msg) ||
      run("SET enable_external_access = false", &msg) ||
      Registry::instance().reload(m_admin, &msg)) {
    *err = "cannot prepare the DuckDB repository: " + msg;
    close_duckdb();
    return true;
  }
  drop_build_leftovers();
  return false;
}

// A DuckLake lake of an earlier version (catalog.ducklake + data/): copied
// once into the native file (2026-11-25). The copy goes to
// repository.duckdb.migrating, every table's row count is compared, and only
// then is it renamed to repository.duckdb and the catalog to
// catalog.ducklake.migrated;
// data/ is left for the DBA to remove. A crash before the rename leaves only
// the .migrating file, removed at the next start, which migrates again.
// A DuckLake lake moved to another path (its catalog stores the old data
// path) is read with OVERRIDE_DATA_PATH when duckdb_lake_relocate=ON.
bool Lake::migrate_from_ducklake(const std::string &lake,
                                 const LakeSettings &s, std::string *err) {
  const std::string catalog = lake + "/" + kLegacyCatalog;
  const std::string data = lake + "/" + kLegacyDataDir + "/";
  const std::string target = lake + "/" + kMigratingFile;
  DUCKDB_LOG_INFO("DuckDB repository: migrating the DuckLake lake %s to %s/%s",
                  catalog.c_str(), lake.c_str(), kLakeFile);
  std::string msg;
  const std::string attach = "ATTACH 'ducklake:" + catalog +
                             "' AS old_lake (DATA_PATH '" + data +
                             "', READ_ONLY";
  if (run(attach + ")", &msg)) {
    const bool moved = msg.find("does not match existing data path") !=
                       std::string::npos;
    if (!moved || !s.relocate ||
        run(attach + ", OVERRIDE_DATA_PATH true)", &msg)) {
      *err = "cannot attach the DuckLake lake " + catalog +
             " to migrate it: " + msg +
             (moved && !s.relocate
                  ? " (it was moved: with duckdb_lake_relocate=ON it is read "
                    "from duckdb_data_path)"
                  : "");
      return true;
    }
  }
  auto fail = [&](const std::string &what) {
    *err = "cannot migrate the DuckLake lake " + catalog + ": " + what +
           " (nothing was changed; the DuckLake files are kept)";
    std::string ignored;
    run("DETACH DATABASE IF EXISTS old_lake", &ignored);
    run("DETACH DATABASE IF EXISTS new_lake", &ignored);
    std::error_code ec;
    fs::remove(target, ec);
    fs::remove(target + ".wal", ec);
    return true;
  };
  if (run("ATTACH '" + target + "' AS new_lake", &msg) ||
      run("COPY FROM DATABASE old_lake TO new_lake", &msg))
    return fail(msg);
  // Every table, both sides: the same row count.
  duckdb_result res;
  if (duckdb_query(m_admin,
                   "SELECT schema_name, table_name FROM duckdb_tables() WHERE "
                   "database_name = 'old_lake'",
                   &res) == DuckDBError) {
    duckdb_destroy_result(&res);
    return fail("cannot list the tables");
  }
  std::vector<std::pair<std::string, std::string>> tables;
  for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
    char *sc = duckdb_value_varchar(&res, 0, r);
    char *t = duckdb_value_varchar(&res, 1, r);
    tables.emplace_back(sc != nullptr ? sc : "", t != nullptr ? t : "");
    duckdb_free(sc);
    duckdb_free(t);
  }
  duckdb_destroy_result(&res);
  unsigned long long rows = 0;
  for (const auto &[schema, table] : tables) {
    const std::string name = quote_ident(schema) + "." + quote_ident(table);
    std::string a, b;
    if (query_value_sql(m_admin, "SELECT count(*) FROM old_lake." + name, &a,
                        &msg) ||
        query_value_sql(m_admin, "SELECT count(*) FROM new_lake." + name, &b,
                        &msg))
      return fail(msg);
    if (a != b)
      return fail("table " + schema + "." + table + " has " + a +
                  " rows in DuckLake and " + b + " in the copy");
    rows += std::stoull(a.empty() ? "0" : a);
  }
  if (run("CHECKPOINT new_lake", &msg) ||
      run("DETACH DATABASE new_lake", &msg) ||
      run("DETACH DATABASE old_lake", &msg))
    return fail(msg);
  std::error_code ec;
  fs::rename(target, lake + "/" + kLakeFile, ec);
  if (ec) return fail("cannot rename " + target + ": " + ec.message());
  fs::rename(catalog, catalog + ".migrated", ec);
  if (fs::exists(catalog + ".wal"))
    fs::rename(catalog + ".wal", catalog + ".migrated.wal", ec);
  DUCKDB_LOG_WARNING(
      "DuckDB repository migrated from DuckLake to %s/%s: %zu tables, %llu rows "
      "checked. The old catalog is now %s.migrated; the old data files in %s "
      "are no longer used and can be removed after a check",
      lake.c_str(), kLakeFile, tables.size(), rows, catalog.c_str(),
      data.c_str());
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
