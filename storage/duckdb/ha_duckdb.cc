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

#include "storage/duckdb/ha_duckdb.h"

#include <strings.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "my_dbug.h"
#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "sql/derror.h"
#include "sql/handler.h"
#include "mysql/components/my_service.h"
#include "mysql/components/services/dynamic_privilege.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/table.h"
#include "sql/sql_plugin.h"   // plugin_lock_by_name, plugin_ref_to_int
#include "string_with_len.h"   // STRING_WITH_LEN
#include "storage/duckdb/duckdb_binlog_reader.h"
#include "storage/duckdb/duckdb_executor.h"
#include "storage/duckdb/duckdb_guard.h"
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_load.h"
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_offload.h"
#include "storage/duckdb/duckdb_registry.h"
#include "storage/duckdb/duckdb_session.h"

// Logging services used by LogPluginErrMsg (declared in log_builtins.h).
static SERVICE_TYPE(registry) *reg_srv = nullptr;
SERVICE_TYPE(log_builtins) *log_bi = nullptr;
SERVICE_TYPE(log_builtins_string) *log_bs = nullptr;

namespace duckdb_se {

namespace {

// System variables; the server adds the duckdb_ prefix.
bool srv_enable = true;
char *srv_data_path = nullptr;
bool srv_lake_relocate = true;
unsigned long long srv_memory_limit = 0;  // 0: not set
unsigned int srv_threads = 0;             // 0: not set
bool srv_encryption = false;
unsigned int srv_executor_threads = 4;
unsigned long long srv_executor_stack_size = 8ULL << 20;
bool srv_apply_debug_log = false;
unsigned long long srv_apply_batch_max_trx = 1000;
unsigned long long srv_apply_batch_max_rows = 100000;
unsigned long long srv_apply_batch_max_delay_ms = 1000;
char *srv_apply_start_position = nullptr;
char *srv_discard_stale_copy = nullptr;
const char *history_scope_names[] = {"LIVE", "FULL", nullptr};
TYPELIB history_scope_typelib = {2, "", history_scope_names, nullptr};
// DWL#5: the dynamic privilege needed for duckdb_history_scope=FULL.
constexpr char kHistoryReaderPrivilege[] = "DUCKDB_HISTORY_READER";
SERVICE_TYPE(dynamic_privilege_register) *privilege_service = nullptr;
bool privilege_registered = false;
unsigned long srv_apply_gap_action = 0;  // STOP

const char *gap_action_names[] = {"STOP", "FIRST_AVAILABLE", nullptr};
TYPELIB gap_action_typelib = {2, "", gap_action_names, nullptr};

// Status variables.
bool status_enabled = false;
char status_lake_path[FN_REFLEN + 1] = "";

// Below 256 MiB DuckDB cannot run meaningful analytical queries.
constexpr unsigned long long kMinMemoryLimit = 256ULL << 20;

// Limits of duckdb_executor_stack_size.
constexpr unsigned long long kMinStack = 2ULL << 20;
constexpr unsigned long long kMaxStack = 64ULL << 20;

// Upper limit of duckdb_threads and duckdb_executor_threads: the number of
// cores.
unsigned int max_threads() {
  return std::max(1U, std::thread::hardware_concurrency());
}

LakeSettings lake_settings() {
  return {srv_data_path,        srv_memory_limit,        srv_threads,
          srv_executor_threads, srv_executor_stack_size, srv_lake_relocate};
}

// The server calls our entry points (handler methods, variable callbacks,
// observers, init and deinit). No C++ exception (for example std::bad_alloc)
// may reach it: we turn exceptions into an error.
template <typename F>
int guarded(int error_result, F &&f) {
  try {
    return f();
  } catch (const std::exception &e) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), e.what());
  } catch (...) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), "internal error");
  }
  return error_result;
}

BinlogReader::Status safe_status() {
  try {
    return BinlogReader::instance().status();
  } catch (...) {
    return BinlogReader::Status();
  }
}

// The binlog reader is woken by the transaction observer's after_commit
// (duckdb_offload.cc), after the InnoDB commit.
st_plugin_int *plugin_handle = nullptr;

// For a storage engine, init() receives the handlerton, not the plugin. The
// observer registration needs our st_plugin_int: we look it up by name (our
// state is still UNINITIALIZED, which the lookup accepts) and release the
// reference right away, as the NDB engine does. The server keeps its own
// reference to the observer's plugin and locks it at every call.
st_plugin_int *own_plugin() {
  const LEX_CSTRING name = {STRING_WITH_LEN("duckdb")};
  plugin_ref ref =
      plugin_lock_by_name(nullptr, name, MYSQL_STORAGE_ENGINE_PLUGIN);
  if (ref == nullptr) return nullptr;
  st_plugin_int *self = plugin_ref_to_int(ref);
  plugin_unlock(nullptr, ref);
  return self;
}

// Opens the lake and starts the binlog reader; the caller holds the lake
// mutex. On failure nothing is left open.
bool open_engine(std::string *err) {
  Lake &lake = Lake::instance();
  if (lake.open(lake_settings(), err)) return true;
  BinlogReader::instance().set_debug_log(srv_apply_debug_log);
  BinlogReader::instance().set_batch_max_trx(srv_apply_batch_max_trx);
  BinlogReader::instance().set_batch_max_rows(srv_apply_batch_max_rows);
  BinlogReader::instance().set_batch_max_delay_ms(srv_apply_batch_max_delay_ms);
  BinlogReader::instance().set_gap_first_available(srv_apply_gap_action == 1);
  if (BinlogReader::instance().start(
          static_cast<size_t>(srv_executor_stack_size), err)) {
    lake.close();
    return true;
  }
  return false;
}

