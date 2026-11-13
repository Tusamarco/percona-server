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

#include "storage/duckdb/duckdb_applier.h"

#include <mutex>
#include <string>
#include <vector>

#include "duckdb.h"
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_types.h"

namespace duckdb_se {

namespace {

// Lake column without a binlog column: the archive time of history tables.
constexpr int kArchivedAtColumn = -1;

// Each append converts the value to the column type; a value out of range is
// an error (checked against the in-tree DuckDB).
bool append_value(duckdb_appender app, const Value &v) {
  switch (v.kind) {
    case Value::Kind::NUL:
      return duckdb_append_null(app) == DuckDBError;
    case Value::Kind::INT:
      return duckdb_append_int64(app, v.i) == DuckDBError;
    case Value::Kind::UINT:
      return duckdb_append_uint64(app, v.u) == DuckDBError;
    case Value::Kind::DOUBLE:
      return duckdb_append_double(app, v.d) == DuckDBError;
    case Value::Kind::TEXT:
      return duckdb_append_varchar_length(app, v.s.data(), v.s.size()) ==
             DuckDBError;
    case Value::Kind::BLOB:
      return duckdb_append_blob(app, v.s.data(), v.s.size()) == DuckDBError;
    case Value::Kind::DATE:
      return duckdb_append_date(app,
                                duckdb_date{static_cast<int32_t>(v.i)}) ==
             DuckDBError;
    case Value::Kind::TIMESTAMP:
      return duckdb_append_timestamp(app, duckdb_timestamp{v.i}) ==
             DuckDBError;
  }
  return true;
}

// 0 means "not archived": NULL.
bool append_archive_time(duckdb_appender app, uint64_t micros) {
  if (micros == 0) return duckdb_append_null(app) == DuckDBError;
  return duckdb_append_timestamp(
             app, duckdb_timestamp{static_cast<int64_t>(micros)}) ==
         DuckDBError;
}

std::string appender_error(duckdb_appender app) {
  const char *msg = duckdb_appender_error(app);
  return msg != nullptr ? msg : "DuckDB appender error";
}

std::string lake_table(const TableInfo &info) {
  return "lake." + quote_ident(info.db) + "." + quote_ident(info.table);
}

// The lake columns in table order, mapped to binlog column indexes by name.
// The load copies only some columns (the read set), and adds __archived_at
// to history tables.
bool map_columns(duckdb_connection c, const TableInfo &info,
                 std::vector<int> *map, std::string *err) {
  duckdb_result res;
  const std::string sql =
      "SELECT column_name FROM duckdb_columns() WHERE database_name = 'lake' "
      "AND schema_name = " +
      quote_literal(info.db) + " AND table_name = " +
      quote_literal(info.table) + " ORDER BY column_index";
  if (duckdb_query(c, sql.c_str(), &res) == DuckDBError) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "cannot read the DuckDB table columns";
    duckdb_destroy_result(&res);
    return true;
  }
  std::vector<std::string> names;
  for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
    char *v = duckdb_value_varchar(&res, 0, r);
    names.emplace_back(v != nullptr ? v : "");
    duckdb_free(v);
  }
  duckdb_destroy_result(&res);
  if (names.empty()) {
    *err = "table not found in the lake";
    return true;
  }
  for (const std::string &name : names) {
    if (name == "__archived_at") {
      map->push_back(kArchivedAtColumn);
      continue;
    }
    int found = -1;
    for (size_t i = 0; i < info.columns.size() && found < 0; i++)
      if (info.columns[i].name == name) found = static_cast<int>(i);
    if (found < 0) {
      *err = "column " + name + " of the DuckDB table is not in the binary log";
      return true;
    }
    map->push_back(found);
  }
  return false;
}

std::string key_columns(const BatchTable &bt) {
  std::string cols;
  for (unsigned int i : bt.changes.primary_key())
    cols += (cols.empty() ? "" : ", ") + quote_ident(bt.info.columns[i].name);
  return cols;
}

std::string key_join(const BatchTable &bt) {
  std::string on;
  for (unsigned int i : bt.changes.primary_key()) {
    const std::string c = quote_ident(bt.info.columns[i].name);
    on += (on.empty() ? "" : " AND ") + ("k." + c + " = t." + c);
  }
  return on;
}

