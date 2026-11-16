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
# Tables cw and cwd for weight strings (section 33). Needs n.
--disable_query_log
--disable_warnings
SET SESSION cte_max_recursion_depth = 30000;
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
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
