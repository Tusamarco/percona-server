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

#include "storage/duckdb/duckdb_binlog_reader.h"

#include <strings.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include "my_sys.h"
#include "mysql/binlog/event/binlog_event.h"
#include "mysql/binlog/event/compression/payload_event_buffer_istream.h"
#include "mysql/binlog/event/control_events.h"
#include "mysql/binlog/event/rows_event.h"
#include "mysql/binlog/event/statement_events.h"
#include "mysql/psi/mysql_thread.h"
#include "sql/binlog.h"         // mysql_bin_log, Log_info
#include "sql/binlog_reader.h"  // Binlog_file_reader
#include "sql/mysqld.h"  // opt_bin_log, binlog_expire_logs_seconds
#include "storage/duckdb/duckdb_ddl.h"
#include "storage/duckdb/duckdb_load.h"  // refresh and rename after DDL
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_session.h"

namespace duckdb_se {

// GTID tag of purge transactions on retain_history tables (DWL#5). Fixed: the
// purge job's SQL is the only place it appears, so nothing can disagree.
constexpr const char *kAvoidPurgeTag = "duckdb_avoid_purge";

namespace bev = mysql::binlog::event;

namespace {

PSI_thread_key key_thread_duckdb_reader = PSI_NOT_INSTRUMENTED;

#ifdef HAVE_PSI_INTERFACE
// Visible in performance_schema.threads as thread/duckdb/binlog_reader.
PSI_thread_info reader_thread_info[] = {
    {&key_thread_duckdb_reader, "binlog_reader", "duckdb_binlog",
     PSI_FLAG_SINGLETON | PSI_FLAG_THREAD_SYSTEM, 0, PSI_DOCUMENT_ME}};
#endif

// The table id, column bitmaps and row data of a rows event are protected
// members of the event library class; a derived class gives us read access
// without changing the library.
struct RowsEventHeader : public bev::Rows_event {
  RowsEventHeader(const char *buf, const bev::Format_description_event *fde)
      : bev::Rows_event(buf, fde) {}
  unsigned long long table_id() const { return m_table_id.id(); }
  bev::Log_event_type type() const { return m_type; }
  const std::vector<uint8_t> &before_columns() const {
    return columns_before_image;
  }
  const std::vector<uint8_t> &after_columns() const {
    return columns_after_image;
  }
  const std::vector<uint8_t> &rows() const { return row; }
};

// At most this many debug log lines per transaction.
constexpr unsigned kMaxDebugLines = 100;

// Without changes to loaded tables we record a new applied position at most
// this often: each commit is a DuckLake snapshot, and the position only
// shortens the re-read after a restart.
constexpr std::chrono::seconds kPositionCommitInterval{60};

unsigned long long now_micros() {
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

// All binary logs live in the directory of the current one; we keep names
// without directory and build the full path only to open a file.
std::string binlog_full_path(const std::string &base) {
  const char *current = mysql_bin_log.get_log_fname();
  return std::string(current, dirname_length(current)) + base;
}

// The binary logs in the index, oldest first, without directory. Empty if the
// index cannot be read.
std::vector<std::string> binlog_files() {
  std::vector<std::string> files;
  auto [error, list] = mysql_bin_log.get_log_index(true);
  // The listing ends with LOG_INFO_EOF when it reaches the end of the index:
  // that is success. Any other value is a read error.
  if (error != LOG_INFO_EOF) return files;
  for (const std::string &f : list) files.push_back(binlog_base_name(f.c_str()));
  return files;
}

bool in_index(const std::vector<std::string> &files, const std::string &file) {
  for (const std::string &f : files)
    if (f == file) return true;
  return false;
}

// The first binary log after `file` (names sort in creation order).
bool file_after(const std::vector<std::string> &files, const std::string &file,
                std::string *next) {
  for (const std::string &f : files) {
    if (file < f) {
      *next = f;
      return true;
    }
  }
  return false;
}

// The sequence number of a binary log name (binlog.000028 -> 28).
unsigned long long binlog_sequence(const std::string &name) {
  const size_t dot = name.rfind('.');
  if (dot == std::string::npos) return 0;
  return std::strtoull(name.c_str() + dot + 1, nullptr, 10);
}

std::string join(const std::vector<std::string> &names) {
  std::string out;
  for (const std::string &n : names) out += (out.empty() ? "" : ", ") + n;
  return out;
}

}  // namespace

bool BinlogReader::resolve_start_position(const std::string &value,
                                          BinlogPos *pos, std::string *err) {
  if (!mysql_bin_log.is_open()) {
    *err = "the binary log is not open";
    return true;
  }
  if (strcasecmp(value.c_str(), "CURRENT") == 0) {
    Log_info info;
    if (mysql_bin_log.get_current_log(&info) != 0) {
      *err = "cannot read the current binary log position";
      return true;
    }
    *pos = BinlogPos{binlog_base_name(info.log_file_name), info.pos};
    return false;
  }
  const std::vector<std::string> files = binlog_files();
  if (files.empty()) {
    *err = "cannot read the binary log index";
    return true;
  }
  if (strcasecmp(value.c_str(), "FIRST") == 0) {
    *pos = BinlogPos{files.front(), 0};
    return false;
  }
  // Only a file name: the start of a file is always a transaction boundary.
  const std::string name = binlog_base_name(value.c_str());
  if (!in_index(files, name)) {
    *err = "binary log " + name +
           " is not in the binary log index (use a name from SHOW BINARY "
           "LOGS, FIRST or CURRENT)";
    return true;
  }
  *pos = BinlogPos{name, 0};
  return false;
}

std::string BinlogReader::expiry_warning() {
  if (!opt_bin_log || !opt_binlog_expire_logs_auto_purge ||
      binlog_expire_logs_seconds == 0 || binlog_expire_logs_seconds >= 86400)
    return "";
  return "binlog_expire_logs_seconds=" +
         std::to_string(binlog_expire_logs_seconds) +
         " is less than one day: binary logs the DuckDB applier has not read "
         "yet (while it is stopped or behind) can be purged, and the DuckDB "
         "copies then need a reload";
}

bool BinlogReader::repoint(const BinlogPos &to, THD *thd,
                           std::vector<std::string> *behind,
                           std::string *err) {
  stop();  // commits the pending batch
  bool failed = false;
  bool moved = false;
  {
    auto con = Lake::instance().connect(thd, err);
    BinlogPos applied;
    std::string gtid;
    failed = !con || read_applied_position(*con, &applied, &gtid, err);
    if (!failed && !applied.empty() && to < applied) {
      *err = "the position " + to.file + ":" + std::to_string(to.pos) +
             " is before the applied position " + applied.file + ":" +
             std::to_string(applied.pos) +
             ": transactions in between would be applied twice";
      failed = true;
    }
    moved = applied.empty() || applied < to;
    if (!failed) failed = store_applied_position(*con, to, "", err);
  }
  if (!failed && moved) *behind = Registry::instance().loaded_before(to);
  std::string start_err;
  if (start(m_stack_size, &start_err)) {
    if (!failed) *err = start_err;
    return true;
  }
  if (!failed)
    DUCKDB_LOG_WARNING(
        "binlog applier repointed to %s:%llu (duckdb_apply_start_position); "
        "loaded tables that may be behind InnoDB: %s",
        to.file.c_str(), to.pos,
        behind->empty() ? "none" : join(*behind).c_str());
  return failed;
}

bool BinlogReader::handle_gap(const std::string &what, const BinlogPos &resume,
                              std::string *err) {
  const std::vector<std::string> behind =
      Registry::instance().loaded_before(resume);
  // No loaded table needs the purged range: nothing is lost.
  if (behind.empty()) {
    DUCKDB_LOG_INFO(
        "binlog applier: %s; no loaded table needs it, continuing at %s",
        what.c_str(), resume.file.c_str());
    return false;
  }
  const std::string msg =
      what + "; loaded tables that may be behind InnoDB: " + join(behind);
  if (!m_gap_first_available) {
    *err = msg +
           ". Reload them and SET GLOBAL duckdb_apply_start_position = "
           "'FIRST'";
    return true;
  }
  DUCKDB_LOG_WARNING(
      "binlog applier: %s. Continuing at %s "
      "(duckdb_apply_gap_action=FIRST_AVAILABLE): reload those tables",
      msg.c_str(), resume.file.c_str());
  return false;
}

BinlogReader &BinlogReader::instance() {
  static BinlogReader reader;
  return reader;
}

void BinlogReader::register_psi() {
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_register("duckdb", reader_thread_info, 1);
#endif
}

void *BinlogReader::thread_main(void *) {
  my_thread_init();
  // For the refresh after DDL (internal SECONDARY_LOAD), at the top of the
  // stack. Without it, a refresh fails and the table is marked stale.
  BinlogReader &reader = BinlogReader::instance();
  reader.m_session_attached = !session_thread_attach();
  if (!reader.m_session_attached)
    DUCKDB_LOG_WARNING(
        "binlog reader: cannot attach to the session service; DDL on loaded "
        "tables will mark them stale instead of refreshing them");
  // An exception must not leave the thread: std::terminate would stop mysqld.
  // An open large transaction holds apply_mutex: we roll it back before
  // waiting in error state, or every unload would wait until the stop.
  try {
    reader.loop();
  } catch (const std::exception &e) {
    reader.abandon_large("internal error");
    reader.fail_and_wait(std::string("internal error: ") + e.what());
  } catch (...) {
    reader.abandon_large("internal error");
    reader.fail_and_wait("internal error");
  }
  reader.abandon_large("reader stopped");
  // The lake cannot close while our connection is open.
  try {
    reader.m_con.reset();
  } catch (...) {
  }
  // my_thread_end(), after detaching from the service.
  session_thread_end(reader.m_session_attached);
  reader.m_session_attached = false;
  my_thread_exit(nullptr);
  return nullptr;
}

bool BinlogReader::start(size_t stack_size, std::string *err) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (m_running) return false;
  if (m_binlog_ended) {
    *err = "the binary log is closed (server shutdown)";
    return true;
  }
  // opt_bin_log is the configured setting, known at plugin init. The binary
  // log itself is opened later during startup (after the storage engines), so
  // the reader thread waits for it.
  if (!opt_bin_log) {
    m_state = "idle: binary log disabled";
    return false;
  }
  m_stack_size = stack_size;
  m_committed = BinlogPos();
  m_position = BinlogPos();
  m_stop = false;
  m_state = "waiting for the binary log";
  const std::string expiry = expiry_warning();
  if (!expiry.empty()) DUCKDB_LOG_WARNING("%s", expiry.c_str());

