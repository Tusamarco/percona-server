# DuckDB secondary engine: how-to

How to build, configure and use the DuckDB secondary engine of Percona Server
9.7 (POC, branch `9.7-duckdb`). It describes the code as it is on 2026-11-12:
DWL#1 to DWL#4 and DWL#3 step 3b are implemented; **query offload (DWL#6) is
not**, so queries still run on InnoDB (section 6 explains what that means
today). Design and details: `duckdb-secondary-engine-design.md` and the
worklogs `DWL-0*.md`; manual tests: `manual-test-plan.md`.

Contents:

1. Build
2. Server configuration
3. Create tables with a DuckDB copy
4. Load, unload and drop
5. Detach and reattach
6. SELECT on a table with a DuckDB copy
7. Check the plan (EXPLAIN)
8. Delete rows: plain and with purge
9. Rename tables and columns, other DDL
10. Monitoring and troubleshooting
11. Parameters reference
12. Limitations of this version

---

## 1. Build

### Requirements

- A compiler with C++23 (the reference build uses clang 21) and CMake 3.x/4.x.
- The three submodules under `extra/duckdb/`: DuckDB, DuckLake, CRoaring.
- About 10 GB of disk for the build directory; the first build compiles DuckDB
  and takes a long time (later builds reuse it).

### Steps

```bash
cd /opt/mysql_source/percona-server
git submodule update --init extra/duckdb/duckdb extra/duckdb/ducklake extra/duckdb/croaring

mkdir -p ../percona-server-9.7-build && cd ../percona-server-9.7-build
cmake ../percona-server \
  -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
  -DCMAKE_BUILD_TYPE=Debug \
  -DWITH_DUCKDB=ON \
  -DWITH_UNIT_TESTS=ON
  # optional: -DDUCKDB_BUILD_TYPE=RelWithDebInfo  (DuckDB debug builds are slow)

cmake --build . -j 8                       # server, client and the plugin
# or only the plugin and its tests after a change in storage/duckdb:
cmake --build . --target duckdb duckdb_apply-t -j 8
```

| CMake option | Default | Meaning |
|---|---|---|
| `WITH_DUCKDB` | OFF | Build DuckDB, DuckLake, CRoaring and the plugin. Without it nothing DuckDB-related is built. |
| `DUCKDB_BUILD_TYPE` | the server's `CMAKE_BUILD_TYPE` | Build type of DuckDB, DuckLake and CRoaring only. |
| `WITH_UNIT_TESTS` | server default | Also builds `duckdb_apply-t` (row decoder, collapse, DDL parser). |

### Results and checks

```bash
ls -l plugin_output_directory/ha_duckdb.so          # the plugin
./runtime_output_directory/duckdb_apply-t | tail -1  # [  PASSED  ] 38 tests.
ctest -R duckdb_build_smoke --output-on-failure      # DuckDB libraries alone
```

DuckDB is rebuilt only when a submodule pin or
`extra/duckdb/extension_config.cmake` changes. To force it, delete
`storage/duckdb/duckdb_ext-prefix/src/duckdb_ext-stamp/duckdb_ext-build` in the
build directory.

---

## 2. Server configuration

### my.cnf

```ini
[mysqld]
# the plugin
plugin-load-add                 = ha_duckdb.so
loose-duckdb_memory_limit       = 1G     # mandatory, at least 256M
loose-duckdb_threads            = 2      # mandatory, 1 .. number of cores
# optional (defaults shown)
# loose-duckdb_data_path          =        # empty: <datadir>/duckdb_data
# loose-duckdb_executor_threads   = 4
# loose-duckdb_executor_stack_size = 8M

# required by SECONDARY_LOAD (the copies are kept current from the binary log)
log_bin                   = binlog
binlog_format             = ROW
binlog_row_image          = FULL
binlog_row_metadata       = FULL
gtid_mode                 = ON
enforce_gtid_consistency  = ON
sync_binlog               = 1        # required for tables with retain_history
binlog_encryption         = OFF      # encrypted binary logs are not supported
# binlog_transaction_compression = ON  is supported

# recommended
binlog_expire_logs_seconds = 604800  # at least one day: the applier must be able
                                     # to read every binary log it has not applied
log_error_verbosity        = 3       # the plugin's progress messages are notes
```

