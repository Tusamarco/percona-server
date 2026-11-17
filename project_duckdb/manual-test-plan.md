# DuckDB secondary engine: manual test plan

Lines marked `-- EXPECTED ERROR` must fail with that error: it is the result
the test checks, not a problem.

Manual checks for what is implemented so far: DWL#1 to DWL#5 and DWL#6
steps 6a to 6d, A1, A2 and DWL#7 (sections 1 to 34). Since 2026-11-15 most of them are also in the MTR suite
`storage/duckdb/mysql-test/duckdb` (its README maps tests to sections); the
sections that need restarts with special options, a replica, semi-sync or
long runs stay manual. Update this file and the suite whenever a step adds
behavior.

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
SET GLOBAL duckdb_executor_threads = 0;     -- EXPECTED ERROR: wrong value
SET GLOBAL duckdb_executor_threads = 999;   -- EXPECTED ERROR: wrong value (above cores)
SET GLOBAL duckdb_executor_stack_size = 16777216;   -- EXPECTED ERROR: read-only
SET GLOBAL duckdb_executor_threads = 4;
```

## 4. Runtime variables (DWL#2)

```sql
SET GLOBAL duckdb_memory_limit = 2147483648;        -- ok (2 GiB, applied to DuckDB)
SET GLOBAL duckdb_memory_limit = 134217728;         -- EXPECTED ERROR: (below 256 MiB)
SET GLOBAL duckdb_threads = 4;                      -- ok
SET GLOBAL duckdb_threads = 0;                      -- EXPECTED ERROR
SET GLOBAL duckdb_threads = 999;                    -- EXPECTED ERROR: (above cores)
SET GLOBAL duckdb_data_path = '/tmp/x';             -- EXPECTED ERROR: read-only
SET GLOBAL duckdb_encryption = ON;                  -- EXPECTED ERROR: read-only
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
ALTER TABLE mill1 SECONDARY_LOAD;                -- EXPECTED ERROR: already in the lake, run SECONDARY_UNLOAD first
ALTER TABLE mill1 SECONDARY_UNLOAD;              -- ok
ALTER TABLE mill1 SECONDARY_UNLOAD;              -- EXPECTED ERROR: not loaded in secondary engine
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
ALTER TABLE mill6 SECONDARY_LOAD;                -- since 3b: resync, warning; archived rows kept
ALTER TABLE mill6 SECONDARY_UNLOAD;              -- EXPECTED ERROR: ... SET SESSION duckdb_unload_history_confirm = ON first
-- RENAME TABLE and ALTER are allowed since 4d (the copy follows): section 17

-- DROP keeps the history: we test it on a copy, not on mill6
CREATE TABLE tmp_hist_drop LIKE mill6;               -- copies SECONDARY_ENGINE and attribute
INSERT INTO tmp_hist_drop SELECT * FROM mill6 LIMIT 1000;
ALTER TABLE tmp_hist_drop SECONDARY_LOAD;
DROP TABLE tmp_hist_drop;                            -- ok, WARNING: DuckDB copy kept and marked stale
SHOW WARNINGS;
CREATE TABLE tmp_hist_drop LIKE mill6;
ALTER TABLE tmp_hist_drop SECONDARY_LOAD;            -- since 3b: reattached (same structure), archived rows kept
DROP TABLE tmp_hist_drop;                            -- stale again; discard: section 19 case 7
```

## 7. Preconditions and attributes (DWL#3)

```sql
CREATE TABLE t_nopk (a INT) SECONDARY_ENGINE = duckdb;
ALTER TABLE t_nopk SECONDARY_LOAD;               -- EXPECTED ERROR: requires a primary key

CREATE TABLE t_parent (id INT PRIMARY KEY);
CREATE TABLE t_fk (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES t_parent(id)) SECONDARY_ENGINE = duckdb;
ALTER TABLE t_fk SECONDARY_LOAD;                 -- ok since 2026-11-17 (innodb_native_foreign_keys=OFF:
                                                 -- cascades are binlogged; with ON a cascading child is refused)

CREATE TABLE t_enum (id INT PRIMARY KEY, g GEOMETRY) SECONDARY_ENGINE = duckdb;
ALTER TABLE t_enum SECONDARY_LOAD;               -- EXPECTED ERROR: column g: type not supported
                                                 -- (ENUM, SET, TIME, YEAR, BIT are supported since 2026-11-16)

CREATE TABLE t_attr (id INT PRIMARY KEY) SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_hist": true}';
ALTER TABLE t_attr SECONDARY_LOAD;               -- EXPECTED ERROR: unknown SECONDARY_ENGINE_ATTRIBUTE key 'retain_hist'

SET GLOBAL binlog_row_metadata = MINIMAL;
ALTER TABLE mill2 SECONDARY_ENGINE = duckdb;
ALTER TABLE mill2 SECONDARY_LOAD;                -- EXPECTED ERROR: requires binlog_row_metadata=FULL
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
ALTER TABLE t_zero SECONDARY_LOAD;               -- EXPECTED ERROR: column d: zero dates cannot be loaded
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
-- session 1: EXPECTED ERROR ... failed: interrupted
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
ALTER TABLE mill1 SECONDARY_LOAD;                -- EXPECTED ERROR: already in the lake (not removed while OFF)
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
ALTER TABLE tmp_hist_sync SECONDARY_LOAD;      -- EXPECTED ERROR: retain_history requires sync_binlog=1
ALTER TABLE mill5 SECONDARY_ENGINE = duckdb;
ALTER TABLE mill5 SECONDARY_LOAD;    -- ok with WARNING about sync_binlog
SET GLOBAL sync_binlog = 1;
ALTER TABLE tmp_hist_sync SECONDARY_LOAD;      -- ok