  my_thread_attr_t attr;
  if (my_thread_attr_init(&attr) != 0) {
    *err = "cannot start the DuckDB binlog reader";
    return true;
  }
  const bool failed =
      my_thread_attr_setstacksize(&attr, stack_size) != 0 ||
      my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE) != 0 ||
      mysql_thread_create(key_thread_duckdb_reader, &m_thread, &attr,
                          thread_main, nullptr) != 0;
  my_thread_attr_destroy(&attr);
  if (failed) {
    *err = "cannot start the DuckDB binlog reader";
    return true;
  }
  m_running = true;
  return false;
}

void BinlogReader::stop() {
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_running) return;
    m_stop = true;
    m_cv.notify_all();
  }
  my_thread_join(&m_thread, nullptr);
  std::lock_guard<std::mutex> lk(m_mutex);
  m_running = false;
  m_state = "stopped";
}

void BinlogReader::binlog_end() {
  m_binlog_ended = true;
  stop();
  std::lock_guard<std::mutex> lk(m_mutex);
  m_state = "stopped: binary log closed";
}

bool BinlogReader::wait_applied(const BinlogPos &pos, THD *thd,
                                bool *not_running, std::string *err,
                                unsigned long long timeout_ms) {
  *not_running = false;
  // The reader commits at once on request, so a long wait means it is far
  // behind.
  const auto limit = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
  std::unique_lock<std::mutex> lk(m_mutex);
  // Done when the applied position reaches pos, or when the reader has read
  // past pos and has nothing pending (only binlog headers after its last
  // commit, which do not move the applied position).
  while (m_applied < pos && !(m_caught_up && !(m_position < pos))) {
    if (!m_running || m_state.compare(0, 5, "error") == 0 ||
        m_state.compare(0, 4, "idle:") == 0) {
      *not_running = true;
      return false;
    }
    if (thd_killed(thd)) {
      *err = "interrupted while waiting for the binlog applier";
      return true;
    }
    if (std::chrono::steady_clock::now() >= limit) {
      *err = "the binlog applier has not reached the load position " +
             pos.file + ":" + std::to_string(pos.pos) +
             " within " + std::to_string(timeout_ms) +
             " ms (it is at " + m_applied.file + ":" +
             std::to_string(m_applied.pos) + "); try again";
      return true;
    }
    m_flush_requested = true;
    m_cv.notify_all();
    m_cv.wait_for(lk, std::chrono::milliseconds(100));
  }
  return false;
}

