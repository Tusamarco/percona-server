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

-- DuckDB applier test helpers: progress you can watch, and a clean stop.
-- Load once:  mysql ... < duckdb_test_helpers.sql
--
-- Run (session 1):   CALL windmills_small.duckdb_many_updates(250, 25);
-- Watch (session 2): CALL windmills_small.duckdb_progress();
-- Stop  (session 2): CALL windmills_small.duckdb_stop();
--   The procedure ends after the current UPDATE (each UPDATE is its own
--   committed transaction, so nothing is left half done).
-- KILL QUERY <id> also stops it after the current statement (state then
-- stays 'running'). Closing or interrupting the client does NOT stop it.

USE windmills_small;

-- Not loaded in DuckDB: the applier reads its transactions but ignores them.
CREATE TABLE IF NOT EXISTS duckdb_test_run (
  id             INT PRIMARY KEY,
  name           VARCHAR(64),
  target         INT,
  done           INT,
  stop_requested TINYINT NOT NULL DEFAULT 0,
  started_at     DATETIME(3),
  updated_at     DATETIME(3),
  finished_at    DATETIME(3),
  state          VARCHAR(16),
  conn_id        BIGINT
) ENGINE = InnoDB;

DROP PROCEDURE IF EXISTS duckdb_many_updates;
DROP PROCEDURE IF EXISTS duckdb_progress;
DROP PROCEDURE IF EXISTS duckdb_stop;

DELIMITER //

-- n single-row UPDATEs of mill1 (ids 1..n), one transaction each.
-- Progress is committed every report_every updates.
CREATE PROCEDURE duckdb_many_updates(IN n INT, IN report_every INT)
BEGIN
  DECLARE i INT DEFAULT 0;
  DECLARE stop_flag TINYINT DEFAULT 0;
  IF report_every IS NULL OR report_every < 1 THEN
    SET report_every = 100;
  END IF;
  REPLACE INTO duckdb_test_run
    VALUES (1, 'many_updates', n, 0, 0, NOW(3), NOW(3), NULL, 'running',
            CONNECTION_ID());
  WHILE i < n AND stop_flag = 0 DO
    SET i = i + 1;
    UPDATE mill1 SET active = 1 - active WHERE id = i;
    IF i % report_every = 0 OR i = n THEN
      UPDATE duckdb_test_run SET done = i, updated_at = NOW(3) WHERE id = 1;
      SELECT stop_requested INTO stop_flag FROM duckdb_test_run WHERE id = 1;
    END IF;
  END WHILE;
  UPDATE duckdb_test_run
     SET done = i, updated_at = NOW(3), finished_at = NOW(3),
         state = IF(stop_flag = 1, 'stopped', 'done')
   WHERE id = 1;
  SELECT done, target, state,
         TIMESTAMPDIFF(MICROSECOND, started_at, finished_at) / 1e6 AS seconds
    FROM duckdb_test_run WHERE id = 1;
END//

-- InnoDB side (the procedure) and DuckDB side (the applier) side by side.
CREATE PROCEDURE duckdb_progress()
BEGIN
  SELECT name, state, done, target,
         ROUND(100 * done / NULLIF(target, 0), 1) AS pct,
         ROUND(TIMESTAMPDIFF(MICROSECOND, started_at, updated_at) / 1e6, 1)
           AS seconds,
         ROUND(done / NULLIF(TIMESTAMPDIFF(MICROSECOND, started_at,
                                           updated_at) / 1e6, 0), 1)
           AS trx_per_sec,
         conn_id
    FROM duckdb_test_run WHERE id = 1;
  SELECT VARIABLE_NAME, VARIABLE_VALUE
    FROM performance_schema.global_status
   WHERE VARIABLE_NAME IN ('duckdb_apply_reader_state',
                           'duckdb_apply_transactions_read',
                           'duckdb_apply_batches',
                           'duckdb_apply_rows_written',
                           'duckdb_apply_large_transactions',
                           'duckdb_apply_pieces',
                           'duckdb_applied_file',
                           'duckdb_applied_position');
END//

-- Asks the running procedure to stop at its next progress report.
CREATE PROCEDURE duckdb_stop()
BEGIN
  UPDATE duckdb_test_run SET stop_requested = 1 WHERE id = 1;
  SELECT 'stop requested: the procedure ends at its next progress report'
    AS info;
END//

DELIMITER ;
