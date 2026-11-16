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
-- DWL#6: exact decimal division and AVG, compared between InnoDB and DuckDB
-- (one per line), on table dv of duckdb_offload_setup.sql (manual-test-plan.md
-- section 30); some lines set div_precision_increment first.
SELECT id, a / b, a / i, i / b, i / 3, a / 7, -a / 3, j / i FROM dv ORDER BY id;
SELECT id, (a + 1) / (b - 1), a / 2.5, 10 / i, i / j, a / 0, i / 0 FROM dv ORDER BY id;
SELECT id, a / b * 2, (a / 3) + (b / 7), ROUND(a / 3, 1), a / b / 3 FROM dv WHERE id > 1990 OR id < 40 ORDER BY id;
SELECT SUM(a) / 6, SUM(a) / COUNT(*), SUM(a) / SUM(b), AVG(a) / 3, SUM(i) / SUM(j) FROM dv;
SELECT i % 10 AS g, SUM(a) / COUNT(a), SUM(b) / NULLIF(SUM(i), 0) FROM dv GROUP BY g ORDER BY g;
SET SESSION div_precision_increment = 0; SELECT id, a / b, i / 3, a / 7, j / i FROM dv ORDER BY id;
SET SESSION div_precision_increment = 9; SELECT id, a / b, i / 3, a / 7, j / i FROM dv ORDER BY id;
SET SESSION div_precision_increment = 9; SELECT SUM(a) / SUM(b), SUM(a) / 7 FROM dv;
SELECT COUNT(*) FROM dv WHERE a / b > 1.5;
SELECT id FROM dv WHERE i / 4 = 2.5 OR a / 3 = 0.0167 ORDER BY id;
SELECT id, ROUND(a / 3, 1), ROUND(i / 7, 3), ROUND(-a / 7, 2) FROM dv WHERE id < 60 ORDER BY id;
SELECT id, a / b / 3, (a / 3) / (b / 7), (a / 3) + (b / 7) FROM dv WHERE id < 60 ORDER BY id;
SET SESSION div_precision_increment = 0; SELECT SUM(a) / SUM(b), AVG(a), AVG(i), AVG(a) / 3 FROM dv;
SET SESSION div_precision_increment = 9; SELECT AVG(a), AVG(i), AVG(DISTINCT i), SUM(a) / 7 FROM dv;
SELECT i % 7 AS g, AVG(a), AVG(i), AVG(j) FROM dv GROUP BY g ORDER BY g;
SELECT i % 5 AS g, AVG(a) + 1, AVG(a) * 2, AVG(a) / 3, ROUND(AVG(a), 2) FROM dv GROUP BY g ORDER BY g;
SET SESSION div_precision_increment = 9; SELECT i % 5 AS g, AVG(a) + 1, AVG(a) / 3, (a / 7) * 3 FROM dv WHERE id < 20 GROUP BY g, a ORDER BY g, a;
SELECT id, qty / 3 FROM p6 WHERE id <= 5 ORDER BY id;