Use the `loose-` prefix for every `duckdb_*` option: without it the server
refuses to start when the plugin is not loaded.

`duckdb_memory_limit` and `duckdb_threads` have no default on purpose: if one
is missing the plugin logs `... is not set ... Plugin not loaded` and the
server starts without it.

On a **replica** the plugin reads the replica's own binary log: set the same
options there, plus `log_replica_updates = ON` (the default).

### Check that it runs

```sql
SELECT PLUGIN_NAME, PLUGIN_STATUS, PLUGIN_AUTHOR
  FROM information_schema.PLUGINS WHERE PLUGIN_NAME = 'duckdb';  -- ACTIVE, Marco Tusa
SHOW ENGINES;                                       -- DUCKDB listed
SHOW GLOBAL STATUS LIKE 'duckdb_enabled';           -- ON
SHOW GLOBAL STATUS LIKE 'duckdb_lake_path';         -- the lake directory
SHOW GLOBAL STATUS LIKE 'duckdb_apply_reader_state';  -- idle
```

The error log shows `DuckDB lake: catalog=..., data=..., created=yes|no` and
`binlog reader started at ...`.

### Turn it off and on at runtime

```sql
SET GLOBAL duckdb_enable = OFF;  -- closes the lake, stops the applier; data kept
SET GLOBAL duckdb_enable = ON;   -- reopens it; the applier resumes where it stopped
```

---

## 3. Create tables with a DuckDB copy

Requirements for a table:

- a primary key;
- no foreign keys (in either direction);
- supported column types: integer types (signed and unsigned), DECIMAL up to
  precision 38, FLOAT, DOUBLE, CHAR, VARCHAR, TEXT types, BINARY, VARBINARY,
  BLOB types, DATE, DATETIME, TIMESTAMP. Other types (JSON, ENUM, SET, BIT,
  TIME, YEAR, spatial, VECTOR) make the load fail, naming the column;
- no zero dates or zero timestamps in the data;
- columns marked `NOT SECONDARY` are not copied.

### Without history (classic copy)

The DuckDB copy follows InnoDB exactly: rows deleted in InnoDB are deleted in
DuckDB.

```sql
CREATE TABLE orders (
  id BIGINT PRIMARY KEY,
  amount DECIMAL(12,2),
  created_at DATETIME
) SECONDARY_ENGINE = duckdb;

-- an existing table:
ALTER TABLE orders SECONDARY_ENGINE = duckdb;
```

### With history (`retain_history`)

Rows removed from InnoDB by a **purge** (section 8) are kept in DuckDB as
archived rows, with the time of the purge in the hidden column
`__archived_at`. InnoDB keeps only the recent data; DuckDB keeps everything.

```sql
CREATE TABLE orders_h (
  id BIGINT PRIMARY KEY,
  amount DECIMAL(12,2),
  created_at DATETIME
) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';

-- an existing table:
ALTER TABLE orders_h SECONDARY_ENGINE = duckdb,
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
```

`SECONDARY_ENGINE_ATTRIBUTE` is a JSON object with two possible keys:

| Key | Type | Meaning |
|---|---|---|
| `retain_history` | boolean | Keep purged rows as archived rows (requires `sync_binlog=1`). |
| `adopt_history_from` | string `"db.table"` or `"table"` | At the next SECONDARY_LOAD, take over the stale history copy of another table (section 5). Requires `retain_history`. |

Any other key is an error at load time.

---

## 4. Load, unload and drop

### Load

```sql
ALTER TABLE orders SECONDARY_LOAD;
```

The load copies the current InnoDB rows into the lake in one operation and
records the binary log position; from then on the applier applies every change
after that position. Writes on the table wait during the load.

Check:

```sql
SELECT TABLE_NAME, CREATE_OPTIONS FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = 'shop' AND TABLE_NAME = 'orders';
-- SECONDARY_ENGINE="duckdb" SECONDARY_LOAD="1"
```

Error log: `loaded shop.orders into the lake: N rows in X s`.

Loading a table that already has a copy:

| Table | Result |
|---|---|
| classic | refused: `SECONDARY_UNLOAD` first |
| history, copy stale | **reattach** (section 5) |
| history, copy loaded | **resync**: live rows replaced from InnoDB, archived rows kept, with a warning |