-- binlog_transaction_compression=ON is supported since 4d (section 17 case 7)
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
SET GLOBAL duckdb_apply_start_position = 'binlog.999999';   -- EXPECTED ERROR: not in the index
SET GLOBAL duckdb_apply_start_position = '<older than applied>';  -- EXPECTED ERROR: refused: applied twice
SET GLOBAL duckdb_enable = OFF;
SET GLOBAL duckdb_apply_start_position = 'CURRENT';         -- EXPECTED ERROR: refused: disabled
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
-- 6. History table (SECONDARY_ENGINE_ATTRIBUTE '{"retain_history": true}')
--    with a tagged purge on the source
--    (SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge'; DELETE ...): the tag
--    reaches the replica's binary log; purge_transactions and archived_rows
--    move on the replica; untagged deletes are deleted.
-- 7. FULL scope on the replica: DUCKDB_HISTORY_READER must be granted on the
--    replica itself with sql_log_bin = 0 (a source without the plugin does
--    not know the privilege: GRANT is a syntax error there). FULL shows live
--    and archived rows; FULL + OFF stopped by the guard.
-- 8. Weight strings: a utf8mb4_0900_ai_ci column, GROUP BY and WHERE on it.
-- 9. Offloaded joins, subqueries, UNION, COUNT(DISTINCT) on the replica:
--    same as InnoDB, or refused for the documented reasons.
```

Helpers used for the run (compare OFF and FORCED on the replica after
waiting for the replica and the reader):
`WAIT_FOR_EXECUTED_GTID_SET('<source gtid_executed>', 300)`, then wait until
`duckdb_apply_reader_state = 'idle'` and `duckdb_apply_reader_position` >=
`SHOW BINARY LOG STATUS` position.

Notes from the 2026-11-15 run:

- `ALTER TABLE ... SECONDARY_LOAD` and `SECONDARY_UNLOAD` are written to the
  binary log with a GTID (server behaviour). On a replica they must run with
  `sql_log_bin = 0`, otherwise each one is an errant GTID.
- Local writes on the replica are errant GTIDs as always; use
  `super_read_only` on real replicas.
- Found (guard, not replica specific): a single-table `UPDATE` or `DELETE`
  of a history table under FULL scope was not stopped: the guard listened to
  READ events only, and the target table of an UPDATE or DELETE raises an
  UPDATE or DELETE event. Fixed 2026-11-16: the guard also listens to UPDATE
  and DELETE (MTR `guard`).
- Found: with `super_read_only = ON` on the replica, the refresh after a
  replicated DDL failed (error 1290: the internal `ALTER TABLE ...
  SECONDARY_LOAD` is refused) and the copy became stale.
  `SECONDARY_LOAD`/`UNLOAD` and the GRANT by hand are refused too (expected).
  Fixed 2026-11-16: the guard lets the internal session pass the read-only
  check (DWL-04, Replicas; MTR `super_read_only`).

```sql
-- 10. super_read_only on the replica (requires duckdb_guard ACTIVE)
SET GLOBAL super_read_only = ON;                  -- replica
-- source: ALTER TABLE ... MODIFY / ADD COLUMN / RENAME / TRUNCATE on a
-- classic and a history table, then DML
-- replica: duckdb_apply_ddl_refreshes moves, duckdb_apply_ddl_failures does
-- not; DuckDB = InnoDB; FULL keeps the archived rows; error log
-- "refreshed ... after DDL"; gtid_executed has no new replica GTID.
-- User statements still refused (1290): SECONDARY_UNLOAD, INSERT.
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

-- 1. Check only, table without a copy: EXPECTED ERROR "has no history copy to take over"
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
ALTER TABLE hr SECONDARY_LOAD;     -- EXPECTED ERROR: "structure check passed: ... can reattach" (check only always ends with an error)
SET SESSION duckdb_history_check_only = OFF;
ALTER TABLE hr SECONDARY_LOAD;     -- log "reattached windmills_small.hr in the lake: 1 rows"

-- 3. Mismatch: refused, every difference in SHOW WARNINGS
DROP TABLE hr;
CREATE TABLE hr (id BIGINT, qty INT, extra INT, PRIMARY KEY (id, qty))
  SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE hr SECONDARY_LOAD;     -- EXPECTED ERROR: "the structure does not match ... (4 differences)"
SHOW WARNINGS;                     -- type of id, extra not in the copy, note missing
                                   -- (1 archived value), primary key (id) vs (id, qty)
DROP TABLE hr;

-- 4. Adopt the stale copy hr under another name
CREATE TABLE hr_new (id INT PRIMARY KEY, qty INT, note VARCHAR(20))
  SECONDARY_ENGINE = duckdb SECONDARY_ENGINE_ATTRIBUTE =
  '{"retain_history": true, "adopt_history_from": "windmills_small.hr"}';
INSERT INTO hr_new VALUES (20,20,'y');
ALTER TABLE hr_new SECONDARY_LOAD; -- log "loaded (adopting the history copy windmills_small.hr) ..."
-- from a copy still in use: EXPECTED ERROR "rename the table with RENAME TABLE instead"

-- 5. Resync of a loaded history table, right after a purge
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge'; DELETE FROM hr_new WHERE id = 20; SET gtid_next = 'AUTOMATIC';
ALTER TABLE hr_new SECONDARY_LOAD; -- warning (purges in an unread range); log "resynced"
                                   -- the purge is kept: it is committed before the swap

-- 6. History unload: refused, then confirmed
ALTER TABLE hr_new SECONDARY_UNLOAD;   -- EXPECTED ERROR: "SET SESSION duckdb_unload_history_confirm = ON first"
SET SESSION duckdb_unload_history_confirm = ON;
ALTER TABLE hr_new SECONDARY_UNLOAD;   -- log "unloaded ... with its archived rows"

-- 7. Discard a stale copy
CREATE TABLE hs (id INT PRIMARY KEY) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE hs SECONDARY_LOAD;
ALTER TABLE hs SECONDARY_ENGINE = NULL;                      -- copy stale, table exists
SET GLOBAL duckdb_discard_stale_copy = 'windmills_small.hs'; -- EXPECTED ERROR: refused: the table exists
DROP TABLE hs;
SET GLOBAL duckdb_discard_stale_copy = 'windmills_small.hs'; -- log "discarded ... with its archived rows"
SET SESSION duckdb_unload_history_confirm = OFF;
DROP TABLE hx, hr_new;
```

Concurrency (two sessions): while a large table loads (minutes), change a
loaded table in session 2 and DROP a loaded history table. The load ends
without "Transaction conflict"; the change is applied and the copy is stale.

## 20. Purge and history scope, DWL#5

Two scratch history tables. Wait for `duckdb_apply_reader_state = idle` and a
few seconds (the batch commit) before checking counters.

```sql
SHOW VARIABLES LIKE 'duckdb_avoid%';                     -- empty: the tag is fixed
CREATE TABLE at1 (id INT PRIMARY KEY, v INT) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
CREATE TABLE bt1 LIKE at1;
INSERT INTO at1 VALUES (1,1),(2,2); INSERT INTO bt1 VALUES (1,1),(2,2);
ALTER TABLE at1 SECONDARY_LOAD; ALTER TABLE bt1 SECONDARY_LOAD;

-- 1. A purge in session A does not affect session B (two sessions)
-- session A:
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
BEGIN; DELETE FROM at1 WHERE id = 1; DO SLEEP(10);
-- session B, while A waits:
DELETE FROM bt1 WHERE id = 1;                            -- a real delete
-- session A:
COMMIT; SET gtid_next = 'AUTOMATIC';
SHOW GLOBAL STATUS LIKE 'duckdb_apply_purge_transactions';   -- 1
SHOW GLOBAL STATUS LIKE 'duckdb_apply_archived_rows';        -- 1
-- debug log or lake (server stopped): at1 id 1 archived; bt1 id 1 gone
-- mysqlbinlog: only A's transaction has a GTID with :duckdb_avoid_purge:

-- 2. History scope
SET SESSION duckdb_history_scope = FULL;                 -- EXPECTED ERROR 1227: DUCKDB_HISTORY_READER
SELECT /*+ SET_VAR(duckdb_history_scope = FULL) */ 1;    -- warning 1227, hint ignored
GRANT DUCKDB_HISTORY_READER ON *.* TO CURRENT_USER;
SET SESSION duckdb_history_scope = FULL;                 -- ok
SET SESSION duckdb_history_scope = LIVE;

DROP TABLE at1, bt1;                                     -- stale copies kept: discard them (section 19 case 7)
```

## 21. Query offload, step 6a (DWL#6)

Scratch table `g` (200,000 rows, every result type), the query list
`duckdb_offload_6a_queries.sql` and the comparison script
`duckdb_offload_compare.sh` (each query with `use_secondary_engine=OFF` and
`FORCED`; it stops if the server is lost).

```sql
-- with FORCED the INSERT ... SELECT below would fail (n has no secondary engine)
SET SESSION use_secondary_engine = ON;
CREATE TABLE n (n INT PRIMARY KEY);
INSERT INTO n WITH RECURSIVE s(n) AS (SELECT 0 UNION ALL SELECT n+1 FROM s WHERE n < 999) SELECT n FROM s;
CREATE TABLE g (id INT PRIMARY KEY, grp INT NOT NULL, amount DECIMAL(10,2), d DATE, dt DATETIME,
  ts TIMESTAMP NULL, s VARCHAR(20) COLLATE utf8mb4_0900_bin, n INT NULL,
  ci VARCHAR(20) COLLATE utf8mb4_0900_ai_ci) SECONDARY_ENGINE = duckdb;
INSERT INTO g SELECT a.n*1000+b.n, (a.n*1000+b.n) % 7, ((a.n*1000+b.n) % 1000) / 7,
  DATE '2024-01-01' + INTERVAL ((a.n*1000+b.n) % 365) DAY,
  TIMESTAMP '2024-01-01 10:00:00' + INTERVAL ((a.n*1000+b.n) % 1000) MINUTE,
  TIMESTAMP '2024-06-01 12:00:00' + INTERVAL ((a.n*1000+b.n) % 100) SECOND,
  CONCAT('s', (a.n*1000+b.n) % 50), IF((a.n*1000+b.n) % 5 = 0, NULL, (a.n*1000+b.n) % 13),
  CONCAT('Ci', (a.n*1000+b.n) % 3)
  FROM n a, n b WHERE a.n < 200;
ALTER TABLE g SECONDARY_LOAD;
```

```bash
cd project_duckdb
MYSQL="mysql -S /opt/mysql_instances/master96/mysql.sock -u dba -p<password> windmills_small" \
  ./duckdb_offload_compare.sh duckdb_offload_6a_queries.sql
# expected: every query "same, offloaded"; last line 19/19 identical and offloaded
```

```sql
-- 1. Cost threshold: with ON a query is tried in DuckDB only above it
SET SESSION use_secondary_engine = ON;
SELECT * FROM g WHERE id = 5;                           -- cheap: InnoDB, counters unchanged
SELECT ci, COUNT(*) FROM g GROUP BY ci;                 -- below 100000 too: InnoDB, not even refused
SET SESSION secondary_engine_cost_threshold = 1000;
SELECT grp, SUM(amount) FROM g GROUP BY grp;            -- queries +1

-- 2. ON falls back to InnoDB for a refused query, without error
SELECT ci, COUNT(*) FROM g GROUP BY ci;                 -- InnoDB result
SHOW GLOBAL STATUS LIKE 'duckdb_offload%';              -- refused +1, last_refusal collation:ci ...
EXPLAIN FORMAT=TREE SELECT grp, SUM(amount) FROM g GROUP BY grp;   -- "in secondary engine DUCKDB"
SET optimizer_trace = 'enabled=on';
SELECT grp, SUM(amount) FROM g GROUP BY grp;
SELECT TRACE FROM information_schema.OPTIMIZER_TRACE\G -- "duckdb_offload": {"sql": "SELECT CAST(..."}
SET optimizer_trace = 'enabled=off';

-- 3. FORCED refusals
SET SESSION use_secondary_engine = FORCED;
SELECT ci, COUNT(*) FROM g GROUP BY ci;                 -- EXPECTED ERROR 3877: ... collation:ci (utf8mb4_0900_ai_ci)
BEGIN; INSERT INTO g (id, grp) VALUES (900020, 1);
SELECT COUNT(*) FROM g;                                 -- EXPECTED ERROR 3889: ... multi-statement transaction
ROLLBACK;

-- 4. Read your own writes (same connection, FORCED)
SELECT COUNT(*) FROM g WHERE id >= 900000;              -- 0
INSERT INTO g (id, grp) VALUES (900010, 1);
SELECT COUNT(*) FROM g WHERE id >= 900000;              -- 1 at once (about 1 s: it waits for
                                                        -- the applier; OFF does not wait)
DELETE FROM g WHERE id >= 900000;
SELECT COUNT(*) FROM g WHERE id >= 900000;              -- 0 at once

-- 5. History scope on a history table (e.g. mill6): LIVE = InnoDB count,
--    FULL = live + archived (needs DUCKDB_HISTORY_READER)
SELECT COUNT(*) FROM mill6;                             -- with FORCED: equals InnoDB
SET SESSION duckdb_history_scope = FULL;
SELECT COUNT(*) FROM mill6;                             -- adds the archived rows
SET SESSION duckdb_history_scope = LIVE;
SET SESSION use_secondary_engine = ON;
```

## 22. Query offload, step 6b (DWL#6)

Scratch table `p6` (20,000 rows): PAD SPACE strings with trailing spaces, a
tab and an empty string (`b4` utf8mb4_bin, `b3` utf8mb3_bin), binary
strings, NULLs in every column, DATE/DATETIME/TIMESTAMP, DECIMAL, DOUBLE,
UNSIGNED. Query lists `duckdb_offload_6b_queries.sql` (must be identical and
offloaded) and `duckdb_offload_6b_refusals.sql` (must be refused with a
reason, and give the InnoDB rows with ON).

`duckdb_offload_setup.sql` drops and re-creates n, g (section 21) and p6
(this section) in the current database: `mysql ... windmills_small <
duckdb_offload_setup.sql`.

```sql
-- with FORCED the INSERT ... SELECT below would fail
SET SESSION use_secondary_engine = ON;
DROP TABLE IF EXISTS p6;
CREATE TABLE p6 (
  id INT NOT NULL PRIMARY KEY,
  grp INT NOT NULL,
  amount DECIMAL(10,2),
  qty INT,
  price DOUBLE,
  u INT UNSIGNED,
  d DATE,
  dt DATETIME,
  ts TIMESTAMP NULL,
  b4 VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_bin,
  b3 VARCHAR(20) CHARACTER SET utf8mb3 COLLATE utf8mb3_bin,
  raw VARBINARY(20),
  s VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_bin,
  ci VARCHAR(20) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci
) ENGINE=InnoDB;
SET SESSION cte_max_recursion_depth = 30000;
INSERT INTO p6
WITH RECURSIVE seq(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM seq WHERE i < 20000)
SELECT i, i % 7,
  IF(i % 13 = 0, NULL, ((i * 37) % 100000) / 100 - 300),
  IF(i % 11 = 0, NULL, (i * 7919) % 2001 - 1000),
  IF(i % 17 = 0, NULL, ((i * 31) % 1000) / 8),
  (i * 104729) % 4000000000,
  IF(i % 19 = 0, NULL, DATE '2020-01-01' + INTERVAL (i * 37) % 2000 DAY),
  IF(i % 23 = 0, NULL, TIMESTAMP '2020-01-01 00:00:00' + INTERVAL (i * 7919) % 100000000 SECOND),
  IF(i % 29 = 0, NULL, FROM_UNIXTIME(1614556800 + (i * 6007) % 50000000)),
  ELT(i % 12 + 1, 'a', 'a ', 'a  ', CONCAT('a', CHAR(9)), 'b', 'B', 'é', '', ' ', 'ab', 'a b', NULL),
  ELT(i % 10 + 1, 'x', 'x ', CONCAT('x', CHAR(9)), 'y', 'Y ', 'ü', '', 'xy', NULL, 'x  '),
  ELT(i % 5 + 1, 'k', 'k ', 'l', NULL, ''),
  ELT(i % 6 + 1, 'p', 'q', 'pq', 'P', NULL, ''),
  ELT(i % 4 + 1, 'm', 'M', 'n', NULL)
FROM seq;
ALTER TABLE p6 SECONDARY_ENGINE = duckdb;
ALTER TABLE p6 SECONDARY_LOAD;
SELECT COUNT(*), SUM(b4 = 'a') FROM p6;                -- 20000, 5000 ('a', 'a ', 'a  ')
```

```bash
cd project_duckdb
export MYSQL="mysql -S /opt/mysql_instances/master96/mysql.sock -u dba -p<password> windmills_small"
./duckdb_offload_compare.sh duckdb_offload_6b_queries.sql
# expected: 45/45 identical and offloaded
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6b_refusals.sql
# expected: 14/14 refused as expected, each with its reason, e.g.
#   Q6 refused (exact_division), ON = InnoDB
#   Q12 refused (string_number_comparison), ON = InnoDB
./duckdb_offload_compare.sh duckdb_offload_6a_queries.sql
# regression: 19/19 identical and offloaded (table g of section 21)
```

```sql
-- 1. TIMESTAMP constants follow the session time zone (each pair equal)
SET SESSION time_zone = '+05:30';
SET SESSION use_secondary_engine = OFF;
SELECT COUNT(*), MIN(ts), MAX(ts) FROM p6 WHERE ts >= '2021-06-01 10:00:00' AND ts < '2021-10-31 02:30:00';
SET SESSION use_secondary_engine = FORCED;
SELECT COUNT(*), MIN(ts), MAX(ts) FROM p6 WHERE ts >= '2021-06-01 10:00:00' AND ts < '2021-10-31 02:30:00';
SET SESSION time_zone = SYSTEM;                         -- repeat both: other values, still equal

-- 2. PAD SPACE in the generated SQL
SET optimizer_trace = 'enabled=on';
SELECT b4, COUNT(*) FROM p6 GROUP BY b4 ORDER BY b4;
SELECT TRACE FROM information_schema.OPTIMIZER_TRACE\G -- GROUP BY rtrim(t."b4", ' '),
                                                        -- arg_min(t."b4", t."id"), ORDER BY (rtrim(...) || repeat(' ', N))
SET optimizer_trace = 'enabled=off';
SET SESSION use_secondary_engine = ON;
```

Known difference (not a failure): `SELECT b4, COUNT(DISTINCT b3) FROM p6
GROUP BY b4` may show `a` on InnoDB and `a ` in DuckDB (equal under
utf8mb4_bin; MySQL groups by sorting there and does not define which value
of the group is shown). DWL-06, Lessons from step 6b.

## 23. Query offload, step 6c-1 (DWL#6)

Tables of `duckdb_offload_setup.sql`: g, p6, plus dim (7 rows: groups 0-4,
8, 9; `label` utf8mb4_bin, `ci` utf8mb4_0900_ai_ci), hist (history table,
100 rows, 30 purged = archived), nl (no secondary engine) and the view v6.
The setup purges hist with the tag: the account needs TRANSACTION_GTID_TAG.

```bash
cd project_duckdb
export MYSQL="mysql -S /opt/mysql_instances/master96/mysql.sock -u dba -p<password> windmills_small"
$MYSQL < duckdb_offload_setup.sql                    # re-creates all test tables
./duckdb_offload_compare.sh duckdb_offload_6c_queries.sql                  # 37/37 identical and offloaded
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6c_refusals.sql  # 5/5 (set operations moved to 6c-2, NOT IN to 6c-3)
./duckdb_offload_compare.sh duckdb_offload_6a_queries.sql                  # 19/19
./duckdb_offload_compare.sh duckdb_offload_6b_queries.sql                  # 45/45
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6b_refusals.sql  # 12/12 (two moved to 6c)
```

```sql
-- 1. A table without a DuckDB copy in the query
SET SESSION use_secondary_engine = ON, secondary_engine_cost_threshold = 0;
SELECT COUNT(*) FROM p6 JOIN nl ON nl.grp = p6.grp;      -- InnoDB answers (5715)
SET SESSION use_secondary_engine = FORCED;
SELECT COUNT(*) FROM p6 JOIN nl ON nl.grp = p6.grp;      -- EXPECTED ERROR 3889: ... [windmills_small.nl]

-- 2. History table in joins (FORCED; FULL needs DUCKDB_HISTORY_READER)
SELECT COUNT(h.id) FROM dim d LEFT JOIN hist h ON h.grp = d.grp;   -- LIVE: 49
SELECT COUNT(*) FROM dim d WHERE EXISTS (SELECT 1 FROM hist h WHERE h.grp = d.grp AND h.v < 10);  -- LIVE: 0
SET SESSION duckdb_history_scope = FULL;
SELECT COUNT(h.id) FROM dim d LEFT JOIN hist h ON h.grp = d.grp;   -- FULL: 70
SELECT COUNT(*) FROM dim d WHERE EXISTS (SELECT 1 FROM hist h WHERE h.grp = d.grp AND h.v < 10);  -- FULL: 7
SET SESSION duckdb_history_scope = LIVE;

-- 3. The session's optimizer_switch is unchanged after offload, refusal or fallback
SELECT @@optimizer_switch LIKE '%,materialization=on%';  -- 1 before and after
SELECT d.grp FROM dim d WHERE EXISTS (SELECT 1 FROM p6 WHERE p6.grp = d.grp);
SELECT @@optimizer_switch LIKE '%,materialization=on%';  -- 1

-- 4. The generated SQL of a join (aliases t0, t1, ...; EXISTS for IN)
SET optimizer_trace = 'enabled=on';
SELECT COUNT(*) FROM p6 WHERE grp IN (SELECT grp FROM dim WHERE region = 1);
SELECT TRACE FROM information_schema.OPTIMIZER_TRACE\G   -- ... WHERE EXISTS (SELECT 1 FROM ... AS t1 WHERE ...)
SET optimizer_trace = 'enabled=off';
SET SESSION use_secondary_engine = ON;
```

## 24. The applier waits for the InnoDB commit (DWL#4, 2026-11-13)

Semi-sync with no replica holds each commit after the binary log sync and
before the InnoDB commit, for `rpl_semi_sync_source_timeout`: the window in
which a transaction is in the binary log but not visible in InnoDB.

```sql
-- 1. SECONDARY_LOAD requires binlog_order_commits=ON
SET GLOBAL binlog_order_commits = OFF;
ALTER TABLE dim SECONDARY_UNLOAD;
ALTER TABLE dim SECONDARY_LOAD;      -- EXPECTED ERROR 3877: ... requires binlog_order_commits=ON
SET GLOBAL binlog_order_commits = ON;
ALTER TABLE dim SECONDARY_LOAD;

-- 2. The window (needs the semisync_source plugin in the plugin directory)
INSTALL PLUGIN rpl_semi_sync_source SONAME 'semisync_source.so';
SET GLOBAL rpl_semi_sync_source_timeout = 8000, rpl_semi_sync_source_enabled = ON;
SHOW BINARY LOG STATUS;              -- note the position
```

```sql
-- session A (blocks about 8 s):
INSERT INTO dim VALUES (50, 'fifty', 'f', 5, 'F');
```

```sql
-- session B, during the wait:
SHOW BINARY LOG STATUS;              -- the position has moved: the INSERT is in the binary log
SET SESSION use_secondary_engine = OFF;
SELECT COUNT(*) FROM dim WHERE grp = 50;   -- 0 (InnoDB has not committed)
SET SESSION use_secondary_engine = FORCED, duckdb_offload_wait_applied = OFF;
SELECT COUNT(*) FROM dim WHERE grp = 50;   -- 0 (DuckDB does not show it either)
-- after session A returns (and about 1 s): both 1
SET SESSION use_secondary_engine = ON, duckdb_offload_wait_applied = ON;
SET GLOBAL rpl_semi_sync_source_enabled = OFF;
UNINSTALL PLUGIN rpl_semi_sync_source;
DELETE FROM dim WHERE grp = 50;
```

## 25. Query offload, step 6c-2 (DWL#6)

Same tables (`duckdb_offload_setup.sql`).

```bash
cd project_duckdb
./duckdb_offload_compare.sh duckdb_offload_6c2_queries.sql                  # 26/26 identical and offloaded
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6c2_refusals.sql  # 6/6 refused as expected
# regressions: 6a 19/19, 6b 45/45, 6c 37/37, refusals 12/12 and 5/5
```

```sql
SET SESSION use_secondary_engine = FORCED;
-- 1. FOUND_ROWS() as on InnoDB
SELECT grp FROM dim UNION ALL SELECT grp FROM dim WHERE grp < 3;
SELECT FOUND_ROWS();                                    -- 10
-- 2. Mixed kinds are refused
SELECT ts FROM p6 WHERE id < 5 UNION ALL SELECT dt FROM p6 WHERE id < 5 ORDER BY 1;
-- EXPECTED ERROR 3877: ... set_operation_mixed_types
-- 3. The generated SQL
SET optimizer_trace = 'enabled=on';
SELECT grp FROM dim UNION SELECT grp FROM p6 UNION ALL SELECT grp FROM dim ORDER BY 1;
SELECT TRACE FROM information_schema.OPTIMIZER_TRACE\G -- ((...) UNION (...) UNION ALL (...)) AS t3 ORDER BY t3.c1
SET optimizer_trace = 'enabled=off';
SET SESSION use_secondary_engine = ON;
```

## 26. Query offload, step 6c-3 (DWL#6)

Same tables (`duckdb_offload_setup.sql`); dim.region and p6.qty have NULLs.

```bash
cd project_duckdb
./duckdb_offload_compare.sh duckdb_offload_6c3_queries.sql                  # 33/33 identical and offloaded
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6c3_refusals.sql  # 5/5 refused as expected
# regressions: 6a 19/19, 6b 45/45, 6c 37/37, 6c2 26/26; refusals 12/12, 5/5, 6/6
```

```sql
-- 1. A scalar subquery with more than one row: error 1242 in both
SET SESSION use_secondary_engine = OFF;
SELECT COUNT(*) FROM dim WHERE grp = (SELECT grp FROM p6 WHERE id < 10);
-- EXPECTED ERROR 1242: Subquery returns more than 1 row
SET SESSION use_secondary_engine = FORCED;
SELECT COUNT(*) FROM dim WHERE grp = (SELECT grp FROM p6 WHERE id < 10);
-- EXPECTED ERROR 1242: Subquery returns more than 1 row (DuckDB error: Invalid Input Error: More than one row ...)
SELECT d.grp FROM dim d ORDER BY (SELECT p6.qty FROM p6 WHERE p6.grp = d.grp), d.grp;
-- EXPECTED ERROR 1242: ... (DuckDB error: ... scalar subqueries can only return a single row ...)
-- 2. The SQL of an IN left as a subquery (no LIMIT 1, no in-to-exists condition)
SET optimizer_trace = 'enabled=on';
SELECT d.grp, d.region NOT IN (SELECT region FROM dim WHERE grp > 7) FROM dim d ORDER BY 1;
SELECT TRACE FROM information_schema.OPTIMIZER_TRACE\G -- (NOT (t0."region" IN (SELECT ... WHERE (t1."grp" > 7))))
SET optimizer_trace = 'enabled=off';
SET SESSION use_secondary_engine = ON;
```

## 27. Cached tables, KILL and timeout, step 6d-2 (DWL#6)

```sql
-- 1. A cached secondary table across unload/reload, DROP + re-create, enable OFF/ON
SET SESSION use_secondary_engine = FORCED;
SELECT COUNT(*) FROM dim;                              -- 7 (the server caches the table)
ALTER TABLE dim SECONDARY_UNLOAD;
SELECT COUNT(*) FROM dim;                              -- EXPECTED ERROR 3877: Table has not been loaded
ALTER TABLE dim SECONDARY_LOAD;
SELECT COUNT(*) FROM dim;                              -- 7
SET GLOBAL duckdb_enable = OFF;
SELECT COUNT(*) FROM dim;                              -- EXPECTED ERROR 3877: Table has not been loaded
SET GLOBAL duckdb_enable = ON;                         -- wait a few seconds
SELECT COUNT(*) FROM dim;                              -- 7

-- 2. KILL QUERY (session A runs, session B kills after a few seconds)
-- A:
SELECT COUNT(*) FROM g JOIN p6 ON g.amount < p6.amount;
-- B:
SELECT id FROM information_schema.processlist WHERE info LIKE 'SELECT COUNT(*) FROM g JOIN%';
KILL QUERY <id>;
-- A, within about a second:
-- EXPECTED ERROR 1317: Query execution was interrupted (DuckDB error: INTERRUPT Error: Interrupted!)
SHOW GLOBAL STATUS LIKE 'duckdb_executor_threads_busy';   -- 0

-- 3. max_execution_time
SET SESSION max_execution_time = 1500;
SELECT COUNT(*) FROM g JOIN p6 ON g.amount < p6.amount;
-- EXPECTED ERROR 3024: Query execution was interrupted, maximum statement execution time exceeded (DuckDB error: ...)
SET SESSION max_execution_time = 0;
SET SESSION use_secondary_engine = ON;
```

Stress (optional, bash): two sessions run an offloaded join every 0.3 s while
a third runs `ALTER TABLE dim SECONDARY_UNLOAD; ALTER TABLE dim
SECONDARY_LOAD` six times: every query gives the right count or "Table has
not been loaded"; the server stays up.

## 28. EXPLAIN with the DuckDB SQL and plan, step 6d-1 (DWL#6)

```sql
SET SESSION use_secondary_engine = FORCED;
EXPLAIN FORMAT=TREE SELECT d.name, COUNT(*) FROM p6 JOIN dim d ON d.grp = p6.grp WHERE p6.qty > 0 GROUP BY d.name ORDER BY 1;
SHOW WARNINGS;     -- Note 1003, Note 3877 "DuckDB SQL: SELECT ...", Notes 3877 "DuckDB plan (1/n) ..."
EXPLAIN FORMAT=JSON SELECT COUNT(*) FROM p6 WHERE grp IN (SELECT grp FROM dim);
SHOW WARNINGS;     -- the same notes
SET SESSION use_secondary_engine = ON;
EXPLAIN SELECT ci, COUNT(*) FROM dim GROUP BY ci;
SHOW WARNINGS;     -- no DuckDB notes (refused: collation)
EXPLAIN ANALYZE SELECT COUNT(*) FROM p6;   -- with FORCED: EXPECTED ERROR 1235 (server)
```

## 29. Performance, step 6d-3 (DWL#6): release build, production-like workload

Release build (separate directory, the debug build stays as it is):

```bash
mkdir -p /opt/mysql_source/percona-server-9.7-release && cd /opt/mysql_source/percona-server-9.7-release
cmake ../percona-server -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDUCKDB_BUILD_TYPE=RelWithDebInfo \
  -DWITH_DUCKDB=ON -DWITH_UNIT_TESTS=OFF -DWITH_ROUTER=OFF \
  -DWITH_SSL=system -DWITH_CURL=system -DWITH_LDAP=system -DWITH_KERBEROS=system \
  -DCOMPILATION_COMMENT="Marco compile 9.7 with DuckDB (release)"
cmake --build . -j 6 --target mysqld duckdb mysql mysqladmin
# runtime_output_directory/mysqld and plugin_output_directory/ha_duckdb.so
```

Workload, on an instance running the release mysqld and plugin (a database
`perf`; the setup needs TRANSACTION_GTID_TAG, q12 needs DUCKDB_HISTORY_READER):

```bash
cd project_duckdb/perf
(echo "SET @turbines = 200, @days = 365;"; cat duckdb_perf_setup.sql) | mysql ... perf
export MYSQL="mysql -S <socket> -u dba -p<password> perf"
./duckdb_perf_run.sh                      # RUNS=3 by default: median of 3
# columns: query, ran_in (InnoDB/DuckDB), innodb_s, duckdb_s, speedup, note
# expected: no "RESULTS DIFFER"; q13-q16 on InnoDB (lookup, index range,
# _ci collation, window function); the reports in DuckDB
```

## 30. Exact division, AVG, and duckdb_offload_min_rows (DWL#6, 2026-11-14)

```bash
cd project_duckdb
$MYSQL < duckdb_offload_setup.sql                                         # adds table dv
./duckdb_offload_compare.sh duckdb_offload_div_queries.sql                # 18/18 identical and offloaded
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6b_refusals.sql # 11/11 (qty / 3 moved to the division list)
# regressions: 6a 19/19, 6b 45/45, 6c 37/37, 6c2 26/26, 6c3 33/33, refusals 5/5, 6/6, 5/5
```

```sql
-- duckdb_offload_min_rows on the perf database (section 29), with ON:
SET SESSION use_secondary_engine = ON;
SELECT * FROM reading WHERE id = 1234567;                    -- InnoDB (not tried)
SELECT ts, power_kw FROM reading WHERE turbine_id = 42 AND ts >= '2025-12-31 23:00:00';  -- InnoDB
SELECT turbine_id, COUNT(*) FROM reading WHERE turbine_id IN (SELECT id FROM turbine WHERE site_id = 3) GROUP BY turbine_id;
SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries';            -- +1 (cost below the threshold, tried by the rule)
SET SESSION duckdb_offload_min_rows = 0;                     -- the same query stays on InnoDB
```

## 31. Moving the lake, duckdb_lake_relocate (DWL#2, 2026-11-14)

On a throwaway instance (stop it first; paths are examples):

```bash
mv /path/inst /path/inst_moved
sed -i "s|/path/inst|/path/inst_moved|g" /path/inst_moved/my.cnf /path/inst_moved/logs/binlog.index
mysqld --defaults-file=/path/inst_moved/my.cnf &
# error log: DuckDB lake moved: the catalog's data path .../inst/duckdb_data/data/ is now .../inst_moved/duckdb_data/data/
# queries in DuckDB give the same results; a second restart logs no move
# with --loose-duckdb-lake-relocate=OFF after another move: "cannot attach ... (with duckdb_lake_relocate=ON ...)", plugin not loaded
# with a Parquet file of data/ moved away: "... 1 of its N files are not under ...: not relocated", plugin not loaded
```

## 32. Weight strings, storage (DWL#6 step A1, 2026-11-14)

```sql
CREATE TABLE wt (id INT PRIMARY KEY, c VARCHAR(20) COLLATE utf8mb4_0900_ai_ci, g VARCHAR(20) COLLATE utf8mb4_general_ci,
  l VARCHAR(20) CHARACTER SET latin1 COLLATE latin1_swedish_ci, b VARCHAR(20) COLLATE utf8mb4_bin) SECONDARY_ENGINE = duckdb;
INSERT INTO wt VALUES (1, 'apple', 'apple', 'été', 'x'), (2, 'Apple', 'APPLE  ', 'ETE', 'x '), (3, 'ápple', 'apple ', 'ete', 'X'),
  (4, 'banana', 'Banana', 'Ä', 'y'), (5, NULL, NULL, NULL, NULL), (6, '', '', '', ''), (7, 'apple ', 'apple', 'été ', 'x'), (8, 'zebra', 'ZEBRA', 'z', 'z');
ALTER TABLE wt SECONDARY_LOAD;
INSERT INTO wt VALUES (9, 'APPLE', 'Apple', 'ÉTÉ', 'x'), (10, 'Zebra', 'zebra  ', 'Z', 'Z');
UPDATE wt SET c = 'BANANA', l = 'ä' WHERE id = 4;
SELECT COUNT(DISTINCT c), COUNT(DISTINCT g), COUNT(DISTINCT l) FROM wt;   -- 5 4 4
```

Stop the server, then inspect the lake with `tools/duckq` (build line in
`tools/duckq.c`):

```bash
L=<datadir or duckdb_data_path>
./duckq :memory: "ATTACH 'ducklake:$L/catalog.ducklake' AS lake (DATA_PATH '$L/data/', READ_ONLY)" \
  "SELECT string_agg(column_name, ',' ORDER BY column_index) FROM duckdb_columns() WHERE database_name='lake' AND table_name='wt'" \
  "SELECT count(DISTINCT __w_c), count(DISTINCT __w_g), count(DISTINCT __w_l) FROM lake.<db>.wt" \
  "SELECT count(*) FILTER (WHERE c IS NOT NULL AND __w_c IS NULL) FROM lake.<db>.wt"
# id,c,g,l,b,__w_c,__w_g,__w_l   (no weight for utf8mb4_bin)
# 5 4 4                           (as MySQL)
# 0                               (applier rows have weights)
```

History table: purge rows with the tag, `ALTER TABLE ... SECONDARY_LOAD`
(resync), then `ALTER TABLE ... MODIFY c ... COLLATE utf8mb4_0900_as_cs`:
with duckq, archived rows have weights (none missing) and their distinct
count follows the new collation.

## 33. Weight strings, queries (DWL#6 step A2, 2026-11-14)

```bash
cd project_duckdb
$MYSQL < duckdb_offload_setup.sql                                          # adds cw, cwd (weights)
./duckdb_offload_compare.sh duckdb_offload_a2_queries.sql                  # 32/32 identical and offloaded
EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_a2_refusals.sql  # 7/7 refused as expected
# regressions: 6a 19/19, 6b 45/45, 6c 37/37, 6c2 26/26, 6c3 33/33, div 18/18;
# refusals 6b 9/9, 6c 4/4 (three _ci cases moved to the A2 list), 6c2 6/6, 6c3 5/5
```

A copy loaded before A1 has no weights: a query on its `_ci` columns is
refused with "copy without weights: reload it" (SECONDARY_UNLOAD +
SECONDARY_LOAD, or SECONDARY_LOAD for a history table).

## 34. FULL scope guard (DWL#7, 2026-11-15)

Needs DUCKDB_HISTORY_READER; table hist of duckdb_offload_setup.sql (70 live,
30 archived), plus:

```sql
-- FULL scope needs the privilege (nobody has it until granted, root
-- included): without it the SET below fails with error 1227 and the session
-- stays LIVE, so none of the checks apply. Grant it, then reconnect.
GRANT DUCKDB_HISTORY_READER ON *.* TO CURRENT_USER();

CREATE OR REPLACE VIEW vh AS SELECT id, grp, v FROM hist;
DROP PROCEDURE IF EXISTS ph;
CREATE PROCEDURE ph() SELECT COUNT(*) FROM hist;
DROP TABLE IF EXISTS hcopy;
CREATE TABLE hcopy (id INT PRIMARY KEY, v INT);
SELECT PLUGIN_NAME, PLUGIN_STATUS FROM information_schema.PLUGINS WHERE PLUGIN_NAME LIKE 'duckdb%';  -- duckdb, duckdb_guard ACTIVE

SET SESSION use_secondary_engine = OFF, duckdb_history_scope = FULL;   -- must succeed
SELECT @@session.duckdb_history_scope;                                    -- FULL
SELECT COUNT(*) FROM hist;        -- EXPECTED ERROR 3877: ... requires DuckDB execution, but use_secondary_engine=OFF
SET SESSION use_secondary_engine = ON;
SELECT COUNT(*) FROM hist;        -- 100 (below the cost threshold, forced to DuckDB)
SELECT id, ROW_NUMBER() OVER (ORDER BY id) FROM hist LIMIT 1;   -- EXPECTED ERROR 3877: ... window
INSERT INTO hcopy SELECT id, v FROM hist;                       -- EXPECTED ERROR 3877: ... only a SELECT can read it
SELECT COUNT(*) FROM hcopy;                                     -- 0
SELECT COUNT(*) FROM vh; CALL ph();                             -- 100, 100
SET SESSION duckdb_history_scope = LIVE;
SELECT COUNT(*) FROM hist;                                      -- 70
SELECT /*+ SET_VAR(duckdb_history_scope = FULL) */ COUNT(*) FROM hist;   -- 100
PREPARE s FROM 'SELECT COUNT(*) FROM hist';
SET SESSION duckdb_history_scope = FULL;  EXECUTE s;            -- 100
SET SESSION duckdb_history_scope = LIVE;  EXECUTE s;            -- 70
SET GLOBAL duckdb_enable = OFF;
SET SESSION duckdb_history_scope = FULL;
SELECT COUNT(*) FROM hist;        -- EXPECTED ERROR 3877: ... the DuckDB lake is not open
SET GLOBAL duckdb_enable = ON;
SET SESSION duckdb_history_scope = LIVE;
SHOW GLOBAL STATUS LIKE 'duckdb_guard%';
-- restart with --loose-duckdb-guard=OFF: SET duckdb_history_scope = FULL;
--   EXPECTED ERROR 3877: ... requires the duckdb_guard plugin, which is not active
```

## 35. INFORMATION_SCHEMA.DUCKDB_TABLES (2026-11-16)

On any instance with loaded tables (MTR: `i_s_tables`):

```sql
SELECT PLUGIN_NAME, PLUGIN_STATUS, PLUGIN_TYPE FROM information_schema.PLUGINS
 WHERE PLUGIN_NAME = 'DUCKDB_TABLES';                  -- ACTIVE, INFORMATION SCHEMA
SELECT * FROM information_schema.DUCKDB_TABLES ORDER BY TABLE_SCHEMA, TABLE_NAME\G
-- one row per loaded table: STATE LOADED, RETAIN_HISTORY YES for history
-- tables, PRIMARY_KEY, WEIGHT_COLUMNS (the _ci / latin1 columns, not
-- utf8mb4_bin), the load binlog file / position / GTID set.
-- Compare with the tables having SECONDARY_LOAD="1":
SELECT TABLE_SCHEMA, TABLE_NAME FROM information_schema.TABLES
 WHERE CREATE_OPTIONS LIKE '%SECONDARY_LOAD="1"%' ORDER BY 1, 2;
```

Checks: a RENAME TABLE shows the new name after the applier read it; a
dropped history table shows `STALE` until discarded; an unloaded table has no
row; an account without PROCESS sees no rows; `duckdb_enable=OFF`: no rows.

## 36. YEAR and BIT (2026-11-16)

Table yb of `duckdb_offload_setup.sql` (re-run the setup, or only its
"YEAR and BIT" block, which needs table n); MTR: `offload_yb`.

```bash
MYSQL="mysql -S <socket> -u <user> -p<password> windmills_small" \
  ./duckdb_offload_compare.sh duckdb_offload_yb_queries.sql        # 33/33
EXPECT=refused MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_yb_refusals.sql  # 5/5
```

Checks: YEAR 0000 shown as `0000` for the column, `0` for MAX(y); BIT values
as bytes (`SELECT b` in the mysql client prints raw bytes, compare with
`HEX(b)` or `b + 0` under OFF); rows inserted, updated and deleted after the
load (ids 2, 3, 4, 100, 101) are the same in DuckDB; `b64 + 0` from 2^63
positive. Regression: all earlier lists (6b refusals now 9/9 with `u - 1`).

## 37. TIME (2026-11-16)

Table tt of `duckdb_offload_setup.sql` (its "TIME" block needs table n);
MTR: `offload_time`.

```bash
MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_tt_queries.sql                 # 24/24
EXPECT=refused MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_tt_refusals.sql  # 7/7
```

Checks: negative values, more than 24 hours, -838:59:59 / 838:59:59,
fractions of TIME(3) and TIME(6) shown as InnoDB shows them; constants in
every form MySQL accepts; rows changed after the load the same.

## 38. ENUM and SET (2026-11-16)

Table et of `duckdb_offload_setup.sql` (its "ENUM and SET" block needs
table n); MTR: `offload_enum`.

```bash
MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_et_queries.sql                 # 28/28
EXPECT=refused MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_et_refusals.sql  # 6/6
```

History across a definition change (needs DUCKDB_HISTORY_READER):

```sql
CREATE TABLE eh (id INT PRIMARY KEY, e ENUM('a','b','c'), s SET('x','y','z')) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
INSERT INTO eh VALUES (1,'a','x'), (2,'b','x,z'), (3,'c','y'), (4,'a','z'), (5,'b',''), (6,NULL,NULL);
ALTER TABLE eh SECONDARY_LOAD;
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge'; DELETE FROM eh WHERE id IN (1, 2); SET gtid_next = 'AUTOMATIC';
ALTER TABLE eh MODIFY e ENUM('c','b','a','d'), MODIFY s SET('z','y','x');
-- after the refresh (duckdb_apply_ddl_refreshes + 1, failures 0):
SET SESSION duckdb_history_scope = FULL;
SELECT id, e, e + 0, s, s + 0 FROM eh ORDER BY id;
-- 1 a 3 x 4 | 2 b 2 z,x 5 | 3 c 1 y 2 | 4 a 3 z 1 | 5 b 2 '' 0 | 6 NULL ...
-- LIVE: the same as InnoDB (ids 3-6)
```

## 39. Foreign keys and the TPC-H check (2026-11-17)

Foreign keys (MTR `foreign_keys`): a parent with children `ON DELETE
CASCADE`, `ON UPDATE CASCADE`, `ON DELETE SET NULL`, RESTRICT and a history
child; all load; after deleting / updating parents every copy equals InnoDB;
a tagged purge on the parent archives the cascaded rows of the history
child. With `innodb_native_foreign_keys=ON` (restart) the cascading child
is refused with the reason, the parent and the RESTRICT child load.

TPC-H (`tpch/README.md`): HammerDB's MySQL schema with its foreign keys,
generated data (SF ~0.01), the 22 queries compared OFF / FORCED.

```bash
mysql ... < tpch/tpch_schema.sql && mysql ... < tpch/tpch_data.sql
MYSQL="mysql ... tpch" ./duckdb_offload_compare.sh tpch/tpch_queries.sql   # 7/22 on 2026-11-17
```

## 40. LIKE on binary collations (2026-11-17)

Table lt of `duckdb_offload_setup.sql` ("LIKE on binary collations" block);
MTR `offload_like`.

```bash
MYSQL="mysql --default-character-set=utf8mb4 ... windmills_small" ./duckdb_offload_compare.sh duckdb_offload_like_queries.sql  # 19/19
EXPECT=refused MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_like_refusals.sql                                       # 1/1
```

Checks: trailing spaces matter (`'abc ' LIKE 'abc'` false), case and
accents are distinct, `_` matches one character (also é, €), backslash
escapes as in MySQL (also with NO_BACKSLASH_ESCAPES: no escape character),
ESCAPE clause; LIKE with a `_ci` COLLATE refused. TPC-H: 12 of 22.

## 41. Grouped CHAR values in joins (2026-11-17)

MTR `offload_grouped_char`: a join grouped by CHAR columns under
latin1_bin and utf8mb4_bin (values with trailing spaces, one inserted after
the load) equals InnoDB; VARCHAR and `_ci` CHAR refused
(`grouped_string_value_in_join`); `PAD_CHAR_TO_FULL_LENGTH` refused.
TPC-H: 16 of 22.

## 42. Functions reviewed against DuckDB (2026-11-17)

Table fx of `duckdb_offload_setup.sql` ("Functions reviewed" block); MTR
`offload_functions`. The table of functions and guards: DWL-06, "Function
review against DuckDB".

```bash
MYSQL="mysql --default-character-set=utf8mb4 ... windmills_small" ./duckdb_offload_compare.sh duckdb_offload_fn_queries.sql  # 28/28
EXPECT=refused MYSQL="..." ./duckdb_offload_compare.sh duckdb_offload_fn_refusals.sql                                     # 11/11
```

TPC-H: 19 of 22.

## Results log

| Date | Build | Sections run | Result | Notes |
|---|---|---|---|---|
| 2026-11-17 | clang-21, C++23, debug | 42, function review against DuckDB (throwaway instance; MTR offload_functions, offload_refusals) | pass after fixes | fn 28/28 identical and offloaded, refusals 11/11; every earlier list unchanged (6b refusal TRUNCATE now translated, replaced by EXP); TPC-H 19 of 22 (Q7, Q8, Q9). Found by the edge values: DuckDB substring / left / right with 0 or negative values, lpad with an empty pad (error), greatest skipping NULL, sqrt / ln domain (NaN, -inf), extract(microsecond) including seconds, upper('ß'), ascii (code point): guarded or refused; MySQL computes dates down to year 0 (first guard said 1000); the parser builds LOCATE with the string first; constants inside these functions took the date type of an enclosing comparison; the binary-equality rule first also changed GROUP BY keys (TPC-H regression, caught, limited to comparisons) |
| 2026-11-17 | clang-21, C++23, debug | 41, grouped CHAR in joins (throwaway instance; MTR offload_grouped_char) | pass | 3/3 identical (latin1_bin, utf8mb4_bin, trailing spaces from load and binary log), VARCHAR and _ci refused 2/2, PAD_CHAR_TO_FULL_LENGTH refused; every earlier list unchanged; TPC-H 16 of 22 (Q4, Q5, Q10, Q12 added). Found: PAD_CHAR_TO_FULL_LENGTH was not handled (CHAR results without padding), now refused |
| 2026-11-17 | clang-21, C++23, debug | 40, LIKE on binary collations (throwaway instance; MTR offload_like, offload_refusals, offload_weights) | pass after a fix | like 19/19 identical and offloaded, refusal 1/1, NO_BACKSLASH_ESCAPES identical; every earlier list unchanged; TPC-H 12 of 22 (Q2, Q13, Q14, Q20 added). Fixed (older than this step, every collation): LIKE without ESCAPE used no escape in DuckDB, MySQL uses `\\` (Item_func_like::escape() now passed always) |
| 2026-11-17 | clang-21, C++23, debug | DECIMAL above 38 digits, duckdb_decimal_overflow (throwaway instance; MTR decimal_overflow) | pass after a fix | values that fit: identical (run first); SUM of two 38-digit values and a 28-digit value * 1.0000000001: FALLBACK gives InnoDB's values with ON and the reason with FORCED, ROUND as FALLBACK (overflow in DuckDB's own arithmetic), ERROR fails also with ON; a 31-digit value / 3: ROUND gives 3 decimals with a warning. Fixed: an exact division in WHERE overflowed in DuckDB's HUGEINT intermediate without being marked to run first (declared precision below 38): the dividend's digits + shift now decide. BIGNUM tested and unusable (DOUBLE arithmetic). Regressions: all lists pass; TPC-H 8 of 22 (Q11) |
| 2026-11-17 | clang-21, C++23, debug | TUTORIAL.md, every block of sections 2-15 on a fresh server (port 3410) | pass after fixes | only the four errors the tutorial shows on purpose; 4.5: Last_query_cost and the trace show cost 30174 below the threshold, a lower session threshold and each SET_VAR hint offload the query; 13: duckq built from the build libraries, reads the stopped server's lake (registry, query, live / archived counts), exports Parquet and CSV. Fixed over three runs: world-readable lake warning (chmod), `\G` in piped clients, waits after DDL (wait_applier), the 8.4 partition routine, the order of the clean-up |
| 2026-11-17 | clang-21, C++23, debug | 39, foreign keys; TPC-H check; fixes (throwaway instance; MTR foreign_keys, transactions, duckdb_plugin) | pass after fixes | foreign keys with every rule, cascades followed, tagged cascade archived; innodb_native_foreign_keys=ON refuses only the cascading child. TPC-H: 7 of 22 offloaded and identical, 15 refused with known reasons, no wrong result. Fixed: FORCED with autocommit=0 crashed a debug server (empty offload reason, assertion in sql_select.cc); `duckdb_offload_min_rows` and `duckdb_offload_wait_applied` ignored SET_VAR hints (not hint-updatable); the recorded duckdb_plugin result lacked the DUCKDB_TABLES plugin. Verified for the docs: Last_query_cost and the optimizer trace show the threshold decision; duckq reads a stopped server's lake, exports Parquet; with mysqld running the catalog is locked |
| 2026-11-16 | clang-21, C++23, debug | 37 TIME, 38 ENUM and SET (throwaway instance; MTR offload_time, offload_enum, offload_yb; unit tests) | pass after fixes | tt 24/24, refusals 7/7; et 28/28, refusals 6/6; yb 33/33, refusals 7/7; all earlier lists unchanged (6a 19, 6b 45, 6c 37, 6c-2 26, 6c-3 33, div 18, a2 32; refusals 9, 4, 6, 5, 7); history table eh across ENUM/SET reorder: archived indexes recomputed, SET text rewritten (x,z -> z,x), LIVE = InnoDB; unit tests 41 (YEAR, BIT, TIME2, ENUM, SET decoding). Found and fixed: a TIME column inside LIKE / IFNULL would have been translated as its microseconds (special_args() guard for TIME, ENUM, SET, YEAR, BIT); archived SET text kept the old element order. MTR type tests use only table n (the g setup took over 20 minutes with three workers on the HDD) |
| 2026-11-16 | clang-21, C++23, debug | 36, YEAR and BIT (throwaway instance; MTR offload_yb, offload_refusals) | pass after fixes | yb 33/33 identical and offloaded (after load, inserts, updates, delete), refusals 5/5; regressions 6a 19/19, 6b 45/45, 6c 37/37, 6c-2 26/26, 6c-3 33/33, div 18/18, a2 32/32, refusals 6b 9/9 (u + 1 now exact, replaced by u - 1), 6c 4/4, 6c-2 6/6, 6c-3 5/5, a2 7/7. Found: MySQL sends a YEAR column zero-filled but a YEAR expression as a number (holder per case); an unsigned BIGINT from 2^63 was sent negative (existing bug, fixed); the first setup used ELT (strings: BIT stored ASCII bytes) |
| 2026-11-16 | clang-21, C++23, debug | 35, INFORMATION_SCHEMA.DUCKDB_TABLES (MTR i_s_tables) | pass | plugin ACTIVE, type INFORMATION SCHEMA; t1 LOADED, PK id, weights s, l (not the utf8mb4_bin column); h1 LOADED YES, PK id1, id2; load file, position and GTID set present; not-loaded t2 absent; RENAME followed (t3); dropped h1 STALE; unload removes the row; no rows without PROCESS, 1 with it; duckdb_enable=OFF: 0 rows, ON: back |
| 2026-11-16 | clang-21, C++23, debug | MTR suite (16 tests, --parallel=2, --testcase-timeout=45) | pass | all 16 pass after the guard and super_read_only changes (new test super_read_only). A first run timed out in guard's setup (200k-row INSERT took 15 min with swap full): not a plugin problem; tests took 1-22 min each on the loaded machine |
| 2026-11-16 | clang-21, C++23, debug | 18 cases 7 and 10, guard and super_read_only fixes (replica pair 3402/3403, MTR guard, super_read_only) | pass after fixes | guard: UPDATE, DELETE, TRUNCATE, DELETE and UPDATE with a subquery on a history table stopped under FULL, nothing changed (MTR guard). super_read_only=ON on the replica: before the fix the refresh failed (1290) and mt and rh went stale; rh could not be reattached (MODIFY BIGINT + ADD COLUMN: 2 differences, archived rows lost on that test table). After: classic mt MODIFY BIGINT, RENAME, TRUNCATE of r1b, history rh2 MODIFY BIGINT + ADD COLUMN: 4 refreshes, 0 failures, DuckDB = InnoDB, FULL 842 with archived rows; SECONDARY_UNLOAD and INSERT by root refused (1290); no new replica GTID. Seen: srv_session THDs are not audited (m_audited=false): the internal session enables auditing |
| 2026-11-15 | clang-21, C++23, debug | 18, replica (throwaway source 3402 without the plugin, replica 3403 with it) | pass, one open point | CREATE TABLE ... SECONDARY_ENGINE=duckdb and retain_history attribute accepted by the source without the plugin; load on the replica with sql_log_bin=0: gtid_executed equal. DML and a 55k-row transaction in 6 pieces (batch_max_rows 10000): DuckDB = InnoDB. MODIFY BIGINT, ADD COLUMN, RENAME, TRUNCATE: replication never stopped, 3 refreshes, 1 rename, 0 failures. 8 concurrent source sessions, 4 parallel workers (all used): equal, no errant GTID. STOP REPLICA during writes and a replica restart during writes: caught up, equal. Tagged purge: tag in the replica's GTID set, 1 purge, 100 archived rows; FULL 990 rows (890 live + 100 archived), FULL+OFF stopped. Joins, IN and scalar subqueries, COUNT(DISTINCT) equal; a grouped _ci name in a join and a UNION of _ci columns refused (documented). Seen: SECONDARY_LOAD/UNLOAD are binlogged (errant GTID without sql_log_bin=0); DUCKDB_HISTORY_READER must be granted on the replica locally. Open point: UPDATE of a history table under FULL not stopped by the guard (UPDATE event, not READ) |
| 2026-11-15 | clang-21, C++23, debug | MTR suite storage/duckdb/mysql-test/duckdb (15 tests, --parallel=2) | pass after fixes | all 15 pass (replay without --record: 14, history fixed to record counter deltas, then passed after guard on the same server). Fixed while building it: wait include forced to the secondary engine in FORCED sessions, trailing SQL comments in includes, duckdb_discard_stale_copy / duckdb_apply_start_position kept their last value (now empty after running, plugin change), reader state 'waiting to commit' added so that 'idle' means everything committed (plugin change). About 1.5 hours on the HDD machine |
| 2026-11-15 | clang-21, C++23, debug | 34, DWL#7 (throwaway instance) | pass | guard ACTIVE; FULL+OFF error; FULL+ON below threshold 100 (LIVE 70); window under FULL: error; hint, view, procedure correct; INSERT ... SELECT stopped; prepared statement FULL 100 / LIVE 70 / OFF 70; duckdb_enable=OFF and stale copy stopped; guard disabled: FULL refused; regressions all pass (A2 Q29 needed a tie-breaker: 't' and 'T' are equal in 0900_ai_ci) |
| 2026-11-14 | clang-21, C++23, debug | 33, step A2 (throwaway instance) | pass | A2 32/32 identical and offloaded at the first run, refusals 7/7; regressions all pass; three earlier _ci refusals now offloaded (identical) and moved |
| 2026-11-14 | clang-21, C++23, debug | 32, step A1 (throwaway instance) | pass | weight columns for 0900_ai_ci, general_ci, latin1 (none for utf8mb4_bin); distinct weights = MySQL COUNT(DISTINCT) 5/4/4; applier rows weighted; history archived rows weighted after resync, recomputed after a collation change (as_cs: 6); reattach structure check passes; unit tests 39; all offload lists pass after reloading every test table |
| 2026-11-14 | clang-21, C++23, debug | 31 (throwaway instance) | pass | moved with relocate ON: logged, same results, next start normal; OFF: refused, plugin not loaded; a missing Parquet file: refused (1 of 13 files); moved back: relocated. The perf instance was moved to /opt/mysql_instances/duckdb_perf the same day by updating the catalog by hand (before this variable existed). Regressions all pass |
| 2026-11-14 | clang-21, C++23, debug + release | 30 (throwaway instances) | pass after fixes | exact division and AVG 18/18 with div_precision_increment 0, 4, 9; regressions all pass. min_rows rule: PK lookup and index range not tried, a non-unique lookup join tried and offloaded. Fixed during the test: rounding where MySQL truncates (increments 0, 9), F digits where MySQL uses the rounded value (a / b * 2), an overflow scaling decimals by 10^18 |
| 2026-11-13 | clang-21, C++23, RelWithDebInfo | 29, step 6d-3 (throwaway release instance, 10.5M readings) | pass, findings | all results identical; offloaded 55x-584x (q02 31.9 s -> 0.055 s); load of 10.5M rows 15.4 s. Not offloaded although heavy: _ci collations (GROUP BY names, string literals), exact decimal division, InnoDB cost underestimate (382 vs 52,560 rows per key) below the threshold. perf/results-2026-11-13-release.txt |
| 2026-11-13 | clang-21, C++23, debug | 29, workload check (throwaway instance, 20 turbines x 30 days) | pass | setup and runner work; all results identical; at 86,400 readings most queries stay below the default cost threshold (InnoDB); q15 refused (collation:name), q16 (window); debug timings not meaningful (release build compiling at the same time) |
| 2026-11-13 | clang-21, C++23, debug | 28, step 6d-1 (throwaway instance) | pass | EXPLAIN TREE, JSON and traditional: note with the DuckDB SQL and DuckDB's plan in 7 to 10 numbered notes; no notes for a refused query or outside EXPLAIN |
| 2026-11-13 | clang-21, C++23, debug | 27, step 6d-2 (throwaway instance) | pass | registry entries never freed: cached tables across unload/reload, RENAME and back, DROP + re-create, enable OFF/ON, history DROP + discard + re-create; stress with two sessions during six unload/load cycles (about 680 queries, right result or clean refusal, no crash). KILL QUERY: DuckDB interrupted in about 1 s, executor freed, error 1317 with DuckDB's message; max_execution_time: 3024. Regressions all pass. Seen: loads took up to 22 s while the release build ran (HDD) |
| 2026-11-13 | clang-21, C++23, debug | 26, step 6c-3 (throwaway instance) | pass after fixes | 6c-3 33/33 identical and offloaded, refusals 5/5; regressions 6a 19/19, 6b 45/45, 6c 37/37, 6c-2 26/26, refusals 12/12, 5/5, 6/6; 1242 with both messages for a scalar subquery with several rows. Fixed: 6c-1 returned rows instead of error 1242 for a rewritten scalar subquery with several rows (m_reject_multiple_rows ignored); a crash on IN in the select list (Item_in_optimizer has one argument at first); IN and NOT IN wrong with the in-to-exists LIMIT 1 (m_internal_limit) |
| 2026-11-13 | clang-21, C++23, debug | 23, 24, 25, steps 6c-1, 6c-2, after_commit (reference instance) | pass | reported by Marco: all lists as expected |
| 2026-11-13 | clang-21, C++23, debug | 25, step 6c-2 (throwaway instance) | pass after a fix | 6c-2 26/26 identical and offloaded, refusals 6/6; regressions 6a 19/19, 6b 45/45, 6c 37/37, refusals 12/12 and 6/6; FOUND_ROWS() 10 and 2 as InnoDB. Fixed: TIMESTAMP UNION ALL DATETIME was offloaded with TIMESTAMP values in UTC (one hour off): the NULL-literal test used is_null() on a column; a FROM-less operand (SELECT 'x') was refused as no_table |
| 2026-11-13 | clang-21, C++23, debug | 24 (throwaway instance) | pass | reader woken by after_commit: during an 8 s semi-sync wait the binary log had moved (481 -> 798), InnoDB 0, DuckDB 0; both 1 after the commit. SECONDARY_LOAD refused with binlog_order_commits=OFF. Regressions after the change: 6a 19/19, 6b 45/45, 6c 37/37, refusals 12/12 and 8/8, unit tests 38. Seen: a batch of three tables took about 4 s to commit (HDD), so a query right after it can exceed the 5 s freshness wait (applier_behind); re-running the setup reattached the old hist copy (FULL 130 rows): the setup now discards it |
| 2026-11-13 | clang-21, C++23, debug | 23, step 6c-1 (throwaway instance) | pass after fixes | 6c 37/37 identical and offloaded, 6c refusals 8/8, regressions 6a 19/19, 6b 45/45, 6b refusals 12/12; table without copy: InnoDB with ON, 3889 with FORCED; hist LIVE 49/0, FULL 70/7; optimizer_switch unchanged after offload, refusal and fallback. Fixed during the test: NOT EXISTS crashed the server in the join planner (statement-wide best cost in compare_secondary_engine_cost); EXISTS refused (semi-join materialization rewrote WHERE to <subquery2>); YEAR() column of a derived table refused. Seen: correlated scalar subqueries are rewritten by the server into joins and offloaded |
| 2026-11-13 | clang-21, C++23, debug | 22, step 6b (reference instance) | pass | setup with duckdb_offload_setup.sql; 45/45 identical and offloaded, 14/14 refused as expected, 6a regression 19/19 |
| 2026-11-12 | clang-21, C++23, debug | 22, step 6b (throwaway instance) | pass after fixes | 45/45 identical and offloaded, 14/14 refused as expected, 6a 19/19; TIMESTAMP ranges and DST-ambiguous IN lists equal under +05:30, -08:00, SYSTEM. Fixed during the test: a server crash on GROUP BY DAYOFWEEK(dt) (holders typed from the select list instead of JOIN::fields, which the metadata used); YEAR() refused (result type YEAR); integer DIV refused as exact division; SUM of DECIMAL refused above 38 digits; MIN/MAX of utf8mb4_bin ties ('' and ' ') picked a different row; LIKE on a binary column failed in DuckDB's binder; b4 = 1 failed at execution instead of being refused. Accepted: representative of a PAD group under sort-based grouping |
| 2026-11-07 | clang-21, C++23, debug | 1 | pass | smoke test, 480 files C++23 |
| 2026-11-09 | clang-21, C++23, debug | 5 (`mill1` load) | pass | before the executor pool, with `thread_stack=8M` |
| 2026-11-10 | clang-21, C++23, debug | 12 (first check) | pass | `UPDATE mill1` of 10 rows: transactions_read 1, row_events_read 1, last_gtid set, position 198 -> 2974 |
| 2026-11-10 | clang-21, C++23, debug | 12 (rest) | pass | unloaded table and rollback not counted; purge tag read (`<uuid>:duckdb_avoid_purge:1`); rotation followed to the new file after the next commit (FLUSH BINARY LOGS alone does not notify) |
| 2026-11-10 | clang-21, C++23, debug | 13 + stability fixes | pass | decoder and collapse on mill1/mill6; decoding at the start of a new binlog file after the format-description fix; duckdb_enable OFF/ON |
| 2026-11-11 | clang-21, C++23, debug | 14 (batching, 2000 updates) | not concluded | about 5 commits/s (HDD fsync), no progress shown and info-level log lines dropped (`log_error_verbosity=2`); stopped by shutdown. Added the test helpers and the verbosity setting; batching step reduced to 250 updates |
| 2026-11-12 | clang-21, C++23, debug | 19 (reference instance) | pass | through case 7: the discard refused while hs exists (expected error), then done after DROP TABLE |
| 2026-11-12 | clang-21, C++23, debug | 21, step 6a, behaviour (reference instance) | pass | ON: GROUP BY ci answered by InnoDB (below the default threshold, so not even tried: step order fixed in the plan); EXPLAIN shows DUCKDB with threshold 1000; read your own writes 0-1-0 with FORCED (0.08 s without a pending commit, about 0.9 s right after one: the freshness wait) |
| 2026-11-12 | clang-21, C++23, debug | 21, step 6a, comparison (reference instance) | pass | 19/19 identical and offloaded. First run invalid: table g missing, both sides failed with the same error and the script counted it as "same" (script fixed: an InnoDB error fails the test, a pass needs the FORCED run to reach DuckDB); setup failed once with use_secondary_engine=FORCED left in the session (setup now sets ON) |
| 2026-11-12 | clang-21, C++23, debug | 21, step 6a (throwaway instance) | pass after fixes | 19/19 queries offloaded with results identical to InnoDB; ON falls back without error; cost threshold; EXPLAIN shows DUCKDB; trace has the SQL; FORCED refusals; read your own writes 2-3-4-0 in one connection; LIVE 2 / FULL 3 on a history table. Fixed during the test: a DECIMAL result read as text crashed the server (cast fixed, type check added); date literals refused; read your own writes did not wait (THD position reset per statement: transaction observer) |
| 2026-11-12 | clang-21, C++23, debug | 20 after removing the configurable tag (throwaway instance) | pass | variable gone, old purge_tags table dropped at open; session A's tagged purge open 3 s and session B's untagged delete committed meanwhile: at1 id 1 archived, bt1 id 1 deleted, only A's GTID tagged; counters purge_transactions 1, archived_rows 1 |
| 2026-11-12 | clang-21, C++23, debug | 20 with a configurable tag (removed the same day) (throwaway instance) | pass | invalid tags refused, NewTag stored as newtag; live change and change while OFF classified by position (lake: archived 1,2,4,5; 3 and 6 deleted; 'third' recorded at the SET position, not at the restart); counters 1 then 3 archived rows; history_scope FULL refused without DUCKDB_HISTORY_READER (hint: warning), allowed after GRANT. Seen: after a restart a SET GLOBAL tag reverts to the configured one (documented: use SET PERSIST) |
| 2026-11-12 | clang-21, C++23, debug | fixes (throwaway instance) | pass | use_secondary_engine=FORCED: error 213 "query offload is not implemented in this version" instead of an empty result (COUNT(*) was 0); a stale history copy is no longer refreshed when the applier reads the ALTER that sets the engine back before the user's SECONDARY_LOAD (ddl_refreshes 0, then "reattached") |
| 2026-11-11 | clang-21, C++23, debug | 19 (throwaway instance) | pass after a fix | check only, reattach, mismatch (4 differences), unknown PK, adopt, adopt of a copy in use refused, resync, confirmed unload, discard (not stale / not confirmed / table exists refused, then done), PK backfill by the applier. First reattach failed with a DuckLake conflict: the applier wrote the registry during the load's single transaction. Fixed with the two-transaction load; then a 1M-row load with a change and a DROP of a history table during it passed, a planted build table was dropped at open, and a resync right after a pending purge kept the archived row |
| 2026-11-11 | clang-21, C++23, debug | 17 case 4b (reference instance, ddl_r) | pass | applier stopped during TRUNCATE + INSERT + RENAME + INSERT; at ON: deferred, renamed to ddl_r2, refreshed with 5 rows; lake: 5 rows, ddl_r2 loaded. The counters read in the same pasted block were still 0: the reader starts about 2 s after ON |
| 2026-11-11 | clang-21, C++23, debug | 17 (throwaway instance): deferred refresh | pass | applier stopped (duckdb_enable=OFF) during TRUNCATE + RENAME and history RENAME COLUMN + RENAME TABLE: both deferred, renamed, refreshed (lake: 5 rows; 45 live + 5 archived with old values under the new column name); restart with a copy in refresh_pending: changes skipped, refreshed at its RENAME |
| 2026-11-11 | clang-21, C++23, debug | 17 (reference instance, scratch copies ddl_c, ddl_h, ddl_t) | fail, fixed | ddl_c refreshes, type change and compressed pieces pass; ddl_h 960 live + 40 archived = InnoDB (the purge DELETE ran twice); ddl_t2 stale: TRUNCATE and RENAME back to back, the refresh found no table (fixed by the deferred refresh) |
| 2026-11-11 | clang-21, C++23, debug | 17 (throwaway instance, spike schema) | pass | compressed payloads: DML and 3000-row transactions in pieces, DuckDB = InnoDB; DDL: ADD INDEX no refresh; ADD COLUMN DEFAULT 7 and MODIFY BIGINT refreshed (lake: BIGINT, 2511 rows = 7, later 5000000000 applied); history RENAME COLUMN + MODIFY kept 10 archived rows under the new name; TRUNCATE, RENAME TABLE followed; retain_history removal refused, stale, applier continued; 12 statements = 12 GTIDs (none from the plugin). First start failed: a `thread_local` exhausted the static TLS block (fixed) |
| 2026-11-11 | clang-21, C++23, debug | 11 (startup abort, pending batch at shutdown) | pass | throwaway instance: before the fix, an aborted startup crashed in the reader (`get_current_log()` on the destroyed `LOCK_log`); after it, 3/3 aborts end cleanly and a pending batch is committed during the shutdown |
| 2026-11-10 | clang-21, C++23, debug | 15 (repoint) | pass | gap on binlog.000027 (expired, 2000 s); after the LOG_INFO_EOF fix SET GLOBAL duckdb_apply_start_position='FIRST' restarted the reader |
