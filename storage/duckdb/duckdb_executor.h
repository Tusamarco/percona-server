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

#ifndef STORAGE_DUCKDB_DUCKDB_EXECUTOR_H_
#define STORAGE_DUCKDB_DUCKDB_EXECUTOR_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>

#include "my_thread.h"

namespace duckdb_se {

/**
  Pool of plugin-owned threads that run every DuckDB call. DuckDB plans and
  binds statements recursively on the calling thread, which overflows the small
  stack of MySQL connection threads (thread_stack), so DuckDB code never runs on
  a MySQL thread. Pool threads have their own stack size and no THD: they run
  only DuckDB C API calls. MySQL work (reading InnoDB, my_error) stays on the
  caller's thread.
*/
class Executor {
 public:
  static Executor &instance();
  /** Registers the thread key with the Performance Schema, at plugin init. */
  static void register_psi();

  bool start(unsigned int threads, size_t stack_size, std::string *err);
  /** Stops all threads; queued tasks are cancelled. */
  void stop();
  /** Changes the number of threads; extra threads exit after their task. */
  bool resize(unsigned int threads, std::string *err);

  /**
    Runs fn on a pool thread and waits for it. While waiting we call killed()
    every 100 ms; when it returns true a queued task is dropped and a running
    task gets interrupt() once, then we wait for it to end.
    @retval false fn ran
    @retval true  fn did not run (cancelled while queued, or pool stopped)
  */
  bool run(const std::function<void()> &fn,
           const std::function<bool()> &killed = {},
           const std::function<void()> &interrupt = {});

  unsigned long busy();
  unsigned long queue_length();
  unsigned long long tasks() const { return m_tasks.load(); }
  /** Calls that waited for a free executor thread (more than 1 ms), and
      their total wait in microseconds (2026-11-26). */
  unsigned long long queue_waits() const { return m_queue_waits.load(); }
  unsigned long long queue_wait_us() const { return m_queue_wait_us.load(); }

 private:
  enum class State { QUEUED, RUNNING, DONE, CANCELLED };
  struct Task {
    const std::function<void()> *fn{nullptr};
    State state{State::QUEUED};
    std::chrono::steady_clock::time_point queued_at{
        std::chrono::steady_clock::now()};
    std::condition_variable done;
  };
  struct Worker {
    my_thread_handle handle;
    bool exited{false};
  };

  Executor() = default;
  static void *thread_main(void *arg);
  void worker_loop(Worker *self);
  bool spawn(std::string *err);  // caller holds m_mutex
  void join_exited();            // caller holds m_mutex

  std::mutex m_mutex;
  std::condition_variable m_work;    // new task, resize or stop
  std::condition_variable m_exited;  // a worker exited
  std::deque<std::shared_ptr<Task>> m_queue;
  std::list<Worker> m_workers;
  unsigned int m_target{0};
  unsigned int m_alive{0};
  unsigned long m_busy{0};
  size_t m_stack_size{0};
  bool m_running{false};
  std::atomic<unsigned long long> m_tasks{0};
  std::atomic<unsigned long long> m_queue_waits{0};
  std::atomic<unsigned long long> m_queue_wait_us{0};
};

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_EXECUTOR_H_
