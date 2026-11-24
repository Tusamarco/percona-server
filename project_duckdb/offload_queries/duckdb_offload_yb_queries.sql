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
-- YEAR and BIT columns (table yb of duckdb_offload_setup.sql): each query
-- must give the same result with use_secondary_engine OFF and FORCED.
-- YEAR: a column is sent zero-filled (0000), an expression as a number (0).
SELECT id, y, b, b1, b64 FROM yb ORDER BY id
SELECT y, COUNT(*) FROM yb GROUP BY y ORDER BY y
SELECT MIN(y), MAX(y), COUNT(y), COUNT(DISTINCT y) FROM yb
SELECT id FROM yb WHERE y = 0 ORDER BY id
SELECT id FROM yb WHERE y = 5 ORDER BY id
SELECT id FROM yb WHERE y = '5' ORDER BY id
SELECT id FROM yb WHERE y = '0' ORDER BY id
SELECT id FROM yb WHERE y = '2155' OR y = 1901 ORDER BY id
SELECT id FROM yb WHERE y BETWEEN 70 AND 99 ORDER BY id
SELECT id FROM yb WHERE y < 2000 AND y > 0 ORDER BY id
SELECT id FROM yb WHERE y IN (0, 1999, 2069) ORDER BY id
SELECT id, y + 0, y + 1, y * 2, 3 * y FROM yb ORDER BY id
SELECT DISTINCT y FROM yb ORDER BY y DESC
SELECT y, SUM(g) FROM yb GROUP BY y HAVING SUM(g) > 1 ORDER BY y
SELECT id, IFNULL(y, 1999), COALESCE(y, 0) FROM yb ORDER BY id
SELECT x.y FROM (SELECT y FROM yb WHERE id < 10) x ORDER BY x.y
SELECT a.id, c.id FROM yb a JOIN yb c ON a.y = c.y AND a.id < c.id WHERE a.id < 20 ORDER BY 1, 2
SELECT id, YEAR(d), YEAR(d) = y FROM yb ORDER BY id
-- BIT: sent as ceil(n/8) bytes; arithmetic and comparisons use the number
SELECT id, b + 0, b1 + 0, b64 + 0 FROM yb ORDER BY id
SELECT id FROM yb WHERE b = 10 ORDER BY id
SELECT id FROM yb WHERE b = b'1010' OR b = 0x3FF ORDER BY id
SELECT id FROM yb WHERE b64 = 18446744073709551615 ORDER BY id
SELECT id FROM yb WHERE b64 = 18446744073709551614 ORDER BY id
SELECT id FROM yb WHERE b64 > 9223372036854775807 ORDER BY id
SELECT MIN(b), MAX(b), MAX(b64), MIN(b1) FROM yb
SELECT b, COUNT(*) FROM yb GROUP BY b ORDER BY b LIMIT 10
SELECT SUM(b), AVG(b), SUM(b64) FROM yb
SELECT id FROM yb WHERE b > 500 AND b1 = 1 ORDER BY id
SELECT id FROM yb WHERE b1 ORDER BY id
SELECT b1, COUNT(*) FROM yb GROUP BY b1 ORDER BY b1
SELECT id, b FROM yb ORDER BY b DESC, id LIMIT 5
SELECT id, b64 FROM yb WHERE b64 IS NOT NULL ORDER BY b64 DESC, id LIMIT 5
-- unsigned + and * that cannot leave 0..2^64-1 (INT UNSIGNED column)
SELECT id, u + 1, u * 3, 2 + u FROM p6 WHERE id <= 5 ORDER BY id
