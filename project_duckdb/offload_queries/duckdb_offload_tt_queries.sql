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
-- TIME columns (table tt of duckdb_offload_setup.sql): stored as signed
-- microseconds; each query must give the same result with OFF and FORCED.
SELECT id, t, t3, t6 FROM tt ORDER BY id
SELECT t, COUNT(*) FROM tt GROUP BY t ORDER BY t
SELECT MIN(t), MAX(t), MIN(t3), MAX(t3), MIN(t6), MAX(t6), COUNT(t), COUNT(DISTINCT t) FROM tt
SELECT id FROM tt WHERE t = '12:30:45' ORDER BY id
SELECT id FROM tt WHERE t = 123045 ORDER BY id
SELECT id FROM tt WHERE t = 12 ORDER BY id
SELECT id FROM tt WHERE t = '1 02:00:00' ORDER BY id
SELECT id FROM tt WHERE t > '24:00:00' ORDER BY id
SELECT id FROM tt WHERE t < 0 ORDER BY id
SELECT id FROM tt WHERE t BETWEEN '-01:00:00' AND '01:00:00' ORDER BY id
SELECT id FROM tt WHERE t IN ('00:00:00', '838:59:59', '-838:59:59') ORDER BY id
SELECT id FROM tt WHERE t NOT IN ('00:00:00', '26:00:00') ORDER BY id
SELECT id FROM tt WHERE t3 = '-01:02:03.004' ORDER BY id
SELECT id FROM tt WHERE t3 > '01:00:00.124' ORDER BY id
SELECT id FROM tt WHERE t6 >= '00:00:00.000001' ORDER BY id
SELECT id FROM tt WHERE t6 = 12.5 ORDER BY id
SELECT id FROM tt WHERE t = t3 OR t6 < t ORDER BY id
SELECT id, t FROM tt ORDER BY t DESC, id LIMIT 7
SELECT DISTINCT t3 FROM tt ORDER BY t3
SELECT g, MIN(t), MAX(t3) FROM tt GROUP BY g ORDER BY g
SELECT a.id, b.id FROM tt a JOIN tt b ON a.t = b.t AND a.id < b.id WHERE a.id < 25 ORDER BY 1, 2
SELECT x.t FROM (SELECT t FROM tt WHERE id < 12) x ORDER BY x.t
SELECT id FROM tt WHERE t IS NULL OR t3 IS NULL ORDER BY id
SELECT COUNT(*) FROM tt WHERE t <> '00:00:00'