void BinlogReader::fail_and_wait(const std::string &err) {
  std::unique_lock<std::mutex> lk(m_mutex);
  m_state = "error: " + err;
  DUCKDB_LOG_ERROR("binlog reader stopped: %s", err.c_str());
  m_cv.wait(lk, [this] { return m_stop.load(); });
}

void BinlogReader::notify_committed(const char *file, my_off_t pos) {
  if (file == nullptr) return;
  std::lock_guard<std::mutex> lk(m_mutex);
  if (!m_running) return;
  BinlogPos committed{file, pos};
  if (m_committed < committed) {
    m_committed = std::move(committed);
    m_cv.notify_one();
  }
}

BinlogReader::Status BinlogReader::status() {
  std::lock_guard<std::mutex> lk(m_mutex);
  Status s;
  s.state = m_state;
  s.position = m_position;
  s.transactions = m_transactions;
  s.row_events = m_row_events;
  s.rows_decoded = m_rows_decoded;
  s.changes = m_changes;
  s.last_gtid = m_last_gtid;
  s.batches = m_batches;
  s.rows_written = m_rows_written;
  s.large_transactions = m_large_transactions;
  s.pieces = m_pieces;
  s.ddl_refreshes = m_ddl_refreshes;
  s.ddl_renames = m_ddl_renames;
  s.ddl_failures = m_ddl_failures;
  s.ddl_deferred = m_ddl_deferred;
  s.archived_rows = m_archived_rows;
  s.purge_transactions = m_purge_transactions;
  s.applied = m_applied;
  s.applied_gtid = m_applied_gtid;
  return s;
}

bool BinlogReader::current_end(BinlogPos *pos, std::string *err) {
  Log_info info;
  if (mysql_bin_log.get_current_log(&info) != 0) {
    *err = "cannot read the current binary log position";
    return true;
  }
  *pos = BinlogPos{binlog_base_name(info.log_file_name), info.pos};
  return false;
}

// Without an applied position (first start), we start at the earliest load
// position among loaded tables, or at the current end of the binary log.
bool BinlogReader::initial_position(BinlogPos *pos, std::string *err) {
  if (current_end(pos, err)) return true;
  BinlogPos earliest;
  if (Registry::instance().earliest_load_position(&earliest) &&
      earliest < *pos)
    *pos = earliest;
  return false;
}

