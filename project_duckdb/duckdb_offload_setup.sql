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
-- DWL#6: drops and re-creates the offload test tables (manual-test-plan.md
-- sections 21 and 22) in the current database: n (helper), g (step 6a),
-- p6 (step 6b). Run with: mysql ... <database> < duckdb_offload_setup.sql
SET SESSION use_secondary_engine = ON;
SET SESSION cte_max_recursion_depth = 30000;
DROP TABLE IF EXISTS g, p6, n;

CREATE TABLE n (n INT PRIMARY KEY);
INSERT INTO n WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n < 999) SELECT n FROM s;

CREATE TABLE g (id INT PRIMARY KEY, grp INT NOT NULL, amount DECIMAL(10,2), d DATE, dt DATETIME,
  ts TIMESTAMP NULL, s VARCHAR(20) COLLATE utf8mb4_0900_bin, n INT NULL,
  ci VARCHAR(20) COLLATE utf8mb4_0900_ai_ci) SECONDARY_ENGINE = duckdb;
INSERT INTO g SELECT a.n*1000+b.n, (a.n*1000+b.n) % 7, ((a.n*1000+b.n) % 1000) / 7,
  DATE '2024-01-01' + INTERVAL ((a.n*1000+b.n) % 365) DAY,
  TIMESTAMP '2024-01-01 10:00:00' + INTERVAL ((a.n*1000+b.n) % 1000) MINUTE,
  TIMESTAMP '2024-06-01 12:00:00' + INTERVAL ((a.n*1000+b.n) % 100) SECOND,
  CONCAT('s', (a.n*1000+b.n) % 50), IF((a.n*1000+b.n) % 5 = 0, NULL, (a.n*1000+b.n) % 13),
  CONCAT('Ci', (a.n*1000+b.n) % 3)
  FROM n a, n b WHERE a.n < 200;
ALTER TABLE g SECONDARY_LOAD;

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

SELECT 'g' AS t, COUNT(*) AS row_count FROM g UNION ALL SELECT 'p6', COUNT(*) FROM p6;   -- 200000, 20000
SELECT table_name, create_options FROM information_schema.tables
 WHERE table_schema = DATABASE() AND table_name IN ('g', 'p6');                        -- SECONDARY_LOAD="1"
