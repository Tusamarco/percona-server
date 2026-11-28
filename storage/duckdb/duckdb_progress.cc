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

#include "storage/duckdb/duckdb_progress.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>

#include "mysql/psi/mysql_stage.h"
#include "sql/sql_class.h"
#include "storage/duckdb/duckdb_binlog_reader.h"
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_session.h"  // is_internal_session

namespace duckdb_se {

namespace {

using Clock = std::chrono::steady_clock;

// Performance Schema stages: stage/duckdb/<name>, with progress
// (events_stages_current.WORK_COMPLETED / WORK_ESTIMATED, as InnoDB's
// stage/innodb/alter table).
PSI_stage_info stage_copy_load = {0, "copy load", PSI_FLAG_STAGE_PROGRESS,
                                  PSI_DOCUMENT_ME};
PSI_stage_info stage_wait_refresh = {0, "waiting for copy refresh",
                                     PSI_FLAG_STAGE_PROGRESS, PSI_DOCUMENT_ME};
PSI_stage_info stage_wait_applier = {0, "waiting for applier",
                                     PSI_FLAG_STAGE_PROGRESS, PSI_DOCUMENT_ME};
// While an offloaded query runs in DuckDB during optimization (the first
// chunk, the DECIMAL or grouped-value first run): until then MySQL showed
// its last own stage ("Creating tmp table"), 2026-11-26.
PSI_stage_info stage_running = {0, "running query in DuckDB", 0,
                                PSI_DOCUMENT_ME};
PSI_stage_info stage_slot_wait = {0, "waiting for query slot", 0,
                                  PSI_DOCUMENT_ME};

struct Entry {
  std::string db, table;
  bool refresh{false};
  unsigned long long total{0};
  Clock::time_point start{Clock::now()};
  std::atomic<unsigned long long> done{0};
  std::string phase{"copying rows"};  // under g_mutex
};

std::mutex g_mutex;
std::map<std::pair<std::string, std::string>, std::shared_ptr<Entry>>
    g_loads;
// The last finished refresh of each table (rows, end): the note at the end
// of a wait reports refreshes that ended between two looks.
struct Finished {
  unsigned long long rows{0};
  Clock::time_point end;
};
std::map<std::string, Finished> g_refreshed;  // "db.table"

double seconds_since(Clock::time_point t) {
  return std::chrono::duration<double>(Clock::now() - t).count();
}

int percent(unsigned long long done, unsigned long long total) {
  if (total == 0) return 0;
  // InnoDB's row count is an estimate: never show more than 99% before the
  // end.
  const unsigned long long p = done * 100 / total;
  return static_cast<int>(p > 99 ? 99 : p);
}

// What a session shows while we work for it: its state text in SHOW
// PROCESSLIST and a Performance Schema stage with progress. The previous
// stage comes back at the end. The text lives in two buffers of the
// statement's memory, written in turn: SHOW PROCESSLIST in another thread
// reads the one we are not writing.
class ThreadDisplay {
 public:
  ThreadDisplay(THD *thd, PSI_stage_info *stage) : m_thd(thd) {
    if (m_thd == nullptr) return;
    m_buf[0] = static_cast<char *>(m_thd->mem_root->Alloc(kSize));
    m_buf[1] = static_cast<char *>(m_thd->mem_root->Alloc(kSize));
    if (m_buf[0] == nullptr || m_buf[1] == nullptr) {
      m_thd = nullptr;
      return;
    }
    m_thd->enter_stage(stage, &m_old, __func__, __FILE__, __LINE__);
    m_stage = stage;
  }
  ~ThreadDisplay() {
    if (m_thd != nullptr)
      m_thd->enter_stage(&m_old, nullptr, __func__, __FILE__, __LINE__);
  }
  ThreadDisplay(const ThreadDisplay &) = delete;
  ThreadDisplay &operator=(const ThreadDisplay &) = delete;

  void stage(PSI_stage_info *stage) {
    if (m_thd == nullptr || stage == m_stage) return;
    m_thd->enter_stage(stage, nullptr, __func__, __FILE__, __LINE__);
    m_stage = stage;
  }
  void show(const std::string &text, unsigned long long done,
            unsigned long long total) {
    if (m_thd == nullptr) return;
    char *b = m_buf[m_next];
    m_next ^= 1;
    snprintf(b, kSize, "%s", text.c_str());
    m_thd->set_proc_info(b);
    PSI_stage_progress *p = m_thd->m_stage_progress_psi;
    if (p != nullptr) {
      mysql_stage_set_work_estimated(p, total > done ? total : done);
      mysql_stage_set_work_completed(p, done);
    }
  }

