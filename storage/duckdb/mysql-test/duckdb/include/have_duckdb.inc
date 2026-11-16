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
# Skips the test when the DuckDB engine is not active.
--disable_query_log
if (!`SELECT COUNT(*) FROM information_schema.PLUGINS WHERE PLUGIN_NAME = 'duckdb' AND PLUGIN_STATUS = 'ACTIVE'`)
{
  --skip Needs the duckdb plugin (ha_duckdb.so in plugin_output_directory)
}
# Expected plugin warnings in a test server (MTR's secure_file_priv is its
# var directory, which holds the lake).
call mtr.add_suppression("Plugin duckdb reported: 'DuckDB lake directory .* is inside secure_file_priv");
call mtr.add_suppression("Plugin duckdb reported: 'binlog_expire_logs_seconds");
call mtr.add_suppression("Plugin duckdb reported: 'DuckDB copy of .* kept and marked stale");
call mtr.add_suppression("Plugin duckdb reported: 'discarded the stale copy");
call mtr.add_suppression("Plugin duckdb reported: 'unloaded .* with its archived rows");
--enable_query_log
