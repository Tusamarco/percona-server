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

#include "storage/duckdb/duckdb_i_s.h"

#include <string>
#include <vector>

#include "mysql/plugin.h"
#include "mysqld_error.h"
#include "sql/auth/auth_acls.h"  // PROCESS_ACL
#include "sql/auth/sql_security_ctx.h"
#include "sql/field.h"
#include "sql/sql_class.h"
#include "sql/sql_show.h"  // schema_table_store_record
#include "sql/table.h"     // ST_FIELD_INFO, ST_SCHEMA_TABLE
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_registry.h"

namespace duckdb_se {

namespace {

// Column positions in fields[].
enum Column {
  TABLE_SCHEMA,
  TABLE_NAME,
  STATE,
  RETAIN_HISTORY,
  PRIMARY_KEY,
  WEIGHT_COLUMNS,
  LOAD_BINLOG_FILE,
  LOAD_BINLOG_POSITION,
  LOAD_GTID_SET
};

ST_FIELD_INFO fields[] = {
    {"TABLE_SCHEMA", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, nullptr, 0},
    {"TABLE_NAME", NAME_CHAR_LEN, MYSQL_TYPE_STRING, 0, 0, nullptr, 0},
    {"STATE", 16, MYSQL_TYPE_STRING, 0, 0, nullptr, 0},
    {"RETAIN_HISTORY", 3, MYSQL_TYPE_STRING, 0, 0, nullptr, 0},
    {"PRIMARY_KEY", 2048, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, nullptr, 0},
    {"WEIGHT_COLUMNS", 2048, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, nullptr,
     0},
    {"LOAD_BINLOG_FILE", FN_REFLEN, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL,
     nullptr, 0},
    {"LOAD_BINLOG_POSITION", 21, MYSQL_TYPE_LONGLONG, 0,
     MY_I_S_MAYBE_NULL | MY_I_S_UNSIGNED, nullptr, 0},
    {"LOAD_GTID_SET", 65535, MYSQL_TYPE_STRING, 0, MY_I_S_MAYBE_NULL, nullptr,
     0},
    {nullptr, 0, MYSQL_TYPE_NULL, 0, 0, nullptr, 0}};

void store(TABLE *table, Column c, const std::string &s) {
  Field *f = table->field[c];
  f->set_notnull();
  f->store(s.data(), s.size(), system_charset_info);
}

// NULL for an empty value (copies loaded before the column was recorded).
void store_or_null(TABLE *table, Column c, const std::string &s) {
  if (s.empty())
    table->field[c]->set_null();
  else
    store(table, c, s);
}

// Registry lists are \x1f separated: shown as "a, b".
std::string list_text(const std::string &s) {
  std::string out;
  for (char ch : s) {
    if (ch == '\x1f')
      out += ", ";
    else
      out += ch;
  }
  return out;
}

int fill_rows(THD *thd, TABLE *table) {
  for (const Registry::Listed &l : Registry::instance().list()) {
    store(table, TABLE_SCHEMA, l.db);
    store(table, TABLE_NAME, l.table);
    store(table, STATE,
          l.state.stale             ? "STALE"
          : l.state.refresh_pending ? "REFRESH_PENDING"
                                    : "LOADED");
    store(table, RETAIN_HISTORY, l.state.retain_history ? "YES" : "NO");
    store_or_null(table, PRIMARY_KEY, list_text(l.state.pk_columns));
    store_or_null(table, WEIGHT_COLUMNS, list_text(l.state.weight_columns));
    if (l.state.load_position.empty()) {
      table->field[LOAD_BINLOG_FILE]->set_null();
      table->field[LOAD_BINLOG_POSITION]->set_null();
    } else {
      store(table, LOAD_BINLOG_FILE, l.state.load_position.file);
      table->field[LOAD_BINLOG_POSITION]->set_notnull();
      table->field[LOAD_BINLOG_POSITION]->store(
          static_cast<longlong>(l.state.load_position.pos), true);
    }
    store_or_null(table, LOAD_GTID_SET, l.load_gtid_set);
    if (schema_table_store_record(thd, table)) return 1;
  }
  return 0;
}

int fill(THD *thd, Table_ref *tables, Item *) {
  // As InnoDB's INFORMATION_SCHEMA tables: names of every table in the lake.
  if (!thd->security_context()->check_access(PROCESS_ACL)) return 0;
  if (!Lake::instance().is_open()) return 0;
  try {
    return fill_rows(thd, tables->table);
  } catch (...) {
    my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0),
             "INFORMATION_SCHEMA.DUCKDB_TABLES: internal error");
    return 1;
  }
}

}  // namespace

st_mysql_information_schema i_s_tables_descriptor = {
    MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};

int i_s_tables_init(void *p) {
  auto *schema = static_cast<ST_SCHEMA_TABLE *>(p);
  schema->fields_info = fields;
  schema->fill_table = fill;
  return 0;
}

}  // namespace duckdb_se