void BinlogReader::loop() {
  // We wait until the server has opened (and recovered) the binary log.
  {
    std::unique_lock<std::mutex> lk(m_mutex);
    while (!m_stop && !mysql_bin_log.is_open())
      m_cv.wait_for(lk, std::chrono::seconds(1));
    if (m_stop) return;
  }
  std::string err;
  BinlogPos start;
  BinlogPos end;
  BinlogPos applied;
  std::string applied_gtid;
  // We resume after the last committed batch; committed transactions are
  // never applied twice and uncommitted ones are read again.
  m_con = Lake::instance().connect(nullptr, &err);
  bool failed_start =
      !m_con || read_applied_position(*m_con, &applied, &applied_gtid, &err) ||
      current_end(&end, &err);
  if (!failed_start) {
    if (!applied.empty()) {
      start = applied;
      if (end < start) {
        // The binary logs were reset after our last commit: a gap.
        err = "the applied position " + start.file + ":" +
              std::to_string(start.pos) +
              " is after the end of the binary log (binary logs reset?). "
              "Reload the tables and SET GLOBAL duckdb_apply_start_position "
              "= 'FIRST'";
        failed_start = true;
      }
    } else {
      failed_start = initial_position(&start, &err);
    }
  }
  // The start file may have been purged while we were stopped.
  bool resumed_after_gap = false;
  if (!failed_start) {
    const std::vector<std::string> files = binlog_files();
    std::string after;
    if (files.empty()) {
      err = "cannot read the binary log index";
      failed_start = true;
    } else if (!in_index(files, start.file) &&
               file_after(files, start.file, &after)) {
      const BinlogPos resume{after, 0};
      failed_start = handle_gap("binary log " + start.file +
                                    " was purged before it was read",
                                resume, &err);
      if (!failed_start) {
        start = resume;
        resumed_after_gap = true;
      }
    }
  }

  std::unique_lock<std::mutex> lk(m_mutex);
  if (failed_start) {
    m_state = "error: " + err;
    DUCKDB_LOG_ERROR("binlog reader: %s", err.c_str());
    m_cv.wait(lk, [this] { return m_stop.load(); });
    return;
  }
  m_position = start;
  m_applied = applied;
  m_applied_gtid = applied_gtid;
  m_batch = Batch();
  m_batch.end = start;
  m_batch.last_gtid = applied_gtid;
  // After a gap we record the new start, so we never look for the purged
  // files again.
  m_batch.pending = resumed_after_gap;
  m_last_commit = std::chrono::steady_clock::now();
  // The current end of the binary log counts as committed: at server startup
  // crash recovery has committed every transaction in it. At a runtime start
  // (duckdb_enable, duckdb_apply_start_position) a transaction between its
  // binary log sync and its InnoDB commit at that moment can be read before
  // InnoDB shows it (DWL-04, known limitation); later reads wait for
  // after_commit.
  if (m_committed < end) m_committed = end;
  DUCKDB_LOG_INFO("binlog reader started at %s:%llu (%s)", start.file.c_str(),
                  start.pos,
                  applied.empty() ? "no applied position yet"
                                  : "applied position");

  while (!m_stop) {
    if (!(m_position < m_committed)) {
      // Nothing new to read: we commit the pending batch when it is due.
      if (m_batch.pending) {
        const auto deadline = batch_deadline();
        // wait_applied() asks for the pending batch now.
        if (!m_flush_requested &&
            std::chrono::steady_clock::now() < deadline) {
          // Read, not yet committed ("idle" means everything is committed).
          m_state = "waiting to commit";
          m_cv.wait_until(lk, deadline);
          continue;
        }
        m_state = "committing";
        m_flush_requested = false;
        lk.unlock();
        const bool failed = commit_pending(&err);
        lk.lock();
        if (failed) {
          m_state = "error: " + err;
          DUCKDB_LOG_ERROR("binlog applier stopped: %s", err.c_str());
          m_cv.wait(lk, [this] { return m_stop.load(); });
          return;
        }
        continue;
      }
      m_state = "idle";
      // Everything read is committed: wait_applied() may stop waiting.
      m_caught_up = true;
      m_cv.notify_all();
      m_cv.wait(lk);
      continue;
    }
    m_state = "reading";
    m_caught_up = false;
    const BinlogPos from = m_position;
    const BinlogPos end = m_committed;
    lk.unlock();

    BinlogPos reached = from;
    bool eof = false;
    bool gap = false;
    std::string next;
    BinlogPos resume;
    bool failed = read_file(from, end, &reached, &eof, &err);
    if (failed) {
      // Purged before (or while) we read it?
      const std::vector<std::string> files = binlog_files();
      std::string after;
      if (!files.empty() && !in_index(files, from.file) &&
          file_after(files, from.file, &after)) {
        gap = true;
        resume = BinlogPos{after, 0};
        failed = handle_gap("binary log " + from.file +
                                " was purged before it was read",
                            resume, &err);
      }
    } else if (eof) {
      failed = next_file(from.file, &next, &gap, &err);
      if (!failed && gap) {
        resume = BinlogPos{next, 0};
        failed = handle_gap("binary logs between " + from.file + " and " +
                                next + " were purged before they were read",
                            resume, &err);
      }
    }
    // A large transaction cut by an error or a gap is never complete: we roll
    // back what it wrote (outside m_mutex: it runs DuckDB).
    if (failed || gap) abandon_large(failed ? "reader error" : "binlog gap");

    lk.lock();
    if (failed) {
      m_state = "error: " + err;
      DUCKDB_LOG_ERROR("binlog reader stopped: %s", err.c_str());
      m_cv.wait(lk, [this] { return m_stop.load(); });
      return;
    }
    if (gap) {
      // A transaction cut by the gap is incomplete: we drop it.
      m_trx = Trx();
      m_table_map.clear();
      m_table_info.clear();
      m_position = resume;
      m_batch.end = resume;
      m_batch.pending = true;
    } else {
      m_position = eof ? BinlogPos{next, 0} : reached;
    }
  }
  lk.unlock();
  flush_on_stop();
}

// We read one file from `from`. In the active file we stop at the committed
// position; an older file is read to its end (*eof = true).
bool BinlogReader::read_file(const BinlogPos &from, const BinlogPos &end,
                             BinlogPos *reached, bool *eof, std::string *err) {
  Binlog_file_reader reader(true /* verify checksums */);
  if (reader.open(binlog_full_path(from.file).c_str(), from.pos)) {
    *err = "cannot open binary log " + from.file + ": " +
           reader.get_error_str();
    return true;
  }
  const bool active = from.file == end.file;
  while (!m_stop) {
    if (active && reader.position() >= end.pos) break;
    unsigned char *data = nullptr;
    unsigned int length = 0;
    if (reader.read_event_data(&data, &length)) {
      if (!active && reader.get_error_type() == Binlog_read_error::READ_EOF) {
        *eof = true;
        break;
      }
      *err = "cannot read binary log " + from.file + ": " +
             reader.get_error_str();
      return true;
    }
    const BinlogPos at{from.file, reader.position()};
    // A Format_description_event defines how the following events are encoded
    // (checksums in particular). read_event_data() does not record it, and
    // open() reads it only when it starts after it (sql/binlog_reader.h), so
    // we record it ourselves.
    if (data[EVENT_TYPE_OFFSET] == bev::FORMAT_DESCRIPTION_EVENT) {
      bev::Format_description_event fde(reinterpret_cast<const char *>(data),
                                        &reader.format_description_event());
      const bool valid = fde.header()->get_is_valid();
      if (valid) reader.set_format_description_event(fde);
      reader.allocator()->deallocate(data);
      if (!valid) return invalid_event("Format_description", at, err);
      continue;
    }
    const bool failed =
        handle_event(data, at, reader.format_description_event(), err);
    reader.allocator()->deallocate(data);
    if (failed) return true;
  }
  *reached = BinlogPos{from.file, reader.position()};
  return false;
}

// We read `file` to its end, so its own purge loses nothing; a gap is a
// missing sequence number before the next file in the index.
bool BinlogReader::next_file(const std::string &file, std::string *next,
                             bool *gap, std::string *err) {
  const std::vector<std::string> files = binlog_files();
  if (files.empty()) {
    *err = "cannot read the binary log index";
    return true;
  }
  if (!file_after(files, file, next)) {
    *err = "no binary log after " + file + " in the binary log index";
    return true;
  }
  *gap = binlog_sequence(*next) != binlog_sequence(file) + 1;
  return false;
}

