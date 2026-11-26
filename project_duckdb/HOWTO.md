# DuckDB secondary engine: how-to

How to build, configure and use the DuckDB secondary engine of Percona Server
9.7 (POC, branch `9.7-duckdb`). It describes the code as it is on 2026-11-25:
DWL#1 to DWL#5 (with DWL#3 step 3b), **query offload (DWL#6)**: queries with
expressions, joins, subqueries, derived tables, views, CTEs and UNION /
INTERSECT / EXCEPT run in DuckDB, with every collation (section 6), the FULL
scope guard (DWL#7) and `INFORMATION_SCHEMA.DUCKDB_TABLES`. A step by step
guide for new users: `TUTORIAL.md`. Design and details:
`duckdb-secondary-engine-design.md` and the worklogs `DWL-0*.md`; manual
tests: `manual-test-plan.md`.

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
- The three submodules under `extra/duckdb/`: DuckDB, DuckLake, CRoaring
  (DuckLake only migrates a lake of an earlier version, section 2).
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

39 tests (about 1.5 hours with a debug build); see
`storage/duckdb/mysql-test/duckdb/README.md`.

### Results and checks

```bash
ls -l plugin_output_directory/ha_duckdb.so          # the plugin
./runtime_output_directory/duckdb_apply-t | tail -1  # [  PASSED  ] 41 tests.
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

The error log shows `DuckDB repository: file=.../repository.duckdb, created=yes|no, executor
threads=N` and `binlog reader started at ...`.

The lake directory (`duckdb_data_path`) holds `repository.duckdb`, a native DuckDB
database file with every copy, the hidden columns and the plugin's schema
`__mysql` (registry, applied position), its WAL `repository.duckdb.wal`, and `tmp/`
for spills.

### Move the lake (duckdb_data_path)

The lake file stores no path. After moving or restoring the lake directory
elsewhere, set `duckdb_data_path` to the new place and start the server; the
lake opens as it is. `duckdb_lake_relocate` matters only for a DuckLake lake
of an earlier version that is migrated (below).
The binary log index lists absolute paths too: when moving a whole
instance, fix `my.cnf` and the `*.index` files.

### Back up the lake

The lake is one file plus its WAL: copy `repository.duckdb` and
`repository.duckdb.wal` (if present) with mysqld stopped, or while the plugin is
disabled: `SET GLOBAL duckdb_enable = OFF` closes the lake (DuckDB writes
its WAL into the file when it closes), copy, then `SET GLOBAL duckdb_enable
= ON` (the applier resumes from the applied position, which is inside the
file, so the copy and its position always match). Queries run on InnoDB
meanwhile; FULL scope queries fail.

### Upgrade from a DuckLake lake (before 2026-11-25)

The repository file was named `lake.duckdb` on 2026-11-25 and is
`repository.duckdb` since 2026-11-26 ("lake" read as DuckLake): a
`lake.duckdb` and its WAL are renamed at startup (error log: "DuckDB
repository: ... renamed to ..."). The variable names (`duckdb_data_path`,
`duckdb_lake_relocate`, status `duckdb_lake_path`) are unchanged.

After checking the migrated repository, `catalog.ducklake.migrated` (and
its `.wal`) and the old `data/` directory can be deleted: they are kept
only to go back to a version before 2026-11-25 (rename the catalog back to
`catalog.ducklake`).


Earlier versions stored the lake as DuckLake (`catalog.ducklake` and Parquet
files under `data/`). At the first start of this version, if `repository.duckdb`
does not exist and `catalog.ducklake` does, the plugin copies the old lake
into `repository.duckdb.migrating`, compares every table's row count, then renames
it to `repository.duckdb` and the old catalog to `catalog.ducklake.migrated` (and
its `.wal`). The error log shows an info line `DuckDB repository: migrating the
DuckLake lake ...` and a warning `DuckDB repository migrated from DuckLake to ...: N
tables, M rows checked ...`. The old `data/` directory is left in place:
remove it after a check. A crash during the copy leaves only
`repository.duckdb.migrating`, removed at the next start, which migrates again;
nothing old is deleted. If the old catalog stores another data path (the lake
was moved or restored), `duckdb_lake_relocate = ON` (default) reads its files
from `duckdb_data_path`; OFF refuses and the plugin is not loaded. If both
`repository.duckdb` and `catalog.ducklake` are missing but `data/` contains files,
the catalog was lost: the plugin refuses to open the lake until it is
restored.

### Turn it off and on at runtime

```sql
SET GLOBAL duckdb_enable = OFF;  -- closes the lake, stops the applier; data kept
SET GLOBAL duckdb_enable = ON;   -- reopens it; the applier resumes where it stopped
```

---

## 3. Create tables with a DuckDB copy

Requirements for a table:

- a primary key;
- foreign keys are allowed, cascading ones too, with the default
  `innodb_native_foreign_keys = OFF`: MySQL 9.7 runs cascades in the SQL
  layer and writes the cascaded rows to the binary log, so the copies follow
  them (a tagged purge on a parent archives the cascaded rows of a history
  child). With `innodb_native_foreign_keys = ON` InnoDB applies cascades
  without logging them: a table that is the child of an `ON DELETE / ON
  UPDATE CASCADE`, `SET NULL` or `SET DEFAULT` key is refused (do not switch
  that variable to ON with such copies loaded: it is read-only and needs a
  restart, and the copies would miss cascades);
- supported column types: see "Data types and collations" below. Other
  types (VECTOR, DECIMAL above 38 digits) make the load fail,
  naming the column;
- no zero dates or zero timestamps in the data;
- columns marked `NOT SECONDARY` are not copied;
- string columns whose collation DuckDB cannot compare natively (`_ci`,
  `_as_cs`, `general_ci`, latin1 and other non-UTF-8 collations) get a
  hidden weight column in the copy (MySQL's sort weight of each value), so
  queries in DuckDB follow the MySQL collation. Copies loaded before
  2026-11-14 have none: SECONDARY_UNLOAD + SECONDARY_LOAD a classic table,
  SECONDARY_LOAD (resync) a history table.

### Data types and collations

How each MySQL column is stored in the DuckDB copy (the lake), and what that
means for queries. The rule behind every row: a query that runs in DuckDB
returns exactly what InnoDB returns; what cannot be translated exactly is
refused and runs on InnoDB (with `use_secondary_engine = FORCED`: an error
with the reason).

| MySQL type | DuckDB type in the lake | Notes |
|---|---|---|
| `TINYINT`, `SMALLINT`, `MEDIUMINT`, `INT`, `BIGINT` | `TINYINT`, `SMALLINT`, `INTEGER`, `INTEGER`, `BIGINT`; `UTINYINT` ... `UBIGINT` when `UNSIGNED` | Unsigned `+` and `*` run in DuckDB only when they cannot leave 0..2^64-1; unsigned `-` is refused (MySQL error 1690). |
| `DECIMAL(p,s)`, p ≤ 38 | `DECIMAL(p,s)` | Exact. Results MySQL declares above 38 digits: `duckdb_decimal_overflow` (section 6). |
| `DECIMAL(p,s)`, p > 38 | not supported | The table cannot be loaded. |
| `FLOAT`, `DOUBLE` | `FLOAT`, `DOUBLE` | FLOAT values are sent as FLOAT (MySQL's shortest digits). SUM / AVG: last digits can differ (`duckdb_offload_float_aggregates`). |
| `DATE` | `DATE` | Zero dates (`0000-00-00`, month or day 0) cannot be loaded. |
| `DATETIME(n)` | `TIMESTAMP` (microseconds) | Zero dates cannot be loaded. |
| `TIMESTAMP(n)` | `TIMESTAMP`, in UTC | MySQL stores UTC too; constants are converted in the session time zone as MySQL does. Functions on TIMESTAMP columns are refused (session time zone). Zero timestamps cannot be loaded. |
| `TIME(n)` | `BIGINT`, signed microseconds | Keeps MySQL's full range, -838:59:59 to 838:59:59 (DuckDB's TIME covers only one day). Compared, grouped and ordered as MySQL; most functions refused (section 6). |
| `YEAR` | `USMALLINT` (the year number) | A YEAR column is shown `0000` as MySQL does. |
| `BIT(n)` | `UBIGINT` (the value) | Sent to the client as bytes, as MySQL. |
| `ENUM` | `VARCHAR` (the value) + hidden `__n_<col>` `USMALLINT` (the element position) | `ORDER BY e` and `e = 2` / `e + 0` use the position, comparisons and MIN/MAX the string, as MySQL. |
| `SET` | `VARCHAR` (the value) + hidden `__n_<col>` `UBIGINT` (the bitmask) | As ENUM, with the bitmask. |
| `CHAR`, `VARCHAR`, `TINYTEXT` ... `LONGTEXT` | `VARCHAR` (UTF-8) + hidden `__w_<col>` `BLOB` for some collations (below) | Any character set is converted to UTF-8 at load. |
| `BINARY`, `VARBINARY`, `TINYBLOB` ... `LONGBLOB` | `BLOB` | The bytes as they are. |
| `JSON` | `JSON` (DuckDB's JSON type: text, validated) | The text MySQL prints for the value, the same from the load and from the binary log (the applier turns MySQL's binary JSON into it with the server's code). Selecting the column, `IS [NOT] NULL` and `COUNT(j)` run in DuckDB; comparisons, `->` / `->>`, JSON functions, GROUP BY, ORDER BY, DISTINCT, other aggregates and set operations on it are refused. Partial JSON updates (`binlog_row_value_options = PARTIAL_JSON`) make the copy stale: keep that variable empty. |
| Spatial types (`GEOMETRY`, `POINT`, `POLYGON` ...) | `BLOB` (SRID + WKB, MySQL's bytes) | Selecting the column, `IS [NOT] NULL` and `COUNT(g)` run in DuckDB; spatial functions and comparisons are refused. |
| `VECTOR` | not supported | The table cannot be loaded. |

Hidden columns (`__w_`, `__n_`, and `__archived_at` in history copies) are
in the lake only: MySQL users see the table's own columns; they are visible
when reading the lake directly (section 10).

How string comparisons follow the MySQL collation in DuckDB:

| MySQL collation | In DuckDB | Behaviour |
|---|---|---|
| binary strings (`BINARY`, `VARBINARY`, `BLOB`), NO PAD binary (`utf8mb4_0900_bin`) | the bytes | Exact. |
| PAD SPACE binary (`utf8mb4_bin`, `utf8mb3_bin`) | CHAR columns: the value itself (stored without trailing spaces, since 2026-11-26); VARCHAR and expressions: the value without trailing spaces (`rtrim`); a padded key to sort | `'a' = 'a '` as MySQL. |
| `latin1_bin`, `ascii_bin` | the hidden weight column; for expressions (no weight column) the value, as the bytes and their order match | Exact. |
| every other collation: `_ci`, `_ai_ci`, `_as_cs`, `utf8mb4_general_ci`, `utf8mb4_unicode_ci`, `latin1_swedish_ci` and other non-UTF-8 ones | the hidden weight column `__w_<col>`: MySQL's own sort weight of each value, computed at load and by the applier | Comparisons, GROUP BY, ORDER BY, joins and DISTINCT counts exactly as InnoDB, including accents and non-ASCII text. Not supported on such columns: LIKE and most string functions (refused); an expression has no weight column, so comparing or grouping it is refused. |

A value shown from a group (`GROUP BY name` under `_ci`) can be any of the
values the collation calls equal (`'José'`, `'jose'`): MySQL shows the
first row's. DuckDB checks each group of the result and the query is
refused when a group holds more than one stored spelling (section 6).

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
DROP TABLE orders_h;   -- history: its copy is KEPT, marked stale (warning)
```

A stale history copy can be reattached (same name, section 5), adopted by
another table (section 5), or deleted for good.

### Delete a history table and its history

| You want | Do |
|---|---|
| Drop the table **and** its history | `SET SESSION duckdb_unload_history_confirm = ON;` `ALTER TABLE orders_h SECONDARY_UNLOAD;` `SET SESSION duckdb_unload_history_confirm = OFF;` then `DROP TABLE orders_h;` |
| Delete the history of a table already dropped | `SET SESSION duckdb_unload_history_confirm = ON;` `SET GLOBAL duckdb_discard_stale_copy = 'shop.orders_h';` `SET SESSION duckdb_unload_history_confirm = OFF;` |
| Delete the history of a table detached with `SECONDARY_ENGINE = NULL` (the table stays; `SECONDARY_UNLOAD` is no longer possible) | the same `duckdb_discard_stale_copy` |
| Remove a copy that became stale because a refresh after DDL failed | `SECONDARY_UNLOAD` (history: with the confirmation), or `duckdb_discard_stale_copy` |
| Drop the table, keep the history for later | `DROP TABLE orders_h;` (nothing else) |

```sql
-- every stale copy in the lake
SELECT TABLE_SCHEMA, TABLE_NAME, RETAIN_HISTORY
  FROM information_schema.DUCKDB_TABLES WHERE STATE = 'STALE';
```

`duckdb_discard_stale_copy` works on stale copies only (a copy being
applied is removed with `SECONDARY_UNLOAD`), whether the MySQL table still
exists or not (2026-11-19; before, a stale copy whose table still existed
could not be removed). History copies need `duckdb_unload_history_confirm =
ON`; classic copies do not. If the MySQL table still has `SECONDARY_ENGINE =
duckdb`, it is simply not loaded afterwards. Until a stale copy is
discarded, a new table with the same name and `retain_history` reattaches it
on its first SECONDARY_LOAD (if the structure is the same).

Disk space: DuckDB reuses freed space in `repository.duckdb` at checkpoints; the
file does not shrink by itself.

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

The copy is no longer updated. To delete it instead (the table stays):
`duckdb_discard_stale_copy` with the confirmation (section 4, "Delete a
history table and its history"). To reattach it later (live rows from
InnoDB, archived rows kept):

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

**Status: DWL#6 steps 6a to 6d, T1, T2 and T3 steps 1 to 3 (TPC-H: 22 of 22 queries).** A query runs in DuckDB when all of this holds,
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
  LIKE (with ESCAPE; on binary collations of any character set, `\` as
  default escape as in MySQL), IS [NOT] NULL, AND, OR, NOT; `+ - *`, `/` on DOUBLE
  values and exact division of integers and decimals (as MySQL, with
  div_precision_increment), DIV, MOD, ABS, ROUND (not of DOUBLE); CASE, IF,
  IFNULL, COALESCE,
  NULLIF; YEAR, MONTH, DAYOFMONTH, DAYOFYEAR, QUARTER, DAYOFWEEK, WEEKDAY,
  HOUR, MINUTE, SECOND, DATE of DATE and DATETIME columns; and the
  functions of the review against DuckDB (DWL-06): EXTRACT (not WEEK),
  date +/- INTERVAL, DATE_ADD / DATE_SUB, DATEDIFF, TO_DAYS, LAST_DAY,
  DAYNAME / MONTHNAME, NOW() / CURRENT_TIMESTAMP, CURDATE() / CURRENT_DATE,
  CURTIME(), UTC_TIMESTAMP / UTC_DATE / UTC_TIME, UNIX_TIMESTAMP() (the
  value fixed at the start of the statement, as MySQL; not SYSDATE()),
  CAST / CONVERT of numbers and dates to SIGNED, UNSIGNED, DECIMAL, DOUBLE,
  CHAR, DATE, DATETIME (not of strings, DOUBLE or TIMESTAMP), DATE_FORMAT
  (English names; not the week codes %U %u %V %v %X %x; GROUP BY on it, and
  ORDER BY on numeric formats such as '%Y-%m'), FLOOR, CEILING, SIGN, TRUNCATE, SQRT, LN, LOG,
  GREATEST / LEAST of numbers, CHAR_LENGTH, LENGTH, SUBSTRING, LEFT, RIGHT,
  CONCAT, CONCAT_WS, TRIM, REPLACE, REVERSE, LPAD, RPAD, INSTR, LOCATE
  (refused because DuckDB differs: UPPER, LOWER, ASCII, EXP, POW,
  EXTRACT(WEEK)); COUNT, COUNT
  (DISTINCT), SUM, AVG (also DISTINCT), MIN, MAX; DISTINCT, GROUP BY, HAVING,
  ORDER BY, LIMIT, OFFSET;
- string comparisons, joins, GROUP BY, ORDER BY, COUNT(DISTINCT) and
  MIN/MAX follow the column's collation for every collation (`_bin`
  directly, `_ci`, `_as_cs`, `general_ci`, latin1 ... through the weight
  columns of the copy). Not offloaded on non-`_bin` strings: LIKE, SELECT
  DISTINCT, UNION DISTINCT / INTERSECT / EXCEPT, IN subqueries, functions
  over them in comparisons or GROUP BY (UPPER(c) = ...). A grouped string
  column shown in a join (`_ci` or PAD SPACE) is offloaded when each group
  of the result holds a single stored value (checked in the query, the
  statement runs first); a group with `'done'` and `'done '`, or `'Done'`
  and `'done'`, makes the query run on InnoDB, whose value depends on its
  join order (with the table's primary key in GROUP BY, as `GROUP BY s.id,
  s.name`, it is always offloaded) (a CHAR column under a binary
  collation is exact and offloaded: its equal values are identical); MIN/MAX of such strings in a
  join likewise. SELECT DISTINCT of `utf8mb4_bin`/`utf8mb3_bin` columns is
  refused;
- not translated, so answered by InnoDB: unsigned arithmetic that could
  leave 0..2^64-1 (subtraction, possible overflow), functions outside
  the reviewed list above (UPPER / LOWER, STR_TO_DATE, CAST of a string
  ...),
  date functions on TIMESTAMP columns, comparing a string column with a
  number;
- it is not inside a multi-statement transaction (`autocommit = 0`, or
  after BEGIN / START TRANSACTION): MySQL never offloads those (with
  FORCED: an error with the reason).

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
(usually within a second; the session that ran it waits for that, section
9): a query of another session in between runs on InnoDB (FORCED:
"Table has not been loaded").

The result is the same as on InnoDB: types, NULL order, exact SUM and AVG,
TIMESTAMP in the session time zone, trailing spaces ignored by
`utf8mb4_bin`. One accepted difference: grouping by a `utf8mb4_bin` column
shows one value per group, and when MySQL groups by sorting the value shown
may differ in trailing spaces (`'a'` or `'a '`: MySQL does not define which).

Read your own writes: with `duckdb_offload_wait_applied = ON` (default), a
query waits until the applier has applied this connection's last commit (at
most `duckdb_offload_wait_timeout_ms`, 5000 by default, otherwise it runs on
InnoDB; `duckdb_offload_waits` and `duckdb_offload_wait_timeouts` count
them). With OFF it may not see a commit made
less than about a second earlier.

### Prepared statements, stored procedures, user variables

Queries with prepared statement parameters (`PREPARE ... ?` / `EXECUTE
... USING`, and the server-side prepared statements of connectors: JDBC
`useServerPrepStmts=true`, PDO without emulation), with stored procedure
parameters and variables, and with user variables (`@x`) run in DuckDB:
their current value is used as a constant at every execution (since
2026-11-23). A query that assigns a user variable (`SELECT @x := col ...`)
runs on InnoDB.

### When MySQL uses DuckDB: changing the thresholds

With `use_secondary_engine = ON` MySQL plans every query for InnoDB first.
It then tries DuckDB when one of these holds:

1. **The estimated cost** of the InnoDB plan is above
   `secondary_engine_cost_threshold` (server variable, default 100000).
2. **The plan reads a loaded table of at least `duckdb_offload_min_rows`
   rows** (plugin variable, default 1,000,000) by a full table scan, a full
   index scan or a non-unique index lookup, whatever the estimated cost.
   InnoDB's estimates for those plans can be far too low.

If one holds and the plugin can translate the query exactly, it runs in
DuckDB. Otherwise it runs on InnoDB.

See the cost MySQL estimated, and why a query stayed on InnoDB:

```sql
SET SESSION use_secondary_engine = OFF;
SELECT grp, COUNT(*) FROM g GROUP BY grp;
SHOW SESSION STATUS LIKE 'Last_query_cost';        -- e.g. 18775.549000

SET SESSION use_secondary_engine = ON, optimizer_trace = 'enabled=on';
SELECT grp, COUNT(*) FROM g GROUP BY grp;
SELECT JSON_EXTRACT(TRACE, '$**.secondary_engine_not_used') FROM information_schema.OPTIMIZER_TRACE;
-- [{"cost": 18775.5, "reason": "The estimated query cost does not exceed
--   secondary_engine_cost_threshold.", "threshold": 100000.0}]
SET SESSION optimizer_trace = 'enabled=off';
```

Change them where they should apply:

```sql
-- this session
SET SESSION secondary_engine_cost_threshold = 10000;
SET SESSION duckdb_offload_min_rows = 100000;
-- new sessions from now on / also after a restart
SET GLOBAL secondary_engine_cost_threshold = 10000;
SET PERSIST secondary_engine_cost_threshold = 10000;
SET PERSIST duckdb_offload_min_rows = 100000;
-- one query (optimizer hints)
SELECT /*+ SET_VAR(secondary_engine_cost_threshold = 0) */ grp, COUNT(*) FROM g GROUP BY grp;
SELECT /*+ SET_VAR(duckdb_offload_min_rows = 1000) */ grp, COUNT(*) FROM g GROUP BY grp;
SELECT /*+ SET_VAR(use_secondary_engine = FORCED) */ grp, COUNT(*) FROM g GROUP BY grp;
```

- `secondary_engine_cost_threshold = 0`: every query the plugin can
  translate goes to DuckDB, also small ones (each pays about a millisecond
  of DuckDB setup).
- `duckdb_offload_min_rows = 0` disables rule 2.
- A primary key lookup or a short index range is cheap on InnoDB and
  stays there with the defaults.
- `use_secondary_engine = FORCED` skips both rules: DuckDB, or an error
  with the reason.

### DECIMAL results above 38 digits

MySQL declares DECIMAL results up to 65 digits (an aggregate adds about 22
digits, a division more), DuckDB computes up to 38. The plugin computes such
results as `DECIMAL(38, scale)`: exact while the values fit, which is every
realistic case (the integer part can have 38 - scale digits). Statements
where a value could exceed 38 digits (declared above 38, or an exact
division whose scaled dividend could) run in DuckDB first, during
optimization; the result is kept and sent. `duckdb_decimal_overflow`
decides what an overflow does:

| Value | When a value does not fit |
|---|---|
| `FALLBACK` (default) | The query runs on InnoDB: results always identical (with FORCED: an error with the reason). |
| `ROUND` | The query runs again in DuckDB with fewer decimals, and a warning says how many were kept (`DECIMAL results rounded to 3 decimals ...`). Helps when the overflow comes from the scale (exact division); when DuckDB's own arithmetic overflows (a SUM above 38 digits) it is as FALLBACK. |
| `ERROR` | No first run: the statement streams as usual and fails with DuckDB's error, also with ON. |

```sql
SET SESSION duckdb_decimal_overflow = ROUND;
SELECT /*+ SET_VAR(duckdb_decimal_overflow = FALLBACK) */ SUM(amount) / 3 FROM orders;
```

### YEAR, BIT, TIME, ENUM and SET in queries

They are offloaded with MySQL's results; what is not translated is refused
(InnoDB with ON, an error with FORCED):

| Type | Offloaded | Refused |
|---|---|---|
| YEAR | comparisons (constants as MySQL reads them: `y = 5` is 2005, `y = '0'` 2000, `y = 0` 0000), GROUP BY, ORDER BY, MIN/MAX, `y + 1`; a YEAR column is shown `0000`, an expression such as `MAX(y)` as `0`, as MySQL does | `y - 1900` (MySQL raises error 1690 for 0000), string contexts (`LIKE`, `IF(..., y, 'x')`), a YEAR column of a UNION |
| BIT | comparisons with numbers and `b'..'` / `0x..` literals, `b + 0`, MIN/MAX (sent as bytes), GROUP BY, ORDER BY | `b64 + 1` (may pass 2^64-1), `b + 0.5`, `HEX(b)`, `BIN(b)`, string contexts |
| TIME | comparisons with TIME columns and constants (`'12:30:45'`, `123045`, `'1 02:00:00'`, `12.5` converted to TIME as MySQL does), BETWEEN, IN, GROUP BY, ORDER BY, MIN/MAX, COUNT(DISTINCT); negative values and more than 24 hours | SUM/AVG and `t + 0` (MySQL uses the number HHMMSS), comparison with DATETIME, `HOUR()` and other functions, TIME expressions in the result (`IFNULL(t, ...)`) |
| ENUM, SET | string comparisons with the column collation (`e = 'MEDIUM'` on `_ci`), IN, BETWEEN, LIKE (binary collations), MIN/MAX (by string, as MySQL), GROUP BY, COUNT(DISTINCT); `ORDER BY e` sorts by the element position, `e = 2`, `e + 0`, `s * 2` use the index / bitmask | SUM, `FIND_IN_SET`, `CONCAT`, ORDER BY an ENUM of a materialized derived table, the existing collation rules (LIKE on `_ci`, DISTINCT on PAD SPACE) |

Unsigned arithmetic (`+`, `*`) is offloaded only when it cannot leave
0..2^64-1 (unsigned columns and non-negative constants whose largest values
keep the result within 64 bits); otherwise MySQL may raise error 1690 on
rows DuckDB evaluates differently, so it is refused.

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

### The statement after a DDL waits for the refresh

With `duckdb_ddl_wait_refresh = ON` (default), after a DDL on a table with a
copy the session's **next statement** waits until the applier has brought
the copy in line (refresh, rename). The DDL itself returns at once: it
holds the table's metadata lock until the server answers, and the refresh
needs that lock. Whatever the session runs after the DDL sees the copy in
DuckDB; scripts need no waiting loop.

```sql
ALTER TABLE orders ADD COLUMN channel VARCHAR(10) NOT NULL DEFAULT 'web';
SELECT channel, COUNT(*) FROM orders GROUP BY channel;   -- waits, then runs in DuckDB
```

While it waits, the session shows the progress (any other session can look):

```sql
SHOW PROCESSLIST;
-- State: Waiting for DuckDB copy refresh of shop.orders: 2400000 of about 5000000 rows (48%), 35 s
SELECT TABLE_NAME, STATE, REFRESH_ROWS_DONE, REFRESH_ROWS_TOTAL
  FROM INFORMATION_SCHEMA.DUCKDB_TABLES WHERE STATE = 'REFRESHING';
-- Performance Schema stages, with WORK_COMPLETED / WORK_ESTIMATED
-- (enable stage/duckdb/% and the events_stages_current consumer):
SELECT EVENT_NAME, WORK_COMPLETED, WORK_ESTIMATED
  FROM performance_schema.events_stages_current WHERE EVENT_NAME LIKE 'stage/duckdb/%';
```

- `duckdb_ddl_wait_refresh_timeout` (seconds, 0 = until done) limits the
  wait; `KILL QUERY` stops it. The DDL is committed either way: until the
  refresh is done, queries on the table run on InnoDB (under
  `duckdb_history_scope = FULL` they wait). The error log records a wait
  that took 1 s or more, or that stopped.
- `SET SESSION duckdb_ddl_wait_refresh = OFF` before a series of DDL
  statements skips the waits (each refresh still happens).
- A DDL on a table without a copy, and index-only changes, cause no wait
  worth noting (nothing to refresh).
- DML needs nothing similar: an offloaded query already waits for the
  session's last commit (`duckdb_offload_wait_applied`, section 6).
- `SECONDARY_LOAD` shows its own progress in the same places (stage
  `stage/duckdb/copy load`).

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

### Which tables are in the lake

```sql
SELECT TABLE_SCHEMA, TABLE_NAME, STATE, RETAIN_HISTORY, PRIMARY_KEY
  FROM INFORMATION_SCHEMA.DUCKDB_TABLES ORDER BY 1, 2;
```

One row per copy in the lake (the plugin's registry,
`lake.__mysql.tables`):

| Column | Content |
|---|---|
| `TABLE_SCHEMA`, `TABLE_NAME` | the MySQL table (after a RENAME TABLE, the new name once the applier has read it) |
| `STATE` | `LOADED` (kept current by the applier), `REFRESHING` (the applier is reloading it after a DDL), `STALE` (kept but no longer updated: the history copy of a dropped table, or a refresh after DDL that failed; see the error log), `REFRESH_PENDING` (a refresh waits for a later RENAME) |
| `RETAIN_HISTORY` | `YES` for history tables |
| `PRIMARY_KEY` | primary key columns, `a, b`; NULL for copies loaded before the key was recorded (the applier fills it) |
| `WEIGHT_COLUMNS` | columns with a hidden weight column (non-binary collations, section 6); NULL if none or loaded before weights |
| `LOAD_BINLOG_FILE`, `LOAD_BINLOG_POSITION`, `LOAD_GTID_SET` | binary log position and GTID set at load time: the applier skips what the load already contains |
| `REFRESH_ROWS_DONE`, `REFRESH_ROWS_TOTAL` | while `REFRESHING`: rows copied so far and InnoDB's estimate of the total; NULL otherwise |

Tables with `SECONDARY_ENGINE=duckdb` that are not loaded have no row; a
`STALE` row whose table no longer exists is a history copy waiting for a
reattach or `duckdb_discard_stale_copy` (section 5). The table is empty while
the lake is closed (`duckdb_enable=OFF`) and for accounts without the
`PROCESS` privilege (as InnoDB's INFORMATION_SCHEMA tables).
`INFORMATION_SCHEMA.TABLES` shows the same for one table in `CREATE_OPTIONS`
(`SECONDARY_LOAD="1"`), without the state.

### Reading the lake while MySQL is down

The copies are a native DuckDB database file, `repository.duckdb`, in
`duckdb_data_path` (default `<datadir>/duckdb_data`). With MySQL stopped,
DuckDB itself can read it. Use `duckq` (`project_duckdb/tools/duckq.c`),
built from the same DuckDB libraries as the plugin, so the versions match
(DuckDB v2.0.0):

```bash
B=/opt/mysql_source/percona-server-9.7-build          # the build directory
L=$B/extra/duckdb/install/lib
clang++-21 -x c /opt/mysql_source/percona-server/project_duckdb/tools/duckq.c -x none \
  -I$B/extra/duckdb/install/include -o ~/duckq \
  -Wl,--start-group $(ls $L/lib*.a) -Wl,--end-group -lpthread -ldl
```

Each argument after the database is one SQL statement; results are
tab-separated:

```bash
LAKE=/path/to/datadir/duckdb_data
A="ATTACH '$LAKE/repository.duckdb' AS lake (READ_ONLY)"
~/duckq :memory: "$A" \
  "SELECT schema_name, table_name FROM duckdb_tables() WHERE database_name = 'lake' ORDER BY 1, 2" \
  "SELECT db, tbl, state, retain_history FROM lake.__mysql.tables"
~/duckq :memory: "$A" \
  "SELECT region, COUNT(*) FROM lake.shop.orders WHERE \"__archived_at\" IS NULL GROUP BY region"
~/duckq :memory: "$A" \
  "COPY (SELECT * FROM lake.shop.orders) TO '/backup/orders.parquet' (FORMAT parquet)"
```

What to know when reading the tables:

- **Names:** MySQL schema `db`, table `t` is `lake.db.t`. The registry of
  copies is `lake.__mysql.tables`.
- **History tables:** the column `__archived_at` is NULL for live rows and
  holds the purge time for archived rows. `WHERE "__archived_at" IS NULL`
  gives what InnoDB has.
- **Hidden columns:** `__w_<col>` holds MySQL's sort weight (BLOB) for
  collations DuckDB cannot compare. `__n_<col>` holds the ENUM index or SET
  bitmask.
- **Values as stored:**
  - Strings: UTF-8, CHAR without trailing spaces.
  - TIMESTAMP: in UTC.
  - TIME: signed microseconds (`t / 1000000` gives seconds).
  - YEAR and BIT: numbers.
  - ENUM and SET: their string value.
- **Read only:** always attach with `READ_ONLY`. A change made outside MySQL
  is not in InnoDB and is not known to the applier, so the copy would no
  longer match.
- **MySQL must be stopped:** while it runs, mysqld holds a lock on the
  lake file and the attach fails with `Could not set lock on file ...
  Conflicting lock is held in .../mysqld`. To read while MySQL runs, query
  through MySQL (`use_secondary_engine = FORCED`), or copy the whole
  `duckdb_data` directory while MySQL is stopped and read the copy.
- **Other DuckDB clients:** a DuckDB CLI or Python package also works if
  its DuckDB version matches the plugin's (see the `extra/duckdb`
  submodules). A different version may refuse the file or, worse, upgrade
  it.

### Status variables

For PMM / Grafana: a dashboard and custom queries for these numbers, the
copies and the load progress are in `project_duckdb/monitoring/` (README
there).

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

Binary logs reset (`RESET BINARY LOGS AND GTIDS`) while copies are loaded:
at its next start the applier stops with "the applied position ... is after
the end of the binary log". Restart it at the new log, then reload the
copies that changed before the reset (those changes never reached DuckDB):

```sql
SET GLOBAL duckdb_apply_start_position = 'FIRST';   -- accepted after a reset
ALTER TABLE <each changed classic table> SECONDARY_UNLOAD;
ALTER TABLE <each changed classic table> SECONDARY_LOAD;
ALTER TABLE <each changed history table> SECONDARY_LOAD;   -- resync, history kept
```

The repoint forgets the load positions of the old log (they would make the
applier skip the new log's changes) and lists every copy in the warning.
Before 2026-11-20 `FIRST` was refused here ("before the applied position").

A query that should run in DuckDB but does not: `SHOW GLOBAL STATUS LIKE
'duckdb_offload_last_refusal'` gives the reason of the last refusal; run it
with `use_secondary_engine = FORCED` to get the reason as an error, or look
at the optimizer trace (section 7). Common reasons: `collation:<column>`,
`unsupported_function:<name>`, `not_loaded:<table>`.
Below `secondary_engine_cost_threshold` it is not even tried (no refusal).

A DuckDB error while the query starts (out of memory, temporary disk full)
is a refusal too, with the reason `duckdb_execution: <DuckDB error>`: with
`use_secondary_engine = ON` InnoDB answers the query (since 2026-11-23).
DuckDB runs the heavy parts of a query (joins, aggregates, sorts) before
the first rows, so that covers the usual cases; an error after rows were
sent still reaches the client. Many `duckdb_execution` refusals: raise
`duckdb_memory_limit` or check the disk of the lake's `tmp/` directory.

Test helpers (progress you can watch, clean stop): `duckdb_test_helpers.sql`.

---

## 11. Parameters reference

### System variables

| Variable | Scope | Dynamic | Default | Range / values | Description |
|---|---|---|---|---|---|
| `duckdb_enable` | global | yes | ON | ON, OFF | Opens or closes the lake and starts or stops the applier. OFF behaves as if the plugin were not loaded; no data is deleted. |
| `duckdb_lake_relocate` | global | yes | ON | ON, OFF | Only when a DuckLake lake of an earlier version is migrated at startup and its catalog stores another data path (moved or restored elsewhere): ON reads its files from `duckdb_data_path`, OFF refuses. The native lake file stores no path: a moved lake always opens. |
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
| `duckdb_apply_start_position` | global | yes (command) | `""` | binary log name, FIRST, CURRENT | Setting it restarts the applier at the start of that binary log, which becomes the applied position. Positions before the applied position are refused (except after a binary log reset, section 10). A command: the variable stays empty afterwards. Not persisted. |
| `duckdb_discard_stale_copy` | global | yes (command) | `""` | `'db.table'` | Setting it drops a stale copy (after DROP TABLE or SECONDARY_ENGINE = NULL of a history table, or a failed refresh), whether the MySQL table exists or not. History copies need `duckdb_unload_history_confirm=ON`. A command: the variable stays empty afterwards. Not persisted. |
| `duckdb_history_scope` | session, global, `SET_VAR` | yes | LIVE | LIVE, FULL | What queries see in history tables: live rows only, or also archived rows. FULL requires `DUCKDB_HISTORY_READER` and the `duckdb_guard` plugin, and always runs in DuckDB (or fails with the reason). |
| `duckdb_apply_debug_log` | global | yes | OFF | ON, OFF | Testing aid: one error log line per change and per table of each batch, with DuckDB row counts (needs `log_error_verbosity=3`). |
| `duckdb_offload_min_rows` | session, global, `SET_VAR` | yes | 1000000 | 0 .. 2^64-1 | Also try a query in DuckDB when MySQL's plan reads a loaded table of at least this many rows by a full scan, full index scan or non-unique index lookup, whatever its estimated cost. 0: only `secondary_engine_cost_threshold` decides. |
| `duckdb_decimal_overflow` | session, global, `SET_VAR` | yes | FALLBACK | FALLBACK, ROUND, ERROR | DECIMAL results MySQL declares above 38 digits are computed as DECIMAL(38, scale) and the statement runs in DuckDB first; if a value does not fit: FALLBACK runs it on InnoDB (FORCED: error), ROUND runs it again with fewer decimals and a warning (when that helps; otherwise as FALLBACK), ERROR fails with DuckDB's error (no first run). |
| `duckdb_offload_wait_applied` | session, global, `SET_VAR` | yes | ON | ON, OFF | Before a query runs in DuckDB, wait until the applier has applied this connection's last commit (at most `duckdb_offload_wait_timeout_ms`, otherwise InnoDB). |
| `duckdb_offload_wait_timeout_ms` | session, global, `SET_VAR` | yes | 5000 | 0 - 3600000 | How long that wait lasts, in milliseconds. |
| `duckdb_ddl_wait_refresh` | session, global, `SET_VAR` | yes | ON | ON, OFF | After a DDL on a table with a copy, the session's next statement waits until the applier has refreshed the copy (section 9), showing the progress in SHOW PROCESSLIST and Performance Schema. |
| `duckdb_ddl_wait_refresh_timeout` | session, global, `SET_VAR` | yes | 0 | 0 - 31536000 | Longest wait of `duckdb_ddl_wait_refresh`, in seconds; 0 = until the refresh is done. |
| `duckdb_offload_float_aggregates` | session, global, `SET_VAR` | yes | APPROXIMATE | APPROXIMATE, REFUSE | SUM and AVG of FLOAT / DOUBLE: DuckDB adds in parallel in no fixed order, so the last digits can differ from InnoDB and between runs. APPROXIMATE runs them in DuckDB; REFUSE runs such queries on InnoDB (under FULL scope they fail). |
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
| `duckdb_offload_queries` / `duckdb_offload_refused` / `duckdb_offload_last_refusal` | Queries run in DuckDB to the end (not EXPLAIN, not statements that failed while running), queries refused (then run on InnoDB, or an error with FORCED), reason of the last refusal. |
| `duckdb_offload_explained` | EXPLAIN of statements that would run in DuckDB. |
| `duckdb_offload_waits` / `duckdb_offload_wait_timeouts` | Waits for the connection's last commit, and those that timed out (the query ran on InnoDB). |
| `duckdb_offload_decimal_reruns` | Runs again with fewer decimals under `duckdb_decimal_overflow = ROUND`. |
| `duckdb_offload_time_us` | Total time in DuckDB of the queries counted in `duckdb_offload_queries` (microseconds); divided by them, the average offloaded query time. |
| `duckdb_version` | Plugin version, the git commit it was built from (`-dirty`: with uncommitted changes) and the DuckDB library version; also in `INFORMATION_SCHEMA.PLUGINS.PLUGIN_DESCRIPTION` and the error log at startup. |
| `duckdb_apply_lag_seconds` | Age of the oldest transaction committed in MySQL and not yet in DuckDB (0 when everything is applied). |
| `duckdb_apply_lag_bytes` | Binary log between the applied position and its end, across files. |
| `duckdb_apply_reader_state_code` | The applier state as a number: 0 idle, 1 reading, 2 waiting to commit, 3 committing, 4 refreshing, 5 error, 6 stopped (for alerts). |
| `duckdb_memory_used_bytes` / `duckdb_temp_spill_bytes` | DuckDB memory in use and data spilled to its temporary directory, sampled about every 5 s. |
| `duckdb_apply_purge_transactions` / `duckdb_apply_archived_rows` | Tagged purge transactions on loaded tables; rows kept as archived rows. |
| `duckdb_apply_ddl_refreshes` / `duckdb_apply_ddl_renames` / `duckdb_apply_ddl_deferred` / `duckdb_apply_ddl_failures` | Copies refreshed or renamed after DDL, refreshes deferred to a later rename, copies marked stale by a DDL. |
| `duckdb_applied_file` / `duckdb_applied_position` / `duckdb_applied_gtid` | Applied position: what DuckDB contains. |

Counters restart at 0 when the server starts.

---

## 12. Limitations of this version

Upgrade note (2026-11-26): CHAR columns are compared without `rtrim()`,
which needs CHAR values stored without trailing spaces. A table loaded by
an earlier version from a session with `sql_mode` containing
`PAD_CHAR_TO_FULL_LENGTH` may hold padded CHAR values: reload it (`ALTER TABLE ... SECONDARY_UNLOAD;
... SECONDARY_LOAD`). For a history table, SECONDARY_LOAD again resyncs the
live rows; archived rows loaded that way keep their padding (equality on
such a CHAR value then misses them).


- Offload covers the queries and constructs in section 6 (DWL#6 steps 6a
  to 6d, T1 to T3 step 3, weight strings, the types of section 6, the
  reviewed date, number and string functions); everything else runs on
  InnoDB: window functions, ROLLUP, UPPER / LOWER, STR_TO_DATE, functions on
  TIMESTAMP columns, CAST of strings, GROUP_CONCAT, LIKE and string functions
  on non-binary collations, a grouped string in a join whose groups mix
  values equal under the collation ('a' / 'a ' / 'A').
- MySQL never offloads a query inside a multi-statement transaction
  (`autocommit = 0`, or after BEGIN / START TRANSACTION): such queries run
  on InnoDB. HammerDB's TPROC-H driver must run with autocommit on (the
  patched driver `project_duckdb/tpch/mysql_tproch_run.tcl`).
- Not supported: `binlog_encryption=ON`, `duckdb_encryption`, XA transactions
  on loaded tables (the applier stops), partial JSON updates, the types listed
  in section 3.
- `DROP PARTITION` / `TRUNCATE PARTITION` of a history table do not archive:
  the copy is refreshed and the partition's rows are gone. Purge the
  partition first (`DELETE ... PARTITION (p)` in a tagged transaction), then
  drop it (TUTORIAL.md section 8).
- The lake file does not shrink by itself: DuckDB reuses the space of
  deleted rows at checkpoints.
- A refresh after DDL reloads the whole table and pauses the applier for all
  tables while it runs.
- The MTR suite (`storage/duckdb/mysql-test/duckdb`, 39 tests) covers the
  features; restarts with special options, replica, semi-sync, relocation
  and performance stay in `manual-test-plan.md`.
- Replicas: tested with a source without the plugin (section 2, Replicas).
  Loads, unloads and the history privilege need `sql_log_bin = 0` on the
  replica (server behaviour). With `super_read_only = ON` the refresh after a
  replicated DDL works only while `duckdb_guard` is active.
