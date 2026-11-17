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
# Table fx for the functions reviewed against DuckDB (section 42).
--disable_query_log
--disable_warnings
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
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
