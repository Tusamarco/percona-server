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
-- ENUM and SET columns (table et of duckdb_offload_setup.sql): the value
-- in string contexts, the index / bitmask in ORDER BY and as a number; each
-- query must give the same result with OFF and FORCED.
SELECT id, e, es, s, l FROM et ORDER BY id
SELECT id, e FROM et ORDER BY e, id LIMIT 12
SELECT id, es FROM et ORDER BY es DESC, id LIMIT 12
SELECT id, s FROM et ORDER BY s, id LIMIT 12
SELECT e, COUNT(*) FROM et GROUP BY e ORDER BY e
SELECT es, COUNT(*) FROM et GROUP BY es ORDER BY es DESC
SELECT s, COUNT(*) FROM et GROUP BY s ORDER BY s
SELECT l, COUNT(*) FROM et GROUP BY l ORDER BY l
SELECT MIN(e), MAX(e), MIN(es), MAX(es), MIN(s), MAX(s), MIN(l), MAX(l) FROM et
SELECT id FROM et WHERE e = 'medium' ORDER BY id
SELECT id FROM et WHERE e = 'MEDIUM' ORDER BY id
SELECT id FROM et WHERE e = 2 ORDER BY id
SELECT id FROM et WHERE e > 'm' ORDER BY id
SELECT id FROM et WHERE e < 3 ORDER BY id
SELECT id FROM et WHERE es IN ('a', 'c') ORDER BY id
SELECT id FROM et WHERE es BETWEEN 'a' AND 'b' ORDER BY id
SELECT id FROM et WHERE e = '' ORDER BY id
SELECT id, e + 0, es + 0, s + 0, e - 1, s * 2 FROM et ORDER BY id
SELECT id FROM et WHERE e + 0 = 0 OR s = 0 ORDER BY id
SELECT id FROM et WHERE s = 'red,blue' ORDER BY id
SELECT id FROM et WHERE s = 'blue,red' ORDER BY id
SELECT id FROM et WHERE es LIKE 'a%' ORDER BY id
SELECT COUNT(DISTINCT e), COUNT(DISTINCT s), COUNT(e), COUNT(*) FROM et
SELECT g, MIN(e), MAX(s) FROM et GROUP BY g ORDER BY g
SELECT a.id, b.id FROM et a JOIN et b ON a.es = b.es AND a.id < b.id WHERE a.id < 15 ORDER BY 1, 2
SELECT id FROM et WHERE l = 'ETE' OR l = 'zed' ORDER BY id
SELECT id, e FROM et WHERE id IN (2, 3, 4, 90, 100, 101) ORDER BY id
-- a merged derived table: its column is the base column (index order)
SELECT x.e FROM (SELECT e FROM et WHERE id < 10) x ORDER BY x.e, x.e IS NULL
