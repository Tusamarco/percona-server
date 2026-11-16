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
# Table dv for exact division (section 30). Needs n.
--disable_query_log
--disable_warnings
SET SESSION cte_max_recursion_depth = 30000;
CREATE TABLE dv (id INT PRIMARY KEY, a DECIMAL(10,2), b DECIMAL(6,3), i INT, j BIGINT) SECONDARY_ENGINE = duckdb;
INSERT INTO dv SELECT n, IF(n % 11 = 0, NULL, ((n * 7919) % 200001 - 100000) / 100), IF(n % 13 = 0, 0, ((n * 104729) % 20001 - 10000) / 1000),
  IF(n % 17 = 0, 0, (n * 31) % 2001 - 1000), (n * 2654435761) % 1000000007 FROM n WHERE n < 1000;
INSERT INTO dv VALUES (2001, 1.00, 3.000, 3, 6), (2002, 2.00, 3.000, -3, -6), (2003, 0.05, 0.200, 8, 16), (2004, -0.05, 0.200, -8, 7),
  (2005, 1.25, 8.000, 2, 2), (2006, 99999999.99, 0.001, 1, 1), (2007, -12.35, 10.000, 7, 3);
ALTER TABLE dv SECONDARY_LOAD;
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
