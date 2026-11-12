# DuckDB secondary engine: manual test plan

Manual checks for what is implemented so far: DWL#1 (build), DWL#2 (plugin,
lake, variables, executor pool) and DWL#3 step 3a (SECONDARY_LOAD and
SECONDARY_UNLOAD). They become the MTR suite `duckdb` in step 3b. Update this
file whenever a step adds behavior.

Reference instance: `/opt/mysql_instances/master96`, schema `windmills_small`,
test tables `mill1`..`mill10` (see DWL-03, Reference test tables).

Test tables with `retain_history` created by this plan use the prefix
`tmp_hist_`. Dropping them keeps their DuckDB copy in the lake, marked stale
(by design: history is never deleted implicitly). Until DWL#3 step 3b adds
the confirmed discard (`duckdb_unload_history_confirm`), they stay in the
lake; re-creating one with the same name and loading it is refused. Use a new
name, or start from an empty lake (move `duckdb_data` aside while mysqld is
stopped). On the reference instance `t_h` (created by an earlier version of
this plan) is such a leftover.

## 0. Setup

`my.cnf` (no `thread_stack` override is needed: DuckDB runs on the executor
pool):

```ini
[mysqld]
plugin-load-add                = ha_duckdb.so
loose-duckdb_memory_limit      = 1G
loose-duckdb_threads           = 2
# optional, these are the defaults:
# loose-duckdb_executor_threads    = 4
# loose-duckdb_executor_stack_size = 8M

log_bin                  = binlog
binlog_format            = ROW
binlog_row_image         = FULL
binlog_row_metadata      = FULL
gtid_mode                = ON
enforce_gtid_consistency = ON
```

Shell variables used below:

```bash
DATADIR=/opt/mysql_instances/master96/data
ERRLOG=$(mysql -NBe "SELECT @@log_error")          # adjust if relative
MYSQL="mysql -S /opt/mysql_instances/master96/mysql.sock -u dba -p windmills_small"
```

The plugin writes its progress messages (`duckdb_apply_debug_log` lines,
"binlog reader started", "applied in pieces") at information level. The
default `log_error_verbosity=2` drops them, so for these tests:

```sql
SET GLOBAL log_error_verbosity = 3;
```

Test helpers for sections 14 and 16 (load once):

```bash
$MYSQL < project_duckdb/duckdb_test_helpers.sql
```

- `CALL duckdb_many_updates(n, report_every)`: n single-row UPDATEs of `mill1`
  (ids 1..n), one transaction each; the progress is committed every
  `report_every` updates in `duckdb_test_run` (not loaded in DuckDB, ignored by
  the applier).
- `CALL duckdb_progress()` (another session): the procedure's progress and
  transactions per second, next to the applier counters.
- `CALL duckdb_stop()` (another session): the procedure ends at its next
  progress report, state `stopped`. `KILL QUERY <conn_id>` also stops it after
  the current statement. Closing or interrupting the client does not: the
  server notices only when it writes to the connection, and the loop never
  does, so it runs to the end.

Commit speed: on the reference instance (HDD, btrfs, about 32 ms per fsync)
each commit takes about 200 ms with `sync_binlog=1` and
`innodb_flush_log_at_trx_commit=1`. For functional tests you may set both to
0 and 2: `after_sync` is still called, so the applier is notified. Set them
back to 1 for crash and restart tests (section 14 last case, section 16 cases
6 and 7).

## 1. Build (DWL#1)

```bash
cd /opt/mysql_source/percona-server-9.7-build
make -j8 duckdb_build_smoke && ctest -R duckdb_build_smoke --output-on-failure
# expect: DuckDB v2.0.0: live=50000 full=100000 archived=50000, OK, Test Passed
make -j8 duckdb            # plugin_output_directory/ha_duckdb.so
make -j8                   # a second run must not rebuild DuckDB
nm -D --defined-only plugin_output_directory/ha_duckdb.so | grep -c _service   # > 0
nm -D --defined-only plugin_output_directory/ha_duckdb.so | grep -ci duckdb_   # 0
```

## 2. Startup and lake (DWL#2)

```bash
grep -i duckdb $ERRLOG | tail -5
# expect: DuckDB lake: catalog=.../duckdb_data/catalog.ducklake, ... executor threads=4
ls -la $DATADIR/duckdb_data/        # catalog.ducklake  data/  tmp/   (mode drwxr-x---)
```

```sql
SHOW ENGINES;                               -- duckdb listed
SHOW VARIABLES LIKE 'duckdb%';              -- 7 variables
SHOW STATUS    LIKE 'duckdb%';              -- duckdb_enabled=ON, duckdb_lake_path, 3 executor counters
SELECT @@thread_stack;                      -- server default
```

## 3. Executor pool (DWL#2)

```sql
SELECT NAME, THREAD_OS_ID FROM performance_schema.threads WHERE NAME LIKE 'thread/duckdb/%';
-- expect 4 rows thread/duckdb/executor

SET GLOBAL duckdb_executor_threads = 6;     -- 6 rows
SELECT COUNT(*) FROM performance_schema.threads WHERE NAME LIKE 'thread/duckdb/%';
SET GLOBAL duckdb_executor_threads = 2;     -- 2 rows (idle threads exit)
SELECT COUNT(*) FROM performance_schema.threads WHERE NAME LIKE 'thread/duckdb/%';
SET GLOBAL duckdb_executor_threads = 0;     -- ERROR wrong value
SET GLOBAL duckdb_executor_threads = 999;   -- ERROR wrong value (above cores)
SET GLOBAL duckdb_executor_stack_size = 16777216;   -- ERROR read-only
SET GLOBAL duckdb_executor_threads = 4;
```

