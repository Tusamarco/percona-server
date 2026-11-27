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

// Query offload, DWL#6 steps 6a and 6b: one loaded table; column references,
// literals, comparisons, BETWEEN, IN lists, LIKE, IS [NOT] NULL, AND, OR, NOT;
// + - * MOD DIV, DOUBLE division, ABS, ROUND of exact numbers, CASE, IF,
// IFNULL, COALESCE, NULLIF; date parts of DATE/DATETIME; COUNT, COUNT
// DISTINCT, SUM, AVG, MIN, MAX; DISTINCT, GROUP BY, HAVING, ORDER BY, LIMIT,
// OFFSET; PAD SPACE binary collations; TIMESTAMP constants. Anything else is
// refused with a reason (the query then runs on InnoDB).

#include "storage/duckdb/duckdb_offload_internal.h"
#include "storage/duckdb/duckdb_offload.h"

#include <chrono>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "duckdb.h"
#include "sql-common/my_decimal.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"
#include "mysqld_error.h"
#include "sql/binlog.h"  // mysql_bin_log, Log_info
#include "sql/handler.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_sum.h"
#include "sql/item_subselect.h"
#include "include/mem_root_deque.h"
#include "sql/opt_trace.h"
#include "sql/protocol.h"  // Protocol::store_short (YEAR holder)
#include "sql/query_result.h"
#include "sql/query_term.h"
#include "sql/replication.h"  // Trans_observer
#include "scope_guard.h"
#include "storage/duckdb/duckdb_progress.h"
#include "sql/sql_class.h"
#include "sql/sql_executor.h"  // QEP_TAB
#include "sql/sql_lex.h"
#include "sql/sql_optimizer.h"
#include "sql/table.h"
#include "sql/item_timefunc.h"  // Item_extract, Item_func_add_interval
#include "sql/sql_locale.h"     // MY_LOCALE (lc_time_names)
#include "sql/mysqld.h"  // system_time_zone_dst_on / _off
#include "sql/tztime.h"
#include "storage/duckdb/duckdb_binlog_reader.h"
#include "storage/duckdb/duckdb_collation.h"
#include "storage/duckdb/duckdb_guard.h"
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_load.h"  // history_scope_full
#include "storage/duckdb/duckdb_registry.h"
#include "storage/duckdb/duckdb_types.h"  // quote_ident

namespace duckdb_se {
namespace offload {

namespace {

// Read your own writes (duckdb_offload_wait_applied): the binlog position of
// the last commit of each connection that has sent a query to DuckDB. The
// server resets THD's own position at every statement, so we record it at
// commit with a transaction observer (as semi-sync replication does).
std::mutex g_commit_mutex;
std::unordered_map<my_thread_id, BinlogPos> g_last_commit;
const handlerton *g_hton = nullptr;
char g_session_marker;  // our per-connection data: close_connection is called

// The transaction observer, after the InnoDB commit of each transaction.
// It also wakes the binlog reader: with binlog_order_commits=ON the server
// commits a group in InnoDB, then calls after_commit for each transaction in
// binary log order (sql/binlog.cc, ordered_commit), so the reader never reads
// a transaction InnoDB has not committed yet. after_sync (earlier) runs
// before the InnoDB commit: DuckDB could show a transaction before InnoDB
// does, for seconds with semi-sync AFTER_SYNC waiting for a replica.
int after_commit(Trans_param *param) {
  try {
    if (param->log_file == nullptr || *param->log_file == '\0') return 0;
    BinlogReader::instance().notify_committed(param->log_file, param->log_pos);
    std::lock_guard<std::mutex> guard(g_commit_mutex);
    auto it = g_last_commit.find(param->thread_id);
    if (it != g_last_commit.end())
      it->second = BinlogPos{binlog_base_name(param->log_file), param->log_pos};
  } catch (...) {
    // Never affect a commit.
  }
  return 0;
}

Trans_observer g_trans_observer = {sizeof(Trans_observer), nullptr, nullptr,
                                   nullptr, after_commit, nullptr, nullptr};
bool g_observer_registered = false;

int close_connection(handlerton *, THD *thd) {
  guard_forget(thd);
  try {
    std::lock_guard<std::mutex> guard(g_commit_mutex);
    g_last_commit.erase(thd->thread_id());
  } catch (...) {
  }
  return 0;
}

// The position this session's next offloaded query must see in DuckDB. The
// first time, the session is registered and we do not know its earlier
// commits: we use the current end of the binary log (all sessions).
bool freshness_target(THD *thd, BinlogPos *target) {
  {
    std::lock_guard<std::mutex> guard(g_commit_mutex);
    auto it = g_last_commit.find(thd->thread_id());
    if (it != g_last_commit.end()) {
      *target = it->second;
      return false;
    }
    g_last_commit.emplace(thd->thread_id(), BinlogPos());
  }
  thd_set_ha_data(thd, g_hton, &g_session_marker);
  Log_info info;
  if (!mysql_bin_log.is_open() || mysql_bin_log.get_current_log(&info) != 0)
    return true;
  *target = BinlogPos{binlog_base_name(info.log_file_name), info.pos};
  return false;
}

std::atomic<unsigned long long> g_offloaded{0};
std::atomic<unsigned long long> g_refused{0};
std::atomic<unsigned long long> g_explained{0};
std::atomic<unsigned long long> g_waits{0};
std::atomic<unsigned long long> g_wait_timeouts{0};
std::atomic<unsigned long long> g_decimal_reruns{0};
// Time in DuckDB of the statements counted in g_offloaded (execution and
// fetch, plus a first run during optimization), in microseconds.
std::atomic<unsigned long long> g_offload_time_us{0};

unsigned long long micros_since(std::chrono::steady_clock::time_point t) {
  return static_cast<unsigned long long>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t)
          .count());
}
std::mutex g_reason_mutex;
std::string g_last_refusal;

// Per statement: the generated SQL, how to read each column, and the DuckDB
// statement prepared at optimization (a DuckDB bind error is then a refusal,
// before the server has sent anything to the client).
class OffloadContext : public Secondary_engine_execution_context {
 public:
  ~OffloadContext() override {
    restore_optimizer_switch();
    if (!con) return;
    std::string ignored;
    try {
      con->execute_cleanup(
          [this](duckdb_connection, std::string *) {
            if (first_chunk != nullptr) duckdb_destroy_data_chunk(&first_chunk);
            if (have_stream) duckdb_destroy_result(&stream_result);
            have_stream = false;
            if (stmt != nullptr) duckdb_destroy_prepare(&stmt);
            if (have_pre_result) duckdb_destroy_result(&pre_result);
            have_pre_result = false;
            return false;
          },
          &ignored);
    } catch (...) {
    }
  }
  std::string sql;
  std::vector<Kind> kinds;
  std::string reason;
  bool executed{false};
  std::unique_ptr<Lake::Connection> con;
  duckdb_prepared_statement stmt{nullptr};
  // A statement with DECIMAL values declared above 38 digits ran during
  // optimization (duckdb_decimal_overflow): its materialized result, sent by
  // execute_offloaded().
  duckdb_result pre_result;
  unsigned long long pre_run_us{0};  // first runs during optimization
  bool have_pre_result{false};
  // T2: every other statement starts in DuckDB during optimization and its
  // first chunk is fetched there (streaming); execute_offloaded() sends it
  // and fetches the rest.
  duckdb_result stream_result;
  bool have_stream{false};
  duckdb_data_chunk first_chunk{nullptr};
  bool first_fetched{false};

