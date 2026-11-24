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
-- ENUM and SET queries that must be refused (EXPECT=refused).
-- MySQL sums the index
SELECT SUM(e) FROM et
-- functions not translated
SELECT id, FIND_IN_SET('red', s) FROM et ORDER BY id
SELECT id, CONCAT(e, '!') FROM et ORDER BY id
-- ORDER BY an ENUM of a derived table (no index there; MySQL sorts on it)
SELECT x.e FROM (SELECT e FROM et WHERE id < 10 LIMIT 100) x ORDER BY x.e
-- existing collation rules, as for VARCHAR: LIKE on a _ci column, DISTINCT
-- on a PAD SPACE column
SELECT id FROM et WHERE s LIKE '%green%' ORDER BY id
SELECT DISTINCT es FROM et ORDER BY es
