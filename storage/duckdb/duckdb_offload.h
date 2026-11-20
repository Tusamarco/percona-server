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

#include "storage/duckdb/duckdb_registry.h"  // BinlogPos

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
  unsigned long long offloaded{0};  // run in DuckDB to the end
  unsigned long long refused{0};
  unsigned long long explained{0};  // EXPLAIN of an offloaded statement
  unsigned long long waits{0};      // waits for the session's last commit
  unsigned long long wait_timeouts{0};
  unsigned long long decimal_reruns{0};  // ROUND runs with fewer decimals
  unsigned long long time_us{0};  // time in DuckDB of the offloaded ones
  std::string last_refusal;
};
OffloadStatus offload_status();

/** The transaction observer that records each connection's last commit
    position (read your own writes). true on error. */
bool register_offload_observer(void *plugin);
void unregister_offload_observer(void *plugin);

/** duckdb_offload_wait_applied of the session (ha_duckdb.cc). */
bool offload_wait_applied(THD *thd);

/** The binary log position of this session's last commit (the current end
    of the binary log the first time). true on error. */
bool session_commit_position(THD *thd, BinlogPos *pos);

/** duckdb_ddl_wait_refresh and duckdb_ddl_wait_refresh_timeout (seconds, 0 =
    no limit) of the session (ha_duckdb.cc). */
bool ddl_wait_refresh(THD *thd);
unsigned long long ddl_wait_refresh_timeout(THD *thd);

/** duckdb_offload_wait_timeout_ms of the session (ha_duckdb.cc). */
unsigned long long offload_wait_timeout_ms(THD *thd);

/** duckdb_offload_float_aggregates (ha_duckdb.cc): SUM / AVG of FLOAT and
    DOUBLE. DuckDB adds in parallel in no fixed order, so the last digits
    differ from InnoDB's (and between runs). */
enum class FloatAggregates : unsigned long { REFUSE = 0, APPROXIMATE = 1 };
FloatAggregates float_aggregates(THD *thd);

/** duckdb_offload_min_rows of the session (ha_duckdb.cc). */
unsigned long long offload_min_rows(THD *thd);

/** duckdb_decimal_overflow (ha_duckdb.cc): a DECIMAL value above DuckDB's
    38 digits in a statement MySQL declares wider. */
enum class DecimalOverflow : unsigned long { FALLBACK = 0, ROUND = 1, ERROR = 2 };
DecimalOverflow decimal_overflow(THD *thd);

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_OFFLOAD_H_