## 4. Runtime variables (DWL#2)

```sql
SET GLOBAL duckdb_memory_limit = 2147483648;        -- ok (2 GiB, applied to DuckDB)
SET GLOBAL duckdb_memory_limit = 134217728;         -- ERROR (below 256 MiB)
SET GLOBAL duckdb_threads = 4;                      -- ok
SET GLOBAL duckdb_threads = 0;                      -- ERROR
SET GLOBAL duckdb_threads = 999;                    -- ERROR (above cores)
SET GLOBAL duckdb_data_path = '/tmp/x';             -- ERROR read-only
SET GLOBAL duckdb_encryption = ON;                  -- ERROR read-only
SET GLOBAL duckdb_memory_limit = 1073741824;
SET GLOBAL duckdb_threads = 2;
```

## 5. Classic table `mill1` (DWL#3)

```sql
ALTER TABLE mill1 SECONDARY_ENGINE = duckdb;     -- if not already set
SHOW CREATE TABLE mill1\G                        -- SECONDARY_ENGINE=duckdb

ALTER TABLE mill1 SECONDARY_UNLOAD;              -- only if loaded earlier
SHOW STATUS LIKE 'duckdb_executor_tasks';        -- note the value
ALTER TABLE mill1 SECONDARY_LOAD;                -- ok
SHOW STATUS LIKE 'duckdb_executor_tasks';        -- grew by about rows/2048 + a few
ALTER TABLE mill1 SECONDARY_LOAD;                -- ERROR: already in the lake, run SECONDARY_UNLOAD first
ALTER TABLE mill1 SECONDARY_UNLOAD;              -- ok
ALTER TABLE mill1 SECONDARY_UNLOAD;              -- ERROR: not loaded in secondary engine
ALTER TABLE mill1 SECONDARY_LOAD;                -- ok again
SELECT COUNT(*) FROM mill1;                      -- still InnoDB (no offload yet)
```

```bash
grep -E "loaded|unloaded" $ERRLOG | tail -5
# "loaded windmills_small.mill1 into the lake: N rows in X s"
find $DATADIR/duckdb_data/data/windmills_small/mill1 -name '*.parquet' | head
```

## 6. History table `mill6` (DWL#3)

```sql
ALTER TABLE mill6 SECONDARY_ENGINE = duckdb, SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE mill6 SECONDARY_LOAD;                -- ok, log says "(retain_history)"
ALTER TABLE mill6 SECONDARY_LOAD;                -- ERROR: retain_history, loaded only once
ALTER TABLE mill6 SECONDARY_UNLOAD;              -- ERROR: not supported yet (archived rows)
RENAME TABLE mill6 TO mill6x;                    -- ERROR: cannot perform DDLs on a table that has a secondary engine
ALTER TABLE mill6 ADD COLUMN x INT;              -- same ERROR

-- DROP keeps the history: we test it on a copy, not on mill6
CREATE TABLE tmp_hist_drop LIKE mill6;               -- copies SECONDARY_ENGINE and attribute
INSERT INTO tmp_hist_drop SELECT * FROM mill6 LIMIT 1000;
ALTER TABLE tmp_hist_drop SECONDARY_LOAD;
DROP TABLE tmp_hist_drop;                            -- ok, WARNING: DuckDB copy kept and marked stale
SHOW WARNINGS;
CREATE TABLE tmp_hist_drop LIKE mill6;
ALTER TABLE tmp_hist_drop SECONDARY_LOAD;            -- ERROR: already in the lake (reattach comes in 3b)
DROP TABLE tmp_hist_drop;
```

## 7. Preconditions and attributes (DWL#3)

```sql
CREATE TABLE t_nopk (a INT) SECONDARY_ENGINE = duckdb;
ALTER TABLE t_nopk SECONDARY_LOAD;               -- ERROR: requires a primary key

CREATE TABLE t_parent (id INT PRIMARY KEY);
CREATE TABLE t_fk (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES t_parent(id)) SECONDARY_ENGINE = duckdb;
ALTER TABLE t_fk SECONDARY_LOAD;                 -- ERROR: foreign keys not supported

CREATE TABLE t_enum (id INT PRIMARY KEY, e ENUM('a','b')) SECONDARY_ENGINE = duckdb;
ALTER TABLE t_enum SECONDARY_LOAD;               -- ERROR: column e: type not supported

CREATE TABLE t_attr (id INT PRIMARY KEY) SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_hist": true}';
ALTER TABLE t_attr SECONDARY_LOAD;               -- ERROR: unknown SECONDARY_ENGINE_ATTRIBUTE key 'retain_hist'

SET GLOBAL binlog_row_metadata = MINIMAL;
ALTER TABLE mill2 SECONDARY_ENGINE = duckdb;
ALTER TABLE mill2 SECONDARY_LOAD;                -- ERROR: requires binlog_row_metadata=FULL
SET GLOBAL binlog_row_metadata = FULL;
ALTER TABLE mill2 SECONDARY_LOAD;                -- ok

DROP TABLE t_nopk, t_fk, t_parent, t_enum, t_attr;
```

## 8. Types and character sets (DWL#3)

