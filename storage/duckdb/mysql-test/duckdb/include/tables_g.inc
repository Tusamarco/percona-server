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
# Table g (200,000 rows, every result type; manual-test-plan.md section 21)
# and the helper n (0..999), loaded into DuckDB.
--disable_query_log
--disable_warnings
SET SESSION cte_max_recursion_depth = 30000;
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
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