  // Semi-join materialization is turned off while the server optimizes a
  // statement for us: that strategy rewrites WHERE to read its own
  // temporary table (<subqueryN>), which DuckDB does not have. The other
  // strategies keep the conditions on the real tables. sql_lex.cc
  // (update_semijoin_strategies) says secondary engines choose their
  // strategies through thd->variables.optimizer_switch.
  void disable_semijoin_materialization(THD *t) {
    thd = t;
    had_materialization = (thd->variables.optimizer_switch &
                           OPTIMIZER_SWITCH_MATERIALIZATION) != 0;
    thd->variables.optimizer_switch &= ~OPTIMIZER_SWITCH_MATERIALIZATION;
    switch_changed = true;
  }
  void restore_optimizer_switch() {
    if (!switch_changed) return;
    if (had_materialization)
      thd->variables.optimizer_switch |= OPTIMIZER_SWITCH_MATERIALIZATION;
    switch_changed = false;
  }

 private:
  THD *thd{nullptr};
  bool had_materialization{false};
  bool switch_changed{false};
};

OffloadContext *context(THD *thd) {
  return static_cast<OffloadContext *>(
      thd->lex->secondary_engine_execution_context());
}

void note_refusal(THD *thd, OffloadContext *ctx, const std::string &reason) {
  ctx->reason = "the DuckDB secondary engine cannot run this query: " + reason;
  g_refused++;
  {
    std::lock_guard<std::mutex> guard(g_reason_mutex);
    g_last_refusal = reason;
  }
  Opt_trace_context *const trace = &thd->opt_trace;
  if (trace->is_started()) {
    const Opt_trace_object wrapper(trace);
    Opt_trace_object oto(trace, "duckdb_offload");
    oto.add_alnum("refused", reason.c_str());
  }
}

// ---- execution --------------------------------------------------------

// A YEAR column's holder: sent zero-filled to 4 digits, as
// Field_year::send_to_protocol does ("0000"); Item::send would send "0".
class Year_column_holder : public Item_cache_int {
 public:
  Year_column_holder() : Item_cache_int(MYSQL_TYPE_YEAR) {}
  bool send(Protocol *protocol, String *) override {
    const longlong nr = val_int();
    if (null_value) return protocol->store_null();
    return protocol->store_short(nr, 4);
  }
};

// A value holder per select-list item, typed like the item, filled per row.
Item_cache *make_holder(Item *item, Kind kind) {
  Item_cache *h = nullptr;
  switch (kind) {
    case Kind::INT:
    case Kind::UINT:
      if (item->data_type() == MYSQL_TYPE_YEAR &&
          item->real_item()->type() == Item::FIELD_ITEM)
        h = new Year_column_holder();
      else
        h = new Item_cache_int(item->data_type());
      break;
    case Kind::BIT:
      h = new Item_cache_str(item);
      break;
    case Kind::TIME: {
      // As the DATE holder: attributes copied by hand.
      auto *t = new Item_cache_time();
      if (t == nullptr) return nullptr;
      t->max_length = item->max_length;
      t->decimals = item->decimals;
      t->collation.set(item->collation);
      return t;
    }
    case Kind::DOUBLE:
      h = new Item_cache_real();
      // A FLOAT is sent as FLOAT (the metadata says so): MySQL prints its
      // shortest float digits, 0.1 and not 0.10000000149011612.
      if (h != nullptr && item->data_type() == MYSQL_TYPE_FLOAT)
        h->set_data_type(MYSQL_TYPE_FLOAT);
      break;
    case Kind::DECIMAL:
      h = new Item_cache_decimal();
      break;
    case Kind::DATE: {
      // store_value(Date_val) requires no example item: we copy the
      // attributes by hand.
      auto *d = new Item_cache_date();
      if (d == nullptr) return nullptr;
      d->max_length = item->max_length;
      d->decimals = item->decimals;
      d->collation.set(item->collation);
      return d;
    }
    case Kind::DATETIME:
    case Kind::TIMESTAMP:
      h = new Item_cache_datetime(MYSQL_TYPE_DATETIME);
      break;
    case Kind::STRING:
    case Kind::BLOB:
      h = new Item_cache_str(item);
      break;
  }
  if (h == nullptr || h->setup(item)) return nullptr;
  return h;
}

// The DuckDB type each kind is read as: checked before reading a chunk, so a
// mismatch is a query error, never a wrong read.
duckdb_type expected_type(Kind kind) {
  switch (kind) {
    case Kind::INT:
    case Kind::TIME: return DUCKDB_TYPE_BIGINT;
    case Kind::UINT:
    case Kind::BIT: return DUCKDB_TYPE_UBIGINT;
    case Kind::DOUBLE: return DUCKDB_TYPE_DOUBLE;
    case Kind::DECIMAL: return DUCKDB_TYPE_VARCHAR;
    case Kind::STRING: return DUCKDB_TYPE_VARCHAR;
    case Kind::BLOB: return DUCKDB_TYPE_BLOB;
    case Kind::DATE: return DUCKDB_TYPE_DATE;
    case Kind::DATETIME:
    case Kind::TIMESTAMP: return DUCKDB_TYPE_TIMESTAMP;
  }
  return DUCKDB_TYPE_INVALID;
}

bool vector_type_is(duckdb_vector v, Kind kind) {
  duckdb_logical_type t = duckdb_vector_get_column_type(v);
  const bool ok = duckdb_get_type_id(t) == expected_type(kind);
  duckdb_destroy_logical_type(&t);
  return ok;
}

std::string duckdb_text(duckdb_vector v, idx_t row) {
  auto *s = static_cast<duckdb_string_t *>(duckdb_vector_get_data(v)) + row;
  return std::string(duckdb_string_t_data(s), duckdb_string_t_length(*s));
}

// Converts one value; false on success.
bool store_cell(THD *thd, Item_cache *h, Item *not_null, Kind kind,
                duckdb_vector v, idx_t row, String *buf) {
  uint64_t *validity = duckdb_vector_get_validity(v);
  if (validity != nullptr && !duckdb_validity_row_is_valid(validity, row)) {
    h->store_null();
    return false;
  }
  void *data = duckdb_vector_get_data(v);
  switch (kind) {
    case Kind::INT:
      down_cast<Item_cache_int *>(h)->store_value(
          not_null, static_cast<int64_t *>(data)[row]);
      return false;
    case Kind::UINT:
      down_cast<Item_cache_int *>(h)->store_value(
          not_null, static_cast<longlong>(static_cast<uint64_t *>(data)[row]));
      // store_value() copies unsigned_flag from not_null (signed): values
      // from 2^63 would be sent as negative numbers.
      h->unsigned_flag = true;
      return false;
    case Kind::DOUBLE:
      down_cast<Item_cache_real *>(h)->store_value(
          not_null, static_cast<double *>(data)[row]);
      return false;
    case Kind::TIME: {
      const int64_t micros = static_cast<int64_t *>(data)[row];
      const uint64_t abs_us = micros < 0 ? 0 - static_cast<uint64_t>(micros)
                                         : static_cast<uint64_t>(micros);
      Time_val t;
      if (Time_val::make_time(micros < 0,
                              static_cast<uint32_t>(abs_us / 3600000000ULL),
                              static_cast<uint32_t>(abs_us / 60000000ULL % 60),
                              static_cast<uint32_t>(abs_us / 1000000ULL % 60),
                              static_cast<uint32_t>(abs_us % 1000000ULL), &t))
        return true;
      down_cast<Item_cache_time *>(h)->store_value(t);
      return false;
    }
    case Kind::BIT: {
      // Field_bit::val_str: the value big-endian over ceil(n/8) bytes.
      uint64_t bits = static_cast<uint64_t *>(data)[row];
      const size_t n = (h->max_length + 7) / 8;
      if (n < 1 || n > 8) return true;
      char bytes[8];
      for (size_t k = n; k-- > 0;) {
        bytes[k] = static_cast<char>(bits & 0xFF);
        bits >>= 8;
      }
      buf->copy(bytes, n, &my_charset_bin);
      down_cast<Item_cache_str *>(h)->store_value(not_null, *buf);
      return false;
    }
    case Kind::DECIMAL: {
      const std::string text = duckdb_text(v, row);
      my_decimal d;
      const char *end = text.data() + text.size();
      if (str2my_decimal(E_DEC_FATAL_ERROR, text.data(), &d, &end) != 0)
        return true;
      down_cast<Item_cache_decimal *>(h)->store_value(not_null, &d);
      return false;
    }
    case Kind::STRING:
    case Kind::BLOB: {
      const std::string text = duckdb_text(v, row);
      unsigned errors = 0;
      if (kind == Kind::BLOB)
        buf->copy(text.data(), text.size(), &my_charset_bin);
      else
        buf->copy(text.data(), text.size(), &my_charset_utf8mb4_bin,
                  h->collation.collation, &errors);
      down_cast<Item_cache_str *>(h)->store_value(not_null, *buf);
      return false;
    }
    case Kind::DATE: {
      const duckdb_date_struct d =
          duckdb_from_date(static_cast<duckdb_date *>(data)[row]);
      down_cast<Item_cache_date *>(h)->store_value(
          Date_val(d.year, d.month, d.day));
      return false;
    }
    case Kind::DATETIME:
    case Kind::TIMESTAMP: {
      const duckdb_timestamp ts = static_cast<duckdb_timestamp *>(data)[row];
      MYSQL_TIME t{};
      if (kind == Kind::TIMESTAMP) {
        // UTC instant: shown in the session time zone, as MySQL does.
        const int64_t micros = ts.micros;
        int64_t secs = micros / 1000000;
        int64_t frac = micros % 1000000;
        if (frac < 0) {
          frac += 1000000;
          secs -= 1;
        }
        thd->time_zone()->gmt_sec_to_TIME(&t, static_cast<my_time_t>(secs));
        t.second_part = static_cast<unsigned long>(frac);
      } else {
        const duckdb_timestamp_struct s = duckdb_from_timestamp(ts);
        t.year = s.date.year;
        t.month = s.date.month;
        t.day = s.date.day;
        t.hour = s.time.hour;
        t.minute = s.time.min;
        t.second = s.time.sec;
        t.second_part = s.time.micros;
      }
      t.time_type = MYSQL_TIMESTAMP_DATETIME;
      down_cast<Item_cache_datetime *>(h)->store_value(
          not_null, TIME_to_longlong_datetime_packed(t));
      return false;
    }
  }
  return true;
}

// EXPLAIN (any format): the DuckDB SQL and DuckDB's own plan as notes. MySQL
// prints its plan for the query; DuckDB plans the SQL itself. SHOW WARNINGS
// shows at most 512 bytes of a message, so long text is split in numbered
// notes (at line ends when possible, never inside a UTF-8 character).
void push_notes(THD *thd, const std::string &title, const std::string &text) {
  constexpr size_t kChunk = 480;  // + the title, under 512 bytes
  std::vector<std::string> parts;
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = std::min(text.size(), pos + kChunk);
    if (end < text.size()) {
      const size_t nl = text.rfind('\n', end);
      if (nl != std::string::npos && nl > pos + kChunk / 2) {
        end = nl + 1;
      } else {
        while (end > pos && (static_cast<unsigned char>(text[end]) & 0xC0) ==
                                0x80)
          end--;
      }
    }
    parts.push_back(text.substr(pos, end - pos));
    pos = end;
  }
  for (size_t i = 0; i < parts.size(); i++) {
    std::string msg = title;
    if (parts.size() > 1)
      msg += " (" + std::to_string(i + 1) + "/" +
             std::to_string(parts.size()) + ")";
    msg += ":\n" + parts[i];
    push_warning(thd, Sql_condition::SL_NOTE, ER_SECONDARY_ENGINE_PLUGIN,
                 msg.c_str());
  }
}