```sql
CREATE TABLE t_types (
  id INT PRIMARY KEY, ti TINYINT, us SMALLINT UNSIGNED, bi BIGINT UNSIGNED,
  d DECIMAL(12,3), f FLOAT, db DOUBLE,
  c CHAR(5) CHARACTER SET latin1, v VARCHAR(20) CHARACTER SET utf8mb4, t TEXT,
  vb VARBINARY(10), dt DATE, dtt DATETIME(6), ts TIMESTAMP(3) NULL
) SECONDARY_ENGINE = duckdb;
INSERT INTO t_types VALUES
 (1, -5, 65535, 18446744073709551615, -123.456, 1.5, 2.25, 'àé  ', 'ünïcode', 'text', 0x00FF,
  '2024-02-29', '2024-02-29 12:34:56.123456', '2024-02-29 12:34:56.789'),
 (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
ALTER TABLE t_types SECONDARY_LOAD;              -- ok (latin1 converted, binary as BLOB)

SET SESSION sql_mode = '';
CREATE TABLE t_zero (id INT PRIMARY KEY, d DATE) SECONDARY_ENGINE = duckdb;
INSERT INTO t_zero VALUES (1, '0000-00-00');
ALTER TABLE t_zero SECONDARY_LOAD;               -- ERROR: column d: zero dates cannot be loaded
SET SESSION sql_mode = DEFAULT;
DROP TABLE t_zero;
ALTER TABLE t_types SECONDARY_UNLOAD;
DROP TABLE t_types;
```

## 9. KILL during a load (DWL#2 executor, DWL#3)

Use the table with about one million rows.

```sql
-- session 1
ALTER TABLE mill1 SECONDARY_UNLOAD;
ALTER TABLE mill1 SECONDARY_LOAD;
-- session 2, while session 1 runs
SHOW PROCESSLIST;                    -- find the id
KILL QUERY <id>;
-- session 1: ERROR ... failed: interrupted
-- then:
ALTER TABLE mill1 SECONDARY_LOAD;    -- ok (nothing was left behind)
```

## 10. duckdb_enable (DWL#2)

```sql
SET GLOBAL duckdb_enable = OFF;
SHOW STATUS LIKE 'duckdb%';                      -- duckdb_enabled=OFF, executor counters 0
SELECT COUNT(*) FROM performance_schema.threads WHERE NAME LIKE 'thread/duckdb/%';   -- 0
ALTER TABLE mill3 SECONDARY_ENGINE = duckdb;     -- accepted (metadata)
ALTER TABLE mill3 SECONDARY_LOAD;                -- WARNING Unknown storage engine 'duckdb', no change
SHOW WARNINGS;
ALTER TABLE mill1 SECONDARY_UNLOAD;              -- no error, lake untouched
SET GLOBAL duckdb_enable = ON;                   -- log: "DuckDB lake: ... created=no"
ALTER TABLE mill1 SECONDARY_LOAD;                -- ERROR already in the lake (not removed while OFF)
```

`OFF` waits for running operations: start the one-million-row
`SECONDARY_LOAD` in session 1 and run `SET GLOBAL duckdb_enable = OFF` in
session 2; session 2 returns only after the load ends.

## 11. Startup cases (DWL#2; each needs a restart; check the error log)

| Test | Change before the restart | Expected |
|---|---|---|
| Missing memory limit | remove `loose-duckdb_memory_limit` | `duckdb_memory_limit is not set ... Plugin not loaded`; server up; `SHOW ENGINES` without duckdb |
| Missing threads | remove `loose-duckdb_threads` | same pattern for `duckdb_threads` |
| Memory too small | `loose-duckdb_memory_limit=128M` | `must be at least 268435456 bytes`, not loaded |
| Encryption | `loose-duckdb_encryption=ON` | `not supported in this version`, not loaded |
| Executor above cores | `loose-duckdb_executor_threads=999` | warning `... using <cores>`, plugin loaded |
| Stack too small | `loose-duckdb_executor_stack_size=1M` | the server adjusts it to the 2 MiB minimum (option warning) |
| Plugin absent, loose options | remove `plugin-load-add` only | server starts normally |
| Custom path | `loose-duckdb_data_path=/opt/duckdb_tmp/lake_test` | lake created there, `duckdb_lake_path` shows it |
| Relative path | `loose-duckdb_data_path=lake_rel` | `$DATADIR/lake_rel/` created |
| Path = datadir | `loose-duckdb_data_path=<datadir>` | `must not be the datadir itself`, not loaded |
| Inside a schema dir | `loose-duckdb_data_path=<datadir>/windmills_small/x` | `inside a directory of the datadir`, not loaded |
| World-writable | `chmod o+w $DATADIR/duckdb_data`, restart | `is world-writable`, not loaded (then `chmod o-w`) |
| Lost catalog | stop server, `mv catalog.ducklake catalog.bak`, start | `catalog ... is missing but data/ contains files`, not loaded (then move it back) |
| Restart keeps tables | normal restart | `ALTER TABLE mill1 SECONDARY_LOAD` gives "already in the lake": the registry survived |
| Startup abort after the binlog opens | a socket path longer than 107 characters (or a port in use) | `Aborting`, then `Shutdown complete`; no `got signal` (the reader stops at `BFN_BINLOG_END`) |
| Pending batch at shutdown | `duckdb_apply_batch_max_delay_ms=60000`, one INSERT on a loaded table, wait for `transactions_read` to move, `mysqladmin shutdown` | after the start: `duckdb_applied_position` is after the INSERT and `duckdb_apply_transactions_read` stays 0 (committed during the shutdown, not read again) |

