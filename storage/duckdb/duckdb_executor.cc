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

#include "storage/duckdb/duckdb_executor.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>

#include "my_sys.h"
#include "mysql/psi/mysql_thread.h"
#include "storage/duckdb/duckdb_log.h"

namespace duckdb_se {

namespace {

PSI_thread_key key_thread_duckdb_executor = PSI_NOT_INSTRUMENTED;

#ifdef HAVE_PSI_INTERFACE
// Visible in performance_schema.threads as thread/duckdb/executor.
PSI_thread_info duckdb_thread_info[] = {
    {&key_thread_duckdb_executor, "executor", "duckdb_exec",
     PSI_FLAG_THREAD_SYSTEM | PSI_FLAG_AUTO_SEQNUM, 0, PSI_DOCUMENT_ME}};
#endif

}  // namespace

Executor &Executor::instance() {
  static Executor executor;
  return executor;
}

void Executor::register_psi() {
#ifdef HAVE_PSI_INTERFACE
  mysql_thread_register("duckdb", duckdb_thread_info, 1);
#endif
}

void *Executor::thread_main(void *arg) {
  my_thread_init();
  Executor::instance().worker_loop(static_cast<Worker *>(arg));
  my_thread_end();
  my_thread_exit(nullptr);
  return nullptr;
}

void Executor::worker_loop(Worker *self) {
  std::unique_lock<std::mutex> lk(m_mutex);
  for (;;) {
    m_work.wait(lk, [this] { return m_alive > m_target || !m_queue.empty(); });
    if (m_alive > m_target) break;  // the pool shrinks or stops
    std::shared_ptr<Task> task = m_queue.front();
    m_queue.pop_front();
    task->state = State::RUNNING;
    m_busy++;
    lk.unlock();
    // An exception must not leave the thread: std::terminate would stop
    // mysqld. The caller's result stays "failed": callers initialize it so.
    try {
      (*task->fn)();
    } catch (const std::exception &e) {
      DUCKDB_LOG_ERROR("DuckDB executor task failed: %s", e.what());
    } catch (...) {
      DUCKDB_LOG_ERROR("DuckDB executor task failed");
    }
    lk.lock();
    m_busy--;
    m_tasks++;
    task->state = State::DONE;
    task->done.notify_all();
  }
  m_alive--;
  self->exited = true;
  m_exited.notify_all();
}

bool Executor::spawn(std::string *err) {
  my_thread_attr_t attr;
  if (my_thread_attr_init(&attr) != 0) {
    *err = "cannot start a DuckDB executor thread";
    return true;
  }
  m_workers.emplace_back();
  Worker &w = m_workers.back();
  const bool failed =
      my_thread_attr_setstacksize(&attr, m_stack_size) != 0 ||
      my_thread_attr_setdetachstate(&attr, MY_THREAD_CREATE_JOINABLE) != 0 ||
      mysql_thread_create(key_thread_duckdb_executor, &w.handle, &attr,
                          thread_main, &w) != 0;
  my_thread_attr_destroy(&attr);
  if (failed) {
    m_workers.pop_back();
    *err = std::string("cannot start a DuckDB executor thread: ") +
           strerror(errno);
    return true;
  }
  m_alive++;
  return false;
}

// A worker sets exited while it holds m_mutex and needs it no more, so we can
// join it while we hold the mutex.
void Executor::join_exited() {
  for (auto it = m_workers.begin(); it != m_workers.end();) {
    if (it->exited) {
      my_thread_join(&it->handle, nullptr);
      it = m_workers.erase(it);
    } else {
      ++it;
    }
  }
}

bool Executor::start(unsigned int threads, size_t stack_size,
                     std::string *err) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (m_running) return false;
  m_stack_size = stack_size;
  m_target = threads;
  m_running = true;
  while (m_alive < m_target) {
    if (spawn(err)) {
      m_target = m_alive;  // the caller stops the pool
      return true;
    }
  }
  return false;
}

void Executor::stop() {
  std::unique_lock<std::mutex> lk(m_mutex);
  m_running = false;
  m_target = 0;
  m_work.notify_all();
  m_exited.wait(lk, [this] { return m_alive == 0; });
  for (auto &task : m_queue) {
    task->state = State::CANCELLED;
    task->done.notify_all();
  }
  m_queue.clear();
  join_exited();
}

bool Executor::resize(unsigned int threads, std::string *err) {
  std::lock_guard<std::mutex> lk(m_mutex);
  if (!m_running) return false;  // used at the next start
  join_exited();
  m_target = threads;
  bool failed = false;
  while (m_alive < m_target) {
    if (spawn(err)) {
      m_target = m_alive;
      failed = true;
      break;
    }
  }
  m_work.notify_all();
  return failed;
}

bool Executor::run(const std::function<void()> &fn,
                   const std::function<bool()> &killed,
                   const std::function<void()> &interrupt) {
  auto task = std::make_shared<Task>();
  task->fn = &fn;
  std::unique_lock<std::mutex> lk(m_mutex);
  if (!m_running || m_alive == 0) return true;
  m_queue.push_back(task);
  m_work.notify_one();
  bool interrupted = false;
  while (task->state == State::QUEUED || task->state == State::RUNNING) {
    task->done.wait_for(lk, std::chrono::milliseconds(100));
    if (interrupted || !killed || !killed()) continue;
    interrupted = true;
    if (task->state == State::QUEUED) {
      for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (*it == task) {
          m_queue.erase(it);
          break;
        }
      }
      task->state = State::CANCELLED;
    } else if (task->state == State::RUNNING && interrupt) {
      lk.unlock();
      interrupt();  // duckdb_interrupt() is safe from another thread
      lk.lock();
    }
  }
  return task->state == State::CANCELLED;
}

unsigned long Executor::busy() {
  std::lock_guard<std::mutex> lk(m_mutex);
  return m_busy;
}

unsigned long Executor::queue_length() {
  std::lock_guard<std::mutex> lk(m_mutex);
  return m_queue.size();
}

}  // namespace duckdb_se
