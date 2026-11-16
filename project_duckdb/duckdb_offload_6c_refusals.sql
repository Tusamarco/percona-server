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
-- DWL#6 step 6c-1: queries that must be refused (manual-test-plan.md section
-- 23). Run with EXPECT=refused ./duckdb_offload_compare.sh.
SELECT a.b4, COUNT(*) FROM p6 a JOIN dim d ON a.grp = d.grp GROUP BY a.b4 ORDER BY a.b4;
SELECT d.name, MIN(a.b4) FROM p6 a JOIN dim d ON a.grp = d.grp GROUP BY d.name ORDER BY 1;
SELECT d.grp, x.c FROM dim d, LATERAL (SELECT COUNT(*) AS c FROM p6 WHERE p6.grp = d.grp) x ORDER BY 1;
WITH RECURSIVE r (k) AS (SELECT 0 UNION ALL SELECT k + 1 FROM r WHERE k < 5) SELECT dim.grp FROM r JOIN dim ON dim.grp = r.k ORDER BY 1;