void explain_notes(THD *thd, OffloadContext *ctx) {
  push_notes(thd, "DuckDB SQL", ctx->sql);
  std::string plan, err;
  const bool failed = ctx->con->execute(
      [&](duckdb_connection c, std::string *e) {
        duckdb_result res;
        if (duckdb_query(c, ("EXPLAIN " + ctx->sql).c_str(), &res) ==
            DuckDBError) {
          const char *m = duckdb_result_error(&res);
          *e = m != nullptr ? m : "EXPLAIN failed";
          duckdb_destroy_result(&res);
          return true;
        }
        const idx_t columns = duckdb_column_count(&res);
        for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
          char *v = duckdb_value_varchar(&res, columns - 1, r);
          if (v != nullptr) plan += v;
          duckdb_free(v);
        }
        duckdb_destroy_result(&res);
        return false;
      },
      &err);
  push_notes(thd, "DuckDB plan", failed ? "not available: " + err : plan);
}

// A DuckDB error during execution. When MySQL has the same error, the client
// gets MySQL's code with both messages (decided 2026-11-13); otherwise
// ER_SECONDARY_ENGINE_PLUGIN with DuckDB's message.
void report_duckdb_error(const std::string &msg) {
  const std::string text = msg.substr(0, 400);
  if (msg.find(kDuckDBMultipleRows) != std::string::npos ||
      msg.find("More than one row returned by a subquery") !=
          std::string::npos) {
    my_printf_error(ER_SUBQUERY_NO_1_ROW,
                    "Subquery returns more than 1 row (DuckDB error: %s)",
                    MYF(0), text.c_str());
    return;
  }
  if (msg.find("INTERRUPT Error") != std::string::npos) {
    // max_execution_time or KILL QUERY interrupted DuckDB.
    THD *thd = current_thd;
    if (thd != nullptr && thd->killed == THD::KILL_TIMEOUT) {
      my_printf_error(ER_QUERY_TIMEOUT,
                      "Query execution was interrupted, maximum statement "
                      "execution time exceeded (DuckDB error: %s)",
                      MYF(0), text.c_str());
      return;
    }
    my_printf_error(ER_QUERY_INTERRUPTED,
                    "Query execution was interrupted (DuckDB error: %s)",
                    MYF(0), text.c_str());
    return;
  }
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
           ("DuckDB error: " + text).c_str());
}