## 12. Binlog reader, step 4a (DWL#4)

Step 4a only reads: nothing is written to DuckDB yet. A statement must really
change rows (`Changed: N` > 0), otherwise InnoDB writes no row events.

After a restart, without touching `duckdb_enable`, the reader must reach
`idle` on its own (it waits for the server to open the binary log; for a moment
the state is `waiting for the binary log`).

```sql
SHOW STATUS LIKE 'duckdb_apply%';
-- duckdb_apply_reader_state = idle, reader_file/position = end of the binlog
SELECT NAME FROM performance_schema.threads WHERE NAME LIKE 'thread/duckdb/%';
-- executor threads + thread/duckdb/binlog_reader

UPDATE mill1 SET active = 1 - active WHERE id <= 10;   -- mill1 loaded
SHOW STATUS LIKE 'duckdb_apply%';
-- transactions_read +1, row_events_read +1, last_gtid = that GTID

UPDATE mill4 SET active = 1 - active WHERE id <= 10;   -- mill4 not loaded
SHOW STATUS LIKE 'duckdb_apply_transactions_read';     -- unchanged

BEGIN; UPDATE mill1 SET active = 1 - active WHERE id = 1; ROLLBACK;
SHOW STATUS LIKE 'duckdb_apply_transactions_read';     -- unchanged

SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM mill1 WHERE id = 1;
SET gtid_next = 'AUTOMATIC';
SHOW STATUS LIKE 'duckdb_apply_last_gtid';             -- <uuid>:duckdb_avoid_purge:<n>

FLUSH BINARY LOGS;
UPDATE mill1 SET active = 1 - active WHERE id = 2;
SHOW STATUS LIKE 'duckdb_apply_reader_file';           -- the new binlog file

SET GLOBAL duckdb_enable = OFF;
SHOW STATUS LIKE 'duckdb_apply_reader_state';          -- stopped
SET GLOBAL duckdb_enable = ON;
SHOW STATUS LIKE 'duckdb_apply_reader_state';          -- idle
```

Load rules added in 4a:

```sql
SET GLOBAL sync_binlog = 0;
CREATE TABLE tmp_hist_sync (id INT PRIMARY KEY) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE tmp_hist_sync SECONDARY_LOAD;      -- ERROR: retain_history requires sync_binlog=1
ALTER TABLE mill5 SECONDARY_ENGINE = duckdb;
ALTER TABLE mill5 SECONDARY_LOAD;    -- ok with WARNING about sync_binlog
SET GLOBAL sync_binlog = 1;
ALTER TABLE tmp_hist_sync SECONDARY_LOAD;      -- ok

SET GLOBAL binlog_transaction_compression = ON;
ALTER TABLE mill3 SECONDARY_ENGINE = duckdb;
ALTER TABLE mill3 SECONDARY_LOAD;    -- ERROR: requires binlog_transaction_compression=OFF
SET GLOBAL binlog_transaction_compression = OFF;
```

A reader error (for example compression turned on after a load) shows in
`duckdb_apply_reader_state` as `error: ...` and in the error log.

## 13. Row decoder and collapse, step 4b (DWL#4)

Still no writes to DuckDB. Unit tests first:

```bash
cd /opt/mysql_source/percona-server-9.7-build
make -j8 duckdb duckdb_apply-t && ctest -R duckdb_apply --output-on-failure
# every type (integers, FLOAT/DOUBLE, DECIMAL, VARCHAR, CHAR incl. long CHAR,
# TEXT, VARBINARY, latin1 -> UTF-8, DATE, DATETIME(6), TIMESTAMP(3)), NULLs,
# partial images, truncated image, dates, and every collapse rule
```

On the server:

```sql
SET GLOBAL duckdb_apply_debug_log = ON;
UPDATE mill1 SET active = 1 - active WHERE id <= 3;
SHOW STATUS LIKE 'duckdb_apply_%';
-- rows_decoded +3, changes +3
-- error log: 3 lines "duckdb apply: <gtid> windmills_small.mill1 key=(1) original=DELETE live=(1, 'uuid...', ...)"

BEGIN;
INSERT INTO mill1 (uuid, millid, kwatts_s, date, location, continent, active, strrecordtype)
  VALUES (uuid(), 1, 100, '2026-11-10', 'test', 'Europe', 1, 'abc');
UPDATE mill1 SET kwatts_s = 200 WHERE id = LAST_INSERT_ID();
COMMIT;
-- rows_decoded +2, changes +1: one line "original=KEEP live=(<new id>, ..., 200, ...)"
-- (insert + update collapse to one insert of the final row)

SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM mill6 WHERE id = 10;          -- mill6 has retain_history
SET gtid_next = 'AUTOMATIC';
-- error log: "... windmills_small.mill6 key=(10) original=ARCHIVE"

SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM mill1 WHERE id = 3;           -- classic table: a purge is a delete
SET gtid_next = 'AUTOMATIC';
-- error log: "... windmills_small.mill1 key=(3) original=DELETE"

-- Decoding at the start of a new binary log file (the reader opens it at
-- position 0 and must read its format description first):
FLUSH BINARY LOGS;
UPDATE mill1 SET active = 1 - active WHERE id <= 2;
SHOW STATUS LIKE 'duckdb_apply_reader%';   -- idle, the new file
-- error log: 2 lines "... mill1 key=(1) original=DELETE live=(1, ...)" with the right values

-- The same after a restart: the reader starts at a file boundary.

SET GLOBAL duckdb_apply_debug_log = OFF;
```