bool BinlogReader::handle_event(const unsigned char *data, const BinlogPos &at,
                                const bev::Format_description_event &fde,
                                std::string *err) {
  const char *buf = reinterpret_cast<const char *>(data);
  // Without the file's format description we would decode the checksum as
  // event data (and the event library asserts on it in debug builds).
  if (fde.footer()->checksum_alg == bev::BINLOG_CHECKSUM_ALG_UNDEF) {
    *err = "format description of " + at.file +
           " not read before its events";
    return true;
  }
  switch (static_cast<bev::Log_event_type>(data[EVENT_TYPE_OFFSET])) {
    case bev::GTID_LOG_EVENT:
    case bev::GTID_TAGGED_LOG_EVENT: {
      bev::Gtid_event ev(buf, &fde);
      if (!ev.header()->get_is_valid()) return invalid_event("GTID", at, err);
      // A large transaction must end (XID, COMMIT or ROLLBACK) before the
      // next one starts; otherwise its pieces would be committed with no end.
      if (m_large.open()) {
        *err = "transaction " + m_trx.gtid + " has no end event before " +
               at.file + ":" + std::to_string(at.pos);
        return true;
      }
      const auto tsid = ev.get_tsid();
      m_trx = Trx();
      m_trx.gtid = tsid.to_string() + ":" + std::to_string(ev.get_gno());
      m_trx.tag = tsid.get_tag().to_string();
      // A purge transaction (DWL#5) carries the fixed tag; the tag belongs
      // to this transaction only (gtid_next is a session variable).
      m_trx.purge = m_trx.tag == kAvoidPurgeTag;
      m_trx.commit_time = ev.original_commit_timestamp;
      // 0 would mean "not archived": we never use it as an archive time.
      if (m_trx.commit_time == 0) m_trx.commit_time = now_micros();
      m_table_map.clear();
      m_table_info.clear();
      return false;
    }
    case bev::TABLE_MAP_EVENT: {
      bev::Table_map_event ev(buf, &fde);
      if (!ev.header()->get_is_valid())
        return invalid_event("Table_map", at, err);
      m_table_map[ev.m_table_id.id()] = {ev.m_dbnam, ev.m_tblnam};
      // A copy: an unload may remove the registry entry at any time.
      TableState state;
      if (!Registry::instance().snapshot(ev.m_dbnam, ev.m_tblnam, &state) ||
          !state.usable())
        return false;
      TableInfo info;
      if (table_info(ev, &info, err)) {
        *err = ev.m_dbnam + "." + ev.m_tblnam + ": " + *err;
        return true;
      }
      m_table_info[ev.m_table_id.id()] = std::move(info);
      return false;
    }
    case bev::WRITE_ROWS_EVENT:
    case bev::UPDATE_ROWS_EVENT:
    case bev::DELETE_ROWS_EVENT:
    case bev::PARTIAL_UPDATE_ROWS_EVENT: {
      return handle_rows(buf, fde, at, err);
    }
    case bev::XID_EVENT:
      return end_transaction(true, at, err);
    case bev::QUERY_EVENT: {
      bev::Query_event ev(buf, &fde, bev::QUERY_EVENT);
      if (!ev.header()->get_is_valid()) return invalid_event("Query", at, err);
      const std::string query(ev.query, ev.q_len);
      if (query == "BEGIN") return false;
      // A savepoint is logged inside its transaction; it does not end it. The
      // server logs ROLLBACK TO SAVEPOINT only for non-transactional changes,
      // which loaded (InnoDB) tables never have.
      if (query.compare(0, 9, "SAVEPOINT") == 0 ||
          query.compare(0, 11, "ROLLBACK TO") == 0)
        return false;
      if (query != "COMMIT" && query != "ROLLBACK") {
        // DDL on loaded tables: the copies are renamed or refreshed here, in
        // binlog order (DWL#4 step 4d).
        const std::string db =
            ev.db != nullptr ? std::string(ev.db, ev.db_len) : std::string();
        const DdlStatement ddl =
            parse_ddl(query, db, lower_case_table_names != 0);
        if (!ddl.empty() && handle_ddl(ddl, at, err)) return true;
      }
      // COMMIT ends a transaction on non-XID engines; ROLLBACK discards it; a
      // DDL statement is a transaction of its own.
      return end_transaction(query != "ROLLBACK", at, err);
    }
    case bev::XA_PREPARE_LOG_EVENT:
      // The changes of an XA transaction are committed later by XA COMMIT, in
      // another binlog transaction. Not supported yet: we stop rather than
      // lose them.
      if (m_trx.relevant) {
        *err = "XA transaction " + m_trx.gtid +
               " changes loaded tables: XA is not supported yet";
        return true;
      }
      m_trx = Trx();
      return false;
    case bev::TRANSACTION_PAYLOAD_EVENT:
      return handle_payload(buf, fde, at, err);
    default:
      return false;
  }
}