// JOIN::override_executor_func: runs the whole statement in DuckDB and sends
// the rows. Called once per query block; single-table queries have one.
bool execute_offloaded(JOIN *join, Query_result *result) {
  THD *thd = join->thd;
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr || ctx->executed) return false;
  ctx->executed = true;
  const auto started = std::chrono::steady_clock::now();

  mem_root_deque<Item *> holders(thd->mem_root);
  std::vector<Item_cache *> caches;
  // Typed like the items the server sent as metadata (the query
  // expression's field list: with GROUP BY these can be temporary table
  // columns, e.g. BIGINT for an INT expression; for a set operation the
  // result columns), so each value is sent as the client was told.
  size_t i = 0;
  for (Item *item :
       *join->query_block->master_query_expression()->get_field_list()) {
    if (item->hidden) continue;
    if (i >= ctx->kinds.size()) {
      my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
               "select list changed after offload planning");
      return true;
    }
    Item_cache *h = make_holder(item, ctx->kinds[i++]);
    if (h == nullptr) return true;
    holders.push_back(h);
    caches.push_back(h);
  }
  Item *not_null = new Item_int(0);
  if (not_null == nullptr) return true;
  not_null->null_value = false;

  std::string err;
  Lake::Connection *con = ctx->con.get();
  if (con == nullptr || ctx->stmt == nullptr) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "the DuckDB statement was not prepared");
    return true;
  }
  duckdb_result res;
  bool have_result = false;
  duckdb_data_chunk chunk = nullptr;
  auto cleanup = [&] {
    std::string ignored;
    con->execute_cleanup(
        [&](duckdb_connection, std::string *) {
          if (chunk != nullptr) duckdb_destroy_data_chunk(&chunk);
          if (have_result) duckdb_destroy_result(&res);
          return false;
        },
        &ignored);
  };
  auto fail = [&](const std::string &msg) {
    cleanup();
    if (!thd->is_error()) report_duckdb_error(msg);
    return true;
  };

  bool use_first = false;
  if (ctx->have_pre_result) {
    // Already run during optimization: the result is ours now.
    res = ctx->pre_result;
    ctx->have_pre_result = false;
    have_result = true;
  } else if (ctx->have_stream) {
    // Started during optimization (T2): the stream and its first chunk are
    // ours now.
    res = ctx->stream_result;
    ctx->have_stream = false;
    have_result = true;
    chunk = ctx->first_chunk;
    ctx->first_chunk = nullptr;
    use_first = ctx->first_fetched;
    ctx->first_fetched = false;
  } else if (con->execute(
          [&](duckdb_connection, std::string *e) {
            if (duckdb_execute_prepared_streaming(ctx->stmt, &res) ==
                DuckDBError) {
              const char *m = duckdb_result_error(&res);
              *e = m != nullptr ? m : "execution failed";
              duckdb_destroy_result(&res);
              return true;
            }
            have_result = true;
            return false;
          },
          &err))
    return fail(err);

  String buf;
  for (;;) {
    if (use_first) {
      use_first = false;  // the chunk fetched during optimization
    } else if (con->execute(
            [&](duckdb_connection, std::string *e) {
              chunk = duckdb_fetch_chunk(res);
              if (chunk == nullptr) {
                const char *m = duckdb_result_error(&res);
                if (m != nullptr && *m != '\0') {
                  *e = m;
                  return true;
                }
              }
              return false;
            },
            &err))
      return fail(err);
    if (chunk == nullptr) break;
    const idx_t rows = duckdb_data_chunk_get_size(chunk);
    if (duckdb_data_chunk_get_column_count(chunk) != caches.size())
      return fail("unexpected column count");
    for (size_t c = 0; c < caches.size(); c++)
      if (!vector_type_is(duckdb_data_chunk_get_vector(chunk, c),
                          ctx->kinds[c]))
        return fail("unexpected type of column " + std::to_string(c + 1));
    for (idx_t r = 0; r < rows; r++) {
      if (thd->killed) return fail("interrupted");
      for (size_t c = 0; c < caches.size(); c++) {
        if (store_cell(thd, caches[c], not_null, ctx->kinds[c],
                       duckdb_data_chunk_get_vector(chunk, c), r, &buf))
          return fail("cannot convert a value of column " +
                      std::to_string(c + 1));
      }
      if (result->send_data(thd, holders)) {
        cleanup();
        return true;
      }
      join->send_records++;
    }
    std::string ignored;
    con->execute_cleanup(
        [&](duckdb_connection, std::string *) {
          duckdb_destroy_data_chunk(&chunk);
          chunk = nullptr;
          return false;
        },
        &ignored);
  }
  cleanup();
  g_offloaded++;  // run in DuckDB to the end
  g_offload_time_us += micros_since(started) + ctx->pre_run_us;
  return false;
}

