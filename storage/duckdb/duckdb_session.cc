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

#include "storage/duckdb/duckdb_session.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "m_string.h"
#include "my_thread.h"  // my_thread_end
#include "mysql/service_command.h"
#include "mysql/service_srv_session.h"
#include "mysql/service_srv_session_info.h"
#include "mysql/strings/m_ctype.h"
#include "sql/auth/sql_security_ctx.h"
#include "sql/sql_audit.h"  // mysql_audit_enable_auditing
#include "sql/sql_class.h"

namespace duckdb_se {

namespace {

std::atomic<const void *> g_plugin{nullptr};

// Internal sessions open now (is_internal_session). The count lets the
// guard skip the lookup for every other statement.
std::atomic<int> g_internal_count{0};
std::mutex g_internal_mutex;
std::unordered_set<const THD *> g_internal;

void register_internal(const THD *thd, bool add) {
  std::lock_guard<std::mutex> lk(g_internal_mutex);
  if (add) {
    g_internal.insert(thd);
    g_internal_count++;
  } else if (g_internal.erase(thd) > 0) {
    g_internal_count--;
  }
}

// Result of one statement. We need only success or the error.
struct Outcome {
  bool ok{false};
  unsigned error{0};
  std::string message;
};

int cb_start_metadata(void *, uint, uint, const CHARSET_INFO *) { return 0; }
int cb_field_metadata(void *, struct st_send_field *, const CHARSET_INFO *) {
  return 0;
}
int cb_end_metadata(void *, uint, uint) { return 0; }
int cb_start_row(void *) { return 0; }
int cb_end_row(void *) { return 0; }
void cb_abort_row(void *) {}
ulong cb_capabilities(void *) { return 0; }
int cb_null(void *) { return 0; }
int cb_integer(void *, longlong) { return 0; }
int cb_longlong(void *, longlong, uint) { return 0; }
int cb_decimal(void *, const decimal_t *) { return 0; }
int cb_double(void *, double, uint32_t) { return 0; }
int cb_date(void *, const MYSQL_TIME *) { return 0; }
int cb_time(void *, const MYSQL_TIME *, uint) { return 0; }
int cb_datetime(void *, const MYSQL_TIME *, uint) { return 0; }
int cb_string(void *, const char *, size_t, const CHARSET_INFO *) { return 0; }
void cb_ok(void *ctx, uint, uint, ulonglong, ulonglong, const char *) {
  static_cast<Outcome *>(ctx)->ok = true;
}
void cb_error(void *ctx, uint sql_errno, const char *msg, const char *) {
  auto *o = static_cast<Outcome *>(ctx);
  o->ok = false;
  o->error = sql_errno;
  o->message = msg != nullptr ? msg : "";
}
void cb_shutdown(void *, int) {}
bool cb_alive(void *) { return true; }

const st_command_service_cbs callbacks = {
    cb_start_metadata, cb_field_metadata, cb_end_metadata, cb_start_row,
    cb_end_row,        cb_abort_row,      cb_capabilities, cb_null,
    cb_integer,        cb_longlong,       cb_decimal,      cb_double,
    cb_date,           cb_time,           cb_datetime,     cb_string,
    cb_ok,             cb_error,          cb_shutdown,     cb_alive};

bool run_one(Srv_session *session, const std::string &sql, std::string *err,
             unsigned *sql_errno) {
  Outcome outcome;
  COM_DATA cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.com_query.query = sql.c_str();
  cmd.com_query.length = static_cast<unsigned>(sql.size());
  const int rc = command_service_run_command(
      session, COM_QUERY, &cmd, &my_charset_utf8mb4_general_ci, &callbacks,
      CS_TEXT_REPRESENTATION, &outcome);
  if (rc == 0 && outcome.ok) return false;
  *sql_errno = outcome.error;
  *err = outcome.message.empty()
             ? "internal statement failed: " + sql
             : outcome.message + " (error " + std::to_string(outcome.error) +
                   ")";
  return true;
}

}  // namespace

void set_session_plugin(const void *plugin) { g_plugin = plugin; }

bool session_thread_attach() {
  const void *plugin = g_plugin.load();
  if (plugin == nullptr) return true;
  // my_thread_init() inside returns at once if the thread is initialized.
  return srv_session_init_thread(plugin) != 0;
}

void session_thread_end(bool attached) {
  if (attached)
    srv_session_deinit_thread();  // includes my_thread_end()
  else
    my_thread_end();
}

bool run_internal_statements(const std::vector<std::string> &statements,
                             std::string *err, unsigned *sql_errno) {
  *sql_errno = 0;
  if (!srv_session_server_is_available()) {
    *err = "the server cannot run internal statements now";
    return true;
  }
  Srv_session *session = srv_session_open(nullptr, nullptr);
  if (session == nullptr) {
    *err = "cannot open an internal session";
    return true;
  }
  // The replication applier's privileges without PRIVILEGE_CHECKS_USER.
  THD *thd = srv_session_info_get_thd(session);
  thd->security_context()->skip_grants();
  // Like the replication applier, not stopped by (super_)read_only: see
  // is_internal_session(). Session service THDs are created with auditing
  // off (sql/srv_session.cc), so no audit event would reach duckdb_guard:
  // turn it on for this session. Audit plugins (e.g. audit_log) then see
  // these internal statements too, as for any connection.
  mysql_audit_enable_auditing(thd);
  bool failed = false;
  try {
    register_internal(thd, true);
  } catch (...) {
    *err = "cannot register the internal session";
    failed = true;
  }
  if (!failed)
    failed = run_one(session, "SET SESSION sql_log_bin = 0", err, sql_errno);
  for (const std::string &sql : statements) {
    if (failed) break;
    failed = run_one(session, sql, err, sql_errno);
  }
  register_internal(thd, false);
  srv_session_close(session);
  return failed;
}

bool is_internal_session(const THD *thd) {
  if (g_internal_count.load(std::memory_order_acquire) == 0) return false;
  std::lock_guard<std::mutex> lk(g_internal_mutex);
  return g_internal.count(thd) > 0;
}

}  // namespace duckdb_se