// Stops the binlog reader and closes the lake; the caller holds the lake mutex.
void close_engine() {
  BinlogReader::instance().stop();
  Lake::instance().close();
}

void refresh_status() {
  Lake &lake = Lake::instance();
  status_enabled = lake.is_open();
  snprintf(status_lake_path, sizeof(status_lake_path), "%s",
           lake.path().c_str());
}

// We accept the same boolean values as the server: ON, OFF, TRUE, FALSE, 1, 0.
bool value_to_bool(st_mysql_value *value, bool *out) {
  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING) {
    char buf[16];
    int len = sizeof(buf);
    const char *s = value->val_str(value, buf, &len);
    if (s == nullptr) return true;
    if (!strcasecmp(s, "ON") || !strcasecmp(s, "TRUE") || !strcmp(s, "1")) {
      *out = true;
      return false;
    }
    if (!strcasecmp(s, "OFF") || !strcasecmp(s, "FALSE") || !strcmp(s, "0")) {
      *out = false;
      return false;
    }
    return true;
  }
  long long v;
  if (value->val_int(value, &v) != 0 || (v != 0 && v != 1)) return true;
  *out = v == 1;
  return false;
}

// We do the real work in the check callbacks: they run without
// LOCK_global_system_variables and can fail. The default update callback then
// only stores the value.
int check_enable_impl(MYSQL_THD, SYS_VAR *, void *save, st_mysql_value *value) {
  bool want;
  if (value_to_bool(value, &want)) return 1;
  Lake &lake = Lake::instance();
  std::lock_guard<std::mutex> guard(lake.mutex());
  if (want && !lake.is_open()) {
    std::string err;
    if (open_engine(&err)) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
      return 1;
    }
  } else if (!want && lake.is_open()) {
    close_engine();
  }
  refresh_status();
  *static_cast<bool *>(save) = want;
  return 0;
}

int check_memory_limit_impl(MYSQL_THD, SYS_VAR *, void *save,
                       st_mysql_value *value) {
  long long v;
  if (value->val_int(value, &v) != 0) return 1;
  if (!value->is_unsigned(value) && v < 0) return 1;
  const unsigned long long bytes = static_cast<unsigned long long>(v);
  if (bytes < kMinMemoryLimit) {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "duckdb_memory_limit",
             std::to_string(bytes).c_str());
    return 1;
  }
  Lake &lake = Lake::instance();
  std::lock_guard<std::mutex> guard(lake.mutex());
  std::string err;
  if (lake.is_open() && lake.set_memory_limit(bytes, &err)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return 1;
  }
  *static_cast<unsigned long long *>(save) = bytes;
  return 0;
}

int check_threads_impl(MYSQL_THD, SYS_VAR *, void *save, st_mysql_value *value) {
  long long v;
  if (value->val_int(value, &v) != 0) return 1;
  if (v < 1 || v > static_cast<long long>(max_threads())) {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "duckdb_threads",
             std::to_string(v).c_str());
    return 1;
  }
  Lake &lake = Lake::instance();
  std::lock_guard<std::mutex> guard(lake.mutex());
  std::string err;
  if (lake.is_open() && lake.set_threads(static_cast<unsigned int>(v), &err)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return 1;
  }
  *static_cast<unsigned int *>(save) = static_cast<unsigned int>(v);
  return 0;
}

int check_executor_threads_impl(MYSQL_THD, SYS_VAR *, void *save,
                           st_mysql_value *value) {
  long long v;
  if (value->val_int(value, &v) != 0) return 1;
  if (v < 1 || v > static_cast<long long>(max_threads())) {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "duckdb_executor_threads",
             std::to_string(v).c_str());
    return 1;
  }
  std::lock_guard<std::mutex> guard(Lake::instance().mutex());
  std::string err;
  // When the lake is closed the pool is stopped: the value is used at the next
  // open.
  if (Executor::instance().resize(static_cast<unsigned int>(v), &err)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return 1;
  }
  *static_cast<unsigned int *>(save) = static_cast<unsigned int>(v);
  return 0;
}

// SET GLOBAL duckdb_apply_start_position works as a command: it restarts the
// binlog reader at the given binary log, which becomes the applied position.
int check_apply_start_position_impl(MYSQL_THD thd, SYS_VAR *, void *save,
                                    st_mysql_value *value) {
  char buf[FN_REFLEN + 1];
  int len = sizeof(buf);
  const char *str = value->val_str(value, buf, &len);
  if (str == nullptr) return 1;
  const std::string target(str, len);
  Lake &lake = Lake::instance();
  std::lock_guard<std::mutex> guard(lake.mutex());
  if (!lake.is_open()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "the DuckDB secondary engine is disabled (duckdb_enable=OFF)");
    return 1;
  }
  BinlogPos to;
  std::vector<std::string> behind;
  std::string err;
  if (BinlogReader::resolve_start_position(target, &to, &err) ||
      BinlogReader::instance().repoint(to, thd, &behind, &err)) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), err.c_str());
    return 1;
  }
  if (!behind.empty()) {
    std::string tables;
    for (const std::string &t : behind)
      tables += (tables.empty() ? "" : ", ") + t;
    const std::string msg =
        "loaded tables that may be behind InnoDB (reload them): " + tables;
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_SECONDARY_ENGINE_PLUGIN,
                        ER_THD(thd, ER_SECONDARY_ENGINE_PLUGIN), msg.c_str());
  }
  // A command, not a setting: the variable stays empty once it ran (the
  // error log records what was done).
  *static_cast<const char **>(save) = thd->strmake("", 0);
  return 0;
}

