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
# $query must be refused: with FORCED it fails with the reason (recorded in
# the result), with ON it gives the InnoDB rows.
# Usage: --let $query= SELECT ...
#        --source ../include/duckdb_refused.inc
--echo # $query
SET SESSION use_secondary_engine = FORCED;
--error ER_SECONDARY_ENGINE_PLUGIN, ER_SECONDARY_ENGINE, ER_SUBQUERY_NO_1_ROW
--eval $query
--disable_query_log
--disable_result_log
--let $duckdb_off= $MYSQLTEST_VARDIR/tmp/duckdb_refused_off.txt
--let $duckdb_on= $MYSQLTEST_VARDIR/tmp/duckdb_refused_on.txt
SET SESSION use_secondary_engine = OFF;
--output $duckdb_off
--eval $query
SET SESSION use_secondary_engine = ON;
--output $duckdb_on
--eval $query
--enable_result_log
--enable_query_log
--diff_files $duckdb_off $duckdb_on
--remove_file $duckdb_off
--remove_file $duckdb_on
