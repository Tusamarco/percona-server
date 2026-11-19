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

#ifndef STORAGE_DUCKDB_DUCKDB_PROGRESS_H_
#define STORAGE_DUCKDB_DUCKDB_PROGRESS_H_

#include <memory>
#include <string>

#include "storage/duckdb/duckdb_registry.h"  // BinlogPos

class THD;

namespace duckdb_se {

/**
  Progress of the copies being loaded (SECONDARY_LOAD, and the refresh after
  DDL, which is an internal SECONDARY_LOAD), and the waits for the binlog
  applier (T0, 2026-11-19).

  The load publishes its rows; whoever waits shows them where MySQL users
  look: SHOW PROCESSLIST (the thread's state text) and Performance Schema
  (stage `stage/duckdb/...` with WORK_COMPLETED / WORK_ESTIMATED). The MySQL
  protocol cannot send a message to a client during its statement: the
  waiting client gets a note at the end.
*/

/** Registers the Performance Schema stages (plugin init). */
void register_progress_stages();

/** One copy being loaded or refreshed. */
struct LoadProgress {
  std::string db;
  std::string table;
  bool refresh{false};  // after DDL (applier), not a user SECONDARY_LOAD
  unsigned long long rows_total{0};  // InnoDB's estimate
  double seconds{0};
  unsigned long long rows_done{0};
  std::string phase;  // "copying rows", "copying archived rows", "committing"
};

/**
  Publishes the progress of one load while it lives (load_table_into_lake).
  When `thd` is a client session (a user SECONDARY_LOAD), its own state and
  stage show the progress too.
*/
class LoadTracker {
 public:
  LoadTracker(THD *thd, const std::string &db, const std::string &table,
              bool refresh, unsigned long long rows_total);
  ~LoadTracker();
  LoadTracker(const LoadTracker &) = delete;
  LoadTracker &operator=(const LoadTracker &) = delete;

  /** Rows read from InnoDB so far (called per chunk). */
  void rows(unsigned long long done);
  void phase(const char *what);

 private:
  struct Impl;
  std::unique_ptr<Impl> m;
};

/** The load of db.table in progress, if any. */
bool load_progress(const std::string &db, const std::string &table,
                   LoadProgress *out);

/** Result of wait_for_applier(). */
enum class WaitResult { APPLIED, TIMEOUT, KILLED, NOT_RUNNING };

/**
  Waits until the binlog applier has applied `target`, showing what it waits
  for (applier state, the refresh in progress with its rows) in the
  thread's state and stage, refreshed about once a second. `timeout_ms` 0
  means no limit (KILL QUERY and max_execution_time still stop it).
  `refreshed` (optional) receives the table whose refresh was seen, and
  `rows` its row count.
*/
WaitResult wait_for_applier(THD *thd, const BinlogPos &target,
                            unsigned long long timeout_ms, const char *why,
                            double *waited_seconds, std::string *refreshed,
                            unsigned long long *rows);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_PROGRESS_H_