// ---- hooks ------------------------------------------------------------

// Before preparing for the secondary engine: the engine must be open and the
// query expensive enough (as the MOCK engine does).
// MySQL's plan for InnoDB reads a big loaded table where its row estimate
// comes from sampled index statistics (full scan, full index scan,
// non-unique lookup): the cost can be far too low (DWL#6 6d-3: 382 instead
// of 52,560 rows per key). Primary key / unique lookups and index ranges
// (index dives) keep MySQL's estimate. The table size is InnoDB's table
// estimate, accurate enough at this scale and free during planning.
bool big_table_scan(THD *thd, std::string *which) {
  const unsigned long long min_rows = offload_min_rows(thd);
  if (min_rows == 0) return false;
  for (const Query_block *qb = thd->lex->all_query_blocks_list; qb != nullptr;
       qb = qb->next_select_in_list()) {
    const JOIN *join = qb->join;
    if (join == nullptr || join->qep_tab == nullptr) continue;
    for (uint i = 0; i < join->primary_tables; i++) {
      const QEP_TAB &tab = join->qep_tab[i];
      const TABLE *table = tab.table();
      if (table == nullptr || table->file == nullptr ||
          !table->s->has_secondary_engine())
        continue;
      const join_type type = tab.type();
      if (type != JT_ALL && type != JT_INDEX_SCAN && type != JT_REF &&
          type != JT_REF_OR_NULL)
        continue;
      if (table->file->stats.records < min_rows) continue;
      *which = std::string(table->s->db.str) + "." + table->s->table_name.str;
      return true;
    }
  }
  return false;
}