int check_apply_start_position(MYSQL_THD thd, SYS_VAR *var, void *save,
                               st_mysql_value *value) {
  return guarded(1, [&] {
    return check_apply_start_position_impl(thd, var, save, value);
  });
}

// SET GLOBAL duckdb_discard_stale_copy = 'db.table' works as a command: it
// drops a stale copy whose table no longer exists (DWL#3 step 3b).
int check_discard_stale_copy_impl(MYSQL_THD thd, SYS_VAR *, void *save,
                                  st_mysql_value *value) {
  char buf[2 * NAME_LEN + 2];
  int len = sizeof(buf);
  const char *str = value->val_str(value, buf, &len);
  if (str == nullptr) return 1;
  Lake &lake = Lake::instance();
  std::lock_guard<std::mutex> guard(lake.mutex());
  if (!lake.is_open()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "the DuckDB secondary engine is disabled (duckdb_enable=OFF)");
    return 1;
  }
  if (discard_stale_copy(thd, std::string(str, len))) return 1;
  // A command, not a setting: the variable stays empty once it ran (the
  // error log records what was done).
  *static_cast<const char **>(save) = thd->strmake("", 0);
  return 0;
}

int check_discard_stale_copy(MYSQL_THD thd, SYS_VAR *var, void *save,
                             st_mysql_value *value) {
  return guarded(1, [&] {
    return check_discard_stale_copy_impl(thd, var, save, value);
  });
}

// duckdb_history_scope=FULL (archived rows visible, DWL#5/DWL#6) requires
// the DUCKDB_HISTORY_READER privilege, at session and global level and in a
// SET_VAR hint.
int check_history_scope_impl(MYSQL_THD thd, SYS_VAR *, void *save,
                             st_mysql_value *value) {
  long long index = -1;
  if (value->value_type(value) == MYSQL_VALUE_TYPE_STRING) {
    char buf[16];
    int len = sizeof(buf);
    const char *str = value->val_str(value, buf, &len);
    if (str != nullptr) {
      const std::string v(str, len);
      for (int i = 0; history_scope_names[i] != nullptr; i++)
        if (strcasecmp(v.c_str(), history_scope_names[i]) == 0) index = i;
    }
  } else if (value->val_int(value, &index) != 0) {
    index = -1;
  }
  if (index != 0 && index != 1) {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "duckdb_history_scope", "?");
    return 1;
  }
  // DWL#7: without the guard a FULL query could run on InnoDB and miss the
  // archived rows.
  if (index == 1 && !guard_active()) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "duckdb_history_scope=FULL requires the duckdb_guard plugin, "
             "which is not active");
    return 1;
  }
  if (index == 1 &&
      !thd->security_context()
           ->has_global_grant(STRING_WITH_LEN(kHistoryReaderPrivilege))
           .first) {
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0),
             kHistoryReaderPrivilege);
    return 1;
  }
  *static_cast<unsigned long *>(save) = static_cast<unsigned long>(index);
  return 0;
}

int check_history_scope(MYSQL_THD thd, SYS_VAR *var, void *save,
                        st_mysql_value *value) {
  return guarded(1, [&] {
    return check_history_scope_impl(thd, var, save, value);
  });
}

int check_enable(MYSQL_THD thd, SYS_VAR *var, void *save, st_mysql_value *value) {
  return guarded(1, [&] { return check_enable_impl(thd, var, save, value); });
}

int check_memory_limit(MYSQL_THD thd, SYS_VAR *var, void *save, st_mysql_value *value) {
  return guarded(1, [&] { return check_memory_limit_impl(thd, var, save, value); });
}

int check_threads(MYSQL_THD thd, SYS_VAR *var, void *save, st_mysql_value *value) {
  return guarded(1, [&] { return check_threads_impl(thd, var, save, value); });
}

int check_executor_threads(MYSQL_THD thd, SYS_VAR *var, void *save, st_mysql_value *value) {
  return guarded(1, [&] { return check_executor_threads_impl(thd, var, save, value); });
}

MYSQL_SYSVAR_BOOL(enable, srv_enable, PLUGIN_VAR_OPCMDARG,
                  "Enable the DuckDB secondary engine. When OFF the server "
                  "behaves as if the plugin were not loaded.",
                  check_enable, nullptr, true);

MYSQL_SYSVAR_BOOL(lake_relocate, srv_lake_relocate, PLUGIN_VAR_OPCMDARG,
                  "When the lake catalog stores another data path than "
                  "duckdb_data_path (the lake was moved or restored "
                  "elsewhere): ON accepts it if every data file the catalog "
                  "uses is under duckdb_data_path and updates the stored "
                  "path; OFF refuses to open the lake. Used when the lake "
                  "opens (startup, duckdb_enable=ON).",
                  nullptr, nullptr, true);

MYSQL_SYSVAR_STR(data_path, srv_data_path,
                 PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG |
                     PLUGIN_VAR_MEMALLOC,
                 "Directory of the DuckDB lake. Empty means "
                 "<datadir>/duckdb_data; a relative path is resolved against "
                 "the datadir.",
                 nullptr, nullptr, "");

MYSQL_SYSVAR_ULONGLONG(memory_limit, srv_memory_limit, PLUGIN_VAR_RQCMDARG,
                       "Memory limit of DuckDB in bytes, at least 256 MiB. "
                       "Mandatory: the plugin does not load without it.",
                       check_memory_limit, nullptr, 0, 0, ULLONG_MAX, 0);

