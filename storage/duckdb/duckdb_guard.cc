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
#include "sql/mysqld.h"  // opt_bin_log
#include "sql/query_options.h"  // OPTION_BIN_LOG
#include "sql/system_variables.h"  // binlog_format, binlog_row_image
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
    return stop(thd, "duckdb_history_scope=FULL: the DuckDB repository is not "
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

// T0: a DDL on a table with a DuckDB copy, and the statement after it.
//
// The binlog applier refreshes the copy (an internal SECONDARY_LOAD) when it
// reads the DDL. The DDL's own client thread cannot wait for that: until the
// statement ends it holds the table's metadata lock, which the refresh needs
// (a deadlock), and the server sends the OK right after releasing it, with
// no plugin event in between. So at the end of the DDL we record what to
// wait for, and the session's next statement waits at its start (pre-parse:
// no lock held yet), showing the progress in SHOW PROCESSLIST and the
// Performance Schema stage. Whatever the session runs after the DDL sees the
// refreshed copy (duckdb_ddl_wait_refresh).
struct PendingDdl {
  BinlogPos target;
  std::string copy;  // db.table, for the messages
};
std::mutex g_ddl_mutex;
std::unordered_map<const THD *, PendingDdl> g_pending_ddl;

// End of a statement (QUERY_STATUS_END): a committed DDL on a table with a
// copy is recorded for the next statement.
void on_statement_end(THD *thd, const mysql_event_query *ev) {
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
  std::lock_guard<std::mutex> g(g_ddl_mutex);
  g_pending_ddl[thd] = PendingDdl{target, copy};
}

// Start of the session's next statement: wait for the refresh of the copy
// the DDL changed. The result goes to the error log (info level): a note
// pushed here would be cleared when the statement starts executing.
void wait_for_ddl_refresh(THD *thd) {
  PendingDdl p;
  {
    std::lock_guard<std::mutex> g(g_ddl_mutex);
    auto it = g_pending_ddl.find(thd);
    if (it == g_pending_ddl.end()) return;
    p = it->second;
    g_pending_ddl.erase(it);
  }
  if (!ddl_wait_refresh(thd) || thd->locked_tables_mode != LTM_NONE) return;
  double waited = 0;
  std::string refreshed;
  unsigned long long rows = 0;
  const WaitResult r = wait_for_applier(
      thd, p.target, ddl_wait_refresh_timeout(thd) * 1000,
      ("refresh of the DuckDB copy of " + p.copy + " after the last DDL")
          .c_str(),
      &waited, &refreshed, &rows);
  if (r == WaitResult::APPLIED) {
    if (waited >= 1)
      DUCKDB_LOG_INFO(
          "connection %u waited %.1f s for the DuckDB copy of %s to be "
          "refreshed after its DDL (%llu rows)",
          thd->thread_id(), waited, p.copy.c_str(), rows);
    return;
  }
  DUCKDB_LOG_INFO(
      "connection %u stopped waiting for the refresh of the DuckDB copy of %s "
      "after %.1f s (%s): until it is done, queries on it run on InnoDB "
      "(duckdb_history_scope=FULL: they wait)",
      thd->thread_id(), p.copy.c_str(), waited,
      r == WaitResult::KILLED    ? "KILL QUERY"
      : r == WaitResult::TIMEOUT ? "duckdb_ddl_wait_refresh_timeout"
                                 : "the binlog applier is not running");
}

// A write to a table with a DuckDB copy that the binary log would not carry
// in full: the copy follows the binary log only (2026-11-26, external
// review). sql_log_bin = 0: not logged at all; binlog_format STATEMENT or
// MIXED: logged as text, which the applier cannot apply; binlog_row_image
// MINIMAL or NOBLOB: rows without all their columns. Refused before
// anything changes, so the copy never silently differs from InnoDB.
// TRUNCATE is logged as a statement in any format: only sql_log_bin
// matters. The replication applier, other system threads and the plugin's
// own sessions are not checked.
int check_logged_write(THD *thd, const mysql_event_table_access *ev) {
  if (thd->system_thread != NON_SYSTEM_THREAD || is_internal_session(thd) ||
      !opt_bin_log)
    return 0;
  // MTR: lets a write through so the applier's own checks are tested.
  DBUG_EXECUTE_IF("duckdb_guard_skip_write_check", return 0;);
  const std::string db(ev->table_database.str, ev->table_database.length);
  const std::string table(ev->table_name.str, ev->table_name.length);
  TableState state;
  if (!Registry::instance().snapshot(db, table, &state)) return 0;
  LEX *lex = thd->lex;
  const bool truncate = lex != nullptr && lex->sql_command == SQLCOM_TRUNCATE;
  // DELETE events are raised for every table of the statement, also the
  // ones it only reads: we check the tables it writes.
  bool written = truncate;
  for (Table_ref *t = lex != nullptr ? lex->query_tables : nullptr;
       t != nullptr && !written; t = t->next_global) {
    if (t->db == nullptr || t->table_name == nullptr) continue;
    if (db == t->db && table == t->table_name &&
        t->lock_descriptor().type >= TL_WRITE_ALLOW_WRITE)
      written = true;
  }
  if (!written) return 0;
  const std::string full = db + "." + table;
  const std::string tail =
      ": the DuckDB copy of " + full +
      " follows the binary log and would silently differ from InnoDB. Change "
      "it with the session default, or unload the copy first";
  if ((thd->variables.option_bits & OPTION_BIN_LOG) == 0)
    return stop(thd, "sql_log_bin = 0: this change to " + full +
                         " would not be written to the binary log" + tail);
  if (truncate) return 0;
  if (thd->variables.binlog_format != BINLOG_FORMAT_ROW)
    return stop(thd, "binlog_format is not ROW: this change to " + full +
                         " would be logged as a statement" + tail);
  if (thd->variables.binlog_row_image != BINLOG_ROW_IMAGE_FULL)
    return stop(thd, "binlog_row_image is not FULL: this change to " + full +
                         " would be logged without all its columns" + tail);
  return 0;
}

int guard_notify(MYSQL_THD thd, mysql_event_class_t event_class,
                 const void *event) {
  if (thd == nullptr) return 0;
  if (event_class == MYSQL_AUDIT_QUERY_CLASS) {
    try {
      on_statement_end(thd, static_cast<const mysql_event_query *>(event));
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
      if (ev->event_subclass == MYSQL_AUDIT_PARSE_PREPARSE) {
        if (is_internal_session(thd))
          thd->set_skip_readonly_check();
        else if (thd->system_thread == NON_SYSTEM_THREAD)
          wait_for_ddl_refresh(thd);  // T0: the statement after a DDL
      }
    } catch (...) {
    }
    return 0;
  }
  if (event_class != MYSQL_AUDIT_TABLE_ACCESS_CLASS) return 0;
  const auto *ev = static_cast<const mysql_event_table_access *>(event);
  if (ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_READ &&
      ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_INSERT &&
      ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_UPDATE &&
      ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_DELETE)
    return 0;
  try {
    if (ev->event_subclass != MYSQL_AUDIT_TABLE_ACCESS_READ &&
        check_logged_write(thd, ev))
      return 1;
    if (ev->event_subclass == MYSQL_AUDIT_TABLE_ACCESS_INSERT) return 0;
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
                                MYSQL_AUDIT_TABLE_ACCESS_INSERT |
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
    {
      std::lock_guard<std::mutex> guard(g_marks_mutex);
      g_marks.erase(thd);
    }
    std::lock_guard<std::mutex> guard(g_ddl_mutex);
    g_pending_ddl.erase(thd);
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
