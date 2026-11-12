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

#ifndef STORAGE_DUCKDB_DUCKDB_APPLIER_H_
#define STORAGE_DUCKDB_DUCKDB_APPLIER_H_

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <utility>

#include "storage/duckdb/duckdb_changes.h"
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_registry.h"  // BinlogPos
#include "storage/duckdb/duckdb_row_decoder.h"

namespace duckdb_se {

/** One table of a transaction or batch: its columns and its changes. */
struct BatchTable {
  BatchTable(TableInfo table_info, bool retain_history,
             BinlogPos table_load_position)
      : info(std::move(table_info)),
        changes(info.primary_key, retain_history),
        load_position(std::move(table_load_position)) {}

  /** Columns as described by the binary log (Table_map). */
  TableInfo info;
  TableChanges changes;
  /**
    Load position of the copy these changes are for. A table unloaded and
    loaded again has a new one: changes for the old copy are already in it.
  */
  BinlogPos load_position;
};

/** (db, table) */
using TableKey = std::pair<std::string, std::string>;
using TableBatches = std::map<TableKey, BatchTable>;

/**
  Committed source transactions applied together in one DuckLake transaction
  (DWL#4 step 4c-1).
*/
struct Batch {
  TableBatches tables;
  /** Transactions that changed loaded tables. */
  unsigned long long transactions{0};
  /** Row operations of those transactions. */
  unsigned long long rows{0};
  /** Position after the last committed transaction read. */
  BinlogPos end;
  /** GTID of the last committed transaction read. */
  std::string last_gtid;
  /** When the first transaction with changes was added. */
  std::chrono::steady_clock::time_point first;
  /** Something to write: changes, or only a new position. */
  bool pending{false};

  /** Adds the changes of one committed transaction. true on error. */
  bool add(TableBatches &&trx, std::string *err);
  /**
    Whether a table of trx was loaded again since its changes in this batch:
    the batch must be committed before trx is added.
  */
  bool reloaded(const TableBatches &trx) const;
  void clear();
};

struct CommitStats {
  unsigned long long deleted{0};
  unsigned long long archived{0};
  unsigned long long inserted{0};
};

/** Creates lake.__mysql.applied_position. */
const char *applied_position_create_sql();

/**
  A DuckLake transaction that may stay open across several writes: one batch
  (4c-1), or the pieces of one large source transaction (4c-2). It holds
  Lake::apply_mutex() while open, so no table is unloaded meanwhile. Any
  error rolls it back and releases the mutex: nothing is written and the
  applied position does not move.
*/
class OpenTransaction {
 public:
  OpenTransaction() = default;
  OpenTransaction(const OpenTransaction &) = delete;
  OpenTransaction &operator=(const OpenTransaction &) = delete;

  /** Takes apply_mutex and runs BEGIN. true on error. */
  bool begin(Lake::Connection &con, std::string *err);
  /**
    Writes the changes of `tables` (a batch, or one piece). Tables unloaded,
    stale or loaded again since their changes were read are skipped. true on
    error (rolled back).
  */
  bool apply(Lake::Connection &con, const TableBatches &tables, bool debug_log,
             std::string *err);
  /**
    Writes the applied position and commits. On success, adds the rows
    written by every apply() to *stats. true on error (rolled back).
  */
  bool commit(Lake::Connection &con, const BinlogPos &end,
              const std::string &gtid, CommitStats *stats, std::string *err);
  /** Rolls back and releases the mutex; never fails. */
  void rollback(Lake::Connection &con);
  bool open() const { return m_lock.owns_lock(); }

 private:
  std::unique_lock<std::mutex> m_lock;
  /** Rows written by apply() in this transaction. */
  CommitStats m_stats;
};

/**
  Writes the batch and the applied position in one DuckLake transaction. On
  error the transaction is rolled back and nothing changes. Tables unloaded,
  stale or loaded again since their changes were read are skipped.
*/
bool commit_batch(Lake::Connection &con, const Batch &batch, bool debug_log,
                  CommitStats *stats, std::string *err);

/** Replaces the applied position (repoint), in its own transaction. */
bool store_applied_position(Lake::Connection &con, const BinlogPos &pos,
                            const std::string &gtid, std::string *err);

/**
  Reads the applied position. pos->file is empty (and the result false) when
  none is recorded yet.
*/
bool read_applied_position(Lake::Connection &con, BinlogPos *pos,
                           std::string *gtid, std::string *err);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_APPLIER_H_
