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

#include "storage/duckdb/duckdb_load.h"

#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "my_rapidjson_size_t.h"  // must precede the rapidjson headers

#include <rapidjson/document.h>

#include "my_bitmap.h"
#include "my_sys.h"
#include "scope_guard.h"
#include "mysqld_error.h"
#include "sql/derror.h"
#include "sql/field.h"
#include "sql/handler.h"
#include "sql/binlog.h"  // mysql_bin_log, Log_info
#include "sql/mysqld.h"  // opt_bin_log, binlog_row_metadata, global_system_variables
#include "sql/rpl_log_encryption.h"  // rpl_encryption
#include "sql/rpl_gtid.h"
#include "sql/dd/cache/dictionary_client.h"
#include "sql/dd/types/table.h"
#include "sql/key.h"
#include "sql/mdl.h"
#include "sql/sql_alter.h"  // Alter_info
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "sql/sql_lex.h"
#include "sql/system_variables.h"
#include "sql/table.h"
#include "storage/duckdb/duckdb_binlog_reader.h"  // expiry_warning
#include "storage/duckdb/duckdb_lake.h"
#include "storage/duckdb/duckdb_log.h"
#include "storage/duckdb/duckdb_registry.h"
#include "storage/duckdb/duckdb_types.h"

namespace duckdb_se {

namespace {

bool report(const std::string &msg) {
  my_error(ER_SECONDARY_ENGINE_PLUGIN, MYF(0), msg.c_str());
  return true;
}

void warn(THD *thd, const std::string &msg) {
  push_warning_printf(thd, Sql_condition::SL_WARNING,
                      ER_SECONDARY_ENGINE_PLUGIN,
                      ER_THD(thd, ER_SECONDARY_ENGINE_PLUGIN), msg.c_str());
}

// "db.table" or "table" (default database).
bool parse_table_name(const std::string &spec, const std::string &default_db,
                      std::string *db, std::string *table) {
  const size_t dot = spec.find('.');
  if (dot == std::string::npos) {
    *db = default_db;
    *table = spec;
  } else {
    *db = spec.substr(0, dot);
    *table = spec.substr(dot + 1);
  }
  return db->empty() || table->empty() ||
         table->find('.') != std::string::npos;
}

// Change propagation (DWL#4) reads the binlog, so the load requires the
// settings it depends on.
bool check_binlog_settings(THD *thd, bool retain_history, std::string *err) {
  if (!opt_bin_log)
    *err = "SECONDARY_LOAD requires log_bin=ON";
  else if (global_system_variables.binlog_format != BINLOG_FORMAT_ROW)
    *err = "SECONDARY_LOAD requires binlog_format=ROW";
  else if (global_system_variables.binlog_row_image != BINLOG_ROW_IMAGE_FULL)
    *err = "SECONDARY_LOAD requires binlog_row_image=FULL";
  else if (binlog_row_metadata != BINLOG_ROW_METADATA_FULL)
    *err = "SECONDARY_LOAD requires binlog_row_metadata=FULL";
  else if (global_gtid_mode.get() != Gtid_mode::ON)
    *err = "SECONDARY_LOAD requires gtid_mode=ON";
  else if (rpl_encryption.is_enabled())
    *err = "SECONDARY_LOAD requires binlog_encryption=OFF in this version";
  // The applier reads a transaction only after the binary log sync. With
  // sync_binlog=1 a synced transaction is always committed by InnoDB crash
  // recovery, so DuckDB never gets ahead of InnoDB. History tables hold data
  // that exists nowhere else, so for them it is required.
  else if (sync_binlog_period != 1 && retain_history)
    *err = "SECONDARY_LOAD of a table with retain_history requires "
           "sync_binlog=1";
  else {
    const std::string expiry = BinlogReader::expiry_warning();
    if (!expiry.empty())
      push_warning_printf(thd, Sql_condition::SL_WARNING,
                          ER_SECONDARY_ENGINE_PLUGIN,
                          ER_THD(thd, ER_SECONDARY_ENGINE_PLUGIN),
                          expiry.c_str());
    if (sync_binlog_period != 1)
      push_warning_printf(thd, Sql_condition::SL_WARNING,
                          ER_SECONDARY_ENGINE_PLUGIN,
                          ER_THD(thd, ER_SECONDARY_ENGINE_PLUGIN),
                          "sync_binlog is not 1: after a crash DuckDB may "
                          "contain changes that InnoDB rolled back");
    return false;
  }
  return true;
}

// SECONDARY_ENGINE_ATTRIBUTE: a JSON object with retain_history (boolean) and
// adopt_history_from ("db.table" or "table", DWL#3 step 3b).
bool parse_attributes(const TABLE_SHARE &share, bool *retain_history,
                      std::string *adopt_from, std::string *err) {
  *retain_history = false;
  adopt_from->clear();
  const LEX_CSTRING &attr = share.secondary_engine_attribute;
  if (attr.length == 0) return false;
  rapidjson::Document doc;
  doc.Parse(attr.str, attr.length);
  if (doc.HasParseError() || !doc.IsObject()) {
    *err = "SECONDARY_ENGINE_ATTRIBUTE must be a JSON object";
    return true;
  }
  for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
    const std::string key = it->name.GetString();
    if (key == "retain_history" && it->value.IsBool()) {
      *retain_history = it->value.GetBool();
    } else if (key == "adopt_history_from" && it->value.IsString() &&
               it->value.GetStringLength() > 0) {
      *adopt_from = it->value.GetString();
    } else {
      *err = "unknown or invalid SECONDARY_ENGINE_ATTRIBUTE key '" + key + "'";
      return true;
    }
  }
  return false;
}

std::string executed_gtid_set() {
  char *buf = nullptr;
  // to_string() needs the write lock of global_tsid_lock; with need_lock=true
  // it takes and releases it itself, as the server does for @@gtid_executed.
  gtid_state->get_executed_gtids()->to_string(&buf, true);
  std::string set = buf != nullptr ? buf : "";
  my_free(buf);
  return set;
}

std::string lake_table(const std::string &db, const std::string &table) {
  return "lake." + quote_ident(db) + "." + quote_ident(table);
}

std::string registry_where(const std::string &db, const std::string &table) {
  return " WHERE db = " + quote_literal(db) +
         " AND tbl = " + quote_literal(table);
}

// Drops a copy and its registry row in one DuckLake transaction; the caller
// holds apply_mutex.
bool drop_copy(Lake::Connection &con, const std::string &db,
               const std::string &table, std::string *err) {
  std::string ignored;
  if (con.run("BEGIN TRANSACTION", err) ||
      con.run("DROP TABLE IF EXISTS " + lake_table(db, table), err) ||
      con.run("DELETE FROM lake.__mysql.tables" + registry_where(db, table),
              err) ||
      con.run("COMMIT", err)) {
    con.execute_cleanup(
        [](duckdb_connection c, std::string *e) {
          return run_sql(c, "ROLLBACK", e);
        },
        &ignored);
    return true;
  }
  Registry::instance().remove(db, table);
  return false;
}

// Whether the MySQL table exists, read from the data dictionary under a
// short metadata lock (the dictionary requires one).
bool mysql_table_exists(THD *thd, const std::string &db,
                        const std::string &table, bool *exists,
                        std::string *err) {
  MDL_request request;
  MDL_REQUEST_INIT(&request, MDL_key::TABLE, db.c_str(), table.c_str(),
                   MDL_SHARED_HIGH_PRIO, MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&request,
                                    thd->variables.lock_wait_timeout)) {
    *err = "cannot lock " + db + "." + table;
    return true;
  }
  auto release = create_scope_guard(
      [&] { thd->mdl_context.release_lock(request.ticket); });
  const dd::cache::Dictionary_client::Auto_releaser releaser(
      thd->dd_client());
  const dd::Table *found = nullptr;
  if (thd->dd_client()->acquire(dd::String_type(db.c_str()),
                                dd::String_type(table.c_str()), &found)) {
    *err = "cannot read the data dictionary for " + db + "." + table;
    return true;
  }
  *exists = found != nullptr;
  return false;
}

