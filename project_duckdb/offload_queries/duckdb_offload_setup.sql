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
-- sections 21 to 23) in the current database: n (helper), g (step 6a),
-- p6 (step 6b), dim, hist, nl and the view v6 (step 6c). The purge of hist
-- needs TRANSACTION_GTID_TAG. Run with:
--   mysql ... <database> < duckdb_offload_setup.sql
SET SESSION use_secondary_engine = ON;
SET SESSION cte_max_recursion_depth = 30000;
DROP VIEW IF EXISTS v6;
DROP TABLE IF EXISTS g, p6, n, dim, hist, nl, dv, cw, cwd;
-- DROP TABLE keeps the DuckDB copy of a history table (stale): we discard it,
-- otherwise the new hist would reattach the old archived rows. No copy (first
-- run): the error is ignored.
DROP PROCEDURE IF EXISTS duckdb_test_discard_hist;
DELIMITER //
CREATE PROCEDURE duckdb_test_discard_hist()
BEGIN
  DECLARE CONTINUE HANDLER FOR SQLEXCEPTION BEGIN END;
  SET SESSION duckdb_unload_history_confirm = ON;
  SET GLOBAL duckdb_discard_stale_copy = CONCAT(DATABASE(), '.hist');
  SET SESSION duckdb_unload_history_confirm = OFF;
END//
DELIMITER ;
CALL duckdb_test_discard_hist();
DROP PROCEDURE duckdb_test_discard_hist;

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

-- step 6c: a dimension (grp 0-4 and 8, 9: p6 groups 5 and 6 have no match;
-- 8 and 9 match nothing), a history table with archived rows, a table
-- without a DuckDB copy, a view.
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
DELETE FROM hist WHERE id < 30;                          -- 30 rows archived in DuckDB
SET gtid_next = 'AUTOMATIC';
CREATE TABLE nl (grp INT PRIMARY KEY, x INT);
INSERT INTO nl VALUES (1, 10), (2, 20);
CREATE VIEW v6 AS SELECT id, grp, amount, qty FROM p6 WHERE qty > 0;

