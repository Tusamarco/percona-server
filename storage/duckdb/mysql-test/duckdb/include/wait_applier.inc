# Copyright (c) 2026-present, Marco Tusa. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
#
# Waits until the binlog applier has read and committed everything in the
# binary log (state idle at the current end), so that DuckDB has every change
# made so far. A debug build can need seconds for one DuckLake commit, longer
# than the freshness wait of an offloaded query (duckdb_offload_wait_applied).
--disable_query_log
# The check reads performance_schema: never in the secondary engine.
--let $duckdb_saved_use= `SELECT @@session.use_secondary_engine`
SET SESSION use_secondary_engine = OFF;
--let $duckdb_binlog_end= query_get_value(SHOW BINARY LOG STATUS, Position, 1)
--let $wait_timeout= 300
--let $wait_condition= SELECT COUNT(*) = 2 FROM performance_schema.global_status WHERE (VARIABLE_NAME = 'duckdb_apply_reader_state' AND VARIABLE_VALUE = 'idle') OR (VARIABLE_NAME = 'duckdb_apply_reader_position' AND CAST(VARIABLE_VALUE AS UNSIGNED) >= $duckdb_binlog_end)
--source include/wait_condition.inc
--eval SET SESSION use_secondary_engine = $duckdb_saved_use
--enable_query_log