MYSQL_SYSVAR_UINT(threads, srv_threads, PLUGIN_VAR_RQCMDARG,
                  "Number of DuckDB worker threads, from 1 to the number of "
                  "cores. Mandatory: the plugin does not load without it.",
                  check_threads, nullptr, 0, 0, 65536, 0);

MYSQL_SYSVAR_BOOL(encryption, srv_encryption,
                  PLUGIN_VAR_READONLY | PLUGIN_VAR_OPCMDARG,
                  "Encrypt the DuckDB lake. Not supported in this version.",
                  nullptr, nullptr, false);

// The update callback is cheap: it only sets a flag of the reader.
void update_apply_debug_log(MYSQL_THD, SYS_VAR *, void *var_ptr,
                            const void *save) {
  const bool on = *static_cast<const bool *>(save);
  *static_cast<bool *>(var_ptr) = on;
  BinlogReader::instance().set_debug_log(on);
}

MYSQL_SYSVAR_BOOL(apply_debug_log, srv_apply_debug_log, PLUGIN_VAR_OPCMDARG,
                  "Write one error log line per change decoded by the binlog "
                  "applier, and per table of each batch committed to DuckDB "
                  "with its row counts (testing aid).",
                  nullptr, update_apply_debug_log, false);

void update_apply_batch_max_trx(MYSQL_THD, SYS_VAR *, void *var_ptr,
                                const void *save) {
  const unsigned long long n = *static_cast<const unsigned long long *>(save);
  *static_cast<unsigned long long *>(var_ptr) = n;
  BinlogReader::instance().set_batch_max_trx(n);
}

void update_apply_batch_max_rows(MYSQL_THD, SYS_VAR *, void *var_ptr,
                                 const void *save) {
  const unsigned long long n = *static_cast<const unsigned long long *>(save);
  *static_cast<unsigned long long *>(var_ptr) = n;
  BinlogReader::instance().set_batch_max_rows(n);
}

void update_apply_batch_max_delay_ms(MYSQL_THD, SYS_VAR *, void *var_ptr,
                                     const void *save) {
  const unsigned long long n = *static_cast<const unsigned long long *>(save);
  *static_cast<unsigned long long *>(var_ptr) = n;
  BinlogReader::instance().set_batch_max_delay_ms(n);
}

MYSQL_SYSVAR_ULONGLONG(apply_batch_max_trx, srv_apply_batch_max_trx,
                       PLUGIN_VAR_RQCMDARG,
                       "The binlog applier commits a batch to DuckDB after "
                       "this many source transactions that change loaded "
                       "tables.",
                       nullptr, update_apply_batch_max_trx, 1000, 1, 1000000,
                       0);

MYSQL_SYSVAR_ULONGLONG(apply_batch_max_rows, srv_apply_batch_max_rows,
                       PLUGIN_VAR_RQCMDARG,
                       "The binlog applier commits a batch to DuckDB after "
                       "this many changed rows. A larger source transaction "
                       "is written in pieces of this many rows, in one "
                       "DuckDB transaction.",
                       nullptr, update_apply_batch_max_rows, 100000, 1000,
                       100000000, 0);

MYSQL_SYSVAR_ULONGLONG(apply_batch_max_delay_ms, srv_apply_batch_max_delay_ms,
                       PLUGIN_VAR_RQCMDARG,
                       "The binlog applier commits a batch to DuckDB at most "
                       "this many milliseconds after its first change.",
                       nullptr, update_apply_batch_max_delay_ms, 1000, 10,
                       60000, 0);

MYSQL_SYSVAR_STR(apply_start_position, srv_apply_start_position,
                 PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC |
                     PLUGIN_VAR_NOPERSIST,
                 "Setting it restarts the DuckDB binlog applier at the start "
                 "of a binary log: a name from SHOW BINARY LOGS, FIRST (the "
                 "oldest available) or CURRENT (the current end). Positions "
                 "before the applied position are refused.",
                 check_apply_start_position, nullptr, "");

void update_apply_gap_action(MYSQL_THD, SYS_VAR *, void *var_ptr,
                             const void *save) {
  const unsigned long v = *static_cast<const unsigned long *>(save);
  *static_cast<unsigned long *>(var_ptr) = v;
  BinlogReader::instance().set_gap_first_available(v == 1);
}

MYSQL_THDVAR_ENUM(history_scope,
                  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_HINTUPDATEABLE,
                  "Which rows of history tables queries see once they run in "
                  "DuckDB (DWL#6): LIVE (the rows InnoDB has) or FULL (also "
                  "the archived rows). FULL requires the "
                  "DUCKDB_HISTORY_READER privilege. Also a SET_VAR hint.",
                  check_history_scope, nullptr, 0, &history_scope_typelib);

MYSQL_THDVAR_BOOL(offload_wait_applied, PLUGIN_VAR_OPCMDARG,
                  "Before a query runs in DuckDB, wait until the binlog "
                  "applier has applied the session's last commit (at most 5 "
                  "s; otherwise the query runs on InnoDB), so the session "
                  "reads its own writes.",
                  nullptr, nullptr, true);

MYSQL_THDVAR_ULONGLONG(offload_min_rows, PLUGIN_VAR_OPCMDARG,
                       "Besides secondary_engine_cost_threshold, a query is "
                       "tried in DuckDB when MySQL's plan reads a loaded "
                       "table of at least this many rows by a full scan, a "
                       "full index scan or a non-unique index lookup (row "
                       "estimates from sampled statistics). 0 disables the "
                       "rule.",
                       nullptr, nullptr, 1000000, 0, ULLONG_MAX, 0);

