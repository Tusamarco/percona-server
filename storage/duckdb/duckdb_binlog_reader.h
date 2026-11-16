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

#ifndef STORAGE_DUCKDB_DUCKDB_BINLOG_READER_H_
#define STORAGE_DUCKDB_DUCKDB_BINLOG_READER_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "my_inttypes.h"
#include "my_thread.h"
#include "storage/duckdb/duckdb_applier.h"
#include "storage/duckdb/duckdb_changes.h"
#include "storage/duckdb/duckdb_ddl.h"
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_registry.h"  // BinlogPos
#include "storage/duckdb/duckdb_row_decoder.h"

class THD;

namespace mysql::binlog::event {
class Format_description_event;
class Table_map_event;
}

namespace duckdb_se {

/**
  DWL#4 steps 4a to 4c-1: follows the binary log after each sync, assembles
  the transactions that touch loaded tables, decodes their rows and collapses
  them per primary key, then merges committed transactions into batches and
  commits each batch to DuckDB in one DuckLake transaction, together with the
  applied position. It runs on one plugin-owned thread without THD and uses
  only the binlog file reader and the standalone event library; DuckDB calls
  run on the executor through the reader's own connection.
*/
class BinlogReader {
 public:
  struct Status {
    std::string state;
    BinlogPos position;
    unsigned long long transactions{0};
    unsigned long long row_events{0};
    unsigned long long rows_decoded{0};
    unsigned long long changes{0};
    std::string last_gtid;
    unsigned long long batches{0};
    unsigned long long rows_written{0};
    unsigned long long large_transactions{0};
    unsigned long long pieces{0};
    unsigned long long ddl_refreshes{0};
    unsigned long long ddl_renames{0};
    unsigned long long ddl_failures{0};
    unsigned long long ddl_deferred{0};
    unsigned long long archived_rows{0};
    unsigned long long purge_transactions{0};
    BinlogPos applied;
    std::string applied_gtid;
  };

  static BinlogReader &instance();
  static void register_psi();

  bool start(size_t stack_size, std::string *err);
  void stop();
  /**
    The server is about to destroy the binary log (BFN_BINLOG_END, called by
    clean_up() before plugin_shutdown()): we stop the reader now, and start()
    refuses from now on. The pending batch is still committed.
  */
  void binlog_end();
  /**
    Waits until the applied position reaches pos, asking the reader to commit
    its pending batch at once (resync of a history table, DWL#3 step 3b).
    *not_running: the reader is stopped or in error, nothing to wait for.
    true on timeout (60 s) or KILL, with *err.
  */
  bool wait_applied(const BinlogPos &pos, THD *thd, bool *not_running,
                    std::string *err, int timeout_seconds = 60);

  /**
    Called by the transaction observer (after_commit) after the InnoDB
    commit of each transaction, in binary log order with
    binlog_order_commits=ON: the reader never reads past what InnoDB has
    committed. It only records the position and wakes the reader.
  */
  void notify_committed(const char *file, my_off_t pos);
  Status status();
  /** duckdb_apply_debug_log: one error log line per collapsed change. */
  void set_debug_log(bool on) { m_debug_log = on; }
  /**
    duckdb_apply_batch_max_trx / _max_rows / _max_delay_ms. _max_rows is also
    the piece size of large transactions (4c-2).
  */
  void set_batch_max_trx(unsigned long long n) { m_batch_max_trx = n; }
  void set_batch_max_rows(unsigned long long n) { m_batch_max_rows = n; }
  void set_batch_max_delay_ms(unsigned long long ms) {
    m_batch_max_delay_ms = ms;
    m_cv.notify_all();
  }
  /**
    duckdb_apply_gap_action: when a binary log we need was purged, stop (false)
    or continue at the first available one (true).
  */
  void set_gap_first_available(bool on) { m_gap_first_available = on; }

  /**
    duckdb_apply_start_position: "FIRST", "CURRENT" or a binary log name (its
    start). Resolves it against the binlog index. true on error.
  */
  static bool resolve_start_position(const std::string &value, BinlogPos *pos,
                                     std::string *err);
  /**
    Restarts the reader at `to`, which becomes the applied position. Refused
    before the applied position (transactions would be applied twice).
    *behind gets the tables that may miss changes. The caller holds
    Lake::mutex() and the lake is open. true on error; the reader runs again
    in every case.
  */
  bool repoint(const BinlogPos &to, THD *thd, std::vector<std::string> *behind,
               std::string *err);
  /** A warning when binlog expiry may purge logs the reader needs, or "". */
  static std::string expiry_warning();

 private:
  struct Trx {
    bool relevant{false};
    std::string gtid;
    std::string tag;
    /** The transaction carries the duckdb_avoid_purge tag (DWL#5). */
    bool purge{false};
    /** Original commit time in microseconds: the archive time (DWL#5). */
    unsigned long long commit_time{0};
    /**
      Collapsed changes per (db, table): of the whole transaction, or of the
      current piece when it is applied in pieces.
    */
    TableBatches changes;
    /** Rows of loaded tables decoded in `changes`. */
    unsigned long long rows{0};
  };