// A temporary table with the key columns of the lake table (same types), plus
// __ts for archives, filled with the keys whose original row has `fate`.
// Temporary tables outlive the transaction, so we replace them each time.
bool fill_keys(duckdb_connection c, const std::string &target,
               const std::string &temp, const BatchTable &bt,
               KeyChange::Original fate, std::string *err) {
  const bool with_time = fate == KeyChange::Original::ARCHIVE;
  if (run_sql(c,
              "CREATE OR REPLACE TEMP TABLE " + temp + " AS SELECT " +
                  key_columns(bt) +
                  (with_time ? ", NULL::TIMESTAMP AS __ts" : "") + " FROM " +
                  target + " LIMIT 0",
              err))
    return true;
  duckdb_appender app = nullptr;
  if (duckdb_appender_create_ext(c, "temp", "main", temp.c_str(), &app) ==
      DuckDBError) {
    *err = appender_error(app);
    duckdb_appender_destroy(&app);
    return true;
  }
  bool failed = false;
  for (const auto &[key, kc] : bt.changes.changes()) {
    if (kc.original != fate) continue;
    for (unsigned int i : bt.changes.primary_key())
      failed = failed || i >= kc.key_row.size() ||
               append_value(app, kc.key_row[i]);
    if (with_time)
      failed = failed || append_archive_time(app, kc.original_archived_at);
    failed = failed || duckdb_appender_end_row(app) == DuckDBError;
    if (failed) break;
  }
  if (!failed) failed = duckdb_appender_close(app) == DuckDBError;
  if (failed) *err = appender_error(app);
  duckdb_appender_destroy(&app);
  return failed;
}

// Applies one table's changes inside the open transaction: deletes, then
// archive marks, then inserts.
bool apply_table(duckdb_connection c, const BatchTable &bt, bool debug_log,
                 CommitStats *stats, std::string *err) {
  const TableInfo &info = bt.info;
  const bool history = bt.changes.retain_history();
  const std::string target = lake_table(info);
  std::vector<int> map;
  if (map_columns(c, info, &map, err)) return true;

  unsigned long long deletes = 0, archives = 0, inserts = 0;
  for (const auto &[key, kc] : bt.changes.changes()) {
    deletes += kc.original == KeyChange::Original::DELETE;
    archives += kc.original == KeyChange::Original::ARCHIVE;
    inserts += kc.archived_rows.size() + (kc.has_live_row ? 1 : 0);
  }

  // 1. Deletes. On history tables only the live row: archived rows of the
  //    same key stay.
  if (deletes > 0 &&
      (fill_keys(c, target, "__duckdb_del", bt, KeyChange::Original::DELETE,
                 err) ||
       run_sql(c,
               "DELETE FROM " + target + " AS t WHERE " +
                   (history ? "t.__archived_at IS NULL AND " : "") +
                   "EXISTS (SELECT 1 FROM __duckdb_del k WHERE " +
                   key_join(bt) + ")",
               err)))
    return true;

  // 2. Archive marks on the live row (history tables only).
  if (archives > 0 &&
      (fill_keys(c, target, "__duckdb_arc", bt, KeyChange::Original::ARCHIVE,
                 err) ||
       run_sql(c,
               "UPDATE " + target +
                   " AS t SET __archived_at = k.__ts FROM __duckdb_arc k "
                   "WHERE " +
                   key_join(bt) + " AND t.__archived_at IS NULL",
               err)))
    return true;

  // 3. Inserts: archived rows (with their archive time) and live rows.
  if (inserts > 0) {
    duckdb_appender app = nullptr;
    if (duckdb_appender_create_ext(c, "lake", info.db.c_str(),
                                   info.table.c_str(), &app) == DuckDBError) {
      *err = appender_error(app);
      duckdb_appender_destroy(&app);
      return true;
    }
    bool failed = false;
    auto append_row = [&](const Row &row, uint64_t archived_at) {
      for (int col : map) {
        if (failed) return;
        if (col == kArchivedAtColumn)
          failed = append_archive_time(app, archived_at);
        else
          failed = static_cast<size_t>(col) >= row.size() ||
                   append_value(app, row[col]);
      }
      failed = failed || duckdb_appender_end_row(app) == DuckDBError;
    };
    for (const auto &[key, kc] : bt.changes.changes()) {
      for (const ArchivedRow &a : kc.archived_rows)
        append_row(a.row, a.archived_at);
      if (kc.has_live_row) append_row(kc.live_row, 0);
      if (failed) break;
    }
    if (!failed) failed = duckdb_appender_close(app) == DuckDBError;
    if (failed) *err = appender_error(app);
    duckdb_appender_destroy(&app);
    if (failed) return true;
  }

  unsigned long long archived_inserts = 0;
  for (const auto &[key, kc] : bt.changes.changes())
    archived_inserts += kc.archived_rows.size();
  stats->deleted += deletes;
  stats->archived += archives;
  stats->inserted += inserts;
  stats->archived_rows += archives + archived_inserts;
  if (debug_log) {
    // Counts inside the transaction, to compare with InnoDB.
    const std::string sql =
        history ? "SELECT count(*) FILTER (WHERE __archived_at IS NULL) || "
                  "' live, ' || count(*) FILTER (WHERE __archived_at IS NOT "
                  "NULL) || ' archived' FROM " +
                      target
                : "SELECT count(*) || ' rows' FROM " + target;
    std::string counts;
    std::string ignored;
    if (query_value_sql(c, sql, &counts, &ignored)) counts = "?";
    DUCKDB_LOG_INFO(
        "duckdb apply: %s.%s deleted=%llu archived=%llu inserted=%llu; "
        "DuckDB now %s",
        info.db.c_str(), info.table.c_str(), deletes, archives, inserts,
        counts.c_str());
  }
  return false;
}

