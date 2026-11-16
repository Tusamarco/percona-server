# DuckDB secondary engine: how-to

How to build, configure and use the DuckDB secondary engine of Percona Server
9.7 (POC, branch `9.7-duckdb`). It describes the code as it is on 2026-11-13:
DWL#1 to DWL#5 (with DWL#3 step 3b) are implemented, and **query offload
(DWL#6) steps 6a, 6b and 6c**: queries with expressions, joins,
subqueries, derived tables, views, CTEs and UNION / INTERSECT / EXCEPT run
in DuckDB (section 6). Design and details: `duckdb-secondary-engine-design.md`
and the worklogs `DWL-0*.md`; manual tests: `manual-test-plan.md`.

Contents:

1. Build
2. Server configuration (**on a replica, read "Replicas" first**)
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

### MTR suite

```bash
cd ../percona-server-9.7-build/mysql-test
MTR_SHUTDOWN_TIMEOUT=120 ./mtr --suite=/opt/mysql_source/percona-server/storage/duckdb/mysql-test/duckdb --parallel=2
```

16 tests (about 1.5 hours with a debug build); see
`storage/duckdb/mysql-test/duckdb/README.md`.

### Results and checks

```bash
ls -l plugin_output_directory/ha_duckdb.so          # the plugin
./runtime_output_directory/duckdb_apply-t | tail -1  # [  PASSED  ] 39 tests.
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
binlog_order_commits      = ON       # required (the default)
sync_binlog               = 1        # required for tables with retain_history,
innodb_flush_log_at_trx_commit = 1   # and recommended for all tables: see below
binlog_encryption         = OFF      # encrypted binary logs are not supported
# binlog_transaction_compression = ON  is supported

# recommended
binlog_expire_logs_seconds = 604800  # at least one day: the applier must be able
                                     # to read every binary log it has not applied
log_error_verbosity        = 3       # the plugin's progress messages are notes
```

Use the `loose-` prefix for every `duckdb_*` option: without it the server
refuses to start when the plugin is not loaded.

**Durability.** The copy follows the binary log, so after an operating
system crash or power loss it matches InnoDB only if both are durable:
`sync_binlog=1` (else the copy can keep transactions InnoDB rolled back) and
`innodb_flush_log_at_trx_commit=1` (else InnoDB can lose transactions the
copy has). A crash of mysqld alone loses nothing either way. With other
settings, after an operating system crash, reload classic tables
(SECONDARY_UNLOAD + SECONDARY_LOAD); history tables require `sync_binlog=1`
because their archived rows exist only in DuckDB.

DuckDB never shows a transaction before InnoDB does: the applier reads a
transaction only after its InnoDB commit. With semi-sync replication
(AFTER_SYNC) that is after the replica's acknowledgement.

`duckdb_memory_limit` and `duckdb_threads` have no default on purpose: if one
is missing the plugin logs `... is not set ... Plugin not loaded` and the
server starts without it.

### Replicas: read this before loading anything on a replica

> [!WARNING]
> **On a replica, run `SECONDARY_LOAD`, `SECONDARY_UNLOAD`,
> `ALTER TABLE ... SECONDARY_ENGINE=...` and `GRANT DUCKDB_HISTORY_READER`
> with `SET sql_log_bin = 0`.** Otherwise each of them creates an **errant
> GTID** on the replica, which can break a later failover.
>
> ```sql
> -- on the replica
> SET sql_log_bin = 0;
> ALTER TABLE orders SECONDARY_LOAD;
> GRANT DUCKDB_HISTORY_READER ON *.* TO 'analyst'@'%';
> SET sql_log_bin = 1;
> ```

**How a replica works with the plugin.** The plugin reads the replica's own
binary log, so set the same options there as on a source (section 2), plus
`log_replica_updates = ON` (the default). The source does not need the
plugin. Create the tables on the source with `SECONDARY_ENGINE = duckdb` (and
`retain_history` if needed): the server accepts the clause without the
plugin, and the definition reaches the replica through replication. Then
load the copy on the replica. Replicated DML, DDL, large transactions, the
multi-threaded applier, restarts and tagged purges all work (test plan
section 18).

**Why `sql_log_bin = 0` is needed for loads.** This is server behaviour, not
a choice of the plugin: MySQL writes `ALTER TABLE ... SECONDARY_LOAD`,
`SECONDARY_UNLOAD` and `SECONDARY_ENGINE=...` to the binary log so that its
replicas stay consistent with it (sql/sql_table.cc). On a replica the
statement is written to the replica's binary log with a GTID that carries the
replica's own `server_uuid`: a transaction the source never had, an *errant
GTID*. The plugin cannot suppress it without a change to the server. Its own
internal statements (the refresh after DDL) already run with
`sql_log_bin = 0`.

**Why the GRANT is needed on every replica.** `DUCKDB_HISTORY_READER` is a
privilege registered by the plugin. A source without the plugin does not
know it and refuses the GRANT, so it cannot arrive through replication. It
must be granted on each replica that serves `duckdb_history_scope=FULL`
queries, and for the same reason as above with `sql_log_bin = 0`. The
accounts themselves can still be created on the source.

**What an errant GTID breaks.** Nothing at first: replication continues.
The problem comes when the topology changes. If this replica is promoted or
another replica is pointed to it, the other servers ask for the errant
transaction: it is replayed there (a `SECONDARY_LOAD` or a GRANT of an
unknown privilege fails on a server without the plugin and stops
replication), or, if its binary log has been purged, replication fails with
error 1236 (the source has purged binary logs containing GTIDs the replica
requires). Failover tools (Orchestrator and others) report errant GTIDs and
may refuse to promote the replica.

**Check and repair.** Compare the replica with the source:

```sql
-- on the replica: should be empty
SELECT GTID_SUBTRACT(@@gtid_executed, '<gtid_executed of the source>');
```

If it is not empty, commit an empty transaction with each errant GTID on the
source, which then replicates to all servers:

```sql
-- on the source, for each errant GTID
SET gtid_next = '<replica server_uuid>:<n>';
BEGIN; COMMIT;
SET gtid_next = 'AUTOMATIC';
```

**Replicas with `super_read_only = ON`** (recommended, it prevents local
writes and so errant GTIDs): `SECONDARY_LOAD`, `SECONDARY_UNLOAD` and the
GRANT are refused with error 1290, even with `sql_log_bin = 0` (verified).
Switch it off only for the operation:

```sql
SET GLOBAL super_read_only = OFF;
SET sql_log_bin = 0;
ALTER TABLE orders SECONDARY_LOAD;
SET sql_log_bin = 1;
SET GLOBAL super_read_only = ON;
```

The plugin itself is not blocked by `super_read_only`: the applier writes
only to DuckDB, and its refresh after a replicated DDL (an internal
`ALTER TABLE ... SECONDARY_LOAD`, run with `sql_log_bin = 0`) passes the
read-only check as the replication applier does. Only your own statements
need the sequence above.

> [!IMPORTANT]
> **The refresh after DDL under `super_read_only` needs the `duckdb_guard`
> plugin active** (it is loaded with the engine, on by default). The server
> clears the read-only exemption at the start of every statement; the guard
> sets it again for the plugin's internal session only, at the pre-parse
> event. With `--duckdb-guard=OFF` on a replica with `super_read_only = ON`,
> every replicated DDL on a loaded table fails to refresh its copy: the copy
> becomes **stale** (error log: `refresh after DDL failed ...
> super-read-only`). A stale classic copy is recovered with
> `SECONDARY_UNLOAD` + `SECONDARY_LOAD`; a stale **history** copy can be
> reattached only while the table still has the copy's structure, which a
> DDL usually changed: its archived rows are then lost. Keep the guard on.
>
> Side effect: the internal session is audited like any connection, so an
> audit plugin (e.g. `audit_log`) records the plugin's internal
> `ALTER TABLE ... SECONDARY_LOAD` statements.

Not tested: a source with the plugin and copies loaded on both servers.

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

### Move the lake (duckdb_data_path)

DuckLake stores the data path in its catalog. After moving or restoring the
lake directory elsewhere, set `duckdb_data_path` to the new place and start
the server: with `duckdb_lake_relocate = ON` (default) the plugin checks that
every file of the catalog is there, stores the new path and logs
`DuckDB lake moved: the catalog's data path ... is now ...`. If a file is
missing (or with OFF) the plugin is not loaded and the error log says why.
The binary log index lists absolute paths too: when moving a whole
instance, fix `my.cnf` and the `*.index` files.

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
- columns marked `NOT SECONDARY` are not copied;
- string columns whose collation DuckDB cannot compare natively (`_ci`,
  `_as_cs`, `general_ci`, latin1 and other non-UTF-8 collations) get a
  hidden weight column in the copy (MySQL's sort weight of each value), so
  queries in DuckDB follow the MySQL collation. Copies loaded before
  2026-11-14 have none: SECONDARY_UNLOAD + SECONDARY_LOAD a classic table,
  SECONDARY_LOAD (resync) a history table.

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

> [!WARNING]
> **On a replica: `SET sql_log_bin = 0` first** (and `super_read_only = OFF`
> for the operation if it is set), or the load creates an errant GTID. The
> same for `SECONDARY_UNLOAD`. Why: section 2, "Replicas".

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

**Status: DWL#6 steps 6a, 6b and 6c (6c-1, 6c-2, 6c-3).** A query runs in DuckDB when all of this holds,
otherwise it runs on InnoDB:

- `use_secondary_engine` is ON (default) or FORCED;
- with ON, its optimizer cost is above `secondary_engine_cost_threshold`
  (default 100000; lower it per session to offload smaller queries), or
  MySQL's plan scans a loaded table of at least `duckdb_offload_min_rows`
  rows (default 1,000,000) by a full scan, full index scan or non-unique
  index lookup (InnoDB's row estimates for those can be far too low).
  Primary key lookups and index ranges stay on InnoDB;
- it is a SELECT whose tables all have a loaded DuckDB copy: joins (inner,
  CROSS, LEFT, RIGHT), `IN (SELECT ...)`, `EXISTS`, `NOT EXISTS`, `NOT IN`
  on non-nullable columns, derived tables, views, `WITH` (not recursive),
  scalar subqueries the server turns into joins, and UNION, INTERSECT,
  EXCEPT (DISTINCT or ALL, nested, with ORDER BY / LIMIT). The columns of a
  set operation must not mix kinds (number with string, TIMESTAMP with
  DATETIME: refused), and UNION DISTINCT, INTERSECT and EXCEPT need
  binary or `_0900_bin` strings. Subqueries: IN, NOT IN (with NULLs),
  EXISTS, scalar subqueries in any clause, correlated or not, ANY / ALL.
  Not offloaded: LATERAL, recursive CTEs, row subqueries `(a, b) IN (...)`,
  IN on `_bin` / `_ci` strings, a correlated scalar subquery without an
  aggregate; A table without a copy anywhere in the query:
  InnoDB (with FORCED the server's error 3889);
- it uses only: columns, literals, `= <> < <= > >=`, BETWEEN, IN (list),
  LIKE (with ESCAPE), IS [NOT] NULL, AND, OR, NOT; `+ - *`, `/` on DOUBLE
  values and exact division of integers and decimals (as MySQL, with
  div_precision_increment), DIV, MOD, ABS, ROUND (not of DOUBLE); CASE, IF,
  IFNULL, COALESCE,
  NULLIF; YEAR, MONTH, DAYOFMONTH, DAYOFYEAR, QUARTER, DAYOFWEEK, WEEKDAY,
  HOUR, MINUTE, SECOND, DATE of DATE and DATETIME columns; COUNT, COUNT
  (DISTINCT), SUM, AVG (also DISTINCT), MIN, MAX; DISTINCT, GROUP BY, HAVING,
  ORDER BY, LIMIT, OFFSET;
- string comparisons, joins, GROUP BY, ORDER BY, COUNT(DISTINCT) and
  MIN/MAX follow the column's collation for every collation (`_bin`
  directly, `_ci`, `_as_cs`, `general_ci`, latin1 ... through the weight
  columns of the copy). Not offloaded on non-`_bin` strings: LIKE, SELECT
  DISTINCT, UNION DISTINCT / INTERSECT / EXCEPT, IN subqueries, functions
  over them in comparisons or GROUP BY (UPPER(c) = ...). A grouped string
  column shown in a join needs the table's primary key in GROUP BY (as
  `GROUP BY s.id, s.name`): otherwise the value InnoDB shows depends on its
  join order and the query runs on InnoDB; MIN/MAX of such strings in a
  join likewise. SELECT DISTINCT of `utf8mb4_bin`/`utf8mb3_bin` columns is
  refused;
- not translated, so answered by InnoDB: arithmetic on UNSIGNED columns,
  TRUNCATE, other functions,
  date functions on TIMESTAMP columns, comparing a string column with a
  number;
- it is not inside a transaction that has written.

```sql
SET SESSION use_secondary_engine = ON;                  -- default
SET SESSION secondary_engine_cost_threshold = 1000;     -- offload smaller queries
SELECT grp, SUM(amount) FROM g GROUP BY grp;            -- runs in DuckDB
SHOW GLOBAL STATUS LIKE 'duckdb_offload%';              -- queries, refused, last_refusal

SET SESSION use_secondary_engine = FORCED;              -- DuckDB or an error
SELECT ci, COUNT(*) FROM g GROUP BY ci;
-- ERROR 3877: the DuckDB secondary engine cannot run this query:
--   collation:ci (utf8mb4_0900_ai_ci)
SET SESSION use_secondary_engine = OFF;                 -- always InnoDB
```

Errors: when DuckDB fails with an error MySQL has too, the client gets
MySQL's code with DuckDB's message: a scalar subquery with more than one
row `ERROR 1242: Subquery returns more than 1 row (DuckDB error: ...)`,
KILL QUERY `ERROR 1317: Query execution was interrupted (DuckDB error:
...)`, `max_execution_time` `ERROR 3024`. Other DuckDB errors during
execution are error 3877 `DuckDB error: ...`. KILL QUERY and
max_execution_time stop the DuckDB query within about a second.

After RENAME TABLE the copy follows once the applier has read the RENAME
(usually within a second): a query in between runs on InnoDB (FORCED:
"Table has not been loaded").

The result is the same as on InnoDB: types, NULL order, exact SUM and AVG,
TIMESTAMP in the session time zone, trailing spaces ignored by
`utf8mb4_bin`. One accepted difference: grouping by a `utf8mb4_bin` column
shows one value per group, and when MySQL groups by sorting the value shown
may differ in trailing spaces (`'a'` or `'a '`: MySQL does not define which).

Read your own writes: with `duckdb_offload_wait_applied = ON` (default), a
query waits until the applier has applied this connection's last commit (at
most 5 s, otherwise it runs on InnoDB). With OFF it may not see a commit made
less than about a second earlier.

### Live data or full history (`duckdb_history_scope`)

`duckdb_history_scope` decides what queries run in DuckDB see in history
tables: `LIVE` (default, the rows InnoDB has: archived rows are filtered out)
or `FULL` (also the archived rows). `FULL` requires the
`DUCKDB_HISTORY_READER` privilege, registered by the plugin; no account has
it until granted, root included:

```sql
GRANT DUCKDB_HISTORY_READER ON *.* TO 'analyst'@'%';
SET SESSION duckdb_history_scope = FULL;
SELECT /*+ SET_VAR(duckdb_history_scope = FULL) */ ...;   -- per query
```

Without the privilege `SET` fails with error 1227; the hint gives a warning
and is ignored.

> [!WARNING]
> **On a replica the GRANT must run on the replica itself, with
> `SET sql_log_bin = 0`.** A source without the plugin does not know the
> privilege, so it cannot come through replication, and a GRANT written to
> the replica's binary log is an errant GTID. Why: section 2, "Replicas".

Under FULL a query that reads a history table never runs on InnoDB (which
has only the live rows): the `duckdb_guard` plugin, loaded with the engine,
sends it to DuckDB whatever its cost, or stops it with an error: with
`use_secondary_engine=OFF`, for a statement that is not a SELECT (INSERT ...
SELECT, CREATE ... SELECT, and every UPDATE, DELETE or TRUNCATE of a history
table or reading one: they first read the rows to change, and InnoDB has
only the live ones; FULL is a read scope, change rows under LIVE; a plain
INSERT is not affected), when DuckDB cannot
run it (the error gives the reason), with `duckdb_enable=OFF`, or when the
copy is stale. FULL cannot be set while the guard is disabled.

```sql
SET SESSION duckdb_history_scope = FULL;
SELECT COUNT(*) FROM orders_h;          -- live + archived, or an error
SHOW GLOBAL STATUS LIKE 'duckdb_guard%';   -- forced, rejected
```

---

## 7. Check the plan (EXPLAIN)

```sql
SET SESSION secondary_engine_cost_threshold = 1000;
EXPLAIN FORMAT=TREE SELECT grp, SUM(amount) FROM g GROUP BY grp;
-- -> ... in secondary engine DUCKDB          runs in DuckDB
SHOW WARNINGS;
-- Note 1003  Query is executed in secondary engine; the actual query plan may diverge ...
-- Note 3877  DuckDB SQL: SELECT CAST(t0."grp" AS BIGINT), ... FROM lake."db"."g" AS t0 GROUP BY t0."grp"
-- Note 3877  DuckDB plan (1/4): ╭─ Hash Group By ...   (DuckDB's own plan, the one that runs)

EXPLAIN FORMAT=TREE SELECT ci, COUNT(*) FROM g GROUP BY ci;
-- an InnoDB plan (no DUCKDB): refused (collation), runs on InnoDB
-- with FORCED: EXPLAIN fails with the reason (error 3877)
```

`in secondary engine DUCKDB` in the plan means the query runs in DuckDB. The
plan lines are MySQL's; DuckDB makes its own plan from the generated SQL,
shown in the notes (`SHOW WARNINGS`, long text split in numbered notes).
EXPLAIN ANALYZE is not available for secondary engines (error 1235). The
generated SQL (or the reason for refusing) is also in the optimizer trace:

```sql
SET optimizer_trace = 'enabled=on';
SELECT grp, SUM(amount) FROM g GROUP BY grp;
SELECT TRACE FROM information_schema.OPTIMIZER_TRACE\G   -- "duckdb_offload": {"sql": ...}
```

The session counter `Secondary_engine_execution_count` also counts the
queries sent there.

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
- The tag is always `duckdb_avoid_purge` (fixed, not configurable).
- The tag affects only the transactions of the session that set it: other
  connections deleting at the same time are not purges (`gtid_next` is a
  session variable, and every transaction has its own GTID in the binary
  log).
- It stays in effect in that session until `gtid_next` is set back to
  `AUTOMATIC`: do that right after the purge, or a later business delete in
  the same session would be archived instead of deleted.
- A purge that forgets the tag is a plain delete: the rows are removed from
  DuckDB too.

Counters: `duckdb_apply_purge_transactions` and `duckdb_apply_archived_rows`.

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
- A stale copy is detached: DDL never refreshes it (a RENAME still renames
  it). It comes back only with an explicit SECONDARY_LOAD: a classic table
  needs SECONDARY_UNLOAD first, a history table is reattached after the
  structure check.
- To give a history to a table under a new name when the old table is gone,
  see `adopt_history_from` (section 5).

---

## 10. Monitoring and troubleshooting

```sql
SHOW GLOBAL STATUS LIKE 'duckdb%';
```

- `duckdb_apply_reader_state`: `idle` (everything read is committed in
  DuckDB), `reading`, `waiting to commit` (a batch waits for its commit
  deadline), `committing`, `refreshing db.t`, or `error: <reason>`.
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

A query that should run in DuckDB but does not: `SHOW GLOBAL STATUS LIKE
'duckdb_offload_last_refusal'` gives the reason of the last refusal; run it
with `use_secondary_engine = FORCED` to get the reason as an error, or look
at the optimizer trace (section 7). Common reasons: `collation:<column>`,
`unsupported_function:<name>`, `not_single_table`, `not_loaded:<table>`.
Below `secondary_engine_cost_threshold` it is not even tried (no refusal).

Test helpers (progress you can watch, clean stop): `duckdb_test_helpers.sql`.

---

## 11. Parameters reference

### System variables

| Variable | Scope | Dynamic | Default | Range / values | Description |
|---|---|---|---|---|---|
| `duckdb_enable` | global | yes | ON | ON, OFF | Opens or closes the lake and starts or stops the applier. OFF behaves as if the plugin were not loaded; no data is deleted. |
| `duckdb_lake_relocate` | global | yes | ON | ON, OFF | When the lake catalog stores another data path (lake moved or restored elsewhere): ON accepts it if all its files are under `duckdb_data_path` and updates the stored path; OFF refuses to open the lake. Used when the lake opens. |
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
| `duckdb_apply_start_position` | global | yes (command) | `""` | binary log name, FIRST, CURRENT | Setting it restarts the applier at the start of that binary log, which becomes the applied position. Positions before the applied position are refused. A command: the variable stays empty afterwards. Not persisted. |
| `duckdb_discard_stale_copy` | global | yes (command) | `""` | `'db.table'` | Setting it drops the stale copy of a table that no longer exists. History copies need `duckdb_unload_history_confirm=ON`. A command: the variable stays empty afterwards. Not persisted. |
| `duckdb_history_scope` | session, global, `SET_VAR` | yes | LIVE | LIVE, FULL | What queries see in history tables: live rows only, or also archived rows. FULL requires `DUCKDB_HISTORY_READER` and the `duckdb_guard` plugin, and always runs in DuckDB (or fails with the reason). |
| `duckdb_apply_debug_log` | global | yes | OFF | ON, OFF | Testing aid: one error log line per change and per table of each batch, with DuckDB row counts (needs `log_error_verbosity=3`). |
| `duckdb_offload_min_rows` | session, global | yes | 1000000 | 0 .. 2^64-1 | Also try a query in DuckDB when MySQL's plan reads a loaded table of at least this many rows by a full scan, full index scan or non-unique index lookup, whatever its estimated cost. 0: only `secondary_engine_cost_threshold` decides. |
| `duckdb_offload_wait_applied` | session | yes | ON | ON, OFF | Before a query runs in DuckDB, wait until the applier has applied this connection's last commit (at most 5 s, otherwise InnoDB). |
| `duckdb_history_check_only` | session | yes | OFF | ON, OFF | SECONDARY_LOAD of a history table only checks that it can take over a history copy (adopt, reattach, resync) and always ends with an error giving the result. |
| `duckdb_unload_history_confirm` | session | yes | OFF | ON, OFF | Allows SECONDARY_UNLOAD of a history table and `duckdb_discard_stale_copy` of a history copy (archived rows are deleted). |

Privilege registered by the plugin: `DUCKDB_HISTORY_READER` (needed for
`duckdb_history_scope = FULL`).

Server variables the plugin depends on: `log_bin`, `binlog_format=ROW`,
`binlog_row_image=FULL`, `binlog_row_metadata=FULL`, `gtid_mode=ON`,
`binlog_order_commits=ON`, `sync_binlog=1` (history tables; recommended for
all), `innodb_flush_log_at_trx_commit=1` (recommended), `binlog_encryption=OFF`,
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
| `duckdb_guard_forced` / `duckdb_guard_rejected` | FULL scope statements on history tables sent to DuckDB only / stopped with an error (DWL#7). |
| `duckdb_offload_queries` / `duckdb_offload_refused` / `duckdb_offload_last_refusal` | Queries run in DuckDB, queries refused (then run on InnoDB, or an error with FORCED), reason of the last refusal. |
| `duckdb_apply_purge_transactions` / `duckdb_apply_archived_rows` | Tagged purge transactions on loaded tables; rows kept as archived rows. |
| `duckdb_apply_ddl_refreshes` / `duckdb_apply_ddl_renames` / `duckdb_apply_ddl_deferred` / `duckdb_apply_ddl_failures` | Copies refreshed or renamed after DDL, refreshes deferred to a later rename, copies marked stale by a DDL. |
| `duckdb_applied_file` / `duckdb_applied_position` / `duckdb_applied_gtid` | Applied position: what DuckDB contains. |

Counters restart at 0 when the server starts.

---

## 12. Limitations of this version

- Offload covers the queries and constructs in section 6 (DWL#6 steps 6a,
  6b, 6c); everything else runs on InnoDB. String operations on `_ci` and other
  non-binary collations: after 6c (weight strings).
- KILL QUERY of a long query running in DuckDB is implemented but not yet
  tested.
- Not supported: `binlog_encryption=ON`, `duckdb_encryption`, XA transactions
  on loaded tables (the applier stops), partial JSON updates, the types listed
  in section 3.
- Space of dropped copies and old versions is not reclaimed yet (snapshot
  expiry, Phase 3).
- A refresh after DDL reloads the whole table and pauses the applier for all
  tables while it runs.
- The MTR suite (`storage/duckdb/mysql-test/duckdb`, 16 tests) covers the
  features; restarts with special options, replica, semi-sync, relocation
  and performance stay in `manual-test-plan.md`.
- Replicas: tested with a source without the plugin (section 2, Replicas).
  Loads, unloads and the history privilege need `sql_log_bin = 0` on the
  replica (server behaviour). With `super_read_only = ON` the refresh after a
  replicated DDL works only while `duckdb_guard` is active.