  BinlogReader() = default;
  static void *thread_main(void *arg);
  void loop();
  /** Puts the reader in error state and waits until it is stopped. */
  void fail_and_wait(const std::string &err);
  bool current_end(BinlogPos *pos, std::string *err);
  bool initial_position(BinlogPos *pos, std::string *err);
  bool read_file(const BinlogPos &from, const BinlogPos &end,
                 BinlogPos *reached, bool *eof, std::string *err);
  /**
    The binary log after `file`. *gap is set when the logs in between were
    purged (or `file` itself, before we finished it).
  */
  bool next_file(const std::string &file, std::string *next, bool *gap,
                 std::string *err);
  /**
    A purged range ending before `resume`: with duckdb_apply_gap_action=STOP
    an error, otherwise a warning and we continue at `resume`.
  */
  bool handle_gap(const std::string &what, const BinlogPos &resume,
                  std::string *err);
  bool handle_event(const unsigned char *data, const BinlogPos &at,
                    const mysql::binlog::event::Format_description_event &fde,
                    std::string *err);
  bool table_info(const mysql::binlog::event::Table_map_event &ev,
                  TableInfo *info, std::string *err);
  bool handle_rows(const char *buf,
                   const mysql::binlog::event::Format_description_event &fde,
                   const BinlogPos &at, std::string *err);
  /** DDL on loaded tables: rename and refresh the copies (step 4d). */
  bool handle_ddl(const DdlStatement &ddl, const BinlogPos &at,
                  std::string *err);
  /** Refreshes one copy after DDL; deferred if the table was renamed. */
  void refresh_copy(const TableName &t, const ColumnRenames &ddl_renames);
  /** A copy that could not follow a DDL: stale, error log, counter. */
  void ddl_failed(const TableName &t, const std::string &why);
  /** A compressed transaction: its inner events, decompressed. */
  bool handle_payload(const char *buf,
                      const mysql::binlog::event::Format_description_event &fde,
                      const BinlogPos &at, std::string *err);
  bool invalid_event(const char *what, const BinlogPos &at, std::string *err);
  /** Ends the current transaction at `at` (after its last event). */
  bool end_transaction(bool commit, const BinlogPos &at, std::string *err);
  /**
    Net changes of `changes`; with duckdb_apply_debug_log, one line per
    changed key (at most kMaxDebugLines).
  */
  unsigned long long count_changes(const TableBatches &changes);
  /**
    Writes the changes of the current transaction collected so far as one
    piece (4c-2). The first piece commits the pending batch and opens the
    DuckLake transaction, which stays open until the transaction ends.
  */
  bool write_piece(std::string *err);
  /** Ends the large transaction: last piece, applied position, COMMIT. */
  bool commit_large(const BinlogPos &at, std::string *err);
  /** Rolls back an open large transaction (error, gap, stop). */
  void abandon_large(const char *why);
  /** Commits the batch when a limit is reached (or `force`). */
  bool maybe_commit(bool force, std::string *err);
  bool commit_pending(std::string *err);
  /** When the pending batch must be committed at the latest. */
  std::chrono::steady_clock::time_point batch_deadline() const;
  /** Commits what is pending when the reader stops (best effort). */
  void flush_on_stop();

  std::mutex m_mutex;
  std::condition_variable m_cv;
  bool m_running{false};
  std::atomic<bool> m_stop{false};
  /** Set by binlog_end(): mysql_bin_log must not be used any more. */
  std::atomic<bool> m_binlog_ended{false};
  /** wait_applied() wants the pending batch committed now (under m_mutex). */
  bool m_flush_requested{false};
  /** Idle with nothing pending: all read changes are committed (m_mutex). */
  bool m_caught_up{false};
  std::atomic<bool> m_debug_log{false};
  std::atomic<unsigned long long> m_batch_max_trx{1000};
  std::atomic<unsigned long long> m_batch_max_rows{100000};
  std::atomic<unsigned long long> m_batch_max_delay_ms{1000};
  std::atomic<bool> m_gap_first_available{false};
  size_t m_stack_size{0};
  my_thread_handle m_thread;
  BinlogPos m_committed;  // last committed position, from the observer
  BinlogPos m_position;  // next position to read
  std::string m_state{"stopped"};
  unsigned long long m_transactions{0};
  unsigned long long m_row_events{0};
  unsigned long long m_rows_decoded{0};
  unsigned long long m_changes{0};
  std::string m_last_gtid;
  unsigned long long m_batches{0};
  unsigned long long m_rows_written{0};
  unsigned long long m_large_transactions{0};
  unsigned long long m_pieces{0};
  unsigned long long m_ddl_refreshes{0};
  unsigned long long m_ddl_renames{0};
  unsigned long long m_ddl_failures{0};
  unsigned long long m_ddl_deferred{0};
  unsigned long long m_archived_rows{0};
  unsigned long long m_purge_transactions{0};
  BinlogPos m_applied;  // last committed (applied) position
  std::string m_applied_gtid;
  // Used only by the reader thread.
  std::map<unsigned long long, std::pair<std::string, std::string>>
      m_table_map;
  /** Decoded Table_map of loaded tables, by table id. */
  std::map<unsigned long long, TableInfo> m_table_info;
  Trx m_trx;
  Batch m_batch;
  /**
    Open while a large transaction is applied in pieces (4c-2): from its
    first piece to its end. Rolled back before m_con is closed.
  */
  OpenTransaction m_large;
  /** The reader thread is attached to the session service (refresh). */
  bool m_session_attached{false};
  /** The reader's DuckDB connection, open while the thread runs. */
  std::unique_ptr<Lake::Connection> m_con;
  /** Last batch commit, for position-only commits. */
  std::chrono::steady_clock::time_point m_last_commit;
};

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_BINLOG_READER_H_
