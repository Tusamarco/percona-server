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

#ifndef STORAGE_DUCKDB_DUCKDB_OFFLOAD_H_
#define STORAGE_DUCKDB_DUCKDB_OFFLOAD_H_

#include <string>

class THD;
struct handlerton;

namespace duckdb_se {

/**
  Query offload (DWL#6): installs the secondary engine hooks and the external
  executor flag on the handlerton. A query the generator cannot translate
  exactly is refused with a reason: the server then runs it on InnoDB, or,
  with use_secondary_engine=FORCED, reports the reason.
*/
void install_offload_hooks(handlerton *hton);

struct OffloadStatus {
  unsigned long long offloaded{0};
  unsigned long long refused{0};
  std::string last_refusal;
};
OffloadStatus offload_status();

/** The transaction observer that records each connection's last commit
    position (read your own writes). true on error. */
bool register_offload_observer(void *plugin);
void unregister_offload_observer(void *plugin);

/** duckdb_offload_wait_applied of the session (ha_duckdb.cc). */
bool offload_wait_applied(THD *thd);

/** duckdb_offload_min_rows of the session (ha_duckdb.cc). */
unsigned long long offload_min_rows(THD *thd);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_OFFLOAD_H_