## 14. Applier writes, step 4c-1 (DWL#4)

Tables must be loaded after the last gap (reload them if section 15 named
them). Compare DuckDB with InnoDB through the debug log counts.

```sql
SET GLOBAL duckdb_apply_debug_log = ON;
SELECT COUNT(*) FROM mill1;                               -- InnoDB count N
UPDATE mill1 SET active = 1 - active WHERE id <= 10;
-- within ~1 s: "duckdb apply: windmills_small.mill1 deleted=10 archived=0
--   inserted=10; DuckDB now N rows"
SHOW STATUS LIKE 'duckdb_appl%';
-- apply_batches +1, apply_rows_written +20, applied_file/position/gtid moved

DELETE FROM mill1 WHERE id = 5;                           -- "DuckDB now N-1 rows"

BEGIN; UPDATE mill1 SET active = 1 - active WHERE id = 6; ROLLBACK;
-- nothing logged, batches unchanged

SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM mill6 WHERE id = 10;                          -- mill6 retain_history
SET gtid_next = 'AUTOMATIC';
-- "mill6 deleted=0 archived=1 inserted=0; DuckDB now <M-1> live, 1 archived"

-- Batching: 250 one-row transactions with max_trx 100: 2 full batches at
-- once, the last 50 at the delay (helpers from section 0)
SET GLOBAL duckdb_apply_batch_max_trx = 100;
SET GLOBAL duckdb_apply_batch_max_delay_ms = 60000;
SHOW GLOBAL STATUS LIKE 'duckdb_apply_batches';           -- note B
CALL duckdb_many_updates(250, 25);                        -- ~1 min at 200 ms per commit
-- session 2, any time: CALL duckdb_progress();  stop: CALL duckdb_stop();
SHOW GLOBAL STATUS LIKE 'duckdb_apply_batches';           -- B+2
-- within 60 s:                                           -- B+3
SET GLOBAL duckdb_apply_batch_max_trx = DEFAULT;
SET GLOBAL duckdb_apply_batch_max_delay_ms = DEFAULT;

-- Unload during apply: SECONDARY_UNLOAD of mill1 right after an UPDATE:
-- no error in the log, no write after the drop.

-- Restart with pending changes: UPDATE, then shutdown within 1 s.
-- After start: "binlog reader started at ... (applied position)"; the
-- DuckDB count equals InnoDB (nothing lost, nothing twice).

SET GLOBAL duckdb_apply_debug_log = OFF;
```

## 15. Gaps and repoint (DWL#4)

```sql
-- Expiry warning
SET GLOBAL binlog_expire_logs_seconds = 2000;
ALTER TABLE mill5 SECONDARY_LOAD;      -- WARNING: binlog_expire_logs_seconds=2000 is less than one day
SET GLOBAL binlog_expire_logs_seconds = 604800;

-- Gap with STOP (default): stop the server, purge the binlog of a load
-- position (or let expiry do it), start:
SHOW STATUS LIKE 'duckdb_apply_reader_state';
-- error: binary log binlog.0000NN was purged before it was read; loaded
--   tables that may be behind InnoDB: windmills_small.mill1, ...
SET GLOBAL duckdb_apply_start_position = 'FIRST';
SHOW WARNINGS;                          -- the same tables
SHOW STATUS LIKE 'duckdb_appl%';        -- reader idle, applied_file = first binlog

-- Refusals
SET GLOBAL duckdb_apply_start_position = 'binlog.999999';   -- not in the index
SET GLOBAL duckdb_apply_start_position = '<older than applied>';  -- refused: applied twice
SET GLOBAL duckdb_enable = OFF;
SET GLOBAL duckdb_apply_start_position = 'CURRENT';         -- refused: disabled
SET GLOBAL duckdb_enable = ON;

-- FIRST_AVAILABLE: same gap, the reader warns and continues by itself
SET GLOBAL duckdb_apply_gap_action = FIRST_AVAILABLE;
```

## 16. Large transactions in pieces, step 4c-2 (DWL#4)

`mill1` and `mill6` loaded after the last gap. A small piece size makes
pieces cheap to produce; the last cases use the default.