// Suffix of the table a load builds before its swap (T1); a leftover one
// (crash between T1 and T2) is dropped when the lake opens.
constexpr const char *kBuildSuffix = "__duckdb_build";

// Refresh requests from the binlog reader (DWL#4 step 4d).
std::mutex g_refresh_mutex;
std::map<std::pair<std::string, std::string>, ColumnRenames> g_refresh;

bool take_refresh(const std::string &db, const std::string &table,
                  ColumnRenames *renames) {
  std::lock_guard<std::mutex> guard(g_refresh_mutex);
  auto it = g_refresh.find({db, table});
  if (it == g_refresh.end()) return false;
  *renames = std::move(it->second);
  g_refresh.erase(it);
  return true;
}

// Column names of a lake table.
bool lake_columns(duckdb_connection c, const std::string &db,
                  const std::string &table, std::set<std::string> *names,
                  std::string *err) {
  duckdb_result res;
  const std::string sql =
      "SELECT column_name FROM duckdb_columns() WHERE database_name = 'lake' "
      "AND schema_name = " +
      quote_literal(db) + " AND table_name = " + quote_literal(table);
  if (duckdb_query(c, sql.c_str(), &res) == DuckDBError) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "cannot read the DuckDB table columns";
    duckdb_destroy_result(&res);
    return true;
  }
  for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
    char *v = duckdb_value_varchar(&res, 0, r);
    if (v != nullptr) names->insert(v);
    duckdb_free(v);
  }
  duckdb_destroy_result(&res);
  return false;
}