MYSQL_THDVAR_BOOL(history_check_only, PLUGIN_VAR_OPCMDARG,
                  "With ON, ALTER TABLE ... SECONDARY_LOAD of a history table "
                  "only checks that it can take over a history copy "
                  "(adopt_history_from, reattach, resync): the structure must "
                  "be the same. It loads nothing and always ends with an "
                  "error that gives the result.",
                  nullptr, nullptr, false);

MYSQL_THDVAR_BOOL(unload_history_confirm, PLUGIN_VAR_OPCMDARG,
                  "With ON, SECONDARY_UNLOAD of a history table and "
                  "duckdb_discard_stale_copy delete its archived rows, which "
                  "exist only in the lake. History is never deleted without "
                  "it.",
                  nullptr, nullptr, false);

MYSQL_SYSVAR_STR(discard_stale_copy, srv_discard_stale_copy,
                 PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC |
                     PLUGIN_VAR_NOPERSIST,
                 "Setting it to 'db.table' drops the stale DuckDB copy of a "
                 "table that no longer exists (after DROP TABLE). History "
                 "copies require duckdb_unload_history_confirm=ON.",
                 check_discard_stale_copy, nullptr, "");

MYSQL_SYSVAR_ENUM(apply_gap_action, srv_apply_gap_action, PLUGIN_VAR_RQCMDARG,
                  "What the DuckDB binlog applier does when binary logs it "
                  "needs were purged and loaded tables miss their changes: "
                  "STOP (error state) or FIRST_AVAILABLE (warn and continue "
                  "at the first available binary log).",
                  nullptr, update_apply_gap_action, 0, &gap_action_typelib);

MYSQL_SYSVAR_UINT(executor_threads, srv_executor_threads, PLUGIN_VAR_RQCMDARG,
                  "Threads that run DuckDB calls, from 1 to the number of "
                  "cores. DuckDB never runs on MySQL connection threads.",
                  check_executor_threads, nullptr, 4, 1, 65536, 0);

MYSQL_SYSVAR_ULONGLONG(executor_stack_size, srv_executor_stack_size,
                       PLUGIN_VAR_READONLY | PLUGIN_VAR_RQCMDARG,
                       "Stack size in bytes of the DuckDB executor threads, "
                       "from 2 MiB to 64 MiB.",
                       nullptr, nullptr, 8ULL << 20, kMinStack, kMaxStack, 0);

SYS_VAR *duckdb_system_variables[] = {MYSQL_SYSVAR(enable),
                                      MYSQL_SYSVAR(lake_relocate),
                                      MYSQL_SYSVAR(data_path),
                                      MYSQL_SYSVAR(memory_limit),
                                      MYSQL_SYSVAR(threads),
                                      MYSQL_SYSVAR(encryption),
                                      MYSQL_SYSVAR(executor_threads),
                                      MYSQL_SYSVAR(executor_stack_size),
                                      MYSQL_SYSVAR(apply_debug_log),
                                      MYSQL_SYSVAR(apply_batch_max_trx),
                                      MYSQL_SYSVAR(apply_batch_max_rows),
                                      MYSQL_SYSVAR(apply_batch_max_delay_ms),
                                      MYSQL_SYSVAR(apply_start_position),
                                      MYSQL_SYSVAR(apply_gap_action),
                                      MYSQL_SYSVAR(history_check_only),
                                      MYSQL_SYSVAR(unload_history_confirm),
                                      MYSQL_SYSVAR(discard_stale_copy),
                                      MYSQL_SYSVAR(history_scope),
                                      MYSQL_SYSVAR(offload_wait_applied),
                                      MYSQL_SYSVAR(offload_min_rows),
                                      nullptr};

int show_executor_busy(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->scope = SHOW_SCOPE_GLOBAL;
  var->value = buff;
  *reinterpret_cast<long *>(buff) =
      static_cast<long>(Executor::instance().busy());
  return 0;
}

int show_executor_queue(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONG;
  var->scope = SHOW_SCOPE_GLOBAL;
  var->value = buff;
  *reinterpret_cast<long *>(buff) =
      static_cast<long>(Executor::instance().queue_length());
  return 0;
}

int show_text(SHOW_VAR *var, char *buff, const std::string &value) {
  var->type = SHOW_CHAR;
  var->scope = SHOW_SCOPE_GLOBAL;
  var->value = buff;
  snprintf(buff, SHOW_VAR_FUNC_BUFF_SIZE, "%s", value.c_str());
  return 0;
}

int show_number(SHOW_VAR *var, char *buff, unsigned long long value) {
  var->type = SHOW_LONGLONG;
  var->scope = SHOW_SCOPE_GLOBAL;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) = static_cast<longlong>(value);
  return 0;
}

int show_reader_state(THD *, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, safe_status().state);
}

int show_reader_file(THD *, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, safe_status().position.file);
}

int show_reader_position(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff,
                     safe_status().position.pos);
}

int show_transactions_read(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().transactions);
}

int show_row_events_read(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().row_events);
}

int show_rows_decoded(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().rows_decoded);
}

int show_changes(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().changes);
}

int show_last_gtid(THD *, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, safe_status().last_gtid);
}

int show_batches(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().batches);
}

int show_rows_written(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().rows_written);
}

int show_large_transactions(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().large_transactions);
}

