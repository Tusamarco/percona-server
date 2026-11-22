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
-- DWL#6 step A2: queries that must be refused (manual-test-plan.md section
-- 33). Run with EXPECT=refused ./duckdb_offload_compare.sh.
SELECT COUNT(*) FROM cw WHERE c LIKE 'a%';
SELECT DISTINCT c FROM cw ORDER BY c;
SELECT c FROM cw WHERE id < 10 UNION SELECT c FROM cwd ORDER BY 1;
SELECT UPPER(c), COUNT(*) FROM cw GROUP BY UPPER(c) ORDER BY 1;
SELECT COUNT(*) FROM cw WHERE UPPER(c) = 'APPLE';
SELECT cw.c, COUNT(*) FROM cw JOIN cwd d ON d.c = cw.c GROUP BY cw.c ORDER BY cw.c;
SELECT cw.c, MIN(d.c) FROM cw JOIN cwd d ON d.c = cw.c WHERE cw.id < 50 GROUP BY cw.id, cw.c ORDER BY cw.id;
