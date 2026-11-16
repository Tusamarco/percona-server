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

#include "storage/duckdb/duckdb_registry.h"

#include <iterator>
#include <set>

#include "my_sys.h"  // dirname_length

namespace duckdb_se {

std::string binlog_base_name(const char *path) {
  return std::string(path + dirname_length(path));
}

Registry &Registry::instance() {
  static Registry registry;
  return registry;
}

const char *Registry::create_table_sql() {
  return "CREATE TABLE IF NOT EXISTS lake.__mysql.tables ("
         "db VARCHAR NOT NULL, tbl VARCHAR NOT NULL, "
         "retain_history BOOLEAN NOT NULL, state VARCHAR NOT NULL, "
         "load_gtid_set VARCHAR NOT NULL, loaded_at TIMESTAMP NOT NULL, "
         "load_binlog_file VARCHAR, load_binlog_pos UBIGINT)";
}

const char *const *Registry::migration_sql() {
  // Registry tables created before step 4a have no binary log position.
  static const char *const statements[] = {
      "ALTER TABLE lake.__mysql.tables ADD COLUMN IF NOT EXISTS "
      "load_binlog_file VARCHAR",
      "ALTER TABLE lake.__mysql.tables ADD COLUMN IF NOT EXISTS "
      "load_binlog_pos UBIGINT",
      // Step 4d: column renames of a refresh waiting for a RENAME.
      "ALTER TABLE lake.__mysql.tables ADD COLUMN IF NOT EXISTS "
      "pending_renames VARCHAR",
      // DWL#3 step 3b: primary key, for the structure check of a takeover.
      "ALTER TABLE lake.__mysql.tables ADD COLUMN IF NOT EXISTS "
      "pk_columns VARCHAR",
      // DWL#6 step A: the columns with a weight column in the copy.
      "ALTER TABLE lake.__mysql.tables ADD COLUMN IF NOT EXISTS "
      "weight_columns VARCHAR",
      // DWL#5 had a configurable purge tag for one day (2026-11-12); the
      // tag is fixed now and its history table is not used.
      "DROP TABLE IF EXISTS lake.__mysql.purge_tags",
      nullptr};
  return statements;
}

bool Registry::reload(duckdb_connection con, std::string *err) {
  duckdb_result res;
  if (duckdb_query(con,
                   "SELECT db, tbl, retain_history, state, load_gtid_set, "
                   "load_binlog_file, load_binlog_pos, pending_renames, "
                   "pk_columns, weight_columns FROM lake.__mysql.tables",
                   &res) == DuckDBError) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "cannot read lake.__mysql.tables";
    duckdb_destroy_result(&res);
    return true;
  }
  std::lock_guard<std::mutex> guard(m_mutex);
  std::set<Key> seen;
  const idx_t rows = duckdb_row_count(&res);
  for (idx_t r = 0; r < rows; r++) {
    char *db = duckdb_value_varchar(&res, 0, r);
    char *tbl = duckdb_value_varchar(&res, 1, r);
    char *state = duckdb_value_varchar(&res, 3, r);
    char *gtids = duckdb_value_varchar(&res, 4, r);
    const Key key(db != nullptr ? db : "", tbl != nullptr ? tbl : "");
    LoadedTable &t = m_tables[key];
    t.present = true;
    t.retain_history = duckdb_value_boolean(&res, 2, r);
    t.stale = state != nullptr && std::string(state) == "stale";
    t.refresh_pending =
        state != nullptr && std::string(state) == "refresh_pending";
    char *renames = duckdb_value_varchar(&res, 7, r);
    t.pending_renames = renames != nullptr ? renames : "";
    duckdb_free(renames);
    char *pk = duckdb_value_varchar(&res, 8, r);
    t.pk_columns = pk != nullptr ? pk : "";
    duckdb_free(pk);
    char *wc = duckdb_value_varchar(&res, 9, r);
    t.weight_columns = wc != nullptr ? wc : "";
    duckdb_free(wc);
    t.load_gtid_set = gtids != nullptr ? gtids : "";
    if (duckdb_value_is_null(&res, 5, r)) {
      t.load_position = BinlogPos();
    } else {
      char *file = duckdb_value_varchar(&res, 5, r);
      t.load_position =
          // Positions saved with a full path by an earlier build still work.
          BinlogPos{binlog_base_name(file != nullptr ? file : ""),
                    duckdb_value_uint64(&res, 6, r)};
      duckdb_free(file);
    }
    seen.insert(key);
    duckdb_free(db);
    duckdb_free(tbl);
    duckdb_free(state);
    duckdb_free(gtids);
  }
  duckdb_destroy_result(&res);
  // Entries no longer in the lake are marked not present (never freed: a
  // cached secondary TABLE may still point to their lock).
  for (auto &entry : m_tables)
    if (seen.count(entry.first) == 0) entry.second.present = false;
  return false;
}