// Archived rows of the old copy into the new one: columns by name (following
// renames), converted with CAST to the new types; columns new to the table
// are NULL in archived rows (they did not exist then). A value that cannot be
// converted is an error: the refresh fails and nothing changes. The old copy
// may be in another schema (adopt_history_from).
bool copy_archived_rows(duckdb_connection c, const std::string &old_db,
                        const std::string &old_table, const std::string &db,
                        const std::string &new_table,
                        const std::vector<std::pair<std::string, std::string>>
                            &new_columns,
                        const ColumnRenames &renames, std::string *err) {
  std::set<std::string> old_columns;
  if (lake_columns(c, old_db, old_table, &old_columns, err)) return true;
  std::string select;
  for (const auto &[column, type] : new_columns) {
    std::string source = column;
    for (const auto &[from, to] : renames)
      if (to == column) source = from;
    if (!select.empty()) select += ", ";
    select += old_columns.count(source) > 0
                  ? "CAST(t." + quote_ident(source) + " AS " + type + ")"
                  : "NULL";
  }
  select += ", t.\"__archived_at\"";
  return run_sql(c,
                 "INSERT INTO " + lake_table(db, new_table) + " SELECT " +
                     select + " FROM " + lake_table(old_db, old_table) +
                     " AS t WHERE t.\"__archived_at\" IS NOT NULL",
                 err);
}

// Primary key column names, stored in lake.__mysql.tables.pk_columns.
constexpr char kNameSep = '\x1f';

std::string join_names(const std::vector<std::string> &names) {
  std::string out;
  for (const std::string &n : names) {
    if (!out.empty()) out += kNameSep;
    out += n;
  }
  return out;
}

std::string show_names(const std::vector<std::string> &names) {
  std::string out;
  for (const std::string &n : names) out += (out.empty() ? "" : ", ") + n;
  return "(" + out + ")";
}

std::vector<std::string> split_names(const std::string &joined) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= joined.size() && !joined.empty()) {
    size_t end = joined.find(kNameSep, start);
    if (end == std::string::npos) end = joined.size();
    out.push_back(joined.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

std::vector<std::string> primary_key_names(const TABLE &table) {
  std::vector<std::string> names;
  const KEY &pk = table.key_info[table.s->primary_key];
  for (unsigned i = 0; i < pk.user_defined_key_parts; i++)
    names.emplace_back(pk.key_part[i].field->field_name);
  return names;
}

// One value of a query, NULL as "".
bool query_text(duckdb_connection c, const std::string &sql, std::string *out,
                bool *is_null, std::string *err) {
  duckdb_result res;
  if (duckdb_query(c, sql.c_str(), &res) == DuckDBError) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "DuckDB query failed";
    duckdb_destroy_result(&res);
    return true;
  }
  *is_null = duckdb_row_count(&res) == 0 || duckdb_value_is_null(&res, 0, 0);
  out->clear();
  if (!*is_null) {
    char *v = duckdb_value_varchar(&res, 0, 0);
    *out = v != nullptr ? v : "";
    duckdb_free(v);
  }
  duckdb_destroy_result(&res);
  return false;
}

// DuckDB's own name for a type (DECIMAL(12,2), VARCHAR, ...), so that the
// load's type strings and the stored ones compare equal when they are equal.
bool canonical_type(duckdb_connection c, const std::string &type,
                    std::string *out, std::string *err) {
  bool is_null;
  return query_text(c, "SELECT typeof(CAST(NULL AS " + type + "))", out,
                    &is_null, err);
}

// DWL#3 step 3b: a table takes over a history copy (adopt, reattach, resync)
// only if its structure is the same: the same columns by name, the same
// DuckDB type for each, the same primary key columns. Column order does not
// matter (columns are matched by name everywhere). Every difference is
// reported. true on a DuckDB error.
bool compare_history_structure(
    duckdb_connection c, const std::string &copy_db,
    const std::string &copy_table,
    const std::vector<std::pair<std::string, std::string>> &new_columns,
    const std::vector<std::string> &new_pk, std::vector<std::string> *diffs,
    std::string *err) {
  // Columns of the copy, with their types.
  duckdb_result res;
  const std::string sql =
      "SELECT column_name, data_type FROM duckdb_columns() WHERE "
      "database_name = 'lake' AND schema_name = " +
      quote_literal(copy_db) + " AND table_name = " +
      quote_literal(copy_table) +
      " AND column_name <> '__archived_at' ORDER BY column_index";
  if (duckdb_query(c, sql.c_str(), &res) == DuckDBError) {
    const char *msg = duckdb_result_error(&res);
    *err = msg != nullptr ? msg : "cannot read the history copy columns";
    duckdb_destroy_result(&res);
    return true;
  }
  std::map<std::string, std::string> copy_columns;
  std::vector<std::string> copy_order;
  for (idx_t r = 0; r < duckdb_row_count(&res); r++) {
    char *n = duckdb_value_varchar(&res, 0, r);
    char *t = duckdb_value_varchar(&res, 1, r);
    copy_columns[n != nullptr ? n : ""] = t != nullptr ? t : "";
    copy_order.emplace_back(n != nullptr ? n : "");
    duckdb_free(n);
    duckdb_free(t);
  }
  duckdb_destroy_result(&res);

  std::set<std::string> new_names;
  for (const auto &[name, type] : new_columns) {
    new_names.insert(name);
    auto it = copy_columns.find(name);
    if (it == copy_columns.end()) {
      diffs->push_back("column " + name +
                       ": in the new table, not in the history copy");
      continue;
    }
    std::string a, b;
    if (canonical_type(c, it->second, &a, err) ||
        canonical_type(c, type, &b, err))
      return true;
    if (a != b)
      diffs->push_back("column " + name + ": type " + a +
                       " in the history copy, " + b + " in the new table");
  }
  for (const std::string &name : copy_order) {
    if (new_names.count(name) > 0) continue;
    std::string count;
    bool is_null;
    if (query_text(c,
                   "SELECT count(" + quote_ident(name) + ") FROM " +
                       lake_table(copy_db, copy_table) +
                       " WHERE \"__archived_at\" IS NOT NULL",
                   &count, &is_null, err))
      return true;
    diffs->push_back("column " + name +
                     ": in the history copy, missing in the new table (" +
                     count + " archived values)");
  }

  // Primary key, as recorded at load (copies loaded before step 3b have none
  // until the applier records it).
  std::string stored;
  bool unknown;
  if (query_text(c,
                 "SELECT pk_columns FROM lake.__mysql.tables" +
                     registry_where(copy_db, copy_table),
                 &stored, &unknown, err))
    return true;
  if (unknown || stored.empty())
    diffs->push_back(
        "primary key: unknown for the history copy (loaded before step 3b "
        "and not changed since), cannot be compared with " +
        show_names(new_pk));
  else if (split_names(stored) != new_pk)
    diffs->push_back("primary key: " + show_names(split_names(stored)) +
                     " in the history copy, " + show_names(new_pk) +
                     " in the new table");
  return false;
}

}  // namespace

