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
# Table et for ENUM and SET (section 38): loaded, then changed through the applier. Needs n.
--disable_query_log
--disable_warnings
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
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
