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

#ifndef STORAGE_DUCKDB_DUCKDB_LAKE_H_
#define STORAGE_DUCKDB_DUCKDB_LAKE_H_

#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "duckdb.h"

class THD;

namespace duckdb_se {

/** Runs one statement on a DuckDB connection. Call only on an executor thread. */
bool run_sql(duckdb_connection con, const std::string &sql, std::string *err);
/** Runs a query and returns its first value as text. Executor thread only. */
bool query_value_sql(duckdb_connection con, const std::string &sql,
                     std::string *value, std::string *err);

/** Settings used when the lake opens (from the duckdb_* variables). */
struct LakeSettings {
  const char *data_path;
  unsigned long long memory_limit;
  unsigned int threads;
  unsigned int executor_threads;
  unsigned long long executor_stack_size;
  /** duckdb_lake_relocate: when an earlier version's DuckLake lake is
      migrated and its catalog stores another data path (moved or restored
      elsewhere), read its files from duckdb_data_path. The native lake
      file stores no path: a moved lake always opens. */
  bool relocate;
};

/**
  The DuckDB instance of this server, in memory, with the lake attached as
  "lake": a native DuckDB database file, lake.duckdb (2026-11-25; DuckLake
  before, migrated at the first start). There is one per process. Callers hold mutex() around open(), close()
  and the setters. Every DuckDB call runs on an executor thread. All functions
  follow the server convention: true means error, with the message in *err.
*/
class Lake {
 public:
  /**
    A DuckDB connection for one operation of one session. Every call runs on an
    executor thread; a KILL of the session interrupts it. While a Connection
    exists the lake cannot be closed: close() waits.
  */
  class Connection {
   public:
    ~Connection();
    Connection(const Connection &) = delete;
    Connection &operator=(const Connection &) = delete;

    /**
      Runs fn(connection, err) on an executor thread. A KILL of the session
      drops the task if it is queued and interrupts it if it runs. true on
      error.
    */
    bool execute(
        const std::function<bool(duckdb_connection, std::string *)> &fn,
        std::string *err);
    /**
      Same as execute(), but not interruptible: for cleanup (ROLLBACK, destroying
      an appender), which must run even after a KILL.
    */
    bool execute_cleanup(
        const std::function<bool(duckdb_connection, std::string *)> &fn,
        std::string *err);
    bool run(const std::string &sql, std::string *err);
    /** Runs a query and returns the first value of the first row as text. */
    bool query_value(const std::string &sql, std::string *value,
                     std::string *err);

   private:
    friend class Lake;
    bool run_task(
        const std::function<bool(duckdb_connection, std::string *)> &fn,
        std::string *err, bool interruptible);
    Connection(std::shared_lock<std::shared_mutex> usage,
               duckdb_connection con, THD *thd);
    std::shared_lock<std::shared_mutex> m_usage;
    duckdb_connection m_con;
    /** Session to check for KILL; nullptr for internal operations. */
    THD *m_thd;
  };

  static Lake &instance();

  std::mutex &mutex() { return m_mutex; }
  /**
    Held by the binlog applier while it commits a batch and by an unload while
    it changes a table and the registry: the applier never writes a table
    being unloaded.
  */
  std::mutex &apply_mutex() { return m_apply_mutex; }
  bool is_open() const { return m_db != nullptr; }
  /** Resolved lake directory, empty when closed. */
  const std::string &path() const { return m_path; }

  /**
    Resolves and checks the lake directory, creates what is missing, starts the
    executor, opens DuckDB and attaches the lake (design doc, Startup sequence).
  */
  bool open(const LakeSettings &settings, std::string *err);
  /** Waits for open Connections, closes DuckDB and stops the executor. */
  void close();

  /** A new connection for thd, or nullptr with *err set. */
  std::unique_ptr<Connection> connect(THD *thd, std::string *err);

  bool set_memory_limit(unsigned long long bytes, std::string *err);
  bool set_threads(unsigned int threads, std::string *err);

 private:
  Lake() = default;
  /** Runs SQL on the admin connection; executor thread only. */
  bool run(const std::string &sql, std::string *err) {
    return run_sql(m_admin, sql, err);
  }
  /** Runs a nullptr-terminated list of statements; executor thread only. */
  bool run_all(const char *const *statements, std::string *err);
  /** DuckDB part of open(): runs on an executor thread. */
  bool open_duckdb(const std::string &lake, const LakeSettings &settings,
                   bool migrate, std::string *err);
  /** Closes DuckDB: runs on an executor thread. */
  void close_duckdb();
  /** Drops build tables left by a load interrupted before its swap. */
  void drop_build_leftovers();
  /** Copies a DuckLake lake of an earlier version into the native file. */
  bool migrate_from_ducklake(const std::string &lake,
                             const LakeSettings &settings, std::string *err);

  std::mutex m_mutex;
  std::mutex m_apply_mutex;
  /** Shared by each Connection, exclusive while opening or closing. */
  std::shared_mutex m_usage;
  duckdb_database m_db{nullptr};
  /** Internal connection for setup and settings; operations get their own. */
  duckdb_connection m_admin{nullptr};
  std::string m_path;
};

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_LAKE_H_