```sql
SHOW VARIABLES LIKE 'duckdb_apply_max_transaction_rows';  -- empty: removed
SET GLOBAL duckdb_apply_debug_log = ON;
SET GLOBAL duckdb_apply_batch_max_rows = 1000;            -- piece size 1000
SELECT COUNT(*) FROM mill1;                               -- InnoDB count N

-- 1. One transaction, 3500 rows: 3 pieces + the last one
UPDATE mill1 SET active = 1 - active WHERE id <= 3500;
-- log: "transaction <gtid> changes more than 1000 rows of loaded tables:
--   applied in pieces ..." then "... applied in pieces, 7000 rows written"
SHOW STATUS LIKE 'duckdb_apply_%';
-- large_transactions +1, pieces +4, batches +1, applied_gtid = <gtid>
-- debug log: "DuckDB now N rows" after each piece

-- 2. Later pieces change rows written by earlier ones
BEGIN;
UPDATE mill1 SET active = 1 - active WHERE id <= 3000;
DELETE FROM mill1 WHERE id <= 1500;
COMMIT;
-- last debug line: "DuckDB now <N-1500> rows" = SELECT COUNT(*) FROM mill1

-- 3. A batch pending before the large transaction is committed first
SET GLOBAL duckdb_apply_batch_max_delay_ms = 60000;
UPDATE mill1 SET active = 1 - active WHERE id = 2000;     -- pending, not committed
UPDATE mill1 SET active = 1 - active WHERE id > 2000 AND id <= 5000;
SHOW STATUS LIKE 'duckdb_apply_batches';                  -- +2 (the pending one, then the large one)
SET GLOBAL duckdb_apply_batch_max_delay_ms = 1000;

-- 4. History table with purge, in pieces
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM mill6 WHERE id <= 2500;
SET gtid_next = 'AUTOMATIC';
-- "DuckDB now <M-2500> live, 2500 archived" (plus earlier archived rows)

-- 5. Unload during a large transaction (session 2, while session 1 runs a
--    large UPDATE on mill1): SECONDARY_UNLOAD of another loaded table waits,
--    then succeeds right after "applied in pieces" is logged.

-- 6. Crash in the middle: UPDATE of the whole mill1 (piece size 1000), kill -9
--    mysqld after the first "applied in pieces ..." line and before
--    "... rows written". After start: applied position before the transaction,
--    the transaction is applied again once; DuckDB count = InnoDB count,
--    large_transactions 1 (counters restart at 0).

-- 7. Clean stop in the middle (same as 6 with a normal shutdown): log
--    "DuckDB transaction of the large transaction <gtid> rolled back
--    (reader stopped)"; after start same result as 6.

-- 8. Default piece size: 250,000 rows -> pieces +3
SET GLOBAL duckdb_apply_batch_max_rows = DEFAULT;          -- 100000
UPDATE mill1 SET active = 1 - active WHERE id <= 250000;
SET GLOBAL duckdb_apply_debug_log = OFF;
```

A rolled-back InnoDB transaction is not written to the binary log, so the
"ROLLBACK in the binary log" path cannot be reached with InnoDB tables; it is
covered by code review only.

## 17. DDL and compressed payloads, step 4d (DWL#4)

`log_error_verbosity = 3`. `mill1` classic, `mill6` retain_history, both
loaded after the last gap; a classic `mill5` for the rename. After each
statement wait until `duckdb_apply_reader_state` is `idle`, then check the
counters and the error log.

```sql
SHOW GLOBAL STATUS LIKE 'duckdb_apply_ddl%';   -- refreshes R, renames N, failures F
SELECT @@gtid_executed;                        -- note it

-- 1. Index only: no refresh
ALTER TABLE mill1 ADD INDEX i_tmp (active);    -- refreshes R
ALTER TABLE mill1 DROP INDEX i_tmp;            -- refreshes R

-- 2. Column added with a default, then type change
ALTER TABLE mill1 ADD COLUMN extra INT DEFAULT 7;
-- log: "refreshed windmills_small.mill1 in the lake: <N> rows"
--      "DuckDB copy of windmills_small.mill1 refreshed after DDL"; refreshes R+1
ALTER TABLE mill1 MODIFY extra BIGINT;          -- refreshes R+2
UPDATE mill1 SET extra = 5000000000 WHERE id = 7;   -- applied (BIGINT value)

-- 3. History table: column rename and type change, archived rows kept
ALTER TABLE mill6 RENAME COLUMN <a column> TO <new name>, MODIFY <int column> BIGINT;
-- log: "refreshed windmills_small.mill6 in the lake: <live> rows ... (retain_history)"
-- debug log after the next change: "<live> live, <archived> archived", archived
-- count unchanged

-- 4. TRUNCATE and RENAME TABLE
TRUNCATE TABLE mill5;                          -- refresh: DuckDB now 0 rows
RENAME TABLE mill5 TO mill5r;                  -- renames N+1
INSERT INTO mill5r SELECT * FROM mill1 LIMIT 10;   -- applied to mill5r

-- 4b. DDL then RENAME before the reader gets there (online schema change
--     pattern). Deterministic with the applier stopped:
SET GLOBAL duckdb_enable = OFF;
TRUNCATE TABLE mill5r; INSERT INTO mill5r SELECT * FROM mill1 LIMIT 3;
RENAME TABLE mill5r TO mill5x; INSERT INTO mill5x SELECT * FROM mill1 ORDER BY id DESC LIMIT 2;
SET GLOBAL duckdb_enable = ON;
-- log: "mill5r was renamed or dropped after the statement: its DuckDB copy is
--      refreshed at its rename", "renamed to windmills_small.mill5x",
--      "refreshed windmills_small.mill5x in the lake: 5 rows";
--      ddl_deferred +1, failures unchanged; mill5x state 'loaded'

-- 5. Failure does not stop the applier
ALTER TABLE mill6 SECONDARY_ENGINE_ATTRIBUTE = '{}';
-- log: "refresh after DDL failed: ... retain_history cannot be removed by DDL
--      ... The DuckDB copy is stale"; failures F+1
UPDATE mill1 SET active = 1 - active WHERE id = 8;  -- still applied

-- 6. Nothing written by the plugin: gtid_executed grew only by the
--    statements above; mysqlbinlog of the current file has no SECONDARY_LOAD
--    of a refresh.

-- 7. Compressed payloads
SET GLOBAL binlog_transaction_compression = ON;
UPDATE mill1 SET active = 1 - active WHERE id <= 3000;   -- applied, in pieces
                                                          -- if over batch_max_rows
SET GLOBAL binlog_transaction_compression = OFF;

-- 8. Put mill5 back
RENAME TABLE mill5x TO mill5;
```

