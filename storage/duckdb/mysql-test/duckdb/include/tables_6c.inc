# Copyright (c) 2026-present, Marco Tusa. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
#
# Tables dim, hist (history table, 30 rows purged), nl (no secondary engine) and view v6 (section 23). Needs n and p6.
--disable_query_log
--disable_warnings
SET SESSION cte_max_recursion_depth = 30000;
CREATE TABLE dim (grp INT PRIMARY KEY, name VARCHAR(20) COLLATE utf8mb4_0900_bin,
  label VARCHAR(20) COLLATE utf8mb4_bin, region INT NULL,
  ci VARCHAR(20) COLLATE utf8mb4_0900_ai_ci) SECONDARY_ENGINE = duckdb;
INSERT INTO dim VALUES (0, 'zero', 'z ', 1, 'Z'), (1, 'one', 'o', 1, 'O'), (2, 'two', 't', 2, 't'),
  (3, 'three', 't ', NULL, 'T'), (4, 'four', 'f', 2, 'f'), (8, 'eight', 'e', 3, 'e'), (9, 'nine', 'n', NULL, 'N');
ALTER TABLE dim SECONDARY_LOAD;
CREATE TABLE hist (id INT PRIMARY KEY, grp INT NOT NULL, v INT) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
INSERT INTO hist SELECT n, n % 10, n FROM n WHERE n < 100;
ALTER TABLE hist SECONDARY_LOAD;
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM hist WHERE id < 30;
SET gtid_next = 'AUTOMATIC';
CREATE TABLE nl (grp INT PRIMARY KEY, x INT);
INSERT INTO nl VALUES (1, 10), (2, 20);
CREATE VIEW v6 AS SELECT id, grp, amount, qty FROM p6 WHERE qty > 0;
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