void request_refresh(const std::string &db, const std::string &table,
                     const ColumnRenames &renames) {
  std::lock_guard<std::mutex> guard(g_refresh_mutex);
  g_refresh[{db, table}] = renames;
}

void cancel_refresh(const std::string &db, const std::string &table) {
  std::lock_guard<std::mutex> guard(g_refresh_mutex);
  g_refresh.erase({db, table});
}

bool load_table_into_lake(THD *thd, const TABLE &table) {
  const auto start = std::chrono::steady_clock::now();
  const std::string db = table.s->db.str;
  const std::string name = table.s->table_name.str;
  std::string err;

  // Preconditions (DWL#3).
  if (table.s->primary_key == MAX_KEY)
    return report("SECONDARY_LOAD requires a primary key");
  if (table.s->foreign_keys > 0 || table.s->foreign_key_parents > 0)
    return report("tables with foreign keys are not supported");
  bool retain_history;
  std::string adopt_spec;
  if (parse_attributes(*table.s, &retain_history, &adopt_spec, &err))
    return report(err);
  if (!adopt_spec.empty() && !retain_history)
    return report("adopt_history_from requires \"retain_history\": true");
  const std::string full = db + "." + name;
  const bool check_only = history_check_only(thd);

  // Columns of the read set, with their DuckDB types.
  std::vector<Field *> fields;
  std::vector<std::pair<std::string, std::string>> column_types;
  std::string columns;
  for (Field **f = table.field; *f != nullptr; ++f) {
    if (!bitmap_is_set(table.read_set, (*f)->field_index())) continue;
    const std::string type = duckdb_column_type(**f, &err);
    if (type.empty()) return report(err);
    if (!columns.empty()) columns += ", ";
    columns += quote_ident((*f)->field_name) + " " + type;
    column_types.emplace_back((*f)->field_name, type);
    fields.push_back(*f);
  }
  if (retain_history) columns += ", \"__archived_at\" TIMESTAMP";
  const std::vector<std::string> pk_names = primary_key_names(table);

  // Which existing copy this load replaces:
  // - a refresh after DDL, requested by the binlog reader (DWL#4 step 4d);
  // - the table's own history copy: reattach (stale) or resync (DWL#3 3b);
  // - another table's stale history copy: adopt_history_from (DWL#3 3b).
  // A classic copy is never replaced: SECONDARY_UNLOAD first.
  LoadedTable *existing = Registry::instance().find(db, name);
  ColumnRenames column_renames;
  const bool ddl_refresh =
      existing != nullptr && take_refresh(db, name, &column_renames);
  std::string src_db = db, src_table = name;  // the copy replaced
  bool takeover = false;  // reattach, resync or adopt: structure must match
  bool resync = false;
  bool adopt = false;
  if (ddl_refresh) {
    if (existing->retain_history && !retain_history)
      return report("table " + full +
                    ": retain_history cannot be removed by DDL, the archived "
                    "rows would be lost");
  } else if (existing != nullptr) {
    // A table with its own copy: adopt_history_from no longer applies.
    if (!existing->retain_history)
      return report("table " + full +
                    " is already in the lake: run ALTER TABLE ... "
                    "SECONDARY_UNLOAD first");
    if (!retain_history)
      return report("table " + full +
                    ": its copy has retain_history, the table must keep "
                    "\"retain_history\": true");
    takeover = true;
    resync = !existing->stale && !existing->refresh_pending;
  } else if (!adopt_spec.empty()) {
    if (parse_table_name(adopt_spec, db, &src_db, &src_table))
      return report("adopt_history_from must be \"db.table\" or \"table\"");
    const std::string src = src_db + "." + src_table;
    TableState state;
    if (!Registry::instance().snapshot(src_db, src_table, &state))
      return report("adopt_history_from: there is no copy named " + src +
                    " in the lake");
    if (!state.retain_history)
      return report("adopt_history_from: " + src +
                    " is not a history copy (no retain_history)");
    if (!state.stale)
      return report("adopt_history_from: the copy " + src +
                    " is still used by its table (not stale): rename the "
                    "table with RENAME TABLE instead");
    takeover = true;
    adopt = true;
  }
  const bool keep_archived =
      takeover || (ddl_refresh && existing->retain_history);
  const bool replace = ddl_refresh || takeover;
  const std::string src_full = src_db + "." + src_table;

  // Check only (duckdb_history_check_only): the checks above and the
  // structure, nothing written. It always ends with an error: a successful
  // SECONDARY_LOAD would make the server record the table as loaded.
  if (check_only && !takeover)
    return report("structure check: " + full +
                  " has no history copy to take over (no copy of its own, no "
                  "adopt_history_from); nothing loaded "
                  "(duckdb_history_check_only=ON)");

  // Every load builds its copy under a build name in a first DuckLake
  // transaction (T1: the data, no registry write), then renames it and writes
  // the registry in a short second one under apply_mutex (T2). The registry
  // is written by other transactions too (unload, stale marking, renames, the
  // applier); a long transaction writing it would conflict with them at its
  // commit. Until T2 the old copy, if any, stays as it was. A crash between
  // T1 and T2 leaves only a build table, dropped when the lake opens.
  const std::string build_name = name + kBuildSuffix;
  if (!check_only &&
      check_binlog_settings(thd, retain_history, &err))
    return report(err);

  auto con = Lake::instance().connect(thd, &err);
  if (!con) return report(err);
  if (takeover) {
    std::vector<std::string> diffs;
    if (con->execute(
            [&](duckdb_connection c, std::string *e) {
              return compare_history_structure(c, src_db, src_table,
                                               column_types, pk_names, &diffs,
                                               e);
            },
            &err))
      return report("structure check of " + full + " failed: " + err);
    if (!diffs.empty()) {
      for (const std::string &d : diffs) warn(thd, d);
      return report(full + ": the structure does not match the history copy " +
                    src_full + " (" + std::to_string(diffs.size()) +
                    " difference" + (diffs.size() > 1 ? "s" : "") +
                    ", see SHOW WARNINGS); nothing loaded");
    }
    if (check_only)
      return report("structure check passed: " + full + " can " +
                    (adopt ? "adopt the history copy " + src_full
                           : std::string(resync ? "resync" : "reattach") +
                                 " its history copy") +
                    "; nothing loaded (duckdb_history_check_only=ON)");
  }
  if (resync)
    warn(thd, "the live rows of " + full +
                  " are replaced with the InnoDB rows and its archived rows "
                  "are kept; purges in a binary log range the applier did "
                  "not read cannot be recovered");

  // One vector per loaded column (+ __archived_at); the appender casts each to
  // the table type.
  std::vector<duckdb_logical_type> types;
  for (Field *f : fields)
    types.push_back(duckdb_create_logical_type(chunk_type(*f)));
  if (retain_history)
    types.push_back(duckdb_create_logical_type(DUCKDB_TYPE_TIMESTAMP));
  auto free_types = create_scope_guard([&types] {
    for (auto &type : types) duckdb_destroy_logical_type(&type);
  });

  // Writes on the table are blocked during the load, so every transaction that
  // touched it before is in this set.
  const std::string gtids = executed_gtid_set();
  Log_info binlog_info;
  if (mysql_bin_log.get_current_log(&binlog_info) != 0)
    return report("cannot read the current binary log position");
  const BinlogPos load_position{binlog_base_name(binlog_info.log_file_name),
                                binlog_info.pos};

  duckdb_appender app = nullptr;
  // The rollback must run even after a KILL: otherwise the appender keeps the
  // DuckDB transaction alive.
  auto rollback = [&](const std::string &msg) {
    std::string ignored;
    con->execute_cleanup(
        [&](duckdb_connection c, std::string *e) {
          if (app != nullptr) duckdb_appender_destroy(&app);
          return run_sql(c, "ROLLBACK", e);
        },
        &ignored);
    return report("SECONDARY_LOAD of " + db + "." + name + " failed: " + msg);
  };

  const std::string target = lake_table(db, build_name);
  if (con->execute(
          [&](duckdb_connection c, std::string *e) {
            if (run_sql(c, "BEGIN TRANSACTION", e) ||
                run_sql(c, "CREATE SCHEMA IF NOT EXISTS lake." + quote_ident(db),
                        e) ||
                // A leftover of a load interrupted between T1 and T2.
                run_sql(c, "DROP TABLE IF EXISTS " + target, e) ||
                run_sql(c, "CREATE TABLE " + target + " (" + columns + ")", e))
              return true;
            if (duckdb_appender_create_ext(c, "lake", db.c_str(),
                                           build_name.c_str(),
                                           &app) == DuckDBError) {
              *e = duckdb_appender_error(app);
              return true;
            }
            return false;
          },
          &err))
    return rollback(err);

  // We read InnoDB on this (session) thread and fill a chunk in memory; each
  // full chunk is appended on an executor thread.
  duckdb_data_chunk chunk =
      duckdb_create_data_chunk(types.data(), types.size());
  auto free_chunk =
      create_scope_guard([&chunk] { duckdb_destroy_data_chunk(&chunk); });
  const idx_t capacity = duckdb_vector_size();
  idx_t row = 0;
  auto flush = [&]() {
    duckdb_data_chunk_set_size(chunk, row);
    const bool failed = con->execute(
        [&](duckdb_connection, std::string *e) {
          if (duckdb_append_data_chunk(app, chunk) == DuckDBError) {
            *e = duckdb_appender_error(app);
            return true;
          }
          return false;
        },
        &err);
    duckdb_data_chunk_reset(chunk);
    row = 0;
    return failed;
  };

  // Full scan of the primary (InnoDB) table, opened by the server.
  handler *file = table.file;
  unsigned long long rows = 0;
  int rc = file->ha_rnd_init(true);
  while (rc == 0 && err.empty()) {
    rc = file->ha_rnd_next(table.record[0]);
    if (rc != 0) break;
    if (thd->is_killed()) {
      err = "interrupted";
      break;
    }
    for (size_t i = 0; i < fields.size() && err.empty(); i++)
      store_field(duckdb_data_chunk_get_vector(chunk, i), row, *fields[i],
                  &err);
    if (!err.empty()) break;
    if (retain_history)
      store_null(duckdb_data_chunk_get_vector(chunk, fields.size()), row);
    row++;
    rows++;
    if (row == capacity && flush()) break;
  }
  file->ha_rnd_end();
  if (rc != 0 && rc != HA_ERR_END_OF_FILE && err.empty())
    err = "error " + std::to_string(rc) + " while reading the InnoDB table";
  if (err.empty() && row > 0) flush();
  if (!err.empty()) return rollback(err);

  // T1: close the appender, check the count, commit the build table.
  if (con->execute(
          [&](duckdb_connection c, std::string *e) {
            if (duckdb_appender_close(app) == DuckDBError) {
              *e = duckdb_appender_error(app);
              return true;
            }
            duckdb_appender_destroy(&app);
            app = nullptr;
            std::string count;
            if (query_value_sql(c, "SELECT count(*) FROM " + target, &count, e))
              return true;
            if (count != std::to_string(rows)) {
              *e = "row count mismatch: InnoDB " + std::to_string(rows) +
                   ", DuckDB " + count;
              return true;
            }
            return run_sql(c, "COMMIT", e);
          },
          &err))
    return rollback(err);

  // From here a failure rolls back T2 and drops the build table.
  auto fail_t2 = [&](const std::string &msg) {
    std::string ignored;
    con->execute_cleanup(
        [&](duckdb_connection c, std::string *e) {
          run_sql(c, "ROLLBACK", e);
          return run_sql(c, "DROP TABLE IF EXISTS " + target, e);
        },
        &ignored);
    return report("SECONDARY_LOAD of " + full + " failed: " + msg);
  };

  // Resync of a loaded history table: the archived rows are taken from the
  // old copy in T2, so every purge the applier read before the load position
  // must be committed there first (later it would be skipped as a change of
  // the old copy). Writes on the table are blocked during the load, so
  // nothing after the load position concerns it.
  if (resync) {
    bool not_running = false;
    if (BinlogReader::instance().wait_applied(load_position, thd,
                                              &not_running, &err))
      return fail_t2(err);
    if (not_running)
      warn(thd, "the binlog applier is not running: changes of " + full +
                    " it has not applied are not in the archived rows");
  }

  // T2, under apply_mutex like every registry writer: archived rows, swap,
  // registry row.
  std::lock_guard<std::mutex> apply_guard(Lake::instance().apply_mutex());
  if (con->execute(
          [&](duckdb_connection c, std::string *e) {
            if (run_sql(c, "BEGIN TRANSACTION", e)) return true;
            if (replace) {
              // Archived rows kept, then the new copy replaces the old one
              // (for adopt_history_from, the other table's copy).
              if ((keep_archived &&
                   copy_archived_rows(c, src_db, src_table, db, build_name,
                                      column_types, column_renames, e)) ||
                  run_sql(c, "DROP TABLE " + lake_table(src_db, src_table),
                          e) ||
                  (adopt && run_sql(c,
                                    "DELETE FROM lake.__mysql.tables" +
                                        registry_where(src_db, src_table),
                                    e)))
                return true;
            }
            if (run_sql(c,
                        "ALTER TABLE " + target + " RENAME TO " +
                            quote_ident(name),
                        e))
              return true;
            // The DELETE is a safety net: a leftover registry row is replaced.
            return run_sql(c,
                           "DELETE FROM lake.__mysql.tables" +
                               registry_where(db, name),
                           e) ||
                   run_sql(c,
                           "INSERT INTO lake.__mysql.tables (db, tbl, "
                           "retain_history, state, load_gtid_set, loaded_at, "
                           "load_binlog_file, load_binlog_pos, pk_columns) "
                           "VALUES (" +
                               quote_literal(db) + ", " + quote_literal(name) +
                               ", " + (retain_history ? "true" : "false") +
                               ", 'loaded', " + quote_literal(gtids) +
                               ", now(), " +
                               quote_literal(load_position.file) + ", " +
                               std::to_string(load_position.pos) + ", " +
                               quote_literal(join_names(pk_names)) + ")",
                           e) ||
                   run_sql(c, "COMMIT", e);
          },
          &err))
    return fail_t2(err);

  // A refresh updates the entry in place: handlers keep its lock.
  if (adopt) Registry::instance().remove(src_db, src_table);
  Registry::instance().add(db, name, retain_history, gtids, load_position,
                           join_names(pk_names));
  const double secs =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  const std::string action =
      ddl_refresh ? "refreshed"
                  : adopt ? "loaded (adopting the history copy " + src_full +
                                ")"
                          : resync ? "resynced" : takeover ? "reattached"
                                                           : "loaded";
  DUCKDB_LOG_INFO("%s %s %s the lake: %llu rows in %.1f s%s", action.c_str(),
                  full.c_str(), replace ? "in" : "into", rows, secs,
                  retain_history ? " (retain_history)" : "");
  return false;
}

