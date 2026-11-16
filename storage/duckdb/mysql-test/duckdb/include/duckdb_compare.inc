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
# Runs $query with use_secondary_engine=OFF (InnoDB) and FORCED (DuckDB),
# fails if the results differ or if the FORCED run did not execute in DuckDB.
# Usage: --let $query= SELECT ...
#        --source ../include/duckdb_compare.inc
--echo # $query
--disable_query_log
--disable_result_log
--let $duckdb_off= $MYSQLTEST_VARDIR/tmp/duckdb_compare_off.txt
--let $duckdb_on= $MYSQLTEST_VARDIR/tmp/duckdb_compare_on.txt
SET SESSION use_secondary_engine = OFF;
--output $duckdb_off
--eval $query
--let $duckdb_before= query_get_value(SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries', Value, 1)
SET SESSION use_secondary_engine = FORCED;
--output $duckdb_on
--eval $query
--let $duckdb_after= query_get_value(SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries', Value, 1)
SET SESSION use_secondary_engine = ON;
--enable_result_log
--enable_query_log
if ($duckdb_after == $duckdb_before)
{
  --die The query did not run in DuckDB: $query
}
--diff_files $duckdb_off $duckdb_on
--remove_file $duckdb_off
--remove_file $duckdb_on
