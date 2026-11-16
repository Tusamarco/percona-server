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

#ifndef STORAGE_DUCKDB_DUCKDB_REGISTRY_H_
#define STORAGE_DUCKDB_DUCKDB_REGISTRY_H_

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "duckdb.h"
#include "thr_lock.h"

namespace duckdb_se {

/**
  A binary log coordinate. Files of one server share the base name and a
  zero-padded sequence number, so their names sort in creation order.
*/
struct BinlogPos {
  std::string file;
  unsigned long long pos{0};
  bool empty() const { return file.empty(); }
  bool operator<(const BinlogPos &o) const {
    return file != o.file ? file < o.file : pos < o.pos;
  }
};

/**
  Binary log name without directory. We keep names in this form everywhere,
  as the server passes them to the after_sync hook (THD::set_trans_pos strips
  the directory).
*/
std::string binlog_base_name(const char *path);

/** One table loaded into the lake, as recorded in lake.__mysql.tables. */
struct LoadedTable {
  bool retain_history{false};
  /** The copy is kept but not usable (DDL on a retain_history table). */
  bool stale{false};
  /**
    A refresh after DDL is waiting for a later RENAME: the table did not
    exist under its name when the reader reached the DDL (DWL#4 step 4d).
    Not usable until then.
  */
  bool refresh_pending{false};
  /** Column renames for that refresh (encode_renames()). */
  std::string pending_renames;
  /** Primary key column names (\x1f separated); empty for copies loaded
      before step 3b until the applier records it. */
  std::string pk_columns;
  /** Columns with a weight column "__w_<column>" in the copy (\x1f
      separated; DWL#6 step A); empty for copies loaded before A1. */
  std::string weight_columns;
  /** GTID set executed when the table was loaded (DWL#4 skips it). */
  std::string load_gtid_set;
  /** Binary log position at load time; empty for tables loaded before 4a. */
  BinlogPos load_position;
  /** Table lock for the handler, as in the MOCK engine. */
  THR_LOCK lock;
  /**
    false once the table left the lake (unload, drop, discard, rename to
    another name). The entry itself is never freed: secondary TABLE objects
    the server keeps in its table cache (DWL#6 opens them for every
    offloaded query) point to its lock, and nothing guarantees they are
    evicted first (our own DDL handling and discard do not evict them).
  */
  bool present{true};

  LoadedTable() { thr_lock_init(&lock); }
  ~LoadedTable() { thr_lock_delete(&lock); }
  LoadedTable(const LoadedTable &) = delete;
  LoadedTable &operator=(const LoadedTable &) = delete;
};

/**
  A copy of the fields of a LoadedTable, taken under the registry mutex. Threads
  that do not hold a metadata lock on the table (the binlog reader) use copies:
  an unload may remove the entry at any time.
*/
struct TableState {
  bool retain_history{false};
  bool stale{false};
  bool refresh_pending{false};
  std::string pending_renames;
  std::string pk_columns;
  std::string weight_columns;
  BinlogPos load_position;
  /** Changes are applied to the copy. */
  bool usable() const { return !stale && !refresh_pending; }
};

/**
  In-memory cache of lake.__mysql.tables, the source of truth for "is this table
  loaded". Entries are never freed (a table that leaves the lake is marked not
  present), so a handler never holds a dangling lock; reload() refreshes them
  when the lake opens.
*/
class Registry {
 public:
  static Registry &instance();
  static const char *create_table_sql();
  /** Statements that bring an older registry table up to date. */
  static const char *const *migration_sql();

  /** Reads lake.__mysql.tables. Called while the lake opens. */
  bool reload(duckdb_connection con, std::string *err);

  /**
    The entry, or nullptr if the table is not in the lake. The pointer stays
    valid for the life of the server (entries are never freed); its fields
    are read only under a metadata lock on the table (handler, load,
    unload). Other threads use snapshot().
  */
  LoadedTable *find(const std::string &db, const std::string &table);
  void add(const std::string &db, const std::string &table,
           bool retain_history, const std::string &load_gtid_set,
           const BinlogPos &load_position, const std::string &pk_columns,
           const std::string &weight_columns);
  /** Records the primary key of a copy loaded before step 3b. */
  void set_pk_columns(const std::string &db, const std::string &table,
                      const std::string &pk_columns);
  /** Copies the entry's fields; false if the table is not in the registry. */
  bool snapshot(const std::string &db, const std::string &table,
                TableState *state);
  /** One table in the lake, for INFORMATION_SCHEMA.DUCKDB_TABLES. */
  struct Listed {
    std::string db;
    std::string table;
    TableState state;
    std::string load_gtid_set;
  };
  /** Every table in the lake, sorted by schema and name. */
  std::vector<Listed> list();
  /** Earliest load position among usable tables; false if there is none. */
  bool earliest_load_position(BinlogPos *pos);
  /**
    Usable tables loaded before pos ("db.table"): they miss the changes of a
    binlog range that ends at pos and that we do not read.
  */
  std::vector<std::string> loaded_before(const BinlogPos &pos);
  void mark_stale(const std::string &db, const std::string &table);
  void mark_refresh_pending(const std::string &db, const std::string &table,
                            const std::string &pending_renames);
  /**
    Moves the entry to a new name (RENAME TABLE, DWL#4 step 4d). The server
    renamed the table under an exclusive metadata lock, so no handler holds
    the old entry. true if the old entry is missing or the new name exists.
  */
  bool rename(const std::string &db, const std::string &table,
              const std::string &new_db, const std::string &new_table);
  void remove(const std::string &db, const std::string &table);

 private:
  Registry() = default;
  using Key = std::pair<std::string, std::string>;
  std::mutex m_mutex;
  /** The entry if present, under m_mutex. */
  LoadedTable *live(const Key &key);
  std::map<Key, LoadedTable> m_tables;
};

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_REGISTRY_H_
