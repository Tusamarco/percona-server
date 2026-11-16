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
# Table p6 (20,000 rows: PAD SPACE strings, NULLs, temporal types; manual-test-plan.md section 22), loaded.
--disable_query_log
--disable_warnings
SET SESSION cte_max_recursion_depth = 30000;
CREATE TABLE p6 (
  id INT NOT NULL PRIMARY KEY,
  grp INT NOT NULL,
  amount DECIMAL(10,2),
  qty INT,
  price DOUBLE,
  u INT UNSIGNED,
  d DATE,
  dt DATETIME,
  ts TIMESTAMP NULL,
  b4 VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
  b3 VARCHAR(20) CHARACTER SET utf8mb3 COLLATE utf8mb3_bin,
  raw VARBINARY(20),
  s VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin,
  ci VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci
) ENGINE=InnoDB;
INSERT INTO p6
WITH RECURSIVE seq(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM seq WHERE i < 20000)
SELECT i, i % 7,
  IF(i % 13 = 0, NULL, ((i * 37) % 100000) / 100 - 300),
  IF(i % 11 = 0, NULL, (i * 7919) % 2001 - 1000),
  IF(i % 17 = 0, NULL, ((i * 31) % 1000) / 8),
  (i * 104729) % 4000000000,
  IF(i % 19 = 0, NULL, DATE '2020-01-01' + INTERVAL (i * 37) % 2000 DAY),
  IF(i % 23 = 0, NULL, TIMESTAMP '2020-01-01 00:00:00' + INTERVAL (i * 7919) % 100000000 SECOND),
  IF(i % 29 = 0, NULL, FROM_UNIXTIME(1614556800 + (i * 6007) % 50000000)),
  ELT(i % 12 + 1, 'a', 'a ', 'a  ', CONCAT('a', CHAR(9)), 'b', 'B', 'é', '', ' ', 'ab', 'a b', NULL),
  ELT(i % 10 + 1, 'x', 'x ', CONCAT('x', CHAR(9)), 'y', 'Y ', 'ü', '', 'xy', NULL, 'x  '),
  ELT(i % 5 + 1, 'k', 'k ', 'l', NULL, ''),
  ELT(i % 6 + 1, 'p', 'q', 'pq', 'P', NULL, ''),
  ELT(i % 4 + 1, 'm', 'M', 'n', NULL)
FROM seq;
ALTER TABLE p6 SECONDARY_ENGINE = duckdb;
ALTER TABLE p6 SECONDARY_LOAD;
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