bool unload_table_from_lake(THD *thd, const char *db, const char *table,
                            bool explicit_unload) {
  LoadedTable *t = Registry::instance().find(db, table);
  if (t == nullptr) {
    if (!explicit_unload) return false;
    return report(std::string("Table ") + db + "." + table +
                  " is not loaded in secondary engine.");
  }
  // ALTER TABLE ... DROP PARTITION calls the unload (sql/sql_table.cc) like
  // DROP TABLE does. The table stays: the binlog applier refreshes its copy
  // when it reads the statement (DWL#4 step 4d), so we keep it.
  if (!explicit_unload && thd != nullptr &&
      thd->lex->sql_command == SQLCOM_ALTER_TABLE &&
      thd->lex->alter_info != nullptr &&
      (thd->lex->alter_info->flags & Alter_info::ALTER_DROP_PARTITION) != 0)
    return false;

  std::string err;
  auto con = Lake::instance().connect(thd, &err);
  if (!con) return report(err);
  // The binlog applier must not write the table while we drop it or mark it
  // stale: it commits under the same mutex and checks the registry under it.
  // It may wait here for one batch commit, or until a large source
  // transaction applied in pieces is committed (DWL#4 step 4c-2).
  std::lock_guard<std::mutex> apply_guard(Lake::instance().apply_mutex());

  // History is never deleted implicitly (DWL#3 step 3b): SECONDARY_UNLOAD of
  // a history table drops it only with the session confirmation; DDL that
  // unloads (DROP TABLE, SECONDARY_ENGINE=NULL) keeps it, stale.
  const bool drop_history =
      t->retain_history && explicit_unload && unload_history_confirmed(thd);
  if (t->retain_history && explicit_unload && !drop_history)
    return report(std::string("SECONDARY_UNLOAD of ") + db + "." + table +
                  " would delete its archived rows, which exist only in the "
                  "lake: SET SESSION duckdb_unload_history_confirm = ON "
                  "first");
  if (t->retain_history && !drop_history) {
    // DDL on a history table: we keep the data and mark the copy stale.
    if (con->run("UPDATE lake.__mysql.tables SET state = 'stale'" +
                     registry_where(db, table),
                 &err))
      return report(err);
    Registry::instance().mark_stale(db, table);
    const std::string msg = std::string("DuckDB copy of ") + db + "." + table +
                            " kept and marked stale: it holds archived rows";
    push_warning_printf(thd, Sql_condition::SL_WARNING,
                        ER_SECONDARY_ENGINE_PLUGIN,
                        ER_THD(thd, ER_SECONDARY_ENGINE_PLUGIN), msg.c_str());
    DUCKDB_LOG_WARNING("%s", msg.c_str());
    return false;
  }

  // Classic copy, or a history copy with the confirmation: we drop it. Its
  // Parquet files are removed later by snapshot expiry and file cleanup
  // (Phase 3).
  if (drop_copy(*con, db, table, &err)) return report(err);
  if (drop_history) {
    DUCKDB_LOG_WARNING(
        "unloaded %s.%s from the lake with its archived rows "
        "(duckdb_unload_history_confirm=ON)",
        db, table);
  } else {
    DUCKDB_LOG_INFO("unloaded %s.%s from the lake", db, table);
  }
  return false;
}