 private:
  static constexpr size_t kSize = 512;
  THD *m_thd;
  PSI_stage_info m_old{};
  PSI_stage_info *m_stage{nullptr};
  char *m_buf[2]{nullptr, nullptr};
  int m_next{0};
};

std::string rows_text(unsigned long long done, unsigned long long total) {
  char b[128];
  if (total > 0)
    snprintf(b, sizeof(b), "%llu of about %llu rows (%d%%)", done, total,
             percent(done, total));
  else
    snprintf(b, sizeof(b), "%llu rows", done);
  return b;
}

std::string seconds_text(double s) {
  char b[32];
  snprintf(b, sizeof(b), "%.0f s", s);
  return b;
}

}  // namespace

void register_progress_stages() {
  PSI_stage_info *stages[] = {&stage_copy_load, &stage_wait_refresh,
                              &stage_wait_applier, &stage_running,
                              &stage_slot_wait};
  mysql_stage_register("duckdb", stages,
                       static_cast<int>(sizeof(stages) / sizeof(stages[0])));
}

namespace {
std::mutex g_slot_mutex;
std::condition_variable g_slot_free;
unsigned long g_slots_used = 0;
std::atomic<unsigned long long> g_slot_waits{0};
std::atomic<unsigned long long> g_slot_wait_us{0};
}  // namespace

std::unique_ptr<QuerySlot> QuerySlot::acquire(THD *thd, unsigned limit) {
  std::unique_lock<std::mutex> lk(g_slot_mutex);
  if (limit == 0 || g_slots_used < limit) {
    g_slots_used++;
    return std::unique_ptr<QuerySlot>(new QuerySlot());
  }
  // Wait, visibly; KILL QUERY and max_execution_time stop the wait.
  const auto start = Clock::now();
  std::unique_ptr<ThreadDisplay> display;
  lk.unlock();
  display = std::make_unique<ThreadDisplay>(thd, &stage_slot_wait);
  lk.lock();
  bool killed = false;
  while (g_slots_used >= limit) {
    char text[160];
    snprintf(text, sizeof(text),
             "Waiting for a DuckDB query slot (%lu of %u running, "
             "duckdb_max_concurrent_queries)",
             g_slots_used, limit);
    lk.unlock();
    display->show(text, 0, 0);
    killed = thd != nullptr && thd->is_killed();
    lk.lock();
    if (killed) break;
    g_slot_free.wait_for(lk, std::chrono::milliseconds(100));
  }
  g_slot_waits++;
  g_slot_wait_us += static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() -
                                                            start)
          .count());
  if (killed) return nullptr;
  g_slots_used++;
  lk.unlock();
  display.reset();  // the previous stage comes back
  return std::unique_ptr<QuerySlot>(new QuerySlot());
}

QuerySlot::~QuerySlot() {
  std::lock_guard<std::mutex> lk(g_slot_mutex);
  if (g_slots_used > 0) g_slots_used--;
  g_slot_free.notify_one();
}

unsigned long QuerySlot::running() {
  std::lock_guard<std::mutex> lk(g_slot_mutex);
  return g_slots_used;
}
unsigned long long QuerySlot::waits() { return g_slot_waits.load(); }
unsigned long long QuerySlot::wait_us() { return g_slot_wait_us.load(); }

struct RunningStage::Impl {
  ThreadDisplay display;
  explicit Impl(THD *thd) : display(thd, &stage_running) {
    display.show("DuckDB: running the query (until its first rows)", 0, 0);
  }
};

RunningStage::RunningStage(THD *thd)
    : m_impl(thd != nullptr ? std::make_unique<Impl>(thd) : nullptr) {}
RunningStage::~RunningStage() = default;

struct LoadTracker::Impl {
  std::shared_ptr<Entry> entry;
  std::unique_ptr<ThreadDisplay> display;
  Clock::time_point last_log{Clock::now()};
  Clock::time_point last_show{};
};

LoadTracker::LoadTracker(THD *thd, const std::string &db,
                         const std::string &table, bool refresh,
                         unsigned long long rows_total)
    : m(new Impl) {
  m->entry = std::make_shared<Entry>();
  m->entry->db = db;
  m->entry->table = table;
  m->entry->refresh = refresh;
  m->entry->total = rows_total;
  {
    std::lock_guard<std::mutex> g(g_mutex);
    g_loads[{db, table}] = m->entry;
  }
  // A user's SECONDARY_LOAD shows its own progress; the internal session of
  // the refresh is not visible: the DDL that waits for it shows it.
  if (thd != nullptr && !is_internal_session(thd)) {
    m->display.reset(new ThreadDisplay(thd, &stage_copy_load));
    rows(0);
  }
}

LoadTracker::~LoadTracker() {
  std::lock_guard<std::mutex> g(g_mutex);
  auto it = g_loads.find({m->entry->db, m->entry->table});
  if (it != g_loads.end() && it->second == m->entry) g_loads.erase(it);
  if (m->entry->refresh) {
    if (g_refreshed.size() > 1000) g_refreshed.clear();  // bounded
    g_refreshed[m->entry->db + "." + m->entry->table] =
        Finished{m->entry->done, Clock::now()};
  }
}