int show_pieces(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().pieces);
}

int show_ddl_refreshes(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().ddl_refreshes);
}

int show_ddl_renames(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().ddl_renames);
}

int show_ddl_failures(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().ddl_failures);
}

int show_archived_rows(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().archived_rows);
}

int show_purge_transactions(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().purge_transactions);
}

int show_guard_forced(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, guard_status().forced);
}

int show_guard_rejected(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, guard_status().rejected);
}

int show_offloaded(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, offload_status().offloaded);
}

int show_offload_refused(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, offload_status().refused);
}

int show_offload_last_refusal(THD *, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, offload_status().last_refusal);
}

int show_ddl_deferred(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().ddl_deferred);
}

int show_applied_file(THD *, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, safe_status().applied.file);
}

int show_applied_position(THD *, SHOW_VAR *var, char *buff) {
  return show_number(var, buff, safe_status().applied.pos);
}

int show_applied_gtid(THD *, SHOW_VAR *var, char *buff) {
  return show_text(var, buff, safe_status().applied_gtid);
}

int show_executor_tasks(THD *, SHOW_VAR *var, char *buff) {
  var->type = SHOW_LONGLONG;
  var->scope = SHOW_SCOPE_GLOBAL;
  var->value = buff;
  *reinterpret_cast<longlong *>(buff) =
      static_cast<longlong>(Executor::instance().tasks());
  return 0;
}

SHOW_VAR duckdb_status_variables[] = {
    {"duckdb_enabled", reinterpret_cast<char *>(&status_enabled), SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_lake_path", status_lake_path, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
    {"duckdb_executor_threads_busy",
     reinterpret_cast<char *>(&show_executor_busy), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_executor_queue_length",
     reinterpret_cast<char *>(&show_executor_queue), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_executor_tasks", reinterpret_cast<char *>(&show_executor_tasks),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_reader_state", reinterpret_cast<char *>(&show_reader_state),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_reader_file", reinterpret_cast<char *>(&show_reader_file),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_reader_position",
     reinterpret_cast<char *>(&show_reader_position), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_transactions_read",
     reinterpret_cast<char *>(&show_transactions_read), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_row_events_read",
     reinterpret_cast<char *>(&show_row_events_read), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_last_gtid", reinterpret_cast<char *>(&show_last_gtid),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_rows_decoded",
     reinterpret_cast<char *>(&show_rows_decoded), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_changes", reinterpret_cast<char *>(&show_changes),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_batches", reinterpret_cast<char *>(&show_batches),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_rows_written", reinterpret_cast<char *>(&show_rows_written),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_large_transactions",
     reinterpret_cast<char *>(&show_large_transactions), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_pieces", reinterpret_cast<char *>(&show_pieces), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_ddl_refreshes",
     reinterpret_cast<char *>(&show_ddl_refreshes), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_ddl_renames", reinterpret_cast<char *>(&show_ddl_renames),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_ddl_failures",
     reinterpret_cast<char *>(&show_ddl_failures), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_ddl_deferred",
     reinterpret_cast<char *>(&show_ddl_deferred), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_offload_queries", reinterpret_cast<char *>(&show_offloaded),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_guard_forced", reinterpret_cast<char *>(&show_guard_forced),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_guard_rejected", reinterpret_cast<char *>(&show_guard_rejected),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_offload_refused", reinterpret_cast<char *>(&show_offload_refused),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_offload_last_refusal",
     reinterpret_cast<char *>(&show_offload_last_refusal), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_archived_rows",
     reinterpret_cast<char *>(&show_archived_rows), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_apply_purge_transactions",
     reinterpret_cast<char *>(&show_purge_transactions), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},

    {"duckdb_applied_file", reinterpret_cast<char *>(&show_applied_file),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"duckdb_applied_position",
     reinterpret_cast<char *>(&show_applied_position), SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"duckdb_applied_gtid", reinterpret_cast<char *>(&show_applied_gtid),
     SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_LONG, SHOW_SCOPE_GLOBAL}};

// clean_up() destroys the binary log (mysql_bin_log.cleanup()) before
// plugin_shutdown(), on every shutdown and on a startup abort. ha_binlog_end()
// calls us just before: the reader must stop now, not at plugin deinit.
int duckdb_binlog_func(handlerton *, THD *, enum_binlog_func fn, void *) {
  if (fn != BFN_BINLOG_END) return 0;
  try {
    BinlogReader::instance().binlog_end();
  } catch (...) {
    // Never fail the server shutdown.
  }
  return 0;
}


handler *create_handler(handlerton *hton, TABLE_SHARE *table_share, bool,
                        MEM_ROOT *mem_root) {
  return new (mem_root) ha_duckdb(hton, table_share);
}

uint partition_flags() {
  return HA_CAN_EXCHANGE_PARTITION | HA_CANNOT_PARTITION_FK |
         HA_TRUNCATE_PARTITION_PRECLOSE;
}

// Unregisters DUCKDB_HISTORY_READER and releases the service; safe to call
// when nothing was acquired.
void release_privilege() {
  if (privilege_service == nullptr) return;
  if (privilege_registered)
    privilege_service->unregister_privilege(
        STRING_WITH_LEN(kHistoryReaderPrivilege));
  privilege_registered = false;
  if (reg_srv != nullptr)
    reg_srv->release(reinterpret_cast<my_h_service>(
        const_cast<SERVICE_TYPE_NO_CONST(dynamic_privilege_register) *>(
            privilege_service)));
  privilege_service = nullptr;
}

int duckdb_init_impl(MYSQL_PLUGIN p) {
  if (init_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs)) return 1;
  Executor::register_psi();
  BinlogReader::register_psi();
  auto fail = [] {
    release_privilege();
    unregister_offload_observer(plugin_handle);
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
    return 1;
  };

  if (srv_memory_limit == 0) {
    DUCKDB_LOG_ERROR(
        "duckdb_memory_limit is not set; add it to the configuration file "
        "(use the loose- prefix). Plugin not loaded.");
    return fail();
  }
  if (srv_memory_limit < kMinMemoryLimit) {
    DUCKDB_LOG_ERROR(
        "duckdb_memory_limit must be at least %llu bytes. Plugin not loaded.",
        kMinMemoryLimit);
    return fail();
  }
  if (srv_threads == 0) {
    DUCKDB_LOG_ERROR(
        "duckdb_threads is not set; add it to the configuration file (use "
        "the loose- prefix). Plugin not loaded.");
    return fail();
  }
  if (srv_threads > max_threads()) {
    DUCKDB_LOG_ERROR(
        "duckdb_threads must be between 1 and %u (the number of cores). "
        "Plugin not loaded.",
        max_threads());
    return fail();
  }
  if (srv_executor_threads > max_threads()) {
    // The default (4) can exceed the cores of a small machine: we lower it
    // instead of refusing to load.
    DUCKDB_LOG_WARNING(
        "duckdb_executor_threads=%u is above the number of cores; using %u",
        srv_executor_threads, max_threads());
    srv_executor_threads = max_threads();
  }
  {
    my_h_service handle = nullptr;
    if (reg_srv->acquire("dynamic_privilege_register", &handle) != 0) {
      DUCKDB_LOG_ERROR(
          "cannot acquire the dynamic privilege service. Plugin not loaded.");
      return fail();
    }
    privilege_service =
        reinterpret_cast<SERVICE_TYPE(dynamic_privilege_register) *>(handle);
    if (privilege_service->register_privilege(
            STRING_WITH_LEN(kHistoryReaderPrivilege))) {
      DUCKDB_LOG_ERROR("cannot register the %s privilege. Plugin not loaded.",
                       kHistoryReaderPrivilege);
      return fail();
    }
    privilege_registered = true;
  }
  if (srv_encryption) {
    DUCKDB_LOG_ERROR(
        "duckdb_encryption=ON is not supported in this version. Plugin not "
        "loaded.");
    return fail();
  }

  handlerton *hton = static_cast<handlerton *>(p);
  hton->create = create_handler;
  hton->state = SHOW_OPTION_YES;
  // DDL on tables with SECONDARY_ENGINE=duckdb is never refused, locally or
  // from replication: the binlog applier refreshes the copies (DWL#4 4d).
  hton->flags = HTON_IS_SECONDARY_ENGINE | HTON_SECONDARY_ENGINE_SUPPORTS_DDL;
  hton->db_type = DB_TYPE_UNKNOWN;
  // Query offload (DWL#6): hooks and the external executor flag.
  install_offload_hooks(hton);
  hton->partition_flags = partition_flags;
  hton->binlog_func = duckdb_binlog_func;

  plugin_handle = own_plugin();
  if (plugin_handle == nullptr || register_offload_observer(plugin_handle)) {
    DUCKDB_LOG_ERROR(
        "cannot register the transaction observer. Plugin not loaded.");
    return fail();
  }
  // The reader runs internal statements (refresh after DDL) as this plugin.
  set_session_plugin(plugin_handle);

  if (srv_enable) {
    Lake &lake = Lake::instance();
    std::lock_guard<std::mutex> guard(lake.mutex());
    std::string err;
    if (open_engine(&err)) {
      DUCKDB_LOG_ERROR("%s. Plugin not loaded.", err.c_str());
      return fail();
    }
  } else {
    DUCKDB_LOG_INFO(
        "duckdb_enable=OFF: the DuckDB secondary engine is inactive");
  }
  refresh_status();
  return 0;
}