### Unload

```sql
-- classic table: drops the copy
ALTER TABLE orders SECONDARY_UNLOAD;

-- history table: refused unless confirmed, because the archived rows exist
-- only in the lake
SET SESSION duckdb_unload_history_confirm = ON;
ALTER TABLE orders_h SECONDARY_UNLOAD;    -- copy and archived rows dropped
SET SESSION duckdb_unload_history_confirm = OFF;
```

### Drop the MySQL table

```sql
DROP TABLE orders;     -- classic: its copy is dropped too
DROP TABLE orders_h;   -- history: its copy is KEPT, marked stale
```

A stale history copy can be reattached (same name, section 5), adopted by
another table (section 5), or deleted:

```sql
SET SESSION duckdb_unload_history_confirm = ON;
SET GLOBAL duckdb_discard_stale_copy = 'shop.orders_h';
SET SESSION duckdb_unload_history_confirm = OFF;
```

`duckdb_discard_stale_copy` is refused if the copy is not stale or if the
MySQL table still exists (use SECONDARY_UNLOAD then). Until a stale copy is
discarded, a new table with the same name and `retain_history` reattaches it
on its first SECONDARY_LOAD (if the structure is the same).

Disk space: DuckLake keeps the Parquet files of dropped copies until snapshot
expiry and file cleanup, which this version does not run yet.

---

## 5. Detach and reattach

### Classic table

```sql
ALTER TABLE orders SECONDARY_UNLOAD;      -- detach: copy dropped
ALTER TABLE orders SECONDARY_LOAD;        -- attach again: full copy from InnoDB
-- or remove the engine altogether:
ALTER TABLE orders SECONDARY_ENGINE = NULL;   -- copy dropped
```

### History table: detach, keep the history

```sql
ALTER TABLE orders_h SECONDARY_ENGINE = NULL;
-- warning: DuckDB copy of shop.orders_h kept and marked stale: it holds archived rows
```

The copy is no longer updated. To reattach it later (live rows from InnoDB,
archived rows kept):

```sql
ALTER TABLE orders_h SECONDARY_ENGINE = duckdb,
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';
ALTER TABLE orders_h SECONDARY_LOAD;      -- log: reattached shop.orders_h ...
```

The same works after `DROP TABLE` + `CREATE TABLE` with the same name.

### The structure must be the same

Reattach, resync and adopt take over existing archived rows, so the table must
have exactly the structure of the copy: the same columns (by name, order does
not matter), the same type for each, the same primary key columns. Any
difference refuses the load and every difference is listed:

```sql
ALTER TABLE orders_h SECONDARY_LOAD;
-- ERROR 3877: shop.orders_h: the structure does not match the history copy
--   shop.orders_h (2 differences, see SHOW WARNINGS); nothing loaded
SHOW WARNINGS;
-- column note: in the history copy, missing in the new table (1200 archived values)
-- column amount: type DECIMAL(12,2) in the history copy, INTEGER in the new table
```

Check without loading anything:

```sql
SET SESSION duckdb_history_check_only = ON;
ALTER TABLE orders_h SECONDARY_LOAD;
-- always an error, giving the result, e.g.:
-- structure check passed: shop.orders_h can reattach its history copy;
--   nothing loaded (duckdb_history_check_only=ON)
SET SESSION duckdb_history_check_only = OFF;
```

(With `mysql -e "...; SHOW WARNINGS"` the statements after an error are
skipped: run the check in an interactive session or with a here-document and
`--force`.)

Copies loaded before this version have no recorded primary key until their
first change after the upgrade; until then a takeover is refused with
`primary key: unknown for the history copy`.

### Give the history to a table with another name (`adopt_history_from`)

When the old table no longer exists (dropped, or its engine removed) and a
table with another name should continue its history:

```sql
CREATE TABLE orders_v2 (
  id BIGINT PRIMARY KEY, amount DECIMAL(12,2), created_at DATETIME
) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE =
    '{"retain_history": true, "adopt_history_from": "shop.orders_h"}';
-- optional: SET SESSION duckdb_history_check_only = ON; ALTER ... SECONDARY_LOAD;
ALTER TABLE orders_v2 SECONDARY_LOAD;
-- log: loaded (adopting the history copy shop.orders_h) shop.orders_v2 in the lake
```

