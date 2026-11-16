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
-- TIME queries that must be refused (EXPECT=refused).
-- MySQL sums TIME as the number HHMMSS
SELECT SUM(t), AVG(t) FROM tt
-- TIME in arithmetic is the number HHMMSS
SELECT id, t + 0 FROM tt ORDER BY id
-- TIME compared with a DATETIME (MySQL adds the current date)
SELECT id FROM tt WHERE t < dt ORDER BY id
-- functions not translated
SELECT id, HOUR(t), TIME_TO_SEC(t) FROM tt ORDER BY id
-- a TIME expression in the result
SELECT id, IFNULL(t, '00:00:00') FROM tt ORDER BY id
-- the column's lake value in other contexts (microseconds): refused
SELECT id FROM tt WHERE t LIKE '12%' ORDER BY id
SELECT id FROM tt WHERE IFNULL(t, '00:00:00') = '00:00:00' ORDER BY id
