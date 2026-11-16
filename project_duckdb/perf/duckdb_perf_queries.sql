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
-- DWL#6 step 6d-3: the workload, one query per line, "-- name" before each.
-- Run with duckdb_perf_run.sh. Expected place: DuckDB unless marked.
-- q01 daily energy per site, one month
SELECT s.name, DATE(r.ts) AS day, SUM(r.power_kw) / 6 AS kwh FROM reading r JOIN turbine t ON t.id = r.turbine_id JOIN site s ON s.id = t.site_id WHERE r.ts >= '2025-06-01' AND r.ts < '2025-07-01' GROUP BY s.id, s.name, DATE(r.ts) ORDER BY s.id, day;
-- q02 top 10 turbines by average power, one quarter
SELECT r.turbine_id, AVG(r.power_kw) AS avg_kw, COUNT(*) AS n FROM reading r WHERE r.ts >= '2025-04-01' AND r.ts < '2025-07-01' GROUP BY r.turbine_id ORDER BY avg_kw DESC, r.turbine_id LIMIT 10;
-- q03 hourly profile over the year
SELECT HOUR(ts) AS h, AVG(wind_ms) AS wind, AVG(power_kw) AS power, MAX(temp_c) FROM reading GROUP BY HOUR(ts) ORDER BY h;
-- q04 capacity factor per model and month
SELECT t.model, MONTH(r.ts) AS m, SUM(r.power_kw) / (SUM(t.capacity_kw) ) AS cf FROM reading r JOIN turbine t ON t.id = r.turbine_id GROUP BY t.model, MONTH(r.ts) ORDER BY t.model, m;
-- q05 alarms per severity and month, offshore sites
SELECT a.severity, MONTH(a.ts) AS m, COUNT(*) FROM alarm a JOIN turbine t ON t.id = a.turbine_id JOIN site s ON s.id = t.site_id WHERE s.offshore = 1 GROUP BY a.severity, MONTH(a.ts) ORDER BY 1, 2;
-- q06 turbines without alarms of severity 4 in December
SELECT t.id FROM turbine t WHERE NOT EXISTS (SELECT 1 FROM alarm a WHERE a.turbine_id = t.id AND a.severity = 4 AND a.ts >= '2025-12-01') ORDER BY t.id;
-- q07 readings above the turbine's yearly average power, per turbine
SELECT r.turbine_id, COUNT(*) FROM reading r JOIN (SELECT turbine_id, AVG(power_kw) AS a FROM reading GROUP BY turbine_id) x ON x.turbine_id = r.turbine_id WHERE r.power_kw > x.a GROUP BY r.turbine_id ORDER BY 1;
-- q08 turbines reporting per day with faults
SELECT DATE(ts) AS day, COUNT(DISTINCT turbine_id), SUM(state = 2) FROM reading WHERE state > 0 GROUP BY DATE(ts) ORDER BY day;
-- q09 quarter over quarter energy per region (CTE)
WITH q AS (SELECT s.region_id, QUARTER(r.ts) AS qt, SUM(r.power_kw) AS e FROM reading r JOIN turbine t ON t.id = r.turbine_id JOIN site s ON s.id = t.site_id GROUP BY s.region_id, QUARTER(r.ts)) SELECT a.region_id, a.qt, a.e, a.e - b.e FROM q a LEFT JOIN q b ON b.region_id = a.region_id AND b.qt = a.qt - 1 ORDER BY 1, 2;
-- q10 two periods side by side (UNION ALL)
SELECT 'h1' AS p, turbine_id, MAX(power_kw) FROM reading WHERE ts < '2025-07-01' GROUP BY turbine_id UNION ALL SELECT 'h2', turbine_id, MAX(power_kw) FROM reading WHERE ts >= '2025-07-01' GROUP BY turbine_id ORDER BY 2, 1;
-- q11 whole table aggregate
SELECT COUNT(*), COUNT(power_kw), SUM(power_kw), AVG(wind_ms), MIN(temp_c), MAX(temp_c) FROM reading;
-- q12 maintenance cost per kind, full history (FULL scope: DUCKDB_HISTORY_READER)
SELECT /*+ SET_VAR(duckdb_history_scope = FULL) */ kind, YEAR(day), COUNT(*), SUM(cost) FROM maintenance GROUP BY kind, YEAR(day) ORDER BY 1, 2;
-- q13 [InnoDB] point lookup by primary key
SELECT * FROM reading WHERE id = 1234567;
-- q14 [InnoDB] last hour of one turbine (index range)
SELECT ts, power_kw FROM reading WHERE turbine_id = 42 AND ts >= '2025-12-31 23:00:00' ORDER BY ts;
-- q15 [InnoDB: _ci collation] energy per site name
SELECT s.name, SUM(r.power_kw) FROM reading r JOIN turbine t ON t.id = r.turbine_id JOIN site s ON s.id = t.site_id GROUP BY s.name ORDER BY s.name;
-- q16 [InnoDB: window function] rank of turbines per site
SELECT t.site_id, r.turbine_id, SUM(r.power_kw), RANK() OVER (PARTITION BY t.site_id ORDER BY SUM(r.power_kw) DESC) FROM reading r JOIN turbine t ON t.id = r.turbine_id GROUP BY t.site_id, r.turbine_id ORDER BY 1, 4;