int duckdb_deinit_impl(MYSQL_PLUGIN) {
  {
    Lake &lake = Lake::instance();
    std::lock_guard<std::mutex> guard(lake.mutex());
    close_engine();
    refresh_status();
  }
  unregister_offload_observer(plugin_handle);
  release_privilege();
  deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
  return 0;
}

// After an exception in init we undo whatever init had set up. Each step is
// safe when the corresponding part was never started.
void cleanup_failed_init() {
  try {
    std::lock_guard<std::mutex> guard(Lake::instance().mutex());
    close_engine();
  } catch (...) {
  }
  unregister_offload_observer(plugin_handle);
  release_privilege();
  if (log_bi != nullptr)
    deinit_logging_service_for_plugin(&reg_srv, &log_bi, &log_bs);
}

// Errors here go to the error log: there is no client session.
int duckdb_init(MYSQL_PLUGIN p) {
  try {
    return duckdb_init_impl(p);
  } catch (const std::exception &e) {
    if (log_bi != nullptr)
      DUCKDB_LOG_ERROR("internal error at startup: %s. Plugin not loaded.",
                       e.what());
  } catch (...) {
    if (log_bi != nullptr)
      DUCKDB_LOG_ERROR("internal error at startup. Plugin not loaded.");
  }
  cleanup_failed_init();
  return 1;
}

int duckdb_deinit(MYSQL_PLUGIN p) {
  try {
    return duckdb_deinit_impl(p);
  } catch (...) {
    return 1;
  }
}

}  // namespace