Conditions: the source copy exists, has `retain_history` and is stale (a copy
still used by its table is refused: rename that table with RENAME TABLE
instead); the new table has no copy of its own; the structure is the same.
After the load the source copy no longer exists; the attribute is ignored as
long as the table has its own copy.

---

## 6. SELECT on a table with a DuckDB copy

**Status: query offload is DWL#6 and is not implemented yet.** The engine
declines every query, so queries run on InnoDB and return InnoDB's result. The
DuckDB copy is kept current but not read by queries yet.

The server variable that controls it (standard MySQL):

```sql
SET SESSION use_secondary_engine = ON;      -- default: the optimizer may use the
                                            -- secondary engine; today: InnoDB
SET SESSION use_secondary_engine = OFF;     -- never use it: InnoDB
SELECT /*+ SET_VAR(use_secondary_engine = OFF) */ COUNT(*) FROM orders;
```

> **Do not use `use_secondary_engine = FORCED` in this version.** It sends the
> query to the DuckDB engine, which cannot execute queries yet: it returns an
> **empty result without an error** (for example `COUNT(*)` = 0). A fix that
> makes it an error is proposed; until DWL#6 forcing is never correct.

`secondary_engine_cost_threshold` (standard, default 100000) is the optimizer
cost above which a query is considered for the secondary engine once DWL#6
exists.

---

## 7. Check the plan (EXPLAIN)

```sql
EXPLAIN FORMAT=TREE SELECT COUNT(*) FROM orders WHERE id > 10;
-- today, with ON or OFF: an InnoDB plan, e.g.
-- -> Covering index range scan on orders using PRIMARY over (10 < id)

-- a plan sent to the secondary engine (only with FORCED, see the warning above):
-- -> Table scan on orders in secondary engine DUCKDB
```

`in secondary engine DUCKDB` in the plan means the query runs in DuckDB. The
session counter `SHOW SESSION STATUS LIKE 'Secondary_engine_execution_count'`
counts the queries sent there.

---

## 8. Delete rows: plain and with purge

### Plain DELETE

```sql
DELETE FROM orders   WHERE id = 7;   -- classic: removed from DuckDB too
DELETE FROM orders_h WHERE id = 7;   -- history: the row is a real delete, removed
                                     -- from DuckDB too (no archived row)
```

A plain DELETE is a correction (the row should not exist), so it is applied as
a delete everywhere. UPDATE is applied as is, also on history tables.

### DELETE with purge (history tables)

A purge removes old rows from InnoDB while DuckDB keeps them as archived rows.
The transaction must carry the GTID tag `duckdb_avoid_purge`:

```sql
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM orders_h WHERE created_at < '2025-01-01';
SET gtid_next = 'AUTOMATIC';
```

- In DuckDB the rows stay, with `__archived_at` set to the commit time of the
  purge; queries on live data skip them.
- Privileges: `TRANSACTION_GTID_TAG` plus one of `SYSTEM_VARIABLES_ADMIN`,
  `SESSION_VARIABLES_ADMIN` or `REPLICATION_APPLIER`; requires `gtid_mode=ON`.
- The tag applies to the whole transaction: put only the purge in it. On a
  classic table a tagged DELETE is a normal delete.
- A row purged and inserted again with the same key: the old version stays
  archived, the new one is live.
- Large purges are fine: transactions larger than `duckdb_apply_batch_max_rows`
  are applied in pieces inside one DuckDB transaction.

To see it applied (testing): `SET GLOBAL duckdb_apply_debug_log = ON;` and the
error log shows `... deleted=0 archived=N ...; DuckDB now L live, A archived`.

---

## 9. Rename tables and columns, other DDL

DDL on a table with `SECONDARY_ENGINE = duckdb` is never refused, locally or
through replication. The applier brings the copy in line when it reads the
statement in the binary log:

| Statement | What happens to the copy |
|---|---|
| `RENAME TABLE a TO b` (also across schemas), `ALTER TABLE a RENAME TO b` | renamed, data and history kept |
| `ALTER TABLE ... RENAME COLUMN x TO y`, `CHANGE x y ...` | refreshed; archived rows keep their values under the new name |
| `ALTER TABLE ... ADD/DROP/MODIFY COLUMN`, `CONVERT TO`, primary key change, `TRUNCATE`, `DROP PARTITION` | refreshed from InnoDB (archived rows kept and converted to the new types) |
| `ADD/DROP INDEX`, `COMMENT`, `ALGORITHM`, `SET DEFAULT` and other index or option changes | nothing (the rows do not change) |
| `DROP TABLE`, `SECONDARY_ENGINE = NULL` | section 4 / 5 |