// Builds the column description of a loaded table from its Table_map event.
// binlog_row_metadata=FULL gives column names, signedness, charsets and the
// primary key.
bool BinlogReader::table_info(const bev::Table_map_event &ev, TableInfo *info,
                              std::string *err) {
  info->db = ev.m_dbnam;
  info->table = ev.m_tblnam;
  bev::Table_map_event::Optional_metadata_fields opt(
      ev.m_optional_metadata, ev.m_optional_metadata_len);
  if (!opt.is_valid || opt.m_column_name.size() != ev.m_colcnt) {
    *err = "column names missing from the binary log (binlog_row_metadata="
           "FULL is required)";
    return true;
  }
  auto charset_of = [&opt](size_t index) -> unsigned int {
    if (!opt.m_column_charset.empty())
      return index < opt.m_column_charset.size() ? opt.m_column_charset[index]
                                                 : 0;
    for (const auto &pair : opt.m_default_charset.charset_pairs)
      if (pair.first == index) return pair.second;
    return opt.m_default_charset.default_charset;
  };
  auto enum_set_charset_of = [&opt](size_t index) -> unsigned int {
    if (!opt.m_enum_and_set_column_charset.empty())
      return index < opt.m_enum_and_set_column_charset.size()
                 ? opt.m_enum_and_set_column_charset[index]
                 : 0;
    for (const auto &pair : opt.m_enum_and_set_default_charset.charset_pairs)
      if (pair.first == index) return pair.second;
    return opt.m_enum_and_set_default_charset.default_charset;
  };

  const unsigned char *meta = ev.m_field_metadata;
  const unsigned char *meta_end = meta + ev.m_field_metadata_size;
  size_t numeric = 0;
  size_t character = 0;
  size_t enum_set = 0, enums = 0, sets = 0;
  for (unsigned long i = 0; i < ev.m_colcnt; i++) {
    ColumnInfo c;
    c.name = opt.m_column_name[i];
    c.type = ev.m_coltype[i];
    if (read_column_metadata(c.type, &meta, meta_end, &c.metadata, err)) {
      *err = "column " + c.name + ": " + *err;
      return true;
    }
    if (type_has_signedness(c.type)) {
      c.is_unsigned =
          numeric < opt.m_signedness.size() && opt.m_signedness[numeric];
      numeric++;
    }
    if (type_is_character(c.type, c.metadata)) c.charset = charset_of(character++);
    if (c.type == MYSQL_TYPE_STRING) {
      unsigned int real_type, length;
      decode_string_metadata(c.metadata, &real_type, &length);
      if (real_type == MYSQL_TYPE_ENUM || real_type == MYSQL_TYPE_SET) {
        // Elements and charset of ENUM / SET columns, counted among ENUM and
        // SET columns in column order.
        const bool is_enum = real_type == MYSQL_TYPE_ENUM;
        const auto &lists =
            is_enum ? opt.m_enum_str_value : opt.m_set_str_value;
        size_t &next = is_enum ? enums : sets;
        if (next >= lists.size()) {
          *err = "column " + c.name +
                 ": ENUM/SET values missing from the binary log "
                 "(binlog_row_metadata=FULL is required)";
          return true;
        }
        c.enum_set = real_type;
        c.elements = lists[next++];
        c.charset = enum_set_charset_of(enum_set++);
      }
    }
    info->columns.push_back(std::move(c));
  }
  for (const auto &pk : opt.m_primary_key) info->primary_key.push_back(pk.first);
  if (info->primary_key.empty()) {
    *err = "primary key missing from the binary log metadata";
    return true;
  }
  return false;
}

bool BinlogReader::handle_rows(const char *buf,
                               const bev::Format_description_event &fde,
                               const BinlogPos &at, std::string *err) {
  RowsEventHeader ev(buf, &fde);
  if (!ev.header()->get_is_valid()) return invalid_event("rows", at, err);
  const auto info_it = m_table_info.find(ev.table_id());
  if (info_it == m_table_info.end()) return false;  // not a loaded table
  const TableInfo &info = info_it->second;
  // A copy: an unload may remove the registry entry at any time.
  TableState state;
  if (!Registry::instance().snapshot(info.db, info.table, &state) ||
      !state.usable())
    return false;
  // Changes up to the load position are already in the loaded copy.
  if (!state.load_position.empty() && !(state.load_position < at))
    return false;

  if (ev.type() == bev::PARTIAL_UPDATE_ROWS_EVENT) {
    *err = info.db + "." + info.table + ": partial JSON updates not supported";
    return true;
  }
  const TableKey key{info.db, info.table};
  auto changes_it = m_trx.changes.find(key);
  if (changes_it == m_trx.changes.end())
    changes_it = m_trx.changes
                     .emplace(key, BatchTable(info, state.retain_history,
                                              state.load_position))
                     .first;
  TableChanges &changes = changes_it->second.changes;

  const std::vector<uint8_t> &data = ev.rows();
  const unsigned char *pos = data.data();
  // The event library appends one zero byte after the row data
  // (libs/mysql/binlog/event/rows_event.cpp), which is not part of any row.
  const unsigned char *end = pos + (data.empty() ? 0 : data.size() - 1);
  Row before;
  Row after;
  unsigned long long decoded = 0;
  while (pos < end) {
    bool failed = false;
    switch (ev.type()) {
      case bev::WRITE_ROWS_EVENT:
        failed = decode_row(info, ev.after_columns(), &pos, end, &after, err) ||
                 changes.insert(after, err);
        break;
      case bev::DELETE_ROWS_EVENT:
        failed = decode_row(info, ev.before_columns(), &pos, end, &before, err);
        if (!failed) changes.remove(before, m_trx.purge, m_trx.commit_time);
        break;
      case bev::UPDATE_ROWS_EVENT:
        failed =
            decode_row(info, ev.before_columns(), &pos, end, &before, err) ||
            decode_row(info, ev.after_columns(), &pos, end, &after, err) ||
            changes.update(before, after, err);
        break;
      default:
        *err = "unexpected rows event type";
        failed = true;
        break;
    }
    if (failed) {
      *err = info.db + "." + info.table + ": " + *err;
      return true;
    }
    decoded++;
  }
  m_trx.relevant = true;
  m_trx.rows += decoded;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_row_events++;
    m_rows_decoded += decoded;
  }
  // A transaction larger than a batch is written in pieces of that size, so
  // memory stays bounded (4c-2). We check after whole rows events.
  if (m_trx.rows >= m_batch_max_rows) return write_piece(err);
  return false;
}

namespace {

std::string mysql_ident(const std::string &name) {
  std::string out = "`";
  for (char c : name) {
    if (c == '`') out += '`';
    out += c;
  }
  return out + "`";
}

}  // namespace