With the server stopped, the lake can be read with a small DuckDB program
(attach `catalog.ducklake` read-only) to check the column types
(`duckdb_columns()`) and the archived rows: on 2026-11-11 this was done on a
throwaway instance (results log).

## 18. Replica (DWL#4; after step 4d)

Two throwaway instances: a source without the plugin, a replica with it
(`log_bin`, `log_replica_updates=ON`, `binlog_row_metadata=FULL`,
`binlog_row_image=FULL`, `gtid_mode=ON` on the replica). The reader on the
replica reads the replica's own binary log (DWL-04, Replicas).

```sql
-- source: the table, with the secondary engine defined there (no plugin
-- needed on the source), so the replica gets it without an errant GTID
CREATE TABLE r1 (id INT PRIMARY KEY, v INT) SECONDARY_ENGINE = duckdb;
INSERT INTO r1 SELECT ... ;                       -- some thousand rows

-- replica: load without writing to the replica's binary log
SET sql_log_bin = 0;
ALTER TABLE r1 SECONDARY_LOAD;
SET sql_log_bin = 1;
SELECT @@gtid_executed;                            -- equal to the source's

-- 1. Replicated DML (INSERT, UPDATE, DELETE, a large transaction): on the
--    replica duckdb_apply_* counters move; DuckDB count = InnoDB count.
-- 2. Replicated DDL (ALTER ... MODIFY v BIGINT, ADD COLUMN, TRUNCATE,
--    RENAME TABLE): replication does not stop (SHOW REPLICA STATUS: no
--    error); the copy is refreshed (section 17 checks).
-- 3. Multi-threaded applier (replica_parallel_workers = 4, commit order on):
--    same results as 1.
-- 4. Errant GTIDs: after all the above, the replica's gtid_executed has no
--    GTID of its own (no refresh or load written to its binary log).
-- 5. Replica restart and replica stopped while the source writes: no loss,
--    nothing applied twice.
```

## 19. History copies: reattach, resync, adopt, discard, step 3b (DWL#3)

Run each block with `mysql --force` and a here-document (with `mysql -e` the
statements after an error are skipped, `SHOW WARNINGS` included). Scratch
tables only: `hr`, `hr_new`, `hw`, `hs`.

```sql
SET GLOBAL log_error_verbosity = 3;
CREATE TABLE hr (id INT PRIMARY KEY, qty INT, note VARCHAR(20))
  SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
INSERT INTO hr VALUES (1,1,'a'),(2,2,'b'),(3,3,'c');
ALTER TABLE hr SECONDARY_LOAD;
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge'; DELETE FROM hr WHERE id = 1; SET gtid_next = 'AUTOMATIC';

-- 1. Check only, table without a copy: error "has no history copy to take over"
SET SESSION duckdb_history_check_only = ON;
CREATE TABLE hx (id INT PRIMARY KEY) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE hx SECONDARY_LOAD;
SET SESSION duckdb_history_check_only = OFF;

-- 2. Reattach: same structure after DROP + CREATE
DROP TABLE hr;                                    -- copy kept, stale
CREATE TABLE hr (id INT PRIMARY KEY, qty INT, note VARCHAR(20))
  SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
INSERT INTO hr VALUES (10,10,'x');
SET SESSION duckdb_history_check_only = ON;
ALTER TABLE hr SECONDARY_LOAD;     -- error "structure check passed: ... can reattach"
SET SESSION duckdb_history_check_only = OFF;
ALTER TABLE hr SECONDARY_LOAD;     -- log "reattached windmills_small.hr in the lake: 1 rows"

-- 3. Mismatch: refused, every difference in SHOW WARNINGS
DROP TABLE hr;
CREATE TABLE hr (id BIGINT, qty INT, extra INT, PRIMARY KEY (id, qty))
  SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE hr SECONDARY_LOAD;     -- "the structure does not match ... (4 differences)"
SHOW WARNINGS;                     -- type of id, extra not in the copy, note missing
                                   -- (1 archived value), primary key (id) vs (id, qty)
DROP TABLE hr;

-- 4. Adopt the stale copy hr under another name
CREATE TABLE hr_new (id INT PRIMARY KEY, qty INT, note VARCHAR(20))
  SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE =
  '{"retain_history": true, "adopt_history_from": "windmills_small.hr"}';
INSERT INTO hr_new VALUES (20,20,'y');
ALTER TABLE hr_new SECONDARY_LOAD; -- log "loaded (adopting the history copy windmills_small.hr) ..."
-- from a copy still in use: refused, "rename the table with RENAME TABLE instead"

-- 5. Resync of a loaded history table, right after a purge
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge'; DELETE FROM hr_new WHERE id = 20; SET gtid_next = 'AUTOMATIC';
ALTER TABLE hr_new SECONDARY_LOAD; -- warning (purges in an unread range); log "resynced"
                                   -- the purge is kept: it is committed before the swap

-- 6. History unload: refused, then confirmed
ALTER TABLE hr_new SECONDARY_UNLOAD;   -- "SET SESSION duckdb_unload_history_confirm = ON first"
SET SESSION duckdb_unload_history_confirm = ON;
ALTER TABLE hr_new SECONDARY_UNLOAD;   -- log "unloaded ... with its archived rows"

-- 7. Discard a stale copy
CREATE TABLE hs (id INT PRIMARY KEY) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE hs SECONDARY_LOAD;
ALTER TABLE hs SECONDARY_ENGINE = NULL;                      -- copy stale, table exists
SET GLOBAL duckdb_discard_stale_copy = 'windmills_small.hs'; -- refused: the table exists
DROP TABLE hs;
SET GLOBAL duckdb_discard_stale_copy = 'windmills_small.hs'; -- log "discarded ... with its archived rows"
SET SESSION duckdb_unload_history_confirm = OFF;
DROP TABLE hx, hr_new;
```