bool pre_prepare(THD *thd) {
  if (!Lake::instance().is_open()) return false;
  std::string big;
  if (thd->m_current_query_cost <=
          static_cast<double>(thd->variables.secondary_engine_cost_threshold) &&
      big_table_scan(thd, &big)) {
    Opt_trace_context *const trace = &thd->opt_trace;
    if (trace->is_started()) {
      const Opt_trace_object wrapper(trace);
      Opt_trace_object oto(trace, "duckdb_offload_candidate");
      oto.add_alnum("reason",
                    "scan of a large loaded table (duckdb_offload_min_rows)");
      oto.add_utf8("table", big.c_str());
      oto.add("cost", thd->m_current_query_cost);
    }
    return true;
  }
  if (thd->m_current_query_cost <=
      static_cast<double>(thd->variables.secondary_engine_cost_threshold)) {
    Opt_trace_context *const trace = &thd->opt_trace;
    if (trace->is_started()) {
      const Opt_trace_object wrapper(trace);
      Opt_trace_object oto(trace, "secondary_engine_not_used");
      oto.add_alnum("reason",
                    "The estimated query cost does not exceed "
                    "secondary_engine_cost_threshold.");
      oto.add("cost", thd->m_current_query_cost);
      oto.add("threshold", thd->variables.secondary_engine_cost_threshold);
    }
    return false;
  }
  return true;
}

bool prepare(THD *thd, LEX *lex) {
  auto *ctx = new (thd->mem_root) OffloadContext;
  if (ctx == nullptr) return true;
  lex->set_secondary_engine_execution_context(ctx);
  ctx->disable_semijoin_materialization(thd);
  // The handler cannot read rows: no constant tables, no subquery evaluated
  // during optimization (as the MOCK engine).
  lex->add_statement_options(OPTION_NO_CONST_TABLES |
                             OPTION_NO_SUBQUERY_DURING_OPTIMIZATION);
  return false;
}

// A session that wrote in its open transaction would not see its own
// uncommitted rows in DuckDB.
bool wrote_in_transaction(THD *thd) {
  if (!thd->in_active_multi_stmt_transaction()) return false;
  Transaction_ctx *trn = thd->get_transaction();
  for (auto &info : trn->ha_trx_info(Transaction_ctx::SESSION))
    if (info.is_trx_read_write()) return true;
  return false;
}

// DuckDB errors that mean a DECIMAL value did not fit in 38 digits: by
// error type (an overflow in arithmetic is OUT_OF_RANGE, in a cast to
// DECIMAL(38, s) CONVERSION), and the value is a DECIMAL (its 128-bit
// integer). Other errors are never retried with fewer decimals.
bool is_decimal_overflow(duckdb_error_type type, const std::string &msg) {
  if (type != DUCKDB_ERROR_OUT_OF_RANGE && type != DUCKDB_ERROR_CONVERSION)
    return false;
  return msg.find("DECIMAL") != std::string::npos ||
         msg.find("HUGEINT") != std::string::npos ||
         msg.find("INT128") != std::string::npos;
}