```sql
RENAME TABLE orders TO orders_old;            -- log: DuckDB copy ... renamed to shop.orders_old
ALTER TABLE orders_h RENAME COLUMN amount TO total;
-- log: refreshed shop.orders_h in the lake ... / ... refreshed after DDL
SHOW GLOBAL STATUS LIKE 'duckdb_apply_ddl%';  -- refreshes, renames, deferred, failures
```

Notes:

- A refresh reloads the whole table from InnoDB; while it runs the applier
  waits for all tables.
- If the table was renamed again before the applier read the first statement
  (an online schema change tool: ALTER, then RENAME), the refresh is deferred
  to that rename (`duckdb_apply_ddl_deferred`).
- If a copy cannot follow a DDL (no primary key any more, unsupported type, an
  archived value that cannot be converted, `retain_history` removed), it is
  marked stale, the error log says why, `duckdb_apply_ddl_failures` grows, and
  the applier goes on with the other tables. Removing `retain_history` by DDL
  is always refused (the archived rows would be lost).
- To give a history to a table under a new name when the old table is gone,
  see `adopt_history_from` (section 5).

---

## 10. Monitoring and troubleshooting

```sql
SHOW GLOBAL STATUS LIKE 'duckdb%';
```

- `duckdb_apply_reader_state`: `idle`, `reading`, `committing`,
  `refreshing db.t`, or `error: <reason>`.
- Applied position: `duckdb_applied_file` / `duckdb_applied_position` /
  `duckdb_applied_gtid` (what DuckDB contains) against the reader position
  `duckdb_apply_reader_file` / `duckdb_apply_reader_position`.

Binary logs purged before the applier read them (the server was down too long,
or `binlog_expire_logs_seconds` is short): the error log names the tables that
may miss changes. With `duckdb_apply_gap_action = STOP` (default) the applier
stops in `error:` state. Then:

```sql
ALTER TABLE <each named classic table> SECONDARY_UNLOAD;
ALTER TABLE <each named classic table> SECONDARY_LOAD;
ALTER TABLE <each named history table> SECONDARY_LOAD;   -- resync, history kept
SET GLOBAL duckdb_apply_start_position = 'FIRST';        -- restart the applier
```

Test helpers (progress you can watch, clean stop): `duckdb_test_helpers.sql`.

---

## 11. Parameters reference

### System variables

