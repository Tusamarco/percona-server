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
# Table tt for TIME (section 37): loaded, then changed through the applier. Needs n.
--disable_query_log
--disable_warnings
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
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