Concurrency (two sessions): while a large table loads (minutes), change a
loaded table in session 2 and DROP a loaded history table. The load ends
without "Transaction conflict"; the change is applied and the copy is stale.

## Results log

| Date | Build | Sections run | Result | Notes |
|---|---|---|---|---|
| 2026-11-07 | clang-21, C++23, debug | 1 | pass | smoke test, 480 files C++23 |
| 2026-11-09 | clang-21, C++23, debug | 5 (`mill1` load) | pass | before the executor pool, with `thread_stack=8M` |
| 2026-11-10 | clang-21, C++23, debug | 12 (first check) | pass | `UPDATE mill1` of 10 rows: transactions_read 1, row_events_read 1, last_gtid set, position 198 -> 2974 |
| 2026-11-10 | clang-21, C++23, debug | 12 (rest) | pass | unloaded table and rollback not counted; purge tag read (`<uuid>:duckdb_avoid_purge:1`); rotation followed to the new file after the next commit (FLUSH BINARY LOGS alone does not notify) |
| 2026-11-10 | clang-21, C++23, debug | 13 + stability fixes | pass | decoder and collapse on mill1/mill6; decoding at the start of a new binlog file after the format-description fix; duckdb_enable OFF/ON |
| 2026-11-11 | clang-21, C++23, debug | 14 (batching, 2000 updates) | not concluded | about 5 commits/s (HDD fsync), no progress shown and info-level log lines dropped (`log_error_verbosity=2`); stopped by shutdown. Added the test helpers and the verbosity setting; batching step reduced to 250 updates |
| 2026-11-11 | clang-21, C++23, debug | 19 (throwaway instance) | pass after a fix | check only, reattach, mismatch (4 differences), unknown PK, adopt, adopt of a copy in use refused, resync, confirmed unload, discard (not stale / not confirmed / table exists refused, then done), PK backfill by the applier. First reattach failed with a DuckLake conflict: the applier wrote the registry during the load's single transaction. Fixed with the two-transaction load; then a 1M-row load with a change and a DROP of a history table during it passed, a planted build table was dropped at open, and a resync right after a pending purge kept the archived row |
| 2026-11-11 | clang-21, C++23, debug | 17 case 4b (reference instance, ddl_r) | pass | applier stopped during TRUNCATE + INSERT + RENAME + INSERT; at ON: deferred, renamed to ddl_r2, refreshed with 5 rows; lake: 5 rows, ddl_r2 loaded. The counters read in the same pasted block were still 0: the reader starts about 2 s after ON |
| 2026-11-11 | clang-21, C++23, debug | 17 (throwaway instance): deferred refresh | pass | applier stopped (duckdb_enable=OFF) during TRUNCATE + RENAME and history RENAME COLUMN + RENAME TABLE: both deferred, renamed, refreshed (lake: 5 rows; 45 live + 5 archived with old values under the new column name); restart with a copy in refresh_pending: changes skipped, refreshed at its RENAME |
| 2026-11-11 | clang-21, C++23, debug | 17 (reference instance, scratch copies ddl_c, ddl_h, ddl_t) | fail, fixed | ddl_c refreshes, type change and compressed pieces pass; ddl_h 960 live + 40 archived = InnoDB (the purge DELETE ran twice); ddl_t2 stale: TRUNCATE and RENAME back to back, the refresh found no table (fixed by the deferred refresh) |
| 2026-11-11 | clang-21, C++23, debug | 17 (throwaway instance, spike schema) | pass | compressed payloads: DML and 3000-row transactions in pieces, DuckDB = InnoDB; DDL: ADD INDEX no refresh; ADD COLUMN DEFAULT 7 and MODIFY BIGINT refreshed (lake: BIGINT, 2511 rows = 7, later 5000000000 applied); history RENAME COLUMN + MODIFY kept 10 archived rows under the new name; TRUNCATE, RENAME TABLE followed; retain_history removal refused, stale, applier continued; 12 statements = 12 GTIDs (none from the plugin). First start failed: a `thread_local` exhausted the static TLS block (fixed) |
| 2026-11-11 | clang-21, C++23, debug | 11 (startup abort, pending batch at shutdown) | pass | throwaway instance: before the fix, an aborted startup crashed in the reader (`get_current_log()` on the destroyed `LOCK_log`); after it, 3/3 aborts end cleanly and a pending batch is committed during the shutdown |
| 2026-11-10 | clang-21, C++23, debug | 15 (repoint) | pass | gap on binlog.000027 (expired, 2000 s); after the LOG_INFO_EOF fix SET GLOBAL duckdb_apply_start_position='FIRST' restarted the reader |