bool optimize(THD *thd, LEX *lex) {
  OffloadContext *ctx = context(thd);
  if (ctx == nullptr) return true;
  ctx->restore_optimizer_switch();  // the server's optimization is done
  std::string reason;
  bool wide = false;  // DECIMAL values declared above 38 digits
  bool grouped_check = false;  // grouped strings checked in the query
  int max_scale = 0;
  auto build = [&](int scale_cap) {
    Generator gen(thd, &reason);
    gen.set_decimal_scale_cap(scale_cap);
    ctx->kinds.clear();
    ctx->sql.clear();
    gen.generate(lex, &ctx->sql, &ctx->kinds);
    wide = gen.decimal_wide();
    max_scale = gen.decimal_max_scale();
    grouped_check = gen.grouped_check();
  };
  if (wrote_in_transaction(thd)) {
    reason = "written_in_transaction";
  } else {
    build(-1);
  }
  // Freshness (duckdb_offload_wait_applied): the session's last commit must
  // be in DuckDB before we read it there.
  if (reason.empty() && offload_wait_applied(thd)) {
    BinlogPos target;
    if (freshness_target(thd, &target)) {
      reason = "binary_log_position_unknown";
    } else if (!target.empty()) {
      g_waits++;
      // A statement that can only run in DuckDB (history table under FULL:
      // InnoDB has no archived rows) waits without a limit; KILL QUERY and
      // max_execution_time stop it. Others wait duckdb_offload_wait_timeout_ms
      // and then run on InnoDB. The wait shows its progress (SHOW
      // PROCESSLIST, Performance Schema).
      const bool only_duckdb = lex->can_execute_only_in_secondary_engine();
      const unsigned long long limit = offload_wait_timeout_ms(thd);
      const WaitResult w = wait_for_applier(
          thd, target, only_duckdb ? 0 : (limit == 0 ? 1 : limit),
          "the session's last commit", nullptr, nullptr, nullptr);
      if (w == WaitResult::TIMEOUT) {
        g_wait_timeouts++;
        reason = "applier_behind";
      } else if (w == WaitResult::KILLED) {
        reason = "applier_behind";
      } else if (w == WaitResult::NOT_RUNNING) {
        reason = "applier_not_running";
      }
    }
  }
  // Prepared now: a DuckDB bind error (types MySQL accepts but DuckDB does
  // not) is a refusal, and the query runs on InnoDB.
  auto prepare_sql = [&](std::string *err) {
    return ctx->con->execute(
        [ctx](duckdb_connection c, std::string *e) {
          if (ctx->stmt != nullptr) duckdb_destroy_prepare(&ctx->stmt);
          ctx->stmt = nullptr;
          if (duckdb_prepare(c, ctx->sql.c_str(), &ctx->stmt) == DuckDBError) {
            const char *m = duckdb_prepare_error(ctx->stmt);
            *e = m != nullptr ? m : "prepare failed";
            duckdb_destroy_prepare(&ctx->stmt);
            ctx->stmt = nullptr;
            return true;
          }
          return false;
        },
        err);
  };
  // Run now, materialized (a DECIMAL overflow is then a refusal, not an
  // error in the middle of sending rows).
  duckdb_error_type run_error = DUCKDB_ERROR_INVALID;
  auto run_first = [&](std::string *err) {
    const auto started = std::chrono::steady_clock::now();
    auto timed = create_scope_guard(
        [&] { ctx->pre_run_us += micros_since(started); });
    return ctx->con->execute(
        [ctx, &run_error](duckdb_connection, std::string *e) {
          if (duckdb_execute_prepared(ctx->stmt, &ctx->pre_result) ==
              DuckDBError) {
            const char *m = duckdb_result_error(&ctx->pre_result);
            *e = m != nullptr ? m : "execution failed";
            run_error = duckdb_result_error_type(&ctx->pre_result);
            duckdb_destroy_result(&ctx->pre_result);
            return true;
          }
          ctx->have_pre_result = true;
          return false;
        },
        err);
  };
  if (reason.empty()) {
    std::string err;
    ctx->con = Lake::instance().connect(thd, &err);
    if (!ctx->con)
      reason = "duckdb_connection: " + err;
    else if (prepare_sql(&err))
      reason = "duckdb_prepare: " + err.substr(0, 200);
  }
  // DECIMAL values MySQL declares above 38 digits (duckdb_decimal_overflow):
  // DuckDB computes them as DECIMAL(38, scale), exact while they fit. With
  // FALLBACK and ROUND the statement runs here first; ERROR streams as
  // usual and an overflow is the statement's error.
  const DecimalOverflow mode = decimal_overflow(thd);
  auto ambiguous = [](const std::string &err) {
    return err.find(kGroupedAmbiguous) != std::string::npos;
  };
  const char *const kAmbiguousReason =
      "grouped_string_value_in_join: a group holds values that are equal "
      "under the collation but stored differently ('a' and 'a ', or 'A' and "
      "'a'); InnoDB shows one of them by its join order";
  // From here DuckDB may run the statement (first run, first chunk): the
  // session shows it (stage/duckdb/running query in DuckDB).
  std::unique_ptr<RunningStage> running;
  if (reason.empty() && !lex->is_explain())
    running = std::make_unique<RunningStage>(thd);
  if (reason.empty() && (grouped_check || (wide && mode != DecimalOverflow::ERROR)) &&
      !lex->is_explain()) {
    std::string err;
    if (run_first(&err)) {
      if (ambiguous(err)) {
        reason = kAmbiguousReason;
      } else if (!wide || mode == DecimalOverflow::ERROR) {
        reason = "duckdb_execution: " + err.substr(0, 200);
      } else if (!is_decimal_overflow(run_error, err)) {
        reason = "duckdb_execution: " + err.substr(0, 200);
      } else if (mode == DecimalOverflow::FALLBACK) {
        reason = "decimal_overflow: a DECIMAL value exceeds DuckDB's 38 "
                 "digits (duckdb_decimal_overflow=FALLBACK): " +
                 err.substr(0, 160);
      } else {
        // ROUND: fewer decimals until the values fit, with a warning.
        // Fewer decimals each time: 1, 2, 4, 8 ... fewer, then none.
        bool done = false;
        int cap = max_scale;
        int step = 1;
        while (!done && reason.empty() && cap > 0) {
          cap = cap > step ? cap - step : 0;
          step *= 2;
          build(cap);
          if (!reason.empty()) break;
          g_decimal_reruns++;
          if (prepare_sql(&err)) {
            reason = "duckdb_prepare: " + err.substr(0, 200);
          } else if (!run_first(&err)) {
            done = true;
            push_warning_printf(
                thd, Sql_condition::SL_WARNING, ER_SECONDARY_ENGINE_PLUGIN,
                "DuckDB: DECIMAL results rounded to %d decimals: a value "
                "exceeds DuckDB's 38 digits with %d decimals "
                "(duckdb_decimal_overflow=ROUND)",
                cap, max_scale);
          } else if (ambiguous(err)) {
            reason = kAmbiguousReason;
          } else if (!is_decimal_overflow(run_error, err)) {
            reason = "duckdb_execution: " + err.substr(0, 200);
          }
        }
        if (!done && reason.empty())
          reason = "decimal_overflow: a DECIMAL value exceeds DuckDB's 38 "
                   "digits, also with fewer decimals (the overflow is in "
                   "DuckDB's own arithmetic; duckdb_decimal_overflow=ROUND)";
      }
    }
  }
  // T2 (2026-11-23): we start the statement here and fetch its first chunk.
  // DuckDB runs the blocking parts of a plan (hash join builds, aggregates,
  // sorts) before it returns a first chunk, which is where out of memory or
  // a full temporary disk happen. An error here is a refusal: MySQL then runs
  // the statement on InnoDB (use_secondary_engine = ON). It cannot once our
  // executor has started (Sql_cmd_dml::execute_inner marks the statement
  // executed before it), so a later error still reaches the client. The rest
  // is streamed by execute_offloaded(), never materialized.
  // Not with duckdb_decimal_overflow = ERROR on a statement with DECIMAL
  // values above 38 digits: there an overflow must be the statement's error
  // (also with ON), and any error here would make MySQL run it on InnoDB.
  if (reason.empty() && !lex->is_explain() && !ctx->have_pre_result &&
      !(wide && mode == DecimalOverflow::ERROR)) {
    bool simulate = false;
    DBUG_EXECUTE_IF("duckdb_offload_fail_first_chunk", simulate = true;);
    std::string err;
    const auto started = std::chrono::steady_clock::now();
    const bool failed = ctx->con->execute(
        [ctx, simulate](duckdb_connection, std::string *e) {
          if (duckdb_execute_prepared_streaming(ctx->stmt,
                                                &ctx->stream_result) ==
              DuckDBError) {
            const char *m = duckdb_result_error(&ctx->stream_result);
            *e = m != nullptr ? m : "execution failed";
            duckdb_destroy_result(&ctx->stream_result);
            return true;
          }
          ctx->have_stream = true;
          if (simulate) {
            *e = "Out of Memory Error: simulated (duckdb_offload_fail_first_chunk)";
            return true;
          }
          ctx->first_chunk = duckdb_fetch_chunk(ctx->stream_result);
          if (ctx->first_chunk == nullptr) {
            const char *m = duckdb_result_error(&ctx->stream_result);
            if (m != nullptr && *m != '\0') {
              *e = m;
              return true;
            }
          }
          ctx->first_fetched = true;
          return false;
        },
        &err);
    ctx->pre_run_us += micros_since(started);
    if (failed) {
      // KILL QUERY / max_execution_time, and a scalar subquery with more
      // than one row: MySQL's own error (InnoDB gives the same; a killed
      // statement is never retried).
      if (thd->is_killed() ||
          err.find("More than one row returned by a subquery") !=
              std::string::npos ||
          err.find(kDuckDBMultipleRows) != std::string::npos) {
        report_duckdb_error(err);
        return true;
      }
      reason = "duckdb_execution: " + err.substr(0, 200);
    }
  }
  if (!reason.empty()) {
    note_refusal(thd, ctx, reason);
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), ctx->reason.c_str());
    return true;
  }
  if (lex->is_explain()) explain_notes(thd, ctx);
  // The server calls the executor of each query block of a set operation;
  // the first call runs the whole statement.
  for (Query_block *qb = lex->unit->first_query_block(); qb != nullptr;
       qb = qb->next_query_block())
    if (qb->join != nullptr) qb->join->override_executor_func = execute_offloaded;
  // Counted when it has run (execute_offloaded); EXPLAIN separately.
  if (lex->is_explain()) g_explained++;
  Opt_trace_context *const trace = &thd->opt_trace;
  if (trace->is_started()) {
    const Opt_trace_object wrapper(trace);
    Opt_trace_object oto(trace, "duckdb_offload");
    oto.add_utf8("sql", ctx->sql.c_str());
  }
  return false;
}

