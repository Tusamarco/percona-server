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

#ifndef STORAGE_DUCKDB_DUCKDB_SESSION_H_
#define STORAGE_DUCKDB_DUCKDB_SESSION_H_

#include <string>
#include <vector>

namespace duckdb_se {

/** The plugin's st_plugin_int, needed to attach threads to the session
    service. Set at plugin init. */
void set_session_plugin(const void *plugin);

/**
  Attaches the calling thread to the session service, once for the life of
  the thread and at the top of its stack (the service records the stack start
  for overrun checks). true on error. The caller keeps the result: no
  thread_local here, the plugin's static TLS space is used up by DuckDB
  (initial-exec TLS model; dlopen fails with "cannot allocate memory in
  static TLS block").
*/
bool session_thread_attach();

/**
  Detaches the thread if it was attached, and ends it for mysys
  (srv_session_deinit_thread() calls my_thread_end()). Called last, in place
  of my_thread_end().
*/
void session_thread_end(bool attached);

/**
  Runs statements in an internal server session, on the calling thread (the
  binlog reader; DWL#4 step 4d), which must be attached. The session has the
  privileges of the
  replication applier without PRIVILEGE_CHECKS_USER (skip_grants,
  sql/rpl_rli.cc) and runs with sql_log_bin=0: nothing it does is written to
  the binary log, so nothing is replicated and no GTID is created. Stops at
  the first error. true on error; *sql_errno gets the MySQL error number (0
  if the statement did not run).
*/
bool run_internal_statements(const std::vector<std::string> &statements,
                             std::string *err, unsigned *sql_errno);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_SESSION_H_