// A DDL statement on loaded tables (DWL#4 step 4d). The server allows it
// (HTON_SECONDARY_ENGINE_SUPPORTS_DDL), locally or from replication; we bring
// the copies in line here, in binlog order: renames first, then a refresh of
// each changed table from InnoDB. The changes before the statement are
// committed first, to the copies as they were. A failure marks the table
// stale and the reader goes on: a DDL never stops the applier.
bool BinlogReader::handle_ddl(const DdlStatement &ddl, const BinlogPos &at,
                              std::string *err) {
  // A copy loaded (or refreshed) after this statement already has its effect.
  auto concerned = [&at](const TableName &t) {
    TableState state;
    return Registry::instance().snapshot(t.db, t.table, &state) &&
           (state.load_position.empty() || state.load_position < at);
  };
  // A stale copy is detached: a DDL renames it with its table, but never
  // refreshes it. It comes back only through an explicit SECONDARY_LOAD,
  // which checks the structure (DWL#3 step 3b).
  auto to_refresh = [&concerned](const TableName &t) {
    TableState state;
    return concerned(t) &&
           Registry::instance().snapshot(t.db, t.table, &state) &&
           !state.stale;
  };
  bool any = false;
  for (const auto &[from, to] : ddl.renames) any = any || concerned(from);
  for (const TableName &t : ddl.refresh) any = any || to_refresh(t);
  if (!any) return false;
  if (commit_pending(err)) return true;

  for (const auto &[from, to] : ddl.renames) {
    if (!concerned(from)) continue;
    TableState before;
    Registry::instance().snapshot(from.db, from.table, &before);
    std::string e;
    if (rename_in_lake(*m_con, from.db, from.table, to.db, to.table, &e)) {
      ddl_failed(from, "rename to " + to.db + "." + to.table + " failed: " + e);
      continue;
    }
    DUCKDB_LOG_INFO("binlog applier: DuckDB copy of %s.%s renamed to %s.%s",
                    from.db.c_str(), from.table.c_str(), to.db.c_str(),
                    to.table.c_str());
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_ddl_renames++;
    }
    // The rename a deferred refresh was waiting for: the table exists again,
    // under its new name. (Its new load position then skips any refresh of
    // the same name later in this statement.)
    if (before.refresh_pending) refresh_copy(to, ColumnRenames());
  }
  for (const TableName &t : ddl.refresh) {
    // After a rename the target name is in the registry, with the old load
    // position: it is refreshed too.
    if (to_refresh(t)) refresh_copy(t, ddl.column_renames);
  }
  std::lock_guard<std::mutex> lk(m_mutex);
  m_state = "reading";
  return false;
}

// Rebuilds the copy of t from InnoDB through an internal SECONDARY_LOAD. The
// load finds the table by its current name. When the reader is behind, the
// table may have been renamed (or dropped) since the statement: then it does
// not exist (ER_NO_SUCH_TABLE), and the refresh waits for that RENAME, which
// is later in the binary log (a DROP has already unloaded the copy).
void BinlogReader::refresh_copy(const TableName &t,
                                const ColumnRenames &ddl_renames) {
  TableState state;
  if (!Registry::instance().snapshot(t.db, t.table, &state)) return;
  // A refresh still waiting adds this statement's renames to its own.
  const ColumnRenames renames =
      state.refresh_pending
          ? compose_renames(decode_renames(state.pending_renames), ddl_renames)
          : ddl_renames;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_state = "refreshing " + t.db + "." + t.table;
  }
  if (!m_session_attached) {
    ddl_failed(t, "refresh after DDL failed: the reader cannot run internal "
                  "statements (not attached to the session service)");
    return;
  }
  request_refresh(t.db, t.table, renames);
  std::string e;
  unsigned sql_errno = 0;
  const bool failed = run_internal_statements(
      {"ALTER TABLE " + mysql_ident(t.db) + "." + mysql_ident(t.table) +
       " SECONDARY_LOAD"},
      &e, &sql_errno);
  cancel_refresh(t.db, t.table);  // if the load did not consume it
  if (!failed) {
    DUCKDB_LOG_INFO("binlog applier: DuckDB copy of %s.%s refreshed after DDL",
                    t.db.c_str(), t.table.c_str());
    std::lock_guard<std::mutex> lk(m_mutex);
    m_ddl_refreshes++;
    return;
  }
  if (sql_errno == ER_NO_SUCH_TABLE) {
    std::string pe;
    if (mark_refresh_pending_in_lake(*m_con, t.db, t.table, renames, &pe)) {
      ddl_failed(t, "cannot record the deferred refresh: " + pe);
      return;
    }
    DUCKDB_LOG_INFO(
        "binlog applier: %s.%s was renamed or dropped after the statement: "
        "its DuckDB copy is refreshed at its rename",
        t.db.c_str(), t.table.c_str());
    std::lock_guard<std::mutex> lk(m_mutex);
    m_ddl_deferred++;
    return;
  }
  ddl_failed(t, "refresh after DDL failed: " + e);
}

void BinlogReader::ddl_failed(const TableName &t, const std::string &why) {
  std::string e;
  if (mark_stale_in_lake(*m_con, t.db, t.table, &e))
    DUCKDB_LOG_WARNING("binlog applier: cannot record %s.%s as stale: %s",
                       t.db.c_str(), t.table.c_str(), e.c_str());
  DUCKDB_LOG_ERROR(
      "binlog applier: %s.%s: %s. The DuckDB copy is stale: its changes are "
      "no longer applied (classic table: SECONDARY_UNLOAD and SECONDARY_LOAD; "
      "history table: SECONDARY_LOAD reattaches it)",
      t.db.c_str(), t.table.c_str(), why.c_str());
  std::lock_guard<std::mutex> lk(m_mutex);
  m_ddl_failures++;
}

// binlog_transaction_compression=ON: the events of a transaction (all but its
// GTID event) are compressed in one payload event. We decode them as the
// replica applier does (Transaction_payload_log_event::do_apply_event): the
// inner events carry no checksum, so they are read with a copy of the format
// description that has checksums off. Their position is the position after
// the payload event, where the transaction ends.
bool BinlogReader::handle_payload(const char *buf,
                                  const bev::Format_description_event &fde,
                                  const BinlogPos &at, std::string *err) {
  bev::Transaction_payload_event tpe(buf, &fde);
  if (!tpe.header()->get_is_valid())
    return invalid_event("Transaction_payload", at, err);
  bev::Format_description_event inner_fde(fde);
  inner_fde.footer()->checksum_alg = bev::BINLOG_CHECKSUM_ALG_OFF;
  bev::compression::Payload_event_buffer_istream istream(tpe);
  bev::compression::Payload_event_buffer_istream::Buffer_ptr_t event;
  while (istream >> event) {
    if (handle_event(event->data(), at, inner_fde, err)) return true;
  }
  if (istream.has_error()) {
    *err = "cannot decompress the transaction payload before " + at.file +
           ":" + std::to_string(at.pos) + ": " + istream.get_error_str();
    return true;
  }
  return false;
}

bool BinlogReader::invalid_event(const char *what, const BinlogPos &at,
                                 std::string *err) {
  *err = std::string("invalid ") + what + " event before " + at.file + ":" +
         std::to_string(at.pos);
  return true;
}