| Variable | Scope | Dynamic | Default | Range / values | Description |
|---|---|---|---|---|---|
| `duckdb_enable` | global | yes | ON | ON, OFF | Opens or closes the lake and starts or stops the applier. OFF behaves as if the plugin were not loaded; no data is deleted. |
| `duckdb_data_path` | global | no | `""` | path | Lake directory. Empty: `<datadir>/duckdb_data`; a relative path is resolved against the datadir. Must not be the datadir or inside a schema directory, must not be world-writable. |
| `duckdb_memory_limit` | global | yes | none (mandatory) | ≥ 256 MiB | Memory limit of DuckDB, applied at once. |
| `duckdb_threads` | global | yes | none (mandatory) | 1 .. cores | DuckDB worker threads, applied at once. |
| `duckdb_executor_threads` | global | yes | 4 | 1 .. cores | Threads that run DuckDB calls (DuckDB never runs on connection threads). Above the number of cores it is lowered with a warning. |
| `duckdb_executor_stack_size` | global | no | 8 MiB | 2 .. 64 MiB | Stack size of the executor threads. |
| `duckdb_encryption` | global | no | OFF | ON, OFF | Not supported: ON prevents the plugin from loading. |
| `duckdb_apply_batch_max_trx` | global | yes | 1000 | 1 .. 1,000,000 | The applier commits a batch after this many source transactions that change loaded tables. |
| `duckdb_apply_batch_max_rows` | global | yes | 100000 | 1000 .. 100,000,000 | The applier commits a batch after this many changed rows. A larger transaction is applied in pieces of this size inside one DuckDB transaction. |
| `duckdb_apply_batch_max_delay_ms` | global | yes | 1000 | 10 .. 60000 | A batch is committed at most this long after its first change (bounds the lag under light load). |
| `duckdb_apply_gap_action` | global | yes | STOP | STOP, FIRST_AVAILABLE | When binary logs the applier needs were purged and loaded tables miss changes: stop in error state, or warn and continue at the first available binary log. |
| `duckdb_apply_start_position` | global | yes (command) | `""` | binary log name, FIRST, CURRENT | Setting it restarts the applier at the start of that binary log, which becomes the applied position. Positions before the applied position are refused. Not persisted. |
| `duckdb_discard_stale_copy` | global | yes (command) | `""` | `'db.table'` | Setting it drops the stale copy of a table that no longer exists. History copies need `duckdb_unload_history_confirm=ON`. Not persisted. |
| `duckdb_apply_debug_log` | global | yes | OFF | ON, OFF | Testing aid: one error log line per change and per table of each batch, with DuckDB row counts (needs `log_error_verbosity=3`). |
| `duckdb_history_check_only` | session | yes | OFF | ON, OFF | SECONDARY_LOAD of a history table only checks that it can take over a history copy (adopt, reattach, resync) and always ends with an error giving the result. |
| `duckdb_unload_history_confirm` | session | yes | OFF | ON, OFF | Allows SECONDARY_UNLOAD of a history table and `duckdb_discard_stale_copy` of a history copy (archived rows are deleted). |

Server variables the plugin depends on: `log_bin`, `binlog_format=ROW`,
`binlog_row_image=FULL`, `binlog_row_metadata=FULL`, `gtid_mode=ON`,
`sync_binlog=1` (history tables), `binlog_encryption=OFF`,
`binlog_expire_logs_seconds` (a warning below one day),
`use_secondary_engine`, `secondary_engine_cost_threshold`,
`log_error_verbosity` (3 to see the plugin's notes).

### Status variables

| Variable | Meaning |
|---|---|
| `duckdb_enabled` | The lake is open. |
| `duckdb_lake_path` | Lake directory in use. |
| `duckdb_executor_threads_busy` / `duckdb_executor_queue_length` / `duckdb_executor_tasks` | Executor pool: busy threads, queued calls, calls run. |
| `duckdb_apply_reader_state` | Applier state (section 10). |
| `duckdb_apply_reader_file` / `duckdb_apply_reader_position` | Next binary log position to read. |
| `duckdb_apply_transactions_read` | Transactions read that change loaded tables. |
| `duckdb_apply_row_events_read` / `duckdb_apply_rows_decoded` | Row events and rows of loaded tables decoded. |
| `duckdb_apply_changes` | Net changes after collapsing per primary key. |
| `duckdb_apply_last_gtid` | Last transaction read. |
| `duckdb_apply_batches` / `duckdb_apply_rows_written` | DuckDB commits with changes, rows written. |
| `duckdb_apply_large_transactions` / `duckdb_apply_pieces` | Transactions applied in pieces, pieces written. |
| `duckdb_apply_ddl_refreshes` / `duckdb_apply_ddl_renames` / `duckdb_apply_ddl_deferred` / `duckdb_apply_ddl_failures` | Copies refreshed or renamed after DDL, refreshes deferred to a later rename, copies marked stale by a DDL. |
| `duckdb_applied_file` / `duckdb_applied_position` / `duckdb_applied_gtid` | Applied position: what DuckDB contains. |

Counters restart at 0 when the server starts.

---

## 12. Limitations of this version

- Queries are not offloaded (DWL#6); `use_secondary_engine = FORCED` returns
  empty results (section 6).
- Not supported: `binlog_encryption=ON`, `duckdb_encryption`, XA transactions
  on loaded tables (the applier stops), partial JSON updates, the types listed
  in section 3.
- Space of dropped copies and old versions is not reclaimed yet (snapshot
  expiry, Phase 3).
- A refresh after DDL reloads the whole table and pauses the applier for all
  tables while it runs.
- The MTR test suite is postponed; tests are manual (`manual-test-plan.md`).
- Replicas: the design reads the replica's binary log, but it has not been
  tested on a replica yet.