// DuckDB makes its own plan from the SQL: the server's join order is not
// used, so its own comparison stands (`cheaper` is already set, per JOIN; a
// statement-wide best cost made every plan of a second JOIN "not cheaper"
// and the planner found none).
bool compare_cost(THD *, const JOIN &, double optimizer_cost,
                  bool *use_best_so_far, bool *,
                  double *secondary_engine_cost) {
  *use_best_so_far = false;
  *secondary_engine_cost = optimizer_cost;
  return false;
}

// The server asks for a reason whenever a FORCED query did not run in the
// secondary engine, also when it never asked us (sql_select.cc,
// find_and_set_offload_fail_reason, asserts a non-empty reason): with
// autocommit=0 the statement is part of a multi-statement transaction and
// the server does not offload it, before any of our hooks runs.
std::string_view fail_reason(THD *thd) {
  OffloadContext *ctx = context(thd);
  if (ctx != nullptr && !ctx->reason.empty()) return ctx->reason;
  if (thd != nullptr && thd->in_multi_stmt_transaction_mode())
    return "autocommit=0 or an open transaction: MySQL does not run queries "
           "of multi-statement transactions in a secondary engine (use "
           "autocommit=1)";
  return "the query was not sent to the DuckDB secondary engine";
}

}  // namespace

}  // namespace offload

using namespace offload;

bool register_offload_observer(void *plugin) {
  if (register_trans_observer(&g_trans_observer, plugin) != 0) return true;
  g_observer_registered = true;
  return false;
}

void unregister_offload_observer(void *plugin) {
  if (!g_observer_registered) return;
  unregister_trans_observer(&g_trans_observer, plugin);
  g_observer_registered = false;
}

void install_offload_hooks(handlerton *hton) {
  g_hton = hton;
  hton->close_connection = close_connection;
  hton->secondary_engine_pre_prepare_hook = pre_prepare;
  hton->prepare_secondary_engine = prepare;
  hton->optimize_secondary_engine = optimize;
  hton->compare_secondary_engine_cost = compare_cost;
  hton->find_secondary_engine_offload_fail_reason = fail_reason;
  hton->secondary_engine_flags = MakeSecondaryEngineFlags(
      SecondaryEngineFlag::SUPPORTS_HASH_JOIN,
      SecondaryEngineFlag::SUPPORTS_NESTED_LOOP_JOIN,
      SecondaryEngineFlag::USE_EXTERNAL_EXECUTOR);
}

bool session_commit_position(THD *thd, BinlogPos *pos) {
  return freshness_target(thd, pos);
}

OffloadStatus offload_status() {
  OffloadStatus s;
  s.offloaded = g_offloaded;
  s.refused = g_refused;
  s.explained = g_explained;
  s.waits = g_waits;
  s.wait_timeouts = g_wait_timeouts;
  s.decimal_reruns = g_decimal_reruns;
  s.time_us = g_offload_time_us;
  std::lock_guard<std::mutex> guard(g_reason_mutex);
  s.last_refusal = g_last_refusal;
  return s;
}

}  // namespace duckdb_se
