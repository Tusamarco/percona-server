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
-- LIKE on binary collations (table lt of duckdb_offload_setup.sql): a
-- character matches only itself, LIKE never pads; MySQL's escape character
-- (\\ by default) is passed to DuckDB. Same results with OFF and FORCED.
SELECT id FROM lt WHERE v LIKE 'abc' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'abc%' ORDER BY id
SELECT id FROM lt WHERE c LIKE 'abc' ORDER BY id
SELECT id FROM lt WHERE c LIKE 'abc %' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'ABC' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'caf_' ORDER BY id
SELECT id FROM lt WHERE v LIKE '_uro' ORDER BY id
SELECT id FROM lt WHERE v LIKE '%é' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'a\_b\%c' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'a!_b%' ESCAPE '!' ORDER BY id
SELECT id FROM lt WHERE v NOT LIKE '%b%' ORDER BY id
SELECT id FROM lt WHERE v LIKE '' ORDER BY id
SELECT id, v LIKE 'ab_' FROM lt ORDER BY id
SELECT id FROM lt WHERE u LIKE 'abc' ORDER BY id
SELECT COUNT(*) FROM lt WHERE c LIKE '%' AND v LIKE '%'
SELECT id FROM lt WHERE v LIKE 'a\\\\b' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'a\\%' ORDER BY id
SELECT id FROM lt WHERE u LIKE 'a\_b%' ORDER BY id
SELECT id FROM lt WHERE v LIKE 'a|_b%' ESCAPE '|' ORDER BY id
