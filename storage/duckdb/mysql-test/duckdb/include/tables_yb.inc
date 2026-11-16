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
# Table yb for YEAR and BIT (section 36): loaded, then changed through the
# applier. Needs n.
--disable_query_log
--disable_warnings
CREATE TABLE yb (id INT PRIMARY KEY, y YEAR, b BIT(10), b1 BIT(1), b64 BIT(64), g INT, d DATE) SECONDARY_ENGINE = duckdb;
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
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
