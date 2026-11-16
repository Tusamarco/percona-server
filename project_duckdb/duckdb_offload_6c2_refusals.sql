-- Copyright (c) 2026-present, Marco Tusa. All rights reserved.
--
-- This program is free software; you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation; version 2 of the License.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with this program; if not, write to the Free Software
-- Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
--
-- DWL#6 step 6c-2: queries that must be refused (manual-test-plan.md
-- section 25). Run with EXPECT=refused ./duckdb_offload_compare.sh.
SELECT grp FROM dim UNION SELECT name FROM dim ORDER BY 1;
SELECT label FROM dim UNION SELECT b4 FROM p6 ORDER BY 1;
SELECT ci FROM dim UNION SELECT ci FROM p6 ORDER BY 1;
SELECT ts FROM p6 WHERE id < 5 UNION ALL SELECT dt FROM p6 WHERE id < 5 ORDER BY 1;
SELECT d FROM p6 WHERE id < 5 UNION ALL SELECT 'x' ORDER BY 1;
SELECT grp FROM dim WHERE grp IN (SELECT grp FROM p6 UNION SELECT 1) ORDER BY 1;
