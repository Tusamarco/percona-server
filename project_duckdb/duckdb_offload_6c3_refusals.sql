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
-- DWL#6 step 6c-3: queries that must be refused (manual-test-plan.md
-- section 26). Run with EXPECT=refused ./duckdb_offload_compare.sh.
SELECT d.grp, (SELECT p6.qty FROM p6 WHERE p6.grp = d.grp AND p6.id = d.grp + 7) FROM dim d ORDER BY 1;
SELECT d.grp, (d.grp, d.region) IN (SELECT grp, region FROM dim WHERE grp < 3) FROM dim d ORDER BY 1;
SELECT d.grp, d.label IN (SELECT b4 FROM p6 WHERE id < 20) FROM dim d ORDER BY 1;
SELECT d.grp, d.ci IN (SELECT ci FROM p6 WHERE id < 20) FROM dim d ORDER BY 1;
SELECT d.grp, d.grp IN (SELECT name FROM dim) FROM dim d ORDER BY 1;