void LoadTracker::rows(unsigned long long done) {
  Entry &e = *m->entry;
  e.done = done;
  const auto now = Clock::now();
  // The error log, for long loads, at info level: every 10 s.
  if (now - m->last_log >= std::chrono::seconds(10)) {
    m->last_log = now;
    DUCKDB_LOG_INFO("%s of %s.%s: %s, %.0f s",
                    e.refresh ? "refresh after DDL" : "SECONDARY_LOAD",
                    e.db.c_str(), e.table.c_str(),
                    rows_text(done, e.total).c_str(), seconds_since(e.start));
  }
  if (m->display && (done == 0 || now - m->last_show >=
                                      std::chrono::milliseconds(250))) {
    m->last_show = now;
    m->display->show("DuckDB SECONDARY_LOAD of " + e.db + "." + e.table +
                         ": " + rows_text(done, e.total) + ", " +
                         seconds_text(seconds_since(e.start)),
                     done, e.total);
  }
}

void LoadTracker::phase(const char *what) {
  {
    std::lock_guard<std::mutex> g(g_mutex);
    m->entry->phase = what;
  }
  if (m->display) {
    const Entry &e = *m->entry;
    m->display->show("DuckDB SECONDARY_LOAD of " + e.db + "." + e.table +
                         ": " + what + " (" + rows_text(e.done, e.total) +
                         "), " + seconds_text(seconds_since(e.start)),
                     e.done, e.total);
  }
}

bool load_progress(const std::string &db, const std::string &table,
                   LoadProgress *out) {
  std::lock_guard<std::mutex> g(g_mutex);
  auto it = g_loads.find({db, table});
  if (it == g_loads.end()) return false;
  const Entry &e = *it->second;
  out->db = e.db;
  out->table = e.table;
  out->refresh = e.refresh;
  out->rows_total = e.total;
  out->rows_done = e.done;
  out->seconds = seconds_since(e.start);
  out->phase = e.phase;
  return true;
}

WaitResult wait_for_applier(THD *thd, const BinlogPos &target,
                            unsigned long long timeout_ms, const char *why,
                            double *waited_seconds, std::string *refreshed,
                            unsigned long long *rows) {
  const auto start = Clock::now();
  ThreadDisplay display(thd, &stage_wait_applier);
  display.show(std::string("Waiting for the DuckDB binlog applier: ") + why,
               0, 0);
  WaitResult result = WaitResult::APPLIED;
  for (;;) {
    unsigned long long slice = 1000;
    if (timeout_ms > 0) {
      const auto spent = static_cast<unsigned long long>(
          std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                                start)
              .count());
      if (spent >= timeout_ms) {
        result = WaitResult::TIMEOUT;
        break;
      }
      if (timeout_ms - spent < slice) slice = timeout_ms - spent;
    }
    bool not_running = false;
    std::string err;
    if (!BinlogReader::instance().wait_applied(target, thd, &not_running, &err,
                                               slice)) {
      result = not_running ? WaitResult::NOT_RUNNING : WaitResult::APPLIED;
      break;
    }
    if (thd_killed(thd)) {
      result = WaitResult::KILLED;
      break;
    }
    // Still waiting: what is the applier doing?
    const std::string state = BinlogReader::instance().status().state;
    const double s = seconds_since(start);
    static const std::string kRefreshing = "refreshing ";
    LoadProgress p;
    bool shown = false;
    if (state.compare(0, kRefreshing.size(), kRefreshing) == 0) {
      const std::string name = state.substr(kRefreshing.size());
      const size_t dot = name.find('.');
      if (dot != std::string::npos &&
          load_progress(name.substr(0, dot), name.substr(dot + 1), &p)) {
        if (refreshed != nullptr) *refreshed = name;
        if (rows != nullptr) *rows = p.rows_done;
        display.stage(&stage_wait_refresh);
        display.show("Waiting for DuckDB copy refresh of " + name + ": " +
                         (p.phase == "copying rows" ? std::string()
                                                    : p.phase + ", ") +
                         rows_text(p.rows_done, p.rows_total) + ", " +
                         seconds_text(s),
                     p.rows_done, p.rows_total);
        shown = true;
      }
    }
    if (!shown) {
      display.stage(&stage_wait_applier);
      display.show("Waiting for the DuckDB binlog applier (" + state +
                       "): " + why + ", " + seconds_text(s),
                   0, 0);
    }
  }
  if (waited_seconds != nullptr) *waited_seconds = seconds_since(start);
  // Refreshes that ended while we waited (also those we never saw running).
  if (refreshed != nullptr) {
    std::lock_guard<std::mutex> g(g_mutex);
    for (const auto &[name, f] : g_refreshed) {
      if (f.end < start) continue;
      if (refreshed->empty() || *refreshed == name) {
        *refreshed = name;
        if (rows != nullptr) *rows = f.rows;
      }
    }
  }
  return result;
}

}  // namespace duckdb_se
