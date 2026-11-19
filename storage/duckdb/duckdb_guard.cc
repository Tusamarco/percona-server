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

#include "storage/duckdb/duckdb_guard.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

#include "mysql/plugin_audit.h"
#include "mysqld_error.h"
#include "sql/sql_class.h"
#include "sql/sql_alter.h"  // Alter_info
#include "sql/sql_lex.h"
#include "sql/sql_parse.h"  // sql_command_flags
#include "sql/table.h"      // Table_ref
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_load.h"  // history_scope_full
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_offload.h"   // ddl_wait_refresh
#include "storage/duckdb/duckdb_progress.h"  // wait_for_applier
#include "storage/duckdb/duckdb_registry.h"
#include "storage/duckdb/duckdb_session.h"  // is_internal_session

namespace duckdb_se {

namespace {

std::atomic<bool> g_active{false};
std::atomic<unsigned long long> g_forced{0};
std::atomic<unsigned long long> g_rejected{0};

// The statements (LEX) the guard marked secondary-only, per connection: the
// server never clears the mark, so a prepared statement marked under FULL
// would stay marked under LIVE; we clear our own mark then.
std::mutex g_marks_mutex;
std::unordered_map<const THD *, const LEX *> g_marks;

int stop(THD *, const std::string &msg) {
  g_rejected++;
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), msg.c_str());
  return 1;
}

// Table access events (sql/sql_audit.cc, mysql_event_tracking_table_access_
// notify): SELECT raises READ for every table; UPDATE raises UPDATE for the
// updated tables and READ for the others; DELETE (and TRUNCATE) raise DELETE
// for every table of the statement, including tables read in a subquery;
// INSERT ... SELECT raises INSERT for the target and READ for the others.
// UPDATE and DELETE first read the rows they change (WHERE, join), and that
// read sees live rows only, so under FULL they are stopped like any other
// statement that reads a history table and is not a SELECT. A plain INSERT
// reads nothing and is not checked.
int on_table_access(THD *thd, const mysql_event_table_access *ev) {
  // Replication applier and other system threads read InnoDB as they must.
  if (thd->system_thread != NON_SYSTEM_THREAD) return 0;
  const std::string db(ev->table_database.str, ev->table_database.length);
  const std::string table(ev->table_name.str, ev->table_name.length);
  TableState state;
  if (!Registry::instance().snapshot(db, table, &state) ||
      !state.retain_history)
    return 0;
  LEX *lex = thd->lex;
  if (!history_scope_full(thd)) {
    // A statement we marked before (prepared statement) runs under LIVE now.
    std::lock_guard<std::mutex> guard(g_marks_mutex);
    auto it = g_marks.find(thd);
    if (it != g_marks.end() && it->second == lex &&
        lex->can_execute_only_in_secondary_engine()) {
      lex->set_execute_only_in_secondary_engine(false, SUPPORTED_IN_PRIMARY);
      g_marks.erase(it);
    }
    return 0;
  }
  const std::string full = db + "." + table;
  if (ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_READ)
    return stop(thd, "duckdb_history_scope=FULL: " + full +
                         " is a history table; FULL is a read scope and an "
                         "UPDATE, DELETE or TRUNCATE would act on the live "
                         "rows only: change rows with "
                         "duckdb_history_scope=LIVE");
  // FULL scope reads archived rows: only an offloadable SELECT can.
  if (lex->sql_command != SQLCOM_SELECT)
    return stop(thd, "duckdb_history_scope=FULL: " + full +
                         " is a history table and only a SELECT can read it "
                         "with its archived rows (this statement also "
                         "writes, or is not a SELECT)");
  if (thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF)
    return stop(thd, "duckdb_history_scope=FULL requires DuckDB execution, "
                     "but use_secondary_engine=OFF (" +
                         full + " is a history table)");
  if (!Lake::instance().is_open())
    return stop(thd, "duckdb_history_scope=FULL: the DuckDB lake is not "
                     "open (duckdb_enable=OFF?): " +
                         full + " cannot be read with its archived rows");
  if (!state.usable())
    return stop(thd, "duckdb_history_scope=FULL: the DuckDB copy of " + full +
                         " is not usable (" +
                         (state.stale ? "stale" : "waiting for a refresh") +
                         "): its archived rows cannot be read");
  // Secondary-only: no cost threshold, no fallback to InnoDB; a refusal is
  // an error with our reason (find_secondary_engine_offload_fail_reason).
  if (!lex->can_execute_only_in_secondary_engine()) {
    lex->set_execute_only_in_secondary_engine(true, TEMPORARY_TABLE_USAGE);
    g_forced++;
    std::lock_guard<std::mutex> guard(g_marks_mutex);
    g_marks[thd] = lex;
  }
  return 0;
}

