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
-- YEAR and BIT queries that must be refused (EXPECT=refused): FORCED fails
-- with the reason, ON gives the InnoDB rows.
-- a YEAR column of a set operation (MySQL sends it zero-filled through its
-- temporary table): unsupported_result_type
SELECT y FROM yb WHERE id = 1 UNION ALL SELECT y FROM yb WHERE id = 2
-- unsigned subtraction: MySQL raises error 1690 below zero (y = 0000)
SELECT id, y - 1900 FROM yb WHERE y > 0 ORDER BY id
-- unsigned addition that can pass 2^64 - 1 (error 1690 in MySQL)
SELECT id, b64 + 1 FROM yb WHERE b64 < 5 ORDER BY id
-- unsigned operand in DECIMAL arithmetic
SELECT id, b + 0.5 FROM yb ORDER BY id
-- functions not translated
SELECT id, HEX(b), BIN(b) FROM yb ORDER BY id
-- in string contexts MySQL shows YEAR as '0000' and BIT as bytes: refused
SELECT id FROM yb WHERE y LIKE '00%' ORDER BY id
SELECT id, IF(y > 2000, y, 'old') FROM yb ORDER BY id
