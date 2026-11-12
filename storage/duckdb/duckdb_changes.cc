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

#include "storage/duckdb/duckdb_changes.h"

#include <utility>

namespace duckdb_se {

namespace {

std::string row_to_string(const Row &row) {
  std::string out = "(";
  for (size_t i = 0; i < row.size(); i++) {
    if (i > 0) out += ", ";
    out += row[i].to_string();
  }
  return out + ")";
}

}  // namespace

std::string KeyChange::to_string() const {
  std::string out = "original=";
  switch (original) {
    case Original::KEEP:
      out += "KEEP";
      break;
    case Original::DELETE:
      out += "DELETE";
      break;
    case Original::ARCHIVE:
      out += "ARCHIVE";
      break;
  }
  for (const ArchivedRow &a : archived_rows)
    out += " archived=" + row_to_string(a.row);
  if (has_live_row) out += " live=" + row_to_string(live_row);
  return out;
}

std::string TableChanges::key_of(const Row &row) const {
  std::string key = "(";
  for (size_t i = 0; i < m_pk.size(); i++) {
    if (i > 0) key += ", ";
    key += m_pk[i] < row.size() ? row[m_pk[i]].to_string() : "?";
  }
  return key + ")";
}

bool TableChanges::insert(const Row &row, std::string *err) {
  m_input_rows++;
  return insert_row(row, err);
}

bool TableChanges::update(const Row &before, const Row &after,
                          std::string *err) {
  m_input_rows++;
  remove_row(before, /*purge=*/false, 0);
  return insert_row(after, err);
}

void TableChanges::remove(const Row &before, bool purge, uint64_t time) {
  m_input_rows++;
  remove_row(before, purge, time);
}

bool TableChanges::insert_row(const Row &row, std::string *err) {
  const std::string key = key_of(row);
  KeyChange &c = m_changes[key];
  if (c.has_live_row) {
    *err = "duplicate insert of key " + key + " in one change set";
    return true;
  }
  if (c.key_row.empty()) c.key_row = row;
  c.has_live_row = true;
  c.live_row = row;
  return false;
}

void TableChanges::remove_row(const Row &before, bool purge, uint64_t time) {
  KeyChange &c = m_changes[key_of(before)];
  if (c.key_row.empty()) c.key_row = before;
  // A purge keeps the row in DuckDB only on retain_history tables (DWL#5).
  const bool archive = purge && m_history;
  if (c.has_live_row) {
    // The live row was inserted in this change set.
    if (archive) c.archived_rows.push_back({std::move(c.live_row), time});
    c.has_live_row = false;
    c.live_row.clear();
    return;
  }
  // Removing a key that has no live row any more (already deleted or archived
  // in this change set) changes nothing.
  if (c.original == KeyChange::Original::KEEP) {
    c.original =
        archive ? KeyChange::Original::ARCHIVE : KeyChange::Original::DELETE;
    if (archive) c.original_archived_at = time;
  }
}

bool TableChanges::merge(const TableChanges &later, std::string *err) {
  m_input_rows += later.m_input_rows;
  for (const auto &[key, c] : later.m_changes) {
    // What `later` does to the row that existed before it is a removal on top
    // of our state: it removes our live row, or the original row.
    if (c.original != KeyChange::Original::KEEP)
      remove_row(c.key_row, c.original == KeyChange::Original::ARCHIVE,
                 c.original_archived_at);
    KeyChange &b = m_changes[key];
    if (b.key_row.empty()) b.key_row = c.key_row;
    for (const ArchivedRow &a : c.archived_rows) b.archived_rows.push_back(a);
    if (c.has_live_row) {
      if (b.has_live_row) {
        *err = "duplicate insert of key " + key + " in one batch";
        return true;
      }
      b.has_live_row = true;
      b.live_row = c.live_row;
    }
  }
  return false;
}

unsigned long long TableChanges::net_changes() const {
  unsigned long long n = 0;
  for (const auto &entry : m_changes)
    if (entry.second.has_effect()) n++;
  return n;
}

}  // namespace duckdb_se
