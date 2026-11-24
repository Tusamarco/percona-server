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
-- DWL#6 step A2: weight strings (non-binary collations), compared between
-- InnoDB and DuckDB (one per line), on cw, cwd, dim, p6, g of
-- duckdb_offload_setup.sql (manual-test-plan.md section 33).
SELECT COUNT(*) FROM cw WHERE c = 'apple';
SELECT COUNT(*) FROM cw WHERE c = 'APPLE ';
SELECT COUNT(*) FROM cw WHERE g = 'apple  ';
SELECT COUNT(*) FROM cw WHERE c IN ('apple', 'BANANA', 'durian');
SELECT COUNT(*) FROM cw WHERE c > 'b';
SELECT COUNT(*) FROM cw WHERE c < 'B' OR c BETWEEN 'cherry' AND 'DURIAN';
SELECT COUNT(*) FROM cw WHERE g > 'apple' AND g < 'b ';
SELECT COUNT(*) FROM cw WHERE l = 'ete';
SELECT COUNT(*) FROM cw WHERE s = 'Apple';
SELECT COUNT(*) FROM cw WHERE c = UPPER('apple') AND c <> 'x';
SELECT c, COUNT(*), SUM(k) FROM cw GROUP BY c ORDER BY c;
SELECT g, COUNT(*) FROM cw GROUP BY g ORDER BY g;
SELECT l, COUNT(*) FROM cw GROUP BY l ORDER BY l;
SELECT s, COUNT(*) FROM cw GROUP BY s ORDER BY s;
SELECT k, COUNT(DISTINCT c), COUNT(DISTINCT g), COUNT(DISTINCT l), COUNT(DISTINCT s) FROM cw GROUP BY k ORDER BY k;
SELECT MIN(c), MAX(c), MIN(g), MAX(g), MIN(l), MAX(s) FROM cw;
SELECT k, MIN(c), MAX(g) FROM cw GROUP BY k ORDER BY k;
SELECT id, c FROM cw WHERE id < 40 ORDER BY c, id;
SELECT id, g FROM cw WHERE id < 40 ORDER BY g DESC, id;
SELECT c, COUNT(*) FROM cw GROUP BY c HAVING c <> 'apple' ORDER BY c;
SELECT SUM(CASE WHEN c = 'apple' THEN 1 ELSE 0 END), COUNT(NULLIF(c, 'banana')) FROM cw;
SELECT d.id, d.c, COUNT(*) FROM cw JOIN cwd d ON d.c = cw.c GROUP BY d.id, d.c ORDER BY d.id;
SELECT d.c, COUNT(cw.id) FROM cwd d LEFT JOIN cw ON cw.c = d.c GROUP BY d.id, d.c ORDER BY d.c;
SELECT COUNT(*) FROM cw WHERE c IN (SELECT c FROM cwd WHERE region = 1);
SELECT 'h1' AS p, k FROM cw WHERE id < 5 UNION ALL SELECT 'h2', k FROM cw WHERE id < 5 ORDER BY p DESC, k;
SELECT x.c, x.n FROM (SELECT c, COUNT(*) AS n FROM cw WHERE id < 900 GROUP BY c) x ORDER BY x.c;
SELECT ci, COUNT(*), SUM(amount) FROM g GROUP BY ci ORDER BY ci;
SELECT COUNT(*) FROM p6 JOIN dim ON dim.ci = p6.ci;
SELECT d.ci, COUNT(*) FROM p6 JOIN dim d ON d.grp = p6.grp GROUP BY d.grp, d.ci ORDER BY d.ci, d.grp;
SELECT COUNT(*) FROM dim WHERE ci = 'm' OR ci = 'T';
SELECT ci, COUNT(*) FROM p6 GROUP BY ci ORDER BY ci;
SELECT COUNT(*) FROM p6 WHERE ci = 'm';
SELECT d.c, COUNT(*) FROM cw JOIN cwd d ON d.c = cw.c GROUP BY d.c ORDER BY d.c;
