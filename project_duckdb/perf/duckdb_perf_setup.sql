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
-- DWL#6 step 6d-3: a production-like workload (wind farm monitoring), in the
-- current database. Tables: region, site, turbine (dimensions, names in the
-- server's default collation), reading (one row per turbine every 10
-- minutes), alarm, maintenance (history table, purged with the tag).
-- Scale: @turbines turbines x @days days x 144 readings
-- (defaults 200 x 365 = 10.5 million readings).
--   (echo "SET @turbines = 200, @days = 365;"; cat duckdb_perf_setup.sql) | mysql ... <database>
-- Data are deterministic (no RAND), so InnoDB and DuckDB results compare.
-- The purge needs TRANSACTION_GTID_TAG.
SET SESSION use_secondary_engine = OFF;
SET @turbines = COALESCE(@turbines, 200), @days = COALESCE(@days, 365);
SET SESSION cte_max_recursion_depth = 100000;
DROP TABLE IF EXISTS reading, alarm, turbine, site, region, maintenance, perf_n;

CREATE TABLE perf_n (n INT PRIMARY KEY);
INSERT INTO perf_n WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n + 1 FROM s WHERE n < 9999) SELECT n FROM s;

CREATE TABLE region (id INT PRIMARY KEY, name VARCHAR(40) NOT NULL, country VARCHAR(40) NOT NULL);
INSERT INTO region VALUES (1, 'North Sea', 'NL'), (2, 'Baltic', 'DE'), (3, 'Iberia', 'ES'),
  (4, 'Alps', 'IT'), (5, 'Highlands', 'UK'), (6, 'Aegean', 'GR');
CREATE TABLE site (id INT PRIMARY KEY, region_id INT NOT NULL, name VARCHAR(60) NOT NULL,
  offshore TINYINT NOT NULL, opened DATE NOT NULL, KEY (region_id));
INSERT INTO site SELECT n + 1, n % 6 + 1, CONCAT('Site ', CHAR(65 + n % 26), n), n % 3 = 0,
  DATE '2010-01-01' + INTERVAL n * 97 DAY FROM perf_n WHERE n < 24;
CREATE TABLE turbine (id INT PRIMARY KEY, site_id INT NOT NULL, model VARCHAR(30) NOT NULL,
  capacity_kw DECIMAL(8,1) NOT NULL, commissioned DATE NOT NULL, status VARCHAR(12) NOT NULL,
  KEY (site_id));
INSERT INTO turbine SELECT n + 1, n % 24 + 1, ELT(n % 4 + 1, 'V90-2.0', 'SG-3.4', 'E-126', 'N149-5.7'),
  ELT(n % 4 + 1, 2000, 3400, 7580, 5700), DATE '2012-01-01' + INTERVAL n * 13 DAY,
  IF(n % 17 = 0, 'maintenance', 'active') FROM perf_n WHERE n < @turbines;

CREATE TABLE reading (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, turbine_id INT NOT NULL,
  ts DATETIME NOT NULL, power_kw DECIMAL(9,2), wind_ms DECIMAL(5,2) NOT NULL,
  temp_c DECIMAL(5,2) NOT NULL, rotor_rpm SMALLINT NOT NULL, state TINYINT NOT NULL,
  KEY (turbine_id, ts), KEY (ts));
CREATE TABLE alarm (id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY, turbine_id INT NOT NULL,
  ts DATETIME NOT NULL, code SMALLINT NOT NULL, severity TINYINT NOT NULL, KEY (turbine_id, ts));
CREATE TABLE maintenance (id INT PRIMARY KEY, turbine_id INT NOT NULL, day DATE NOT NULL,
  kind VARCHAR(20) NOT NULL, hours DECIMAL(5,1) NOT NULL, cost DECIMAL(10,2) NOT NULL,
  KEY (turbine_id)) SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';

DROP PROCEDURE IF EXISTS perf_fill;
DELIMITER //
CREATE PROCEDURE perf_fill()
BEGIN
  DECLARE d INT DEFAULT 0;
  -- one day per transaction: 144 readings per turbine
  WHILE d < @days DO
    INSERT INTO reading (turbine_id, ts, power_kw, wind_ms, temp_c, rotor_rpm, state)
    SELECT t.id, TIMESTAMP '2025-01-01 00:00:00' + INTERVAL d DAY + INTERVAL s.n * 10 MINUTE,
      IF((t.id * 31 + d * 7 + s.n) % 211 = 0, NULL,
         ROUND(t.capacity_kw * ((t.id * 17 + d * 29 + s.n * 3) % 100) / 100, 2)),
      ((t.id * 13 + d * 11 + s.n * 7) % 2500) / 100,
      ROUND(10 + 12 * SIN((d - 100) / 58.1) + ((t.id + s.n) % 50) / 10 - 2.5, 2),
      (t.id * 7 + d + s.n * 5) % 18,
      IF((t.id * 3 + d * 5 + s.n) % 97 = 0, 2, IF((t.id + d + s.n) % 41 = 0, 1, 0))
    FROM turbine t JOIN perf_n s ON s.n < 144;
    SET d = d + 1;
  END WHILE;
END//
DELIMITER ;
CALL perf_fill();
DROP PROCEDURE perf_fill;
INSERT INTO alarm (turbine_id, ts, code, severity)
  SELECT turbine_id, ts, 100 + id % 37, 1 + id % 4 FROM reading WHERE id % 197 = 0;
INSERT INTO maintenance SELECT n + 1, n % @turbines + 1,
  DATE '2023-01-01' + INTERVAL (n * 7) % 1095 DAY, ELT(n % 4 + 1, 'inspection', 'blade', 'gearbox', 'software'),
  1 + n % 23, 150 + (n * 37) % 9000 FROM perf_n WHERE n < @turbines * 25;

ALTER TABLE region SECONDARY_ENGINE = duckdb;
ALTER TABLE site SECONDARY_ENGINE = duckdb;
ALTER TABLE turbine SECONDARY_ENGINE = duckdb;
ALTER TABLE reading SECONDARY_ENGINE = duckdb;
ALTER TABLE alarm SECONDARY_ENGINE = duckdb;
ALTER TABLE region SECONDARY_LOAD;
ALTER TABLE site SECONDARY_LOAD;
ALTER TABLE turbine SECONDARY_LOAD;
ALTER TABLE alarm SECONDARY_LOAD;
ALTER TABLE maintenance SECONDARY_LOAD;
ALTER TABLE reading SECONDARY_LOAD;
-- retention: maintenance older than 2025 leaves InnoDB, stays in DuckDB
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM maintenance WHERE day < '2025-01-01';
SET gtid_next = 'AUTOMATIC';
ANALYZE TABLE region, site, turbine, reading, alarm, maintenance;

SELECT 'reading' AS t, COUNT(*) FROM reading UNION ALL SELECT 'alarm', COUNT(*) FROM alarm
  UNION ALL SELECT 'maintenance (live)', COUNT(*) FROM maintenance;