// The end of a DDL on a table with a DuckDB copy (duckdb_ddl_wait_refresh):
// the binlog applier refreshes the copy (an internal SECONDARY_LOAD) when it
// reads the statement, and the client gets its OK once that is done, so the
// next statements see the copy in DuckDB. The event comes after the DDL has
// committed (DDL commits inside its execution) and before the OK is sent, in
// the client's thread; the progress is in its SHOW PROCESSLIST state and
// Performance Schema stage. Nothing here can fail the DDL: it is committed.
void on_ddl_end(THD *thd, const mysql_event_query *ev) {
  if (ev->event_subclass != MYSQL_AUDIT_QUERY_STATUS_END || ev->status != 0)
    return;
  // The replica applier must not wait (replication would stall), nor our own
  // internal statements (the refresh itself).
  if (thd->system_thread != NON_SYSTEM_THREAD || is_internal_session(thd))
    return;
  LEX *lex = thd->lex;
  if (lex == nullptr ||
      (sql_command_flags[lex->sql_command] & CF_IMPLICIT_COMMIT_END) == 0)
    return;
  // SECONDARY_LOAD / UNLOAD are synchronous already and not in the binlog.
  if (lex->sql_command == SQLCOM_ALTER_TABLE && lex->alter_info != nullptr &&
      (lex->alter_info->flags & (Alter_info::ALTER_SECONDARY_LOAD |
                                 Alter_info::ALTER_SECONDARY_UNLOAD)) != 0)
    return;
  if (!ddl_wait_refresh(thd) || !Lake::instance().is_open()) return;
  // Only DDL on a table with a copy (RENAME lists both names).
  std::string copy;
  for (Table_ref *t = lex->query_tables; t != nullptr && copy.empty();
       t = t->next_global) {
    if (t->db == nullptr || t->table_name == nullptr) continue;
    TableState state;
    if (Registry::instance().snapshot(t->db, t->table_name, &state))
      copy = std::string(t->db) + "." + t->table_name;
  }
  if (copy.empty()) return;
  BinlogPos target;
  if (session_commit_position(thd, &target) || target.empty()) return;

  double waited = 0;
  std::string refreshed;
  unsigned long long rows = 0;
  const WaitResult r = wait_for_applier(
      thd, target, ddl_wait_refresh_timeout(thd) * 1000,
      ("refresh of the DuckDB copy of " + copy + " after this DDL").c_str(),
      &waited, &refreshed, &rows);
  switch (r) {
    case WaitResult::APPLIED:
      if (!refreshed.empty())
        push_warning_printf(
            thd, Sql_condition::SL_NOTE, ER_SECONDARY_ENGINE_PLUGIN,
            "DuckDB: the copy of %s was refreshed after this DDL (%llu "
            "rows); waited %.1f s",
            refreshed.c_str(), rows, waited);
      else if (waited >= 1)
        push_warning_printf(thd, Sql_condition::SL_NOTE,
                            ER_SECONDARY_ENGINE_PLUGIN,
                            "DuckDB: waited %.1f s for the binlog applier to "
                            "apply this DDL to the copy of %s",
                            waited, copy.c_str());
      break;
    case WaitResult::TIMEOUT:
    case WaitResult::KILLED:
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE_PLUGIN,
          "DuckDB: stopped waiting after %.1f s (%s); the DDL is committed "
          "and the copy of %s is still being refreshed: until then queries "
          "on it run on InnoDB (duckdb_history_scope=FULL: they wait)",
          waited,
          r == WaitResult::KILLED ? "KILL QUERY"
                                  : "duckdb_ddl_wait_refresh_timeout",
          copy.c_str());
      break;
    case WaitResult::NOT_RUNNING:
      push_warning_printf(
          thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE_PLUGIN,
          "DuckDB: the binlog applier is not running: the copy of %s is "
          "refreshed when it runs again (duckdb_apply_reader_state)",
          copy.c_str());
      break;
  }
}

int guard_notify(MYSQL_THD thd, mysql_event_class_t event_class,
                 const void *event) {
  if (thd == nullptr) return 0;
  if (event_class == MYSQL_AUDIT_QUERY_CLASS) {
    try {
      on_ddl_end(thd, static_cast<const mysql_event_query *>(event));
    } catch (...) {
      // Never affect the (committed) DDL.
    }
    return 0;
  }
  if (event_class == MYSQL_AUDIT_PARSE_CLASS) {
    // The plugin's internal statements (refresh after DDL) write only to
    // DuckDB and run with sql_log_bin=0: let them pass read_only and
    // super_read_only as the replication applier does
    // (check_readonly(), sql/auth/sql_authorization.cc). The server clears
    // the flag at the start of each statement (THD::reset_for_next_command);
    // pre-parse comes after that and before the check.
    const auto *ev = static_cast<const mysql_event_parse *>(event);
    try {
      if (ev->event_subclass == MYSQL_AUDIT_PARSE_PREPARSE &&
          is_internal_session(thd))
        thd->set_skip_readonly_check();
    } catch (...) {
    }
    return 0;
  }
  if (event_class != MYSQL_AUDIT_TABLE_ACCESS_CLASS) return 0;
  const auto *ev = static_cast<const mysql_event_table_access *>(event);
  if (ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_READ &&
      ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_UPDATE &&
      ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_DELETE)
    return 0;
  try {
    return on_table_access(thd, ev);
  } catch (...) {
    // Never let an exception reach the server; refuse the statement.
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "duckdb_guard: internal error while checking the statement");
    return 1;
  }
}

}  // namespace

st_mysql_audit guard_descriptor = {
    MYSQL_AUDIT_INTERFACE_VERSION,
    nullptr,
    guard_notify,
    {0, 0, static_cast<unsigned long>(MYSQL_AUDIT_PARSE_PREPARSE), 0,
     static_cast<unsigned long>(MYSQL_AUDIT_TABLE_ACCESS_READ |
                                MYSQL_AUDIT_TABLE_ACCESS_UPDATE |
                                MYSQL_AUDIT_TABLE_ACCESS_DELETE),
     0, 0, 0, 0, static_cast<unsigned long>(MYSQL_AUDIT_QUERY_STATUS_END), 0,
     0}};

int guard_init(MYSQL_PLUGIN) {
  g_active = true;
  return 0;
}

int guard_deinit(MYSQL_PLUGIN) {
  g_active = false;
  return 0;
}

bool guard_active() { return g_active; }

void guard_forget(THD *thd) {
  try {
    std::lock_guard<std::mutex> guard(g_marks_mutex);
    g_marks.erase(thd);
  } catch (...) {
  }
}

GuardStatus guard_status() {
  GuardStatus s;
  s.forced = g_forced;
  s.rejected = g_rejected;
  return s;
}

}  // namespace duckdb_se
