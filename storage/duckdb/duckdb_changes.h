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

#ifndef STORAGE_DUCKDB_DUCKDB_CHANGES_H_
#define STORAGE_DUCKDB_DUCKDB_CHANGES_H_

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "storage/duckdb/duckdb_row_decoder.h"

namespace duckdb_se {

using Row = std::vector<Value>;

/** A row inserted and then purge-deleted in a change set (DWL#5). */
struct ArchivedRow {
  Row row;
  /** Commit time of the purge, in microseconds since the epoch. */
  uint64_t archived_at{0};
};

/**
  Net effect of a transaction (4b) or a batch (4c) on one primary key of one
  table, relative to the state before the change set.
*/
struct KeyChange {
  enum class Original { KEEP, DELETE, ARCHIVE };
  /** What happens to the row that existed before the change set. Deleting a
      key that does not exist in DuckDB is a no-op, so DELETE is always safe. */
  Original original{Original::KEEP};
  /** Archive time of the original row, when original == ARCHIVE. */
  uint64_t original_archived_at{0};
  /** A row image holding this key: the typed key values for DuckDB. */
  Row key_row;
  /** Rows inserted and then purge-deleted in the change set: they are added
      with __archived_at set (history tables only). */
  std::vector<ArchivedRow> archived_rows;
  bool has_live_row{false};
  /** Final live row to insert. */
  Row live_row;

  bool has_effect() const {
    return original != Original::KEEP || has_live_row ||
           !archived_rows.empty();
  }
  std::string to_string() const;
};

/** Changes to one table, collapsed per primary key (DWL#4, DWL#5). */
class TableChanges {
 public:
  TableChanges(std::vector<unsigned int> primary_key, bool retain_history)
      : m_pk(std::move(primary_key)), m_history(retain_history) {}

  bool insert(const Row &row, std::string *err);
  /** An update is a real delete of the old key and an insert of the new row. */
  bool update(const Row &before, const Row &after, std::string *err);
  /**
    purge=true: delete in a duckdb_avoid_purge transaction committed at `time`
    (microseconds since the epoch), the archive time.
  */
  void remove(const Row &before, bool purge, uint64_t time);
  /**
    Adds the changes of a later change set of the same table on top of these:
    the result is what applying both in order does. true on error.
  */
  bool merge(const TableChanges &later, std::string *err);

  const std::map<std::string, KeyChange> &changes() const { return m_changes; }
  const std::vector<unsigned int> &primary_key() const { return m_pk; }
  bool retain_history() const { return m_history; }
  /** Row operations received (an update counts once). */
  unsigned long long input_rows() const { return m_input_rows; }
  /** Keys with an effect after collapse. */
  unsigned long long net_changes() const;

  /** The primary-key part of a row, as text. */
  std::string key_of(const Row &row) const;

 private:
  bool insert_row(const Row &row, std::string *err);
  void remove_row(const Row &before, bool purge, uint64_t time);

  std::vector<unsigned int> m_pk;
  bool m_history;
  std::map<std::string, KeyChange> m_changes;
  unsigned long long m_input_rows{0};
};

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_CHANGES_H_