-- exact decimal division (DWL#6 after 6d-3): decimals of several scales,
-- negatives, zeros, NULLs, values on rounding boundaries
CREATE TABLE dv (id INT PRIMARY KEY, a DECIMAL(10,2), b DECIMAL(6,3), i INT, j BIGINT) SECONDARY_ENGINE = duckdb;
INSERT INTO dv SELECT n, IF(n % 11 = 0, NULL, ((n * 7919) % 200001 - 100000) / 100), IF(n % 13 = 0, 0, ((n * 104729) % 20001 - 10000) / 1000),
  IF(n % 17 = 0, 0, (n * 31) % 2001 - 1000), (n * 2654435761) % 1000000007 FROM n WHERE n < 1000;
INSERT INTO dv VALUES (2001, 1.00, 3.000, 3, 6), (2002, 2.00, 3.000, -3, -6), (2003, 0.05, 0.200, 8, 16), (2004, -0.05, 0.200, -8, 7),
  (2005, 1.25, 8.000, 2, 2), (2006, 99999999.99, 0.001, 1, 1), (2007, -12.35, 10.000, 7, 3);
ALTER TABLE dv SECONDARY_LOAD;

-- weight strings (DWL#6 step A): values that differ only in case, accent or
-- trailing spaces, in NO PAD (0900_ai_ci, 0900_as_cs), PAD SPACE
-- (general_ci) and latin1 collations; cwd joins on a _ci column
CREATE TABLE cw (id INT PRIMARY KEY, c VARCHAR(20) COLLATE utf8mb4_0900_ai_ci, g VARCHAR(20) COLLATE utf8mb4_general_ci,
  l VARCHAR(20) CHARACTER SET latin1 COLLATE latin1_swedish_ci, s VARCHAR(20) COLLATE utf8mb4_0900_as_cs, k INT) SECONDARY_ENGINE = duckdb;
INSERT INTO cw SELECT n + 1, IF(n % 23 = 0, NULL, ELT(n % 10 + 1, 'apple', 'Apple', 'ápple', 'APPLE', 'banana', 'Banana ', 'BANANA', 'cherry', 'Cherry', 'apple ')),
  ELT(n % 8 + 1, 'apple', 'APPLE  ', 'apple ', 'Banana', 'banana', 'b', 'B ', 'cherry'),
  ELT(n % 6 + 1, 'été', 'ETE', 'ete', 'Ä', 'ä', 'z'), ELT(n % 5 + 1, 'Apple', 'apple', 'APPLE', 'Äpple', 'apple'), n % 7
  FROM n WHERE n < 1000;
INSERT INTO cw SELECT n + 1001, ELT(n % 4 + 1, 'durian', 'Durian', 'DURIAN ', 'dürian'), ELT(n % 3 + 1, 'zz', 'ZZ ', 'z'),
  ELT(n % 2 + 1, 'o', 'Ö'), ELT(n % 3 + 1, 'Z', 'z', 'Ž'), n % 5 FROM n WHERE n < 1000;
ALTER TABLE cw SECONDARY_LOAD;
CREATE TABLE cwd (id INT PRIMARY KEY, c VARCHAR(20) COLLATE utf8mb4_0900_ai_ci NOT NULL, region INT) SECONDARY_ENGINE = duckdb;
INSERT INTO cwd VALUES (1, 'APPLE', 1), (2, 'banana', 1), (3, 'Cherry', 2), (4, 'durian', 2), (5, 'Elderberry', 3);
ALTER TABLE cwd SECONDARY_LOAD;

-- YEAR and BIT (2026-11-16): YEAR 0000 and the range ends, BIT(1), BIT(10),
-- BIT(64) with its largest values, NULLs; d holds dates whose YEAR() equals y
-- for some rows. Loaded first, changed after the load (applier).
DROP TABLE IF EXISTS yb;
CREATE TABLE yb (id INT PRIMARY KEY, y YEAR, b BIT(10), b1 BIT(1), b64 BIT(64), g INT, d DATE) SECONDARY_ENGINE = duckdb;
-- (numbers, not strings: ELT would give '0', which YEAR reads as 2000 and
-- BIT stores as the byte 0x30)
INSERT INTO yb SELECT n + 1,
  CASE WHEN n % 13 = 12 THEN NULL ELSE CASE n % 8 WHEN 0 THEN 0 WHEN 1 THEN 1901 WHEN 2 THEN 1970
    WHEN 3 THEN 1999 WHEN 4 THEN 2000 WHEN 5 THEN 2005 WHEN 6 THEN 2069 ELSE 2155 END END,
  IF(n % 11 = 10, NULL, (n * 37) % 1024), n % 2,
  CASE WHEN n % 9 = 8 THEN NULL ELSE CASE n % 5 WHEN 0 THEN 0 WHEN 1 THEN 1
    WHEN 2 THEN CAST(9223372036854775808 AS UNSIGNED) WHEN 3 THEN CAST(18446744073709551614 AS UNSIGNED)
    ELSE CAST(18446744073709551615 AS UNSIGNED) END END,
  n % 4, DATE_ADD('1999-06-01', INTERVAL (n % 3) * 366 DAY)
  FROM n WHERE n < 60;
ALTER TABLE yb SECONDARY_LOAD;
INSERT INTO yb VALUES (100, 0, 0, 0, 0, 0, '2000-01-01'),
  (101, 2155, b'1111111111', 1, CAST(18446744073709551615 AS UNSIGNED), 1, '2001-01-01');
UPDATE yb SET y = 1999, b = b'1010' WHERE id IN (2, 3);
DELETE FROM yb WHERE id = 4;

-- TIME (2026-11-16): negative values, more than 24 hours, the range ends,
-- fractions (TIME(3), TIME(6)), NULLs. Loaded first, changed after the load.
DROP TABLE IF EXISTS tt;
CREATE TABLE tt (id INT PRIMARY KEY, t TIME, t3 TIME(3), t6 TIME(6), g INT, dt DATETIME) SECONDARY_ENGINE = duckdb;
INSERT INTO tt SELECT n + 1,
  CASE WHEN n % 11 = 10 THEN NULL ELSE CASE n % 10 WHEN 0 THEN '00:00:00' WHEN 1 THEN '12:30:45' WHEN 2 THEN '-12:30:45'
    WHEN 3 THEN '838:59:59' WHEN 4 THEN '-838:59:59' WHEN 5 THEN '24:00:00' WHEN 6 THEN '26:00:00'
    WHEN 7 THEN '-00:00:01' WHEN 8 THEN '00:00:12' ELSE '100:00:00' END END,
  CASE WHEN n % 7 = 6 THEN NULL ELSE SEC_TO_TIME((n % 13 - 6) * 3600.125 + n * 0.001) END,
  CASE n % 5 WHEN 0 THEN '00:00:00.000001' WHEN 1 THEN '-00:00:00.000001' WHEN 2 THEN '23:59:59.999999'
    WHEN 3 THEN '-838:59:59.000000' ELSE '00:00:12.500000' END,
  n % 4, TIMESTAMP('2024-01-01 00:00:00') + INTERVAL n HOUR
  FROM n WHERE n < 60;
ALTER TABLE tt SECONDARY_LOAD;
INSERT INTO tt VALUES (100, '-00:00:00.5', '-00:00:00.500', '-00:00:00.5', 0, '2024-01-01 00:00:00'),
  (101, '838:59:59', '838:59:59.000', '838:59:59', 1, '2024-01-01 12:30:45');
UPDATE tt SET t = '01:02:03', t3 = '-01:02:03.004' WHERE id IN (2, 3);
DELETE FROM tt WHERE id = 4;

-- ENUM and SET (2026-11-16): element order that differs from the alphabet,
-- _ci, _bin and latin1 definitions, SET combinations and the empty set,
-- NULLs, and the invalid value '' (index 0, inserted in a non-strict mode).
DROP TABLE IF EXISTS et;
CREATE TABLE et (id INT PRIMARY KEY, e ENUM('small', 'medium', 'large', 'x-large'),
  es ENUM('b', 'a', 'c') COLLATE utf8mb4_bin, s SET('red', 'green', 'blue', 'alpha'),
  l ENUM('été', 'ete2', 'Zed') CHARACTER SET latin1, g INT) SECONDARY_ENGINE = duckdb;
INSERT INTO et SELECT n + 1, IF(n % 9 = 8, NULL, n % 4 + 1), IF(n % 7 = 6, NULL, n % 3 + 1),
  IF(n % 11 = 10, NULL, n % 16), IF(n % 5 = 4, NULL, n % 3 + 1), n % 3
  FROM n WHERE n < 60;
SET @old_mode = @@SESSION.sql_mode;
SET SESSION sql_mode = '';
INSERT INTO et VALUES (90, 'bogus', 'zz', 'red', 'Zed', 1);
SET SESSION sql_mode = @old_mode;
ALTER TABLE et SECONDARY_LOAD;
INSERT INTO et VALUES (100, 'MEDIUM', 'c', 'blue,red', 'été', 0), (101, 'x-large', 'a', '', 'ete2', 2);
UPDATE et SET e = 'large', s = 'alpha,green' WHERE id IN (2, 3);
DELETE FROM et WHERE id = 4;

-- LIKE on binary collations (2026-11-17): latin1_bin VARCHAR and CHAR (PAD
-- SPACE, stored as UTF-8), utf8mb4_bin; trailing spaces, case, accents, the
-- cp1252 euro sign, wildcards and backslashes in the data.
DROP TABLE IF EXISTS lt;
CREATE TABLE lt (id INT PRIMARY KEY, v VARCHAR(20) CHARACTER SET latin1 COLLATE latin1_bin,
  c CHAR(10) CHARACTER SET latin1 COLLATE latin1_bin, u VARCHAR(20) COLLATE utf8mb4_bin) SECONDARY_ENGINE = duckdb;
INSERT INTO lt VALUES (1, 'abc', 'abc', 'abc'), (2, 'abc ', 'abc  ', 'abc '), (3, 'ABC', 'ABC', 'ABC'),
  (4, 'café', 'café', 'café'), (5, 'cafe', 'cafe', 'cafe'), (6, '€uro', '€uro', '€uro'), (7, 'a_b%c', 'a_b%c', 'a_b%c'),
  (8, NULL, NULL, NULL), (9, '', '', ''), (10, 'ab', 'ab', 'ab'), (11, 'a\\b', 'a\\b', 'a\\b');
ALTER TABLE lt SECONDARY_LOAD;
INSERT INTO lt VALUES (12, 'abcd', 'abcd', 'abcd');

-- Functions reviewed against DuckDB (2026-11-17): strings with empty
-- values, NULLs, multibyte characters, spaces, latin1; numbers with
-- negatives, zero, NULL; dates at month ends, Feb 29, the range ends.
DROP TABLE IF EXISTS fx;
CREATE TABLE fx (id INT PRIMARY KEY, s VARCHAR(30) COLLATE utf8mb4_bin, sc VARCHAR(30) COLLATE utf8mb4_0900_ai_ci,
  sl VARCHAR(30) CHARACTER SET latin1 COLLATE latin1_bin, ch CHAR(10) COLLATE utf8mb4_bin,
  n INT, d DECIMAL(10,3), f DOUBLE, dt DATE, ts DATETIME, t6 DATETIME(6)) SECONDARY_ENGINE = duckdb;
INSERT INTO fx VALUES
  (1, 'hello', 'Hello', 'hello', 'hello', 5, 2.567, 2.5, '2024-01-31', '2024-01-31 10:20:30', '2024-01-31 10:20:30.123456'),
  (2, '', '', '', '', 0, 0.000, 0, '2024-02-29', '2024-02-29 00:00:00', '2024-02-29 23:59:59.999999'),
  (3, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL),
  (4, 'héllo wörld', 'HÉLLO', 'héllo', 'é', -3, -1.250, -4, '1000-01-01', '1000-01-01 00:00:01', '1000-01-01 00:00:00.000001'),
  (5, '  pad  ', ' Pad ', '  pad ', 'pad', 12, 123.456, 100, '9999-12-31', '9999-12-31 23:59:59', '9999-12-31 23:59:59.500000'),
  (6, 'a,b,c', 'a,B,c', 'a,b', 'abc', 1, -0.001, 0.001, '2023-12-31', '2023-12-31 12:00:00', '2023-12-31 12:00:00.000000'),
  (7, 'xxabcxx', 'XXabc', 'xxab', 'x', 7, 9.999, 1e10, '2024-03-31', '2024-03-31 06:07:08', '2024-03-31 06:07:08.700000'),
  (8, '13-555', '31-555', '13-555', '23', 2, 1.000, 2, '2024-12-01', '2024-12-01 01:02:03', '2024-12-01 01:02:03.000500');
ALTER TABLE fx SECONDARY_LOAD;
INSERT INTO fx VALUES (9, 'Ünïcödé', 'ünïcödé', 'Ünï', 'Ü', -12, -99.995, -0.5, '2020-02-29', '2020-02-29 12:34:56', '2020-02-29 12:34:56.654321');

SELECT 'g' AS t, COUNT(*) AS row_count FROM g UNION ALL SELECT 'p6', COUNT(*) FROM p6
  UNION ALL SELECT 'dim', COUNT(*) FROM dim UNION ALL SELECT 'hist', COUNT(*) FROM hist;  -- 200000, 20000, 7, 70
SELECT table_name, create_options FROM information_schema.tables
 WHERE table_schema = DATABASE() AND table_name IN ('g', 'p6', 'dim', 'hist');           -- SECONDARY_LOAD="1"
