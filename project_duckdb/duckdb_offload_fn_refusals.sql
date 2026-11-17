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
-- Functions refused after the review (EXPECT=refused): DuckDB's result
-- differs from MySQL's.
-- Unicode case mapping differs (DuckDB: upper('ß') = 'ẞ')
SELECT id, UPPER(s), LOWER(s) FROM fx ORDER BY id
-- MySQL: first byte; DuckDB: code point
SELECT id, ASCII(s) FROM fx ORDER BY id
-- MySQL: default_week_format; DuckDB: ISO weeks
SELECT id, EXTRACT(WEEK FROM dt) FROM fx ORDER BY id
-- bytes of latin1 differ from the UTF-8 copy
SELECT id, LENGTH(sl) FROM fx ORDER BY id
-- a search under a _ci collation
SELECT id, INSTR(sc, 'h') FROM fx ORDER BY id
-- MySQL removes the string 'xy', DuckDB any of x, y
SELECT id, TRIM(BOTH 'xy' FROM s) FROM fx ORDER BY id
-- overflow is an error in MySQL, inf in DuckDB
SELECT id, EXP(n), POW(n, 2) FROM fx ORDER BY id
-- DOUBLE formatting
SELECT id, CONCAT(f) FROM fx ORDER BY id
-- interval units with several parts
SELECT id, DATE_ADD(ts, INTERVAL '1:30' HOUR_MINUTE) FROM fx ORDER BY id
-- ordering by an expression of a latin1 binary collation needs weights
SELECT id FROM fx WHERE SUBSTR(sl, 1, 2) > 'h' ORDER BY id
-- grouping by a string expression under PAD SPACE: the value shown is
-- ambiguous ('a' or 'a ')
SELECT LEFT(s, 1) AS c, COUNT(*) FROM fx GROUP BY c ORDER BY c