bool discard_stale_copy(THD *thd, const std::string &spec) {
  std::string db, table, err;
  if (parse_table_name(spec, "", &db, &table))
    return report("duckdb_discard_stale_copy must be \"db.table\"");
  const std::string full = db + "." + table;
  TableState state;
  if (!Registry::instance().snapshot(db, table, &state))
    return report("there is no copy named " + full + " in the lake");
  if (!state.stale)
    return report("the copy " + full +
                  " is not stale: unload it with ALTER TABLE ... "
                  "SECONDARY_UNLOAD");
  if (state.retain_history && !unload_history_confirmed(thd))
    return report("the copy " + full +
                  " holds archived rows, which exist only in the lake: SET "
                  "SESSION duckdb_unload_history_confirm = ON first");
  bool exists = false;
  if (mysql_table_exists(thd, db, table, &exists, &err)) return report(err);
  if (exists)
    return report("the table " + full +
                  " exists: unload its copy with ALTER TABLE ... "
                  "SECONDARY_UNLOAD (or load it again to reattach the copy)");
  auto con = Lake::instance().connect(thd, &err);
  if (!con) return report(err);
  std::lock_guard<std::mutex> apply_guard(Lake::instance().apply_mutex());
  if (drop_copy(*con, db, table, &err)) return report(err);
  DUCKDB_LOG_WARNING("discarded the stale copy %s from the lake%s",
                     full.c_str(),
                     state.retain_history ? " with its archived rows" : "");
  return false;
}