LoadedTable *Registry::live(const Key &key) {
  auto it = m_tables.find(key);
  return it == m_tables.end() || !it->second.present ? nullptr : &it->second;
}

LoadedTable *Registry::find(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  return live(Key(db, table));
}

void Registry::add(const std::string &db, const std::string &table,
                   bool retain_history, const std::string &load_gtid_set,
                   const BinlogPos &load_position,
                   const std::string &pk_columns,
                   const std::string &weight_columns) {
  std::lock_guard<std::mutex> guard(m_mutex);
  LoadedTable &t = m_tables[Key(db, table)];
  t.present = true;
  t.retain_history = retain_history;
  t.stale = false;
  t.refresh_pending = false;
  t.pending_renames.clear();
  t.pk_columns = pk_columns;
  t.weight_columns = weight_columns;
  t.load_gtid_set = load_gtid_set;
  t.load_position = load_position;
}

bool Registry::snapshot(const std::string &db, const std::string &table,
                        TableState *state) {
  std::lock_guard<std::mutex> guard(m_mutex);
  const LoadedTable *t = live(Key(db, table));
  if (t == nullptr) return false;
  state->retain_history = t->retain_history;
  state->stale = t->stale;
  state->refresh_pending = t->refresh_pending;
  state->pending_renames = t->pending_renames;
  state->pk_columns = t->pk_columns;
  state->weight_columns = t->weight_columns;
  state->load_position = t->load_position;
  return true;
}

bool Registry::earliest_load_position(BinlogPos *pos) {
  std::lock_guard<std::mutex> guard(m_mutex);
  bool found = false;
  for (const auto &entry : m_tables) {
    const LoadedTable &t = entry.second;
    if (!t.present || t.stale || t.refresh_pending || t.load_position.empty())
      continue;
    if (!found || t.load_position < *pos) *pos = t.load_position;
    found = true;
  }
  return found;
}

std::vector<std::string> Registry::loaded_before(const BinlogPos &pos) {
  std::lock_guard<std::mutex> guard(m_mutex);
  std::vector<std::string> names;
  for (const auto &entry : m_tables) {
    const LoadedTable &t = entry.second;
    // A table loaded before 4a has no load position: it may be behind too.
    if (!t.present || t.stale || t.refresh_pending ||
        !(t.load_position < pos))
      continue;
    names.push_back(entry.first.first + "." + entry.first.second);
  }
  return names;
}

void Registry::mark_stale(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  if (LoadedTable *t = live(Key(db, table)); t != nullptr) t->stale = true;
}

bool Registry::rename(const std::string &db, const std::string &table,
                      const std::string &new_db, const std::string &new_table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  LoadedTable *from = live(Key(db, table));
  if (from == nullptr || live(Key(new_db, new_table)) != nullptr) return true;
  LoadedTable &to = m_tables[Key(new_db, new_table)];
  to.present = true;
  to.retain_history = from->retain_history;
  to.stale = from->stale;
  to.refresh_pending = from->refresh_pending;
  to.pending_renames = from->pending_renames;
  to.pk_columns = from->pk_columns;
  to.weight_columns = from->weight_columns;
  to.load_gtid_set = from->load_gtid_set;
  to.load_position = from->load_position;
  from->present = false;
  return false;
}

void Registry::mark_refresh_pending(const std::string &db,
                                    const std::string &table,
                                    const std::string &pending_renames) {
  std::lock_guard<std::mutex> guard(m_mutex);
  LoadedTable *t = live(Key(db, table));
  if (t == nullptr) return;
  t->refresh_pending = true;
  t->pending_renames = pending_renames;
}

void Registry::set_pk_columns(const std::string &db, const std::string &table,
                              const std::string &pk_columns) {
  std::lock_guard<std::mutex> guard(m_mutex);
  if (LoadedTable *t = live(Key(db, table)); t != nullptr)
    t->pk_columns = pk_columns;
}

void Registry::remove(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(m_mutex);
  if (LoadedTable *t = live(Key(db, table)); t != nullptr) t->present = false;
}

}  // namespace duckdb_se
