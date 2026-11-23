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

// JSON columns in the binary log (2026-11-23): MySQL writes the binary JSON
// format; the copy stores the text MySQL prints for the value. We use the
// server's own code, as Field_json::val_str does (parse_binary, then
// Json_wrapper::to_string with quoting), so the applier writes exactly the
// text the load wrote. Kept apart from the row decoder, which the unit tests
// link without the server.

#include "storage/duckdb/duckdb_json.h"

#include <string>

#include "sql-common/json_binary.h"
#include "sql/current_thd.h"
#include "sql/sql_class.h"
#include "sql-common/json_dom.h"
#include "sql_string.h"
#include "storage/duckdb/duckdb_row_decoder.h"

namespace duckdb_se {

namespace {

// Used only by the reader thread (set by it at start and stop).
THD *g_json_thd = nullptr;

bool json_binary_to_text(const char *data, size_t len, std::string *text,
                         std::string *err) {
  // Json_wrapper::to_string needs current_thd. After an internal session
  // (refresh after DDL) detaches, the thread may have none: ours again.
  if (current_thd == nullptr && g_json_thd != nullptr)
    g_json_thd->store_globals();
  if (current_thd == nullptr) {
    *err = "no server session to print the JSON value";
    return true;
  }
  const json_binary::Value v = json_binary::parse_binary(data, len);
  if (v.type() == json_binary::Value::ERROR) {
    *err = "invalid binary JSON value";
    return true;
  }
  Json_wrapper wr(v);
  String buf;
  // The applier has no session: the depth error is recorded here instead
  // of raised (JsonDepthErrorHandler needs a THD).
  bool too_deep = false;
  if (wr.to_string(&buf, true, "duckdb", [&too_deep] { too_deep = true; }) ||
      too_deep) {
    *err = "JSON value cannot be printed (nested too deep)";
    return true;
  }
  text->assign(buf.ptr(), buf.length());
  return false;
}

}  // namespace

void install_json_decoder() { set_json_to_text(json_binary_to_text); }

void set_json_thd(THD *thd) { g_json_thd = thd; }

}  // namespace duckdb_se