bool history_check_only(THD *thd) {
  return thd != nullptr && THDVAR(thd, history_check_only);
}

unsigned long long offload_min_rows(THD *thd) {
  return thd != nullptr ? THDVAR(thd, offload_min_rows) : 0;
}

bool offload_wait_applied(THD *thd) {
  return thd != nullptr && THDVAR(thd, offload_wait_applied);
}

bool history_scope_full(THD *thd) {
  return thd != nullptr && THDVAR(thd, history_scope) == 1;
}

bool unload_history_confirmed(THD *thd) {
  return thd != nullptr && THDVAR(thd, unload_history_confirm);
}

ha_duckdb::ha_duckdb(handlerton *hton, TABLE_SHARE *table_share_arg)
    : handler(hton, table_share_arg), m_part_handler(this) {}

int ha_duckdb::open(const char *, int, unsigned int, const dd::Table *) {
  return guarded(HA_ERR_GENERIC, [&] {
    // A table the data dictionary marks as loaded but that is missing from
    // the lake, or is stale, is treated as not loaded: queries run on InnoDB.
    // The pointer is safe here: the table is open under a metadata lock that
    // conflicts with an unload.
    LoadedTable *t = Registry::instance().find(table_share->db.str,
                                               table_share->table_name.str);
    if (!srv_enable || t == nullptr || t->stale || t->refresh_pending) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "Table has not been loaded");
      return HA_ERR_GENERIC;
    }
    thr_lock_data_init(&t->lock, &m_lock, nullptr);
    return 0;
  });
}

bool ha_duckdb::get_error_message(int error, String *buf) {
  if (error == kErrNoOffload) {
    // ER_GET_ERRMSG shows at most 100 characters.
    const char *msg =
        "query offload is not implemented in this version: use "
        "use_secondary_engine=ON or OFF";
    buf->copy(msg, strlen(msg), system_charset_info);
  }
  return false;  // not a temporary error
}

int ha_duckdb::info(unsigned int flags) {
  // We take the cardinality statistics from the primary engine.
  handler *primary = ha_get_primary_handler();
  const int ret = primary->info(flags);
  if (ret == 0) stats.records = primary->stats.records;
  return ret;
}

handler::Table_flags ha_duckdb::table_flags() const {
  // No index access; indexes are only used for cost estimates.
  return HA_NO_INDEX_ACCESS;
}

unsigned long ha_duckdb::index_flags(unsigned int idx, unsigned int part,
                                     bool all_parts) const {
  const handler *primary = ha_get_primary_handler();
  const unsigned long primary_flags =
      primary == nullptr ? 0 : primary->index_flags(idx, part, all_parts);
  // We inherit only the flags used for range estimates (same as MOCK).
  return (HA_READ_RANGE | HA_KEY_SCAN_NOT_ROR) & primary_flags;
}

ha_rows ha_duckdb::records_in_range(unsigned int index, key_range *min_key,
                                    key_range *max_key) {
  return ha_get_primary_handler()->records_in_range(index, min_key, max_key);
}

THR_LOCK_DATA **ha_duckdb::store_lock(THD *, THR_LOCK_DATA **to,
                                      thr_lock_type lock_type) {
  if (lock_type != TL_IGNORE && m_lock.type == TL_UNLOCK)
    m_lock.type = lock_type;
  *to++ = &m_lock;
  return to;
}

int ha_duckdb::load_table(const TABLE &table_arg, bool *skip_metadata_update) {
  if (!srv_enable) {
    // We behave as when the engine is not available: a warning, and the data
    // dictionary is left unchanged.
    THD *thd = current_thd;
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_UNKNOWN_STORAGE_ENGINE,
                        ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE),
                        table_arg.s->secondary_engine.str);
    *skip_metadata_update = true;
    return 0;
  }
  return guarded(HA_ERR_GENERIC, [&] {
    return load_table_into_lake(current_thd, table_arg) ? HA_ERR_GENERIC : 0;
  });
}

int ha_duckdb::unload_table(const char *db_name, const char *table_name,
                            bool error_if_not_loaded) {
  // While disabled we never touch the lake.
  if (!srv_enable) return 0;
  // The server passes error_if_not_loaded=true only for SECONDARY_UNLOAD; DDL
  // passes false (sql/sql_table.cc).
  return guarded(1, [&] {
    return unload_table_from_lake(current_thd, db_name, table_name,
                                  error_if_not_loaded)
               ? 1
               : 0;
  });
}

}  // namespace duckdb_se

static st_mysql_storage_engine duckdb_storage_engine{
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(duckdb){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &duckdb_storage_engine,
    "duckdb",
    "Marco Tusa",
    "DuckDB secondary engine",
    PLUGIN_LICENSE_GPL,
    duckdb_se::duckdb_init,
    nullptr,
    duckdb_se::duckdb_deinit,
    0x0001,
    duckdb_se::duckdb_status_variables,
    duckdb_se::duckdb_system_variables,
    nullptr,
    0,
},
    // DWL#7: keeps FULL scope queries off InnoDB.
    {
        MYSQL_AUDIT_PLUGIN,
        &duckdb_se::guard_descriptor,
        "duckdb_guard",
        "Marco Tusa",
        "Keeps queries that read history tables under "
        "duckdb_history_scope=FULL in the DuckDB secondary engine",
        PLUGIN_LICENSE_GPL,
        duckdb_se::guard_init,
        nullptr,
        duckdb_se::guard_deinit,
        0x0001,
        nullptr,
        nullptr,
        nullptr,
        0,
    } mysql_declare_plugin_end;
