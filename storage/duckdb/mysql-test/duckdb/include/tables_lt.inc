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
# Table lt for LIKE on binary collations (section 40).
--disable_query_log
--disable_warnings
CREATE TABLE lt (id INT PRIMARY KEY, v VARCHAR(20) CHARACTER SET latin1 COLLATE latin1_bin,
  c CHAR(10) CHARACTER SET latin1 COLLATE latin1_bin, u VARCHAR(20) COLLATE utf8mb4_bin) SECONDARY_ENGINE = duckdb;
INSERT INTO lt VALUES (1, 'abc', 'abc', 'abc'), (2, 'abc ', 'abc  ', 'abc '), (3, 'ABC', 'ABC', 'ABC'),
  (4, 'café', 'café', 'café'), (5, 'cafe', 'cafe', 'cafe'), (6, '€uro', '€uro', '€uro'), (7, 'a_b%c', 'a_b%c', 'a_b%c'),
  (8, NULL, NULL, NULL), (9, '', '', ''), (10, 'ab', 'ab', 'ab'), (11, 'a\\b', 'a\\b', 'a\\b');
ALTER TABLE lt SECONDARY_LOAD;
INSERT INTO lt VALUES (12, 'abcd', 'abcd', 'abcd');
--enable_warnings
--enable_query_log
--source ../include/wait_applier.inc
