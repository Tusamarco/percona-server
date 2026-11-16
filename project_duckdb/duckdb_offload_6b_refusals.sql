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
-- DWL#6 step 6b: queries that must be refused, on table p6 (manual-test-plan.md
-- section 22). Run with EXPECT=refused ./duckdb_offload_compare.sh.
SELECT CASE WHEN qty < 0 THEN 'neg' ELSE 'pos' END AS k, COUNT(*) FROM p6 GROUP BY k ORDER BY k;
SELECT HEX(b4), COUNT(*) FROM p6 GROUP BY b4 ORDER BY b4;
SELECT COUNT(*) FROM p6 WHERE raw LIKE 'k_';
SELECT id, TRUNCATE(amount, 1) FROM p6 WHERE id <= 5 ORDER BY id;
SELECT id, ROUND(price) FROM p6 WHERE id <= 5 ORDER BY id;
SELECT id, u + 1 FROM p6 WHERE id <= 5 ORDER BY id;
SELECT DISTINCT b4 FROM p6 ORDER BY b4;
SELECT id, HOUR(ts) FROM p6 WHERE id <= 5 ORDER BY id;
SELECT COUNT(*) FROM p6 WHERE b4 = 1;