// Replaces the applied position; inside the caller's transaction.
bool write_position(duckdb_connection c, const BinlogPos &pos,
                    const std::string &gtid, std::string *err) {
  return run_sql(c, "DELETE FROM lake.__mysql.applied_position", err) ||
         run_sql(c,
                 "INSERT INTO lake.__mysql.applied_position VALUES (1, " +
                     quote_literal(pos.file) + ", " +
                     std::to_string(pos.pos) + ", " + quote_literal(gtid) +
                     ", now())",
                 err);
}

}  // namespace

bool store_applied_position(Lake::Connection &con, const BinlogPos &pos,
                            const std::string &gtid, std::string *err) {
  return con.execute(
      [&](duckdb_connection c, std::string *e) {
        if (run_sql(c, "BEGIN TRANSACTION", e)) return true;
        if (write_position(c, pos, gtid, e) || run_sql(c, "COMMIT", e)) {
          std::string ignored;
          run_sql(c, "ROLLBACK", &ignored);
          return true;
        }
        return false;
      },
      err);
}

bool Batch::add(TableBatches &&trx, std::string *err) {
  for (auto &[key, bt] : trx) {
    rows += bt.changes.input_rows();
    auto it = tables.find(key);
    if (it == tables.end()) {
      tables.emplace(key, std::move(bt));
      continue;
    }
    if (it->second.changes.merge(bt.changes, err)) {
      *err = key.first + "." + key.second + ": " + *err;
      return true;
    }
    it->second.info = std::move(bt.info);
  }
  if (transactions++ == 0) first = std::chrono::steady_clock::now();
  pending = true;
  return false;
}

bool Batch::reloaded(const TableBatches &trx) const {
  for (const auto &[key, bt] : trx) {
    auto it = tables.find(key);
    if (it != tables.end() &&
        (it->second.load_position < bt.load_position ||
         bt.load_position < it->second.load_position))
      return true;
  }
  return false;
}

void Batch::clear() {
  tables.clear();
  transactions = 0;
  rows = 0;
  pending = false;
  // end and last_gtid stay: they are the applied position from now on.
}

const char *applied_position_create_sql() {
  return "CREATE TABLE IF NOT EXISTS lake.__mysql.applied_position ("
         "id INTEGER, binlog_file VARCHAR, binlog_pos UBIGINT, gtid VARCHAR, "
         "applied_at TIMESTAMP)";
}

bool OpenTransaction::begin(Lake::Connection &con, std::string *err) {
  // An unload holds the same mutex: a table is never written after its drop,
  // and we check the registry under the mutex.
  m_lock = std::unique_lock<std::mutex>(Lake::instance().apply_mutex());
  m_stats = CommitStats();
  if (con.run("BEGIN TRANSACTION", err)) {
    m_lock.unlock();
    return true;
  }
  return false;
}