unsigned long long BinlogReader::count_changes(const TableBatches &changes) {
  unsigned long long n = 0;
  unsigned lines = 0;
  for (const auto &table : changes) {
    n += table.second.changes.net_changes();
    if (!m_debug_log) continue;
    for (const auto &key : table.second.changes.changes()) {
      if (!key.second.has_effect() || lines >= kMaxDebugLines) continue;
      lines++;
      DUCKDB_LOG_INFO("duckdb apply: %s %s.%s key=%s %s", m_trx.gtid.c_str(),
                      table.first.first.c_str(), table.first.second.c_str(),
                      key.first.c_str(), key.second.to_string().c_str());
    }
  }
  return n;
}

bool BinlogReader::write_piece(std::string *err) {
  if (!m_large.open()) {
    // Batches end at transaction boundaries: the batch before this
    // transaction is committed on its own first.
    if (commit_pending(err) || m_large.begin(*m_con, err)) return true;
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_large_transactions++;
    }
    DUCKDB_LOG_INFO(
        "binlog applier: transaction %s changes more than %llu rows of loaded "
        "tables: applied in pieces in one DuckDB transaction",
        m_trx.gtid.c_str(), m_batch_max_rows.load());
  }
  const unsigned long long changes = count_changes(m_trx.changes);
  if (m_large.apply(*m_con, m_trx.changes, m_debug_log, err)) return true;
  // The next piece is collapsed on its own, relative to the rows written so
  // far in the DuckDB transaction.
  m_trx.changes.clear();
  m_trx.rows = 0;
  std::lock_guard<std::mutex> lk(m_mutex);
  m_pieces++;
  m_changes += changes;
  return false;
}

bool BinlogReader::commit_large(const BinlogPos &at, std::string *err) {
  const std::string gtid = m_trx.gtid.empty() ? m_batch.last_gtid : m_trx.gtid;
  const unsigned long long changes = count_changes(m_trx.changes);
  CommitStats stats;
  if (m_large.apply(*m_con, m_trx.changes, m_debug_log, err) ||
      m_large.commit(*m_con, at, gtid, &stats, err))
    return true;
  const unsigned long long written =
      stats.deleted + stats.archived + stats.inserted;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_transactions++;
    m_changes += changes;
    m_last_gtid = m_trx.gtid;
    m_pieces++;
    m_batches++;
    m_rows_written += written;
    m_archived_rows += stats.archived_rows;
    if (m_trx.purge) m_purge_transactions++;
    m_applied = at;
    m_applied_gtid = gtid;
  }
  DUCKDB_LOG_INFO("binlog applier: transaction %s applied in pieces, %llu rows "
                  "written",
                  m_trx.gtid.c_str(), written);
  // The batch was committed before the first piece: it starts again here.
  m_batch.end = at;
  m_batch.last_gtid = gtid;
  m_last_commit = std::chrono::steady_clock::now();
  m_trx = Trx();
  return false;
}

void BinlogReader::abandon_large(const char *why) {
  try {
    if (!m_con || !m_large.open()) return;
    m_large.rollback(*m_con);
    DUCKDB_LOG_WARNING(
        "binlog applier: DuckDB transaction of the large transaction %s rolled "
        "back (%s)",
        m_trx.gtid.c_str(), why);
  } catch (...) {
    // Called on error paths: it must not throw.
  }
}

bool BinlogReader::end_transaction(bool commit, const BinlogPos &at,
                                   std::string *err) {
  if (!commit) {
    abandon_large("ROLLBACK in the binary log");
    m_trx = Trx();
    return false;
  }
  if (m_large.open()) return commit_large(at, err);
  if (m_trx.relevant) {
    const unsigned long long changes = count_changes(m_trx.changes);
    {
      std::lock_guard<std::mutex> lk(m_mutex);
      m_transactions++;
      m_changes += changes;
      m_last_gtid = m_trx.gtid;
      if (m_trx.purge) m_purge_transactions++;
    }
    // A table loaded again since its changes in the batch: those changes
    // belong to the old copy, so we commit (and skip) them first.
    if (m_batch.reloaded(m_trx.changes) && commit_pending(err)) return true;
    if (m_batch.add(std::move(m_trx.changes), err)) return true;
  }
  // Every committed transaction moves the position, relevant or not.
  m_batch.end = at;
  if (!m_trx.gtid.empty()) m_batch.last_gtid = m_trx.gtid;
  m_batch.pending = true;
  m_trx = Trx();
  return maybe_commit(false, err);
}

std::chrono::steady_clock::time_point BinlogReader::batch_deadline() const {
  if (m_batch.transactions > 0)
    return m_batch.first + std::chrono::milliseconds(m_batch_max_delay_ms);
  return m_last_commit + kPositionCommitInterval;
}

bool BinlogReader::maybe_commit(bool force, std::string *err) {
  if (!m_batch.pending) return false;
  if (!force && m_batch.transactions < m_batch_max_trx &&
      m_batch.rows < m_batch_max_rows &&
      std::chrono::steady_clock::now() < batch_deadline())
    return false;
  return commit_pending(err);
}

bool BinlogReader::commit_pending(std::string *err) {
  if (!m_batch.pending) return false;
  CommitStats stats;
  if (commit_batch(*m_con, m_batch, m_debug_log, &stats, err)) return true;
  {
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_batch.transactions > 0) m_batches++;
    m_rows_written += stats.deleted + stats.archived + stats.inserted;
    m_archived_rows += stats.archived_rows;
    m_applied = m_batch.end;
    m_applied_gtid = m_batch.last_gtid;
  }
  m_batch.clear();
  m_last_commit = std::chrono::steady_clock::now();
  return false;
}

void BinlogReader::flush_on_stop() {
  // An unfinished large transaction is read again at the next start; the
  // batch before it was already committed.
  abandon_large("reader stopped");
  if (!m_con || !m_batch.pending) return;
  std::string err;
  if (commit_pending(&err))
    DUCKDB_LOG_WARNING(
        "binlog applier: the pending batch was not committed at stop (%s); "
        "it is read again at the next start",
        err.c_str());
}

}  // namespace duckdb_se
