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
# Drops the test tables and discards stale history copies, so that the lake
# and the schema are as the test found them.
--disable_query_log
--disable_warnings
DROP VIEW IF EXISTS v6;
DROP VIEW IF EXISTS vh;
DROP TABLE IF EXISTS g, p6, n, dim, hist, nl, dv, cw, cwd, t1, t2, h1, h2, hcopy;
DROP PROCEDURE IF EXISTS ph;
--enable_warnings
# Dropping a history table keeps its copy (stale): discard it.
SET SESSION duckdb_unload_history_confirm = ON;
--error 0, ER_SECONDARY_ENGINE_PLUGIN
SET GLOBAL duckdb_discard_stale_copy = 'test.hist';
--error 0, ER_SECONDARY_ENGINE_PLUGIN
SET GLOBAL duckdb_discard_stale_copy = 'test.h1';
SET SESSION duckdb_unload_history_confirm = OFF;
--enable_query_log