bool OpenTransaction::apply(Lake::Connection &con, const TableBatches &tables,
                            bool debug_log, std::string *err) {
  CommitStats local;
  if (con.execute(
          [&](duckdb_connection c, std::string *e) {
            for (const auto &[key, bt] : tables) {
              TableState state;
              // Unloaded, stale, waiting for a refresh, or loaded again
              // (the new copy has the changes).
              if (!Registry::instance().snapshot(key.first, key.second,
                                                 &state) ||
                  !state.usable() || state.load_position < bt.load_position ||
                  bt.load_position < state.load_position)
                continue;
              if (apply_table(c, bt, debug_log, &local, e)) {
                *e = key.first + "." + key.second + ": " + *e;
                return true;
              }
              // A copy loaded before DWL#3 step 3b has no recorded primary
              // key: we take it from the binary log (Table_map), for the
              // structure check of a later takeover.
              if (state.pk_columns.empty()) {
                std::string pk;
                for (unsigned int i : bt.changes.primary_key())
                  pk += (pk.empty() ? "" : "\x1f") + bt.info.columns[i].name;
                if (run_sql(c,
                            "UPDATE lake.__mysql.tables SET pk_columns = " +
                                quote_literal(pk) + " WHERE db = " +
                                quote_literal(key.first) +
                                " AND tbl = " + quote_literal(key.second),
                            e))
                  return true;
                Registry::instance().set_pk_columns(key.first, key.second, pk);
              }
            }
            return false;
          },
          err)) {
    rollback(con);
    return true;
  }
  m_stats.deleted += local.deleted;
  m_stats.archived += local.archived;
  m_stats.inserted += local.inserted;
  m_stats.archived_rows += local.archived_rows;
  return false;
}

bool OpenTransaction::commit(Lake::Connection &con, const BinlogPos &end,
                             const std::string &gtid, CommitStats *stats,
                             std::string *err) {
  if (con.execute(
          [&](duckdb_connection c, std::string *e) {
            return write_position(c, end, gtid, e) || run_sql(c, "COMMIT", e);
          },
          err)) {
    rollback(con);
    return true;
  }
  stats->deleted += m_stats.deleted;
  stats->archived += m_stats.archived;
  stats->inserted += m_stats.inserted;
  stats->archived_rows += m_stats.archived_rows;
  m_stats = CommitStats();
  m_lock.unlock();
  return false;
}

void OpenTransaction::rollback(Lake::Connection &con) {
  if (!open()) return;
  std::string ignored;
  // Not interruptible: it must run even after a failure. After a failed
  // COMMIT there may be no transaction left, and ROLLBACK fails harmlessly.
  try {
    con.execute_cleanup(
        [](duckdb_connection c, std::string *e) {
          return run_sql(c, "ROLLBACK", e);
        },
        &ignored);
  } catch (...) {
  }
  m_stats = CommitStats();
  m_lock.unlock();
}

bool commit_batch(Lake::Connection &con, const Batch &batch, bool debug_log,
                  CommitStats *stats, std::string *err) {
  OpenTransaction trx;
  return trx.begin(con, err) || trx.apply(con, batch.tables, debug_log, err) ||
         trx.commit(con, batch.end, batch.last_gtid, stats, err);
}

bool read_applied_position(Lake::Connection &con, BinlogPos *pos,
                           std::string *gtid, std::string *err) {
  *pos = BinlogPos();
  gtid->clear();
  return con.execute(
      [&](duckdb_connection c, std::string *e) {
        duckdb_result res;
        if (duckdb_query(c,
                         "SELECT binlog_file, binlog_pos, gtid FROM "
                         "lake.__mysql.applied_position WHERE id = 1",
                         &res) == DuckDBError) {
          const char *msg = duckdb_result_error(&res);
          *e = msg != nullptr ? msg : "cannot read the applied position";
          duckdb_destroy_result(&res);
          return true;
        }
        if (duckdb_row_count(&res) > 0) {
          char *f = duckdb_value_varchar(&res, 0, 0);
          char *g = duckdb_value_varchar(&res, 2, 0);
          pos->file = f != nullptr ? f : "";
          pos->pos = duckdb_value_uint64(&res, 1, 0);
          *gtid = g != nullptr ? g : "";
          duckdb_free(f);
          duckdb_free(g);
        }
        duckdb_destroy_result(&res);
        return false;
      },
      err);
}

}  // namespace duckdb_se