bool rename_in_lake(Lake::Connection &con, const std::string &db,
                    const std::string &table, const std::string &new_db,
                    const std::string &new_table, std::string *err) {
  // An unload holds the same mutex.
  std::lock_guard<std::mutex> apply_guard(Lake::instance().apply_mutex());
  TableState state;
  if (!Registry::instance().snapshot(db, table, &state)) return false;
  TableState other;
  if (Registry::instance().snapshot(new_db, new_table, &other)) {
    *err = "the lake already has a copy named " + new_db + "." + new_table;
    return true;
  }
  const bool failed = con.execute(
      [&](duckdb_connection c, std::string *e) {
        bool f = run_sql(c, "BEGIN TRANSACTION", e);
        if (!f && db == new_db) {
          f = run_sql(c,
                      "ALTER TABLE " + lake_table(db, table) + " RENAME TO " +
                          quote_ident(new_table),
                      e);
        } else if (!f) {
          // DuckLake renames within a schema only: we copy the table (all its
          // rows, archived ones included) and drop the old one.
          f = run_sql(c, "CREATE SCHEMA IF NOT EXISTS lake." +
                             quote_ident(new_db),
                      e) ||
              run_sql(c,
                      "CREATE TABLE " + lake_table(new_db, new_table) +
                          " AS SELECT * FROM " + lake_table(db, table),
                      e) ||
              run_sql(c, "DROP TABLE " + lake_table(db, table), e);
        }
        f = f ||
            run_sql(c,
                    "UPDATE lake.__mysql.tables SET db = " +
                        quote_literal(new_db) + ", tbl = " +
                        quote_literal(new_table) + registry_where(db, table),
                    e) ||
            run_sql(c, "COMMIT", e);
        if (f) {
          std::string ignored;
          run_sql(c, "ROLLBACK", &ignored);
        }
        return f;
      },
      err);
  if (failed) return true;
  Registry::instance().rename(db, table, new_db, new_table);
  return false;
}

bool mark_refresh_pending_in_lake(Lake::Connection &con, const std::string &db,
                                  const std::string &table,
                                  const ColumnRenames &renames,
                                  std::string *err) {
  std::lock_guard<std::mutex> apply_guard(Lake::instance().apply_mutex());
  const std::string encoded = encode_renames(renames);
  Registry::instance().mark_refresh_pending(db, table, encoded);
  return con.run("UPDATE lake.__mysql.tables SET state = 'refresh_pending', "
                 "pending_renames = " +
                     quote_literal(encoded) + registry_where(db, table),
                 err);
}

bool mark_stale_in_lake(Lake::Connection &con, const std::string &db,
                        const std::string &table, std::string *err) {
  std::lock_guard<std::mutex> apply_guard(Lake::instance().apply_mutex());
  // The registry first: the reader stops applying changes to it even if the
  // lake cannot be written now.
  Registry::instance().mark_stale(db, table);
  return con.run("UPDATE lake.__mysql.tables SET state = 'stale'" +
                     registry_where(db, table),
                 err);
}

}  // namespace duckdb_se
