# DuckDB as a MySQL Secondary Engine

**Design and Implementation Plan**

Status: v0.4, 2026-11-17: the POC is implemented and the design below describes what was built (decisions taken during development are marked with their date). Owner: Marco. Target: Percona Server for MySQL; the POC is built on the Percona-Server-9.7.2-2 branch (final target release still open, see Open questions). Step by step status: `worklog-index.md`; user guides: `HOWTO.md`, `TUTORIAL.md`.

Changes from v0.3 (what the implementation added or changed, 2026-11-07 to 2026-11-17):

- All five POC features work end to end (DWL#1 to DWL#7), with an MTR suite of 26 tests, unit tests and a release-build performance run (offloaded queries 54x to 584x faster than InnoDB).
- DDL no longer unloads a copy: the applier refreshes it (archived rows kept), follows RENAME TABLE, and marks it stale only when it cannot follow (DWL#4 step 4d).
- History copies survive DROP TABLE and can be reattached, resynced or adopted by another table (DWL#3 step 3b).
- The applier is one reader thread that decodes, collapses and commits batches; the parallel prepare workers of v0.3 were not needed and are deferred.
- Collations: hidden weight columns (`__w_<col>`) give MySQL's exact semantics for every collation in comparisons, grouping, ordering and joins (DWL#6 step A).
- Types added: YEAR, BIT, TIME, ENUM, SET (hidden `__n_<col>` index column); exact DECIMAL division as MySQL computes it.
- Offload also when a large table is scanned (`duckdb_offload_min_rows`), with read-your-own-writes and error mapping (1242, 1317, 3024).
- The guard also stops UPDATE / DELETE of history tables under FULL and lets the plugin's own refresh pass `super_read_only`.
- `INFORMATION_SCHEMA.DUCKDB_TABLES`, lake relocation (`duckdb_lake_relocate`), tables with foreign keys (2026-11-17), replicas tested.
- DECIMAL results beyond 38 digits (`duckdb_decimal_overflow`), LIKE on binary collations, grouped CHAR in joins, the reviewed date / number / string functions (DWL#6); TPC-H 19 of 22 offloaded.
- New sections: production readiness checklist and SQL gaps by priority (plan steps 5-10), SQL coverage tracker (what queries can contain, kept current) and an updated phased plan with the TPC-H / HammerDB proof.

Changes from v0.2:

- Storage is DuckLake (DuckDB catalog file plus Parquet data files) under `<datadir>/duckdb_data`, configurable with `duckdb_data_path`.
- No change to core server code: everything lives in the plugin. Change propagation therefore uses a binlog reader inside the plugin, not commit hooks.
- Purge deletes are identified by a tagged GTID (`duckdb_avoid_purge`, configurable), replacing `duckdb_purge_mode`.
- Applier model: one binlog reader, parallel prepare workers, one committer with one DuckLake commit per batch.
- Query offload uses the server's external executor hook; SQL text is generated from the query block; joins are offloaded when all tables are DuckDB tables.
- A guard plugin in the same library makes FULL scope never fall back to InnoDB.
- Full list of plugin variables, startup sequence and directory checks.
- Build integration verified: DuckDB v2.0.0, DuckLake and CRoaring built inside one CMake run (ExternalProject) with the server's C++ standard (C++23), verified with C++20 and a smoke test.
- New section: POC scope. The phased plan is updated accordingly.

## Summary

We propose to build a secondary engine plugin that embeds DuckDB inside mysqld. InnoDB stays the primary store for writes and keeps only live data; selected tables get a columnar copy in DuckDB, stored as a DuckLake, that also retains the full history, and the optimizer offloads eligible analytical queries to it. Analysts keep using the MySQL protocol, accounts and SQL, while large scans, aggregations and joins run vectorized on columnar data instead of row by row in InnoDB.

Purge deletes on InnoDB (removing old rows to keep the table small) are not applied to DuckDB as deletes: they are recorded as archive marks, so DuckDB can answer both "live data" and "full history" queries. Which of the two a query on the base table name sees is driven by a parameter at global, session and statement level.

Everything is implemented as a plugin, without changes to core server code. The secondary engine framework provides load, unload and offload; changes reach DuckDB through a binlog reader inside the plugin. The effort and risk concentrate in change propagation, plan translation, semantic equivalence, and the durability of the history, because DuckDB becomes the only copy of archived rows.

## Goals

- Run analytical queries on MySQL data with DuckDB vectorized execution, through the normal MySQL protocol.
- Keep InnoDB tables small, holding live data only, while the complete dataset stays queryable for OLAP in DuckDB.
- Let each server and each session choose whether a base table name means live data or the full history.
- Never return a silently different answer: when a query cannot be answered correctly, it fails with an explicit error.
- No change to core server code: the whole feature ships as one plugin library.

## Background

### DuckDB in brief

DuckDB is an embedded analytical database. It has no server process: it runs as a library inside the program that uses it. Data is stored by column, compressed, and processed in vectorized batches using all available cores. This makes it very fast for scans, aggregations and joins over many rows, and weak at point lookups and frequent small writes. Only one process can write to a DuckDB database at a time.

### DuckLake in brief

DuckLake is a DuckDB extension that splits storage into two parts: a catalog (metadata, snapshots, table definitions) kept in an SQL database, and the table data kept as Parquet files in a data path. Every commit creates a snapshot; small changes can be kept in the catalog (data inlining, on by default with a threshold of 10 rows) until they are flushed to Parquet. DuckLake supports DuckDB, SQLite and PostgreSQL as catalog backends; we use a DuckDB file.

### Three ways to put DuckDB inside MySQL

| Approach | How it works | Main strength | Main problem |
|---|---|---|---|
| Storage engine | DuckDB implements the handler API; tables are created with ENGINE=DuckDB | Deepest integration, data lives only in DuckDB | The MySQL executor pulls rows one at a time, which discards vectorized execution; two-phase commit with the binlog and atomic DDL are hard to support |
| Secondary engine | InnoDB keeps the data; DuckDB holds a loaded copy and receives whole offloaded queries | DuckDB runs the complete query, so we keep its execution model | We must build change propagation and plan translation ourselves |
| Component or UDF | A loadable function runs SQL in DuckDB and returns JSON | Fast to build, no engine integration needed | No table semantics, no optimizer awareness, long queries run unmanaged inside mysqld |

The secondary engine is the approach that best matches how DuckDB works and the only one that supports the hot and cold split we want (live rows in InnoDB, full history in DuckDB), which is why we choose it here. Appendix A summarizes how AliSQL solved the storage engine problems instead.

### State of the framework and prior work

- MySQL ships the secondary engine framework and a test stub, the MOCK engine in storage/secondary_engine_mock. Its header states that no secondary engine is yet mature enough to be merged into mysql-trunk, so the stub exists to test the server code paths. The stub implements load_table() and unload_table() without storing data, offers no index access, and lets the MySQL executor run the plan (its rnd_next() returns end of file, so offloaded queries return no rows). The MTR suite in mysql-test/suite/secondary_engine shows the expected behavior from the SQL side.
- The framework in this tree also has an external executor hook: with the `USE_EXTERNAL_EXECUTOR` secondary engine flag (sql/handler.h) the server builds no iterators and hands each query block to `JOIN::override_executor_func` (sql/sql_optimizer.h, called from sql/sql_union.cc). No open-source engine uses it yet; we would be the first.
- HeatWave is the only production secondary engine from Oracle and it is closed source. Its user-facing syntax (SECONDARY_ENGINE, SECONDARY_ENGINE_ATTRIBUTE, SECONDARY_LOAD, use_secondary_engine) is in community MySQL and we reuse it as is.
- ShannonBase is an open-source MySQL fork, compatible with MySQL 8.4 LTS, whose Rapid engine is an in-memory column store working as a secondary engine. It offloads queries whose cost exceeds secondary_engine_cost_threshold, loads tables with a full scan at SECONDARY_LOAD, and keeps InnoDB and Rapid in sync through DML notification, which required server changes. It also requires a primary key on secondary tables.
- AliSQL, Alibaba's open-source MySQL 8.0.44 branch, integrates DuckDB as a storage engine. They explicitly rejected the secondary engine approach because it needs load, refresh and consistency state for two copies. Their design is summarized in Appendix A.

## Architecture

```
                         mysqld
 +---------------------------------------------------------------+
 |  Parser -> optimizer --(offload?)--+                          |
 |              | no                  | yes                      |
 |              v                     v                          |
 |       InnoDB executor     query block -> DuckDB SQL           |
 |              |                     |  (external executor)     |
 |              |                     v                          |
 |  InnoDB --commit--> binlog   DuckDB engine (in memory)        |
 |  (live data)          |        |  ATTACH ducklake             |
 |                       v        v                              |
 |        plugin applier (reader -> workers -> committer)        |
 |        purge-tagged deletes = archive marks                   |
 |                                |                              |
 |          <datadir>/duckdb_data: catalog.ducklake + Parquet    |
 |          (full history)                                       |
 +---------------------------------------------------------------+
```

One DuckDB instance lives inside mysqld, in memory, with one DuckLake attached as `lake`. Each MySQL session that touches DuckDB gets its own DuckDB connection. No DuckDB call runs on a MySQL thread: DuckDB plans statements recursively and overflows the small stack of connection threads, so every DuckDB call runs on a plugin-owned executor pool (`duckdb_executor_threads`, `duckdb_executor_stack_size`) while MySQL work stays on the session thread (DWL#2, DuckDB executor threads). DuckDB, its extensions, DuckLake and CRoaring are linked statically into the plugin library; DuckDB and DuckLake are MIT licensed, compatible with GPL-2.0 (third-party licenses to be confirmed for the SBOM).

The library `ha_duckdb.so` contains three plugins:

- `duckdb`: the secondary engine (load, unload, offload, applier, variables).
- `duckdb_guard`: an audit (event tracking) plugin that enforces the FULL scope rules (see Query offload) and lets the plugin's internal refresh pass `read_only` / `super_read_only`.
- `DUCKDB_TABLES`: an INFORMATION_SCHEMA plugin, `INFORMATION_SCHEMA.DUCKDB_TABLES`, one row per copy in the lake (state, history flag, primary key, weight columns, load position).

## User interface

```
-- We declare DuckDB as the secondary engine and enable history retention
ALTER TABLE orders
  SECONDARY_ENGINE = duckdb,
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';

-- We load the current data into the secondary copy
ALTER TABLE orders SECONDARY_LOAD;

-- We let the optimizer offload eligible queries
-- (FORCED makes offload mandatory, useful in tests)
SET use_secondary_engine = ON;

-- We set the server default: base table names mean live data only
SET PERSIST duckdb_history_scope = 'LIVE';

-- We switch one session to the full history under the same table name
SET SESSION duckdb_history_scope = 'FULL';

-- We ask for the full history in a single statement only
SELECT /*+ SET_VAR(duckdb_history_scope = 'FULL') */
       YEAR(created_at), SUM(amount)
FROM orders GROUP BY 1;

-- We tag the purge job so its deletes become archive marks in DuckDB
-- (requires the TRANSACTION_GTID_TAG privilege)
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM orders WHERE created_at < NOW() - INTERVAL 90 DAY;
SET gtid_next = 'AUTOMATIC';
```

Configuration file:

```ini
[mysqld]
plugin-load-add            = ha_duckdb.so
loose-duckdb_memory_limit  = 8G
loose-duckdb_threads       = 8
# optional
# loose-duckdb_data_path   = /data/duckdb_data
```

The `loose-` prefix lets the server start when the plugin is not loaded; without it, mysqld refuses to start with "unknown variable".

## Configuration

All variables are plugin variables of the `duckdb` plugin, so the server adds the `duckdb_` prefix. If a variable ever has to live in the server, it is named with the same prefix.

| Variable | Type | Default | Scope | Dynamic | Notes |
|---|---|---|---|---|---|
| `duckdb_enable` | BOOL | TRUE | global | yes | FALSE: the server behaves as if the plugin were not loaded |
| `duckdb_data_path` | string | empty = `<datadir>/duckdb_data` | global | no | Relative values are resolved against the datadir |
| `duckdb_memory_limit` | size | none, mandatory | global | yes | Plugin init fails if not set; minimum 256 MiB |
| `duckdb_threads` | int | none, mandatory | global | yes | Plugin init fails if not set; range 1 to the number of cores |
| `duckdb_encryption` | BOOL | OFF | global | no | Fixed when the catalog is created; not in the POC |
| `duckdb_executor_threads` | int | 4 | global | yes | Threads that run DuckDB calls; 1 to the number of cores; above the cores at startup it is lowered with a warning |
| `duckdb_executor_stack_size` | size | 8 MiB | global | no | Stack of the executor threads; 2 to 64 MiB |
| `duckdb_history_scope` | enum LIVE, FULL | LIVE | global, session, SET_VAR hint | yes | Affects only tables with retain_history |
| `duckdb_lake_relocate` | BOOL | ON | global | yes | A lake moved to another path: ON accepts it after checking every file, OFF refuses to open it |
| `duckdb_apply_batch_max_trx` | int | 1000 | global | yes | Batch closes at this many source transactions |
| `duckdb_apply_batch_max_rows` | int | 100000 | global | yes | Batch closes at this many row changes; larger transactions are applied in pieces inside one DuckDB transaction |
| `duckdb_apply_batch_max_delay_ms` | int | 1000 | global | yes | Maximum wait before a batch closes; bounds lag under light load |
| `duckdb_apply_gap_action` | enum STOP, FIRST_AVAILABLE | STOP | global | yes | Binary logs purged before the applier read them |
| `duckdb_apply_start_position` | string (command) | empty | global | yes | Restart the applier at a binary log (repair after a gap) |
| `duckdb_apply_debug_log` | BOOL | OFF | global | yes | One error log line per applied change (testing) |
| `duckdb_offload_min_rows` | int | 1000000 | global, session, SET_VAR hint | yes | Also offload when the plan scans a loaded table of at least this many rows |
| `duckdb_offload_wait_applied` | BOOL | ON | session, SET_VAR hint | yes | Read your own writes: wait (at most 5 s) until the session's last commit is applied |
| `duckdb_history_check_only` | BOOL | OFF | session | yes | SECONDARY_LOAD of a history table only checks the structure |
| `duckdb_unload_history_confirm` | BOOL | OFF | session | yes | Required to unload or discard a history copy |
| `duckdb_discard_stale_copy` | string (command) | empty | global | yes | Drop a stale copy (table dropped, detached with SECONDARY_ENGINE = NULL, or a failed refresh); history copies need `duckdb_unload_history_confirm` |

`duckdb_apply_workers` (parallel prepare workers) was not implemented: one reader thread decodes, collapses and commits fast enough so far; it stays an option if the apply lag requires it.

### Mandatory variables

`duckdb_memory_limit` and `duckdb_threads` have no usable default: the internal default 0 means "not set". If either is missing (from my.cnf, the command line or mysqld-auto.cnf), plugin init fails with an error such as `DuckDB: duckdb_memory_limit is not set; add it to the configuration file. Plugin not loaded.` They are mandatory also when `duckdb_enable=FALSE`. With the default plugin load option the server still starts and only the plugin is not loaded; with `--duckdb=FORCE` or `FORCE_PLUS_PERMANENT` the server refuses to start, which is standard MySQL behavior. At runtime, setting either to 0 is rejected.

### Dynamic variables: where the work happens

The update callback of a plugin variable runs under `LOCK_global_system_variables` and cannot fail (sql/sql_plugin_var.cc); the check callback runs before, without the lock, and can return an error. We therefore do the real work in the check callback (open or close DuckDB, `SET memory_limit`, `SET threads`) and only store the value in the update callback. An internal plugin mutex serializes concurrent changes. Known edge case: in a multi-variable `SET` where a later variable fails its check, our change is already applied; our checks are idempotent and we document it.

- `duckdb_memory_limit`: lowering it below current usage makes DuckDB evict buffers; if that is not possible the SET fails with the DuckDB message and the old value stays.
- `duckdb_threads`: applies to the shared DuckDB instance; running queries pick it up at their next task scheduling.
- The purge tag is fixed, `duckdb_avoid_purge` (decided 2026-11-12, DWL#5): a configurable tag lived in two places, the server variable and the purge job's SQL, and any mismatch silently turned purges into deletes.

### duckdb_enable

- FALSE at startup: `Init()` only registers the handlertons; no directory, no DuckDB, no path validation. Files on disk are untouched.
- Set to FALSE at runtime: new statements are not offloaded (they run on InnoDB); offloaded queries already running finish; `use_secondary_engine=FORCED` gives the same error as when no secondary engine is available; `ALTER TABLE ... SECONDARY_ENGINE=duckdb` is accepted as metadata only; `SECONDARY_LOAD` gives the "engine not available" warning; `SECONDARY_UNLOAD` clears the data dictionary flag but lake data is never deleted while disabled. The DuckDB instance is closed to release memory.
- Set to TRUE at runtime: the full startup sequence runs; if any step fails, the SET fails and the value stays FALSE.
- Every hook checks `duckdb_enable` itself and returns the same result as "engine absent", because the secondary engine paths check whether the plugin is loaded, not the handlerton state. An MTR test compares a script run without the plugin and with the plugin loaded and disabled.
- Requires `SYSTEM_VARIABLES_ADMIN`.
- Phase 3 caveat: changes on InnoDB while disabled must be caught up from the stored applied position, or affected tables marked stale (see Open questions).

## Storage: DuckLake

```
DuckDB instance (in memory, one per mysqld)
  └─ ATTACH 'ducklake:<path>/catalog.ducklake' AS lake (DATA_PATH '<path>/data/')
       ├─ lake.<mysql_schema>.<table>       one per loaded table
       ├─ lake.__mysql.tables               registry: state, history flag, load position,
       │                                    primary key, weight columns
       └─ lake.__mysql.applied_position     applied GTID, same transaction as data
  temp_directory = <path>/tmp/              spills from large queries
```

- Catalog backend: a local DuckDB file. mysqld is the only writer, so the single-writer limit does not matter, and it adds no external dependency.
- Each MySQL database maps to a DuckLake schema with the same name, each table to a table with the same name, plus hidden columns: `__archived_at TIMESTAMP` on retain_history tables, `__w_<col>` (BLOB, MySQL's sort weight) for string columns of collations DuckDB cannot compare, `__n_<col>` (ENUM index / SET bitmask).
- The lake can be read by DuckDB itself while mysqld is stopped (`duckq`, HOWTO "Reading the lake while MySQL is down"); a moved lake is accepted with `duckdb_lake_relocate=ON`.
- Default location `<datadir>/duckdb_data`. The name `duckdb_data` is reserved in the datadir when the default is used; a MySQL database with that name would fail with "database directory exists".
- Hardening after attach: `autoload_known_extensions=false`, `autoinstall_known_extensions=false` (runtime settings in DuckDB 2.0), `enable_external_access=false` so DuckDB SQL cannot read arbitrary files or reach the network (implemented: set at open, before the attach the extension settings, after it `enable_external_access`; DuckLake keeps reading and writing DATA_PATH, as every test since DWL#2 shows). `lock_configuration` is not used, because we change `memory_limit` and `threads` at runtime; users never send SQL to DuckDB directly and our translator never emits SET.
- Unload: `DROP TABLE` in DuckLake, then a cleanup call so the Parquet files are removed from disk.
- Updates in DuckLake are written as a delete file plus a new Parquet file (seen in the smoke test), so the applier collapses changes per key before writing.
- Snapshot expiry and file compaction (merge adjacent files, expire snapshots, clean up old files) are needed under continuous propagation; scheduling and variables are Phase 3 work. History is never tied to DuckLake time travel, so snapshot expiry cannot delete history.

### Startup sequence

1. Resolve the path: `duckdb_data_path` or `<datadir>/duckdb_data`; relative values against the datadir; then `realpath`, so checks apply to the real location.
2. Run the location checks (below).
3. Create what is missing: `duckdb_data/`, `data/`, `tmp/` with mode 0750 (DuckLake does not create the parent directory, verified), and `catalog.ducklake`, which the ATTACH creates when the file does not exist.
4. Refuse to start if `data/` contains files but `catalog.ducklake` is missing: the catalog was lost, and creating an empty one would orphan the only copy of archived rows. The DBA must restore the catalog.
5. Register the static extensions, open DuckDB, attach the lake, set `temp_directory`, `memory_limit`, `threads` and the hardening settings. A catalog locked by another process gives a readable error.
6. Reconcile with the data dictionary: tables the DD marks as loaded but missing from the lake are treated as unloaded, with a warning in the error log.
7. Start the applier (Change propagation) and log `DuckDB lake: catalog=..., data=..., created=yes|no`.

### Location and permission checks

Not too rigid: we refuse only clear mistakes and warn otherwise.

| Condition | Action |
|---|---|
| Path is the datadir itself, or inside a schema directory | Refuse to start |
| Directory is world-writable | Refuse to start |
| Path inside the `secure_file_priv` directory | Warning (users with FILE could read or overwrite lake files) |
| `secure_file_priv` is empty | Warning (FILE reaches every path, including the lake) |
| World-readable, group-writable, or owner is not the mysqld user | Warning |

### Encryption (designed now, outside the POC)

- DuckLake encryption of Parquet files keeps per-file keys in the catalog, so the catalog DuckDB file must also be encrypted (DuckDB database encryption, `ENCRYPTION_KEY` on attach).
- The master key comes from the server keyring component through the existing services (`include/mysql/components/services/keyring_generator.h`, `keyring_reader_with_status.h`, `keyring_writer.h`), acquired with `mysql_plugin_registry_acquire()`. It is generated at catalog creation and never appears in a file or a variable.
- `duckdb_encryption` (default OFF) takes effect only when the catalog is created. A mismatch with an existing catalog, or ON without a keyring, makes startup fail; there is no fallback to unencrypted.
- To verify before implementing: DuckLake encryption support in the pinned version (the 1.0 announcement does not mention it), encryption of spill files, exact ATTACH option names, key rotation, and the crypto library (mbedtls bundled in DuckDB; we do not patch DuckDB to use OpenSSL).

## Components

### Plugin skeleton

We start from a copy of storage/secondary_engine_mock into `storage/duckdb/`. We keep from the mock: handlerton registration, `FakePartitionHandler`, `info()` and `records_in_range()` from the primary, `index_flags()`, `store_lock()`, `HA_NO_INDEX_ACCESS`, the cost-threshold pre-prepare hook with its optimizer trace, and the `DBUG_EXECUTE_IF` error injection points. We replace the in-memory `LoadedTables` map with the DuckLake catalog, and add per-THD DuckDB connections stored in the handlerton THD slot, freed in `close_connection`. Hook names and signatures are those of the 9.7 tree.

### Initial load

`load_table()` scans the InnoDB table through the primary handler and writes it into DuckLake through the DuckDB Appender, which works directly on a DuckLake table (verified in the smoke test; no staging table needed). One SECONDARY_LOAD is one DuckLake transaction and one snapshot. The server holds `MDL_SHARED_NO_WRITE` during the load (sql/sql_table.cc), so writes on the table are blocked and the scan is consistent.

- Preconditions, checked at load; the load is refused otherwise: a primary key, supported column types, `log_bin=ON`, `binlog_format=ROW`, `binlog_row_image=FULL`, `gtid_mode=ON`, `binlog_row_metadata=FULL`, `binlog_order_commits=ON`, `sync_binlog=1` for history tables. Foreign keys (2026-11-17): allowed, cascading ones too, with `innodb_native_foreign_keys=OFF` (the 9.7 default: MySQL runs cascades in the SQL layer and writes the cascaded rows to the binary log, verified); with ON a child of a cascading key is refused, because InnoDB's own cascades are not logged.
- Types: signed and unsigned integers, DECIMAL up to 38 digits, FLOAT and DOUBLE, CHAR, VARCHAR, TEXT, BINARY, VARBINARY, BLOB in any character set (text stored as UTF-8), DATE, DATETIME, TIMESTAMP (UTC), YEAR, BIT, TIME (signed microseconds), ENUM and SET (value plus `__n_<col>`). JSON, spatial and VECTOR fail the load with an error naming the column.
- The load runs in two DuckLake transactions: the data under a build name, then the rename and the registry row under the apply mutex (a long transaction writing the registry would conflict with the applier). DuckDB work runs on the executor pool.
- `NOT SECONDARY` columns are honored through the table read set.
- The load records the binlog position and GTID set it represents; the applier skips events for that table up to that point.
- For a retain_history table that already has a copy, SECONDARY_LOAD never discards it: it reattaches a stale copy, resyncs a current one (live rows replaced from InnoDB, archived rows kept) or adopts another table's stale copy (`adopt_history_from`), after a strict structure check (DWL#3 step 3b).

### Change propagation: binlog reader in the plugin

Because core code must not change, the plugin reads the binary log. It uses only public interfaces:

- Notification: a `Trans_observer` registered with `register_trans_observer()` (sql/replication.h), the API semisync uses. `after_commit` gives the binlog file and position of each transaction after its InnoDB commit, in binary log order with `binlog_order_commits=ON`, so we never poll and DuckDB never shows a transaction before InnoDB does (`after_sync`, used before, runs before the InnoDB commit; with semi-sync AFTER_SYNC that gap lasts until the replica acknowledges).
- Parsing: the in-tree event library (`libs/mysql/binlog/event`): `Gtid` events (transaction id and tag), `Table_map`, `Write_rows`, `Update_rows`, `Delete_rows`. Compressed transaction payloads are decoded by the same library. Events for tables that are not loaded are ignored.

```
binlog files ──► [reader] ──► [prepare worker 1..N] ──► [committer] ──► DuckLake
                 decode,       convert to DuckDB         one DuckLake
                 filter,       vectors, collapse         transaction per batch
                 detect tag    per primary key           + applied GTID
```

- As implemented: one reader thread reads and decodes the binlog, filters loaded tables, detects the purge tag, converts values, collapses changes per primary key and commits once per batch, in GTID order. Cross-table source transactions stay atomic in DuckDB, there is a single applied position, and DuckDB parallelizes the commit work with its own `duckdb_threads`. The prepare workers below were designed but not needed so far:
- Prepare workers (`duckdb_apply_workers`, deferred): would convert values and collapse changes per primary key inside the batch, split by table or by hash of the primary key.
- Collapse rules within a batch: insert then updates becomes one insert of the final row; insert then a real delete becomes nothing; insert then a purge-tagged delete becomes an insert with `__archived_at` set; several updates become one update with the final image.
- Batch boundaries are always at transaction boundaries; a batch closes at the first of `duckdb_apply_batch_max_trx`, `duckdb_apply_batch_max_rows`, `duckdb_apply_batch_max_delay_ms`.
- Crash safety: each batch stores its last GTID in `lake.__mysql.applied_position` in the same DuckLake transaction as the data; on restart the reader resumes from there.
- DDL on a loaded table never blocks replication (DWL#4 step 4d, `HTON_SECONDARY_ENGINE_SUPPORTS_DDL`): after a column change, TRUNCATE or partition DDL the applier refreshes the copy from InnoDB (an internal `SECONDARY_LOAD`, archived rows kept and converted); it follows RENAME TABLE; a refresh waiting for a later rename is deferred; a copy that cannot follow is marked stale. The internal session passes `super_read_only` through the guard (replicas).
- Replicas (tested 2026-11-15): the reader reads the replica's own binary log (`log_replica_updates=ON`); the source does not need the plugin. Loads and the history privilege on a replica need `sql_log_bin=0` (HOWTO, Replicas).
- Transactions larger than `duckdb_apply_batch_max_rows` are applied in pieces inside one DuckDB transaction. Binary logs purged before the applier read them: `duckdb_apply_gap_action` (STOP or FIRST_AVAILABLE, the tables that miss changes are named).

Why not the replica model (`replica_parallel_workers`): its gain comes from many small commits in parallel, which suits InnoDB, where a commit is cheap and row operations dominate. In DuckLake each commit writes new files and a catalog snapshot, commits serialize on the catalog, and concurrency control is optimistic, so parallel small commits would multiply the most expensive step and create many small files. We keep its principle (independent work in parallel, order where it matters) in the prepare workers. The dependency information (`last_committed`, `sequence_number`) in the Gtid events stays available if a future per-table commit mode is ever needed.

Lag is visible through status variables; queries see data up to the last applied transaction (Freshness and consistency).

### Delete classification

For tables with history retention, the applier distinguishes two kinds of delete:

- Purge delete: a delete in a transaction whose GTID carries the fixed tag `duckdb_avoid_purge`. The tag belongs to that transaction only: `gtid_next` is a session variable. The row is kept in DuckDB and `__archived_at` is set to the commit time. The tag is written in the same transaction, it replicates to replicas, and setting it requires the server's existing `TRANSACTION_GTID_TAG` privilege, because a mistaken purge keeps data that was meant to disappear. `ALTER TABLE ... DROP PARTITION` / `TRUNCATE PARTITION` are not archive operations: the copy is refreshed and the partition's rows are gone; to archive a partition, purge it first (`DELETE ... PARTITION (p)` in a tagged transaction), then drop it (TUTORIAL section 8). A tagged DROP PARTITION as archiving is designed (record partition bounds at load, archive the partition's rows at the DDL position, then refresh; about 3-4 days) and planned (Phased plan, phase 4).
- Business delete: any other delete, including cancellations and legal erasure requests. The live row is removed from DuckDB (`WHERE pk = ? AND __archived_at IS NULL`).

A primary key purged and inserted again gives one archived row and one live row with the same key; the key is unique only among live rows.

### Query offload and plan translation

The server decides whether to try offload: `use_secondary_engine` is ON or FORCED, the statement is not part of a multi-statement transaction (MySQL never uses a secondary engine there: `BEGIN`, `autocommit=0`), every table in the query has `SECONDARY_ENGINE=duckdb` and is loaded, and our pre-prepare hook finds the estimated cost above `secondary_engine_cost_threshold` or a scan of a loaded table of at least `duckdb_offload_min_rows` rows (InnoDB's estimates can be far too low). Then:

1. Prepare hook: creates our per-statement context.
2. Optimize hook: we walk the query block (select list, FROM and joins, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT, subqueries, set operations). Every item must be on the allow list (the SQL coverage tracker lists what is covered). If all is supported, we generate DuckDB SQL text, prepare it in DuckDB (a bind error is a refusal, before anything is sent), and set `join->override_executor_func`, with the `USE_EXTERNAL_EXECUTOR` flag so the server builds no iterators. Otherwise we reject with a reason (optimizer trace, status counter, the error with FORCED), and in LIVE scope the query runs on InnoDB.
3. Executor function: runs the SQL on the session's DuckDB connection, reads results chunk by chunk, converts each value to the MySQL type of its select-list item, and sends rows to the client.

- Joins are offloaded when all tables in the query are DuckDB tables and loaded; generating SQL from the query block lets DuckDB apply its own optimizer, including join order.
- LIVE scope adds `__archived_at IS NULL` for each history table; FULL omits it.
- Limits: only top-level SELECT results sent to the client are offloaded; INSERT ... SELECT, CREATE ... SELECT, SELECT ... INTO and subqueries inside DML run on InnoDB. No offload inside a transaction that has already written (and MySQL itself does not offload in multi-statement transactions).
- EXPLAIN shows the generated DuckDB SQL and DuckDB's plan as notes; DuckDB errors with a MySQL equivalent return MySQL's code with both messages (1242, 1317 for KILL, 3024 for max_execution_time).
- The framework provides the hooks; the translation is entirely our work.

### FULL scope guard (duckdb_guard)

Under FULL scope a query that ends up on InnoDB would return only live rows, silently. When `use_secondary_engine=OFF`, or when the server decides not to try offload, our engine hooks are never called, so the engine alone cannot prevent it. The guard closes the gap without core changes:

- The server raises table access READ events in `open_tables()` (sql/sql_base.cc), before `open_secondary_engine_tables()` decides between primary and secondary, and before any row is read.
- The server already supports statements that can execute only in the secondary engine (`lex->set_execute_only_in_secondary_engine()`, used for CUBE, TABLESAMPLE, GROUPING SETS). With `use_secondary_engine=OFF` such a statement fails; otherwise it is treated as FORCED, so failing to offload is an error and never a fallback.

On each table access event (READ, and since 2026-11-16 UPDATE and DELETE, so UPDATE / DELETE / TRUNCATE of a history table under FULL are stopped: they read only live rows) the guard:

1. Ignores tables that are not history tables (registry of loaded retain_history tables kept by the plugin).
2. Ignores the statement if the effective `duckdb_history_scope` (including SET_VAR) is not FULL.
3. With `use_secondary_engine=OFF`, raises our own error: `duckdb_history_scope=FULL requires DuckDB execution, but use_secondary_engine=OFF`.
4. With the table not loaded or `duckdb_enable=FALSE`, raises our own error saying so.
5. Otherwise marks the statement secondary-only. If our optimizer hook then rejects it, the error text comes from our `find_secondary_engine_offload_fail_reason` hook.

- Statements that write and also read a history table under FULL are rejected in the POC (Open questions).
- The reason value passed to `set_execute_only_in_secondary_engine()` comes from a fixed server list with no DuckDB entry; its text is shown only if the server raises its own error, which steps 3 and 4 precede. We still verify it has no side effect beyond the message.
- Replica applier threads do not raise these events; FULL scope concerns user reads only.
- The engine refuses `duckdb_history_scope=FULL` unless the guard plugin is active; both come from the same plugin-load-add.

### Semantic equivalence

The same query must return the same result on both engines. Areas to cover:

- Type mapping: DECIMAL precision and scale, unsigned integers, DATETIME and TIMESTAMP with time zones, BIT, ENUM, SET, JSON.
- Collations. GROUP BY, DISTINCT, ORDER BY and comparisons on strings must follow the column collation. Under utf8mb4_0900_ai_ci, "José" and "jose" are equal; under the DuckDB default comparison they are not. This is the largest source of silent wrong results. Implemented (DWL#6 step A, 2026-11-14): for each string column whose collation DuckDB cannot compare, a hidden weight column computed with the server's own `strnxfrm` at load and apply time; comparisons, joins, GROUP BY, ORDER BY, MIN/MAX and COUNT(DISTINCT) use it, which gives exactly MySQL's semantics; PAD SPACE binary collations use rtrim keys. Still refused under non-binary collations: LIKE, SELECT DISTINCT, IN subqueries, DISTINCT set operations. A grouped string value shown in a join needs the table's primary key in GROUP BY (otherwise the value InnoDB shows depends on its join order).
- Numbers: exact DECIMAL division and AVG as MySQL computes them (9-digit words, truncation, `div_precision_increment`); unsigned arithmetic only when it cannot leave 0..2^64-1 (MySQL raises error 1690). Columns with a special lake value (TIME, ENUM, SET, YEAR, BIT) are translated only by the functions that know them.
- NULL handling and MySQL implicit type conversions.
- Function mapping, one function at a time, with an allow list. Anything not on the list causes rejection.

### Freshness and consistency

Propagation is asynchronous, so a query on DuckDB sees data up to the last applied transaction. In the POC we expose the lag through status variables. Later we need a policy, configurable per session: accept a bounded lag, wait until DuckDB has applied the GTID the session needs (similar to WAIT_FOR_EXECUTED_GTID_SET), or, in LIVE scope only, fall back to InnoDB when the lag is above a threshold. Implemented in the POC (DWL#6): read your own writes, `duckdb_offload_wait_applied` (session, default ON) waits until the applier has applied the connection's last commit, at most 5 s, then the query runs on InnoDB (with FULL scope: `applier_behind` error). Snapshot isolation across statements is not an issue: MySQL never offloads inside a multi-statement transaction.

## History retention and query scope

### The duckdb_history_scope parameter

The variable exists at global and session level and can be set per statement with the SET_VAR optimizer hint (plugin variables support `PLUGIN_VAR_HINTUPDATEABLE`, so no server change is needed). It affects only tables with retain_history enabled; all other tables behave as a classic secondary engine.

|  | LIVE | FULL |
|---|---|---|
| orders means | Live rows only | Live and archived rows |
| Where it can run | InnoDB or DuckDB, same result | DuckDB only |
| If offload is not possible | Silent fallback to InnoDB | Error, never fallback (duckdb_guard) |
| Separate history name | orders_history gives the full history | Not needed |

With this, both models coexist: LIVE gives the separate-name model and FULL gives the same-name model. Exposing orders_history is not possible from a plugin alone, because MySQL resolves table names against the data dictionary (see Open questions).

### Rules for FULL scope

- No silent fallback. If a query touching a history table cannot be offloaded, it fails with a clear error. Returning 90 days when the user asked for five years is the worst possible outcome.
- Mixed read and write statements. Writes always go to InnoDB. INSERT ... SELECT, UPDATE or DELETE with a subquery, and CREATE TABLE ... AS SELECT that read a history table are rejected in the POC (see Open questions). An UPDATE that targets an archived row affects 0 rows.
- Read-your-writes. DuckDB sees only committed and applied changes, so a session that writes to a history table and then reads it under FULL in the same transaction does not see its own changes. Such reads are refused.
- Resolution time. The scope is resolved at execution, not when a statement is prepared or a view or routine is created.
- Privilege. Switching to FULL requires a dynamic privilege, for example DUCKDB_HISTORY_READER, because archived data was removed from InnoDB on purpose.
- Same global value everywhere. Different global values on different servers make the same query return different answers depending on routing, so the global setting belongs in the configuration.
- Observability. EXPLAIN shows the scope used, and status counters report FULL scope rejections.

### Durability of the history

With history retention, DuckDB is no longer a disposable copy: it holds the only copy of archived rows, so it must be treated as primary storage for that data.

- Backup: Percona XtraBackup must capture `catalog.ducklake`, the Parquet files it references and the applied position consistently with InnoDB. Parquet files are immutable, which simplifies copying.
- Replicas: a new replica built from a backup needs the DuckDB history too; it cannot be rebuilt from binlogs that have been purged. The avoid-purge tag replicates, so a replica's applier archives the same rows.
- DDL (since DWL#4 step 4d, `HTON_SECONDARY_ENGINE_SUPPORTS_DDL`): ALTER, TRUNCATE, partition DDL and RENAME TABLE are allowed; the applier refreshes the copy keeping the archived rows (converted to new column types; ENUM / SET indexes recomputed with the new definition) and follows renames. DROP TABLE and `SECONDARY_ENGINE = NULL` keep the history in the lake and mark it stale. SECONDARY_LOAD reattaches a stale copy (strict structure check, every difference reported); `adopt_history_from` gives it to a table with another name (DWL#3).
- Unload: discarding history (SECONDARY_UNLOAD of a history table, or removing a stale copy) requires `SET SESSION duckdb_unload_history_confirm = ON`, because it destroys data that exists nowhere else. Lake data is never deleted implicitly.
- Binlog retention: if binlog files are purged before the applier has read them, the copy has a gap. The plugin cannot block binlog purge without core changes, so it detects the gap, names the affected tables and stops (`duckdb_apply_gap_action=STOP`) or continues at the first available binary log; the tables are reloaded and `duckdb_apply_start_position` restarts the applier.

## Operations

- Resource limits: `duckdb_memory_limit` and `duckdb_threads` are mandatory and dynamic.
- DDL on a base table without history invalidates the secondary copy and requires a reload; with history, see Durability of the history.
- Observability (implemented): 31 status variables (applier state and position, applied position and GTID, batches, changes, pieces, purges and archived rows, DDL refreshes / renames / failures, offloaded and refused queries with the last reason, guard counters, executor pool) and `INFORMATION_SCHEMA.DUCKDB_TABLES`; the optimizer trace and EXPLAIN notes for offload. Since 2026-11-20 also the apply lag in seconds and in bytes across files, a numeric applier state, DuckDB memory and spill, the total offloaded query time, and a PMM / Grafana dashboard with custom queries (`monitoring/`). Still to do: time split between decode and commit, lake size on disk (Phased plan, phase 3).
- Testing (implemented): MTR suite `duckdb` (26 tests: plugin, applier, history, guard, super_read_only, foreign keys, transactions, DECIMAL overflow, LIKE, grouped CHAR, functions, INFORMATION_SCHEMA, cached tables, offload rules, the offload query and refusal lists, types); every query runs with use_secondary_engine OFF and FORCED and results are compared; unit tests (41); manual tests for restarts, replicas, semi-sync, relocation and performance (`manual-test-plan.md`).

## Build and source integration

The whole feature builds in one CMake run together with the server, with the same compiler and C++ standard as the server (C++23, top-level CMakeLists.txt). DuckDB accepts 17, 20, 23 and 26. The setup was verified with C++20 on 2026-11-05 with GCC 15.3 on Fedora 43: all 480 C++ files of DuckDB, its extensions and DuckLake compiled with `-std=c++20` and no errors, and a statically linked C++20 smoke test passed (Appendix B). On 2026-11-07 the first in-tree build compiled all 480 C++ files with clang++-21 and `-std=c++23`, and the build smoke test passed.

### Upstream sources (git submodules)

| Path | Repository | Pin |
|---|---|---|
| `extra/duckdb/duckdb` | https://github.com/duckdb/duckdb.git | tag v2.0.0 (d54cbbe3df17), released 2026-10-21 |
| `extra/duckdb/ducklake` | https://github.com/duckdb/ducklake.git | commit 55cc50cf080f; its `.github/duckdb-version` names d54cbbe (v2.0.0). The repo has no release tags |
| `extra/duckdb/croaring` | https://github.com/RoaringBitmap/CRoaring.git | tag v5.5.5. DuckLake needs it for deletion vectors; upstream gets it through vcpkg |

- Code comes from the original upstream repositories. We do not patch DuckDB or DuckLake unless a bug is a hard stopper; then the preferred path is an upstream fix and a pin move, with a patch file applied at build time only as a last resort.
- From DuckDB we build `src/`, `third_party/` and the extensions `core_functions`, `parquet`, `json`, `icu`; we do not build `tools/` (shell), tests or benchmarks. From DuckLake we use `src/` and its CMakeLists; its own `duckdb` and `extension-ci-tools` submodules are not used, so the repository is cloned without `--recursive`.
- The DuckDB submodule is marked `shallow = true`; because of the shallow clone we pass `OVERRIDE_GIT_DESCRIBE=v2.0.0` to get the right version string.
- Release tarballs must include the three submodules, as for rocksdb, and the SBOM lists them with their pins and licenses.

### Files we write (not from upstream)

- `extra/duckdb/extension_config.cmake`: loads the five extensions, DuckLake from our submodule, links them statically, forces the server's C++ standard on every target (DuckLake sets `CMAKE_CXX_STANDARD 17` and DuckDB's bundled re2 adds `-std=c++17`; at the end of configuration we set the standard on all targets and drop those flags, without touching upstream files), and keeps DuckDB's `clangd_cache` helper out of the build.
- `cmake/duckdb.cmake` and `extra/duckdb/CMakeLists.txt`: two ExternalProjects, `croaring_ext` then `duckdb_ext`, and imported static targets.
- `storage/duckdb/`: the plugin.

### ExternalProject wiring

- At configure time nothing is built; at build time the plugin target depends on `duckdb_ext`, which depends on `croaring_ext`, so everything builds in the same `make` or `ninja` run. DuckDB is not rebuilt on every build: it is reconfigured and rebuilt incrementally when a submodule pin moves, when our extension config changes, or when the forwarded CMake arguments change. Hand edits inside the DuckDB sources are not tracked (we do not patch DuckDB); deleting the `duckdb_ext-build` stamp forces a rebuild.
- Forwarded from the MySQL build: C and C++ compilers, `CMAKE_CXX_STANDARD` of the server (23), build type (cache variable `DUCKDB_BUILD_TYPE`, default the MySQL build type, overridable because DuckDB debug assertions are slow), sanitizer flags (DuckDB's `ENABLE_SANITIZER` and `ENABLE_UBSAN` follow `WITH_ASAN` and `WITH_UBSAN`; they default to ON upstream), PIC, the compiler launcher (ccache).
- DuckDB options: `BUILD_SHELL=OFF`, `BUILD_UNITTESTS=OFF`, `ENABLE_UNITTEST_CPP_TESTS=OFF`, `BUILD_BENCHMARKS=OFF`, `ENABLE_JEMALLOC=OFF` (no second allocator in mysqld), `ENABLE_EXTENSION_LOAD=OFF`, `ENABLE_BUILTIN_HTTPLIB=OFF`, `DUCKDB_EXTENSION_CONFIGS=<our config>` (the CMake name in DuckDB 2.0; `EXTENSION_CONFIGS` is the Makefile name and is ignored by CMake), `DUCKLAKE_SOURCE_DIR`, `CMAKE_PREFIX_PATH=<croaring install>`.
- DuckDB warnings stay DuckDB's own: the server's `-Werror` maintainer flags do not apply to DuckDB sources. C++20 and later give deprecation warnings in DuckDB headers (for example `std::atomic_load` on `shared_ptr`), warnings only.
- The installed DuckDB CMake package exists only after the build, so we declare imported targets ourselves. Link set verified in the smoke test: `libducklake_extension.a`, `libparquet_extension.a`, `libjson_extension.a`, `libicu_extension.a`, `libcore_functions_extension.a`, `libduckdb_static.a` (contains fmt, re2, zstd, mbedtls, utf8proc and the other bundled libraries), `libroaring.a`, `-ldl`, `-lpthread`. Headers: `duckdb.h`, `duckdb_static_extension.h`. CRoaring installs into `lib64/` on Fedora.
- The plugin is `MYSQL_ADD_PLUGIN(duckdb ... STORAGE_ENGINE MODULE_ONLY LINK_LIBRARIES ...)`, built with `-fvisibility=hidden` and a version script that exports only the plugin entry points, so DuckDB's bundled libraries do not clash with the server's.
- The plugin uses the DuckDB C API (`duckdb.h`) only: a small stable header, and DuckDB exceptions never cross into server code.
- In `Init()`, before opening the database, the plugin registers each extension with `duckdb_register_static_extension(duckdb_extension_<name>_describe)`.

### Debugging

- DuckDB is linked into `ha_duckdb.so`, not into mysqld; DWARF info points to `extra/duckdb/duckdb/src/...`, so one gdb or IDE session covers server, plugin and DuckDB (`set breakpoint pending on`, because the plugin is loaded after mysqld starts). `./mtr --gdb` and `--manual-gdb` work as for any plugin. Hidden visibility does not remove debug symbols.
- DuckDB writes its own `compile_commands.json` in its build directory; a post-build step merges it into the top-level one for clangd and IDE navigation.
- With sanitizers enabled, DuckDB is built with the same flags, so errors inside DuckDB give full stack traces back into the server.

### Build facts

- Full DuckDB build from scratch: about 65 minutes with 6 jobs on the test machine; then incremental. The DuckDB build directory takes about 6 GB.
- The RelWithDebInfo smoke binary is 1.5 GB, almost all debug info; `ha_duckdb.so` will be similar, so packaging needs stripping or separate debuginfo.
- Packaging is decided later; everything must ship in one tarball.

## POC scope

Features:

1. Create a table with a secondary engine: `SECONDARY_ENGINE=duckdb`, `retain_history` attribute, preconditions checked at load.
2. Load from the primary: SECONDARY_LOAD through the Appender into DuckLake, binlog position recorded.
3. DML on InnoDB applied to DuckDB: binlog observer, reader, prepare workers, committer, crash-safe applied position.
4. Purge on InnoDB without affecting DuckDB: tagged GTID `duckdb_avoid_purge` becomes archive marks.
5. Reporting reads from DuckDB: offload through the external executor, SQL generated from the query block, single-table and join queries, LIVE and FULL scope, FULL guarded by `duckdb_guard`.

Also in the POC: build integration (Build and source integration), all variables except encryption, startup sequence and location checks, `duckdb_enable`, status variables for lag and offload.

Out of the POC: encryption, `binlog_encryption`, DROP PARTITION as purge, the separate `orders_history` name, XtraBackup integration, compaction scheduling, write-and-read statements under FULL (rejected). Added to the POC during development: the freshness wait (`duckdb_offload_wait_applied`), DDL on loaded and history tables (refresh with archived rows kept), weight columns for every collation, the types YEAR / BIT / TIME / ENUM / SET, exact decimal division, `duckdb_offload_min_rows`, lake relocation, `INFORMATION_SCHEMA.DUCKDB_TABLES`, foreign keys.

**Status (2026-11-17): the five features work end to end** in MTR and on Marco's reference instance; the exit criterion is met: on the release build with 10.5 million rows, offloaded queries ran 54x to 584x faster than InnoDB (`perf/results-2026-11-1*-release*.txt`), with identical results.

No core code changes: everything lives in `storage/duckdb/`, `extra/duckdb/` and `cmake/duckdb.cmake`.

POC tests (MTR suite `duckdb`):

- Startup: default and custom `duckdb_data_path`, creation of directory and catalog, refusal when data files exist without catalog, permission refusals and warnings, missing mandatory variables, `loose-` options without the plugin.
- `duckdb_enable`: same output as without the plugin; runtime OFF and ON.
- Load: preconditions (PK, FK, types, binlog settings), retain_history load once.
- Propagation: INSERT, UPDATE, DELETE, REPLACE, INSERT ... ON DUPLICATE KEY UPDATE, LOAD DATA; collapse cases; restart and crash recovery from the applied position.
- Purge: tagged and untagged deletes; purge then reinsert of the same key; tag change at runtime.
- Offload: OFF versus FORCED result comparison in LIVE; joins; rejections and fallback; FULL errors from the guard.

Exit criterion: the five features work end to end in MTR, and a reference analytical workload shows a measured speedup versus InnoDB.

## SQL coverage tracker

What an offloaded query can contain, against the SQL standard (SQL:2016
core and the common optional features) and the MySQL dialect that
applications use. Only reads are offloaded: writes, DDL and transactions
stay on InnoDB by design. Anything not covered is not an error: with
`use_secondary_engine = ON` the query runs on InnoDB (with FORCED it fails
with the reason). The rule for every item is the same: offload only when
the result is exactly InnoDB's.

**Keep this table current**: every step that adds or removes coverage
updates it, with the step and its test list. Status: **done**, **partial**
(what is missing in the notes), **missing**, **n/a** (not part of MySQL, or
not a read). Last update: 2026-11-17 (function review, TPC-H 19/22,
`tpch/README.md`).

### Query structure

| Feature | Status | Notes / tests |
|---|---|---|
| SELECT, FROM, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT / OFFSET | done | 6a, 6b |
| DISTINCT, COUNT(DISTINCT) | partial | not SELECT DISTINCT on non-binary or PAD binary strings |
| Inner, CROSS, LEFT, RIGHT joins | done | 6c-1 |
| FULL OUTER JOIN | n/a | not in MySQL |
| NATURAL / USING joins | done | resolved by MySQL into equalities |
| LATERAL derived tables | missing | |
| Derived tables, views, non-recursive CTEs | done | 6c-1 |
| Recursive CTEs | missing | |
| UNION / INTERSECT / EXCEPT, DISTINCT and ALL, nested | partial | DISTINCT forms need binary or `_0900_bin` strings; a YEAR column refused |
| Subqueries: scalar, IN, NOT IN, EXISTS, NOT EXISTS, ANY / ALL, correlated | partial | 6c-3; IN on `_bin` / `_ci` strings; a correlated scalar subquery without aggregate; equalities propagated between EXISTS and NOT EXISTS nests (TPC-H Q21) |
| Row value constructors `(a, b) IN (...)`, `(a, b) = (...)` | missing | |
| GROUP BY ROLLUP, CUBE, GROUPING SETS, GROUPING() | missing | refused `rollup` |
| Window functions (ROW_NUMBER, RANK, DENSE_RANK, NTILE, LAG, LEAD, FIRST_VALUE, aggregates OVER, frames) | missing | refused `window` |
| TABLESAMPLE | missing | |
| VALUES / TABLE statements, JSON_TABLE | missing | |
| A grouped string shown in a join | done | the table's primary key in GROUP BY, CHAR under a binary collation (2026-11-17); otherwise each result group is checked in the query to hold one stored value, the statement runs first and an ambiguous group is a refusal (2026-11-22, MTR `offload_join_distinct`) |

### Predicates

| Feature | Status | Notes / tests |
|---|---|---|
| =, <>, <, <=, >, >= ; BETWEEN ; IN (list) ; IS [NOT] NULL ; AND, OR, NOT | done | every collation through weight columns (step A) |
| IS [NOT] TRUE / FALSE / UNKNOWN | done | |
| NULL-safe equality `<=>` (IS NOT DISTINCT FROM) | missing | |
| LIKE, ESCAPE | partial | every binary collation, PAD or not (2026-11-17), MySQL's default escape `\`; `_ci` and other non-binary missing |
| REGEXP / RLIKE (SIMILAR TO is not in MySQL) | missing | |
| String compared with a number | missing | MySQL converts to DOUBLE |

### Expressions and functions

| Feature | Status | Notes / tests |
|---|---|---|
| + - * on signed numbers, exact DECIMAL division, DIV, MOD, unary minus | done | 6b, B (exact division) |
| Unsigned arithmetic | partial | + and * that cannot overflow; subtraction refused (MySQL error 1690) |
| DECIMAL results declared above 38 digits | done | 2026-11-17: computed as DECIMAL(38, scale), run first; `duckdb_decimal_overflow` FALLBACK / ROUND / ERROR (TPC-H Q11) |
| CASE, IF, IFNULL, COALESCE, NULLIF | done | |
| CAST / CONVERT | missing | not reviewed yet |
| Numeric functions | partial | ABS, ROUND (not of DOUBLE), FLOOR, CEIL, SIGN, TRUNCATE (exact), SQRT, LN, LOG, LOG2, LOG10, GREATEST / LEAST of numbers (2026-11-17 review, DWL-06); refused: EXP, POW (overflow errors), RAND |
| String functions | partial | CHAR_LENGTH, LENGTH (UTF-8 charsets), SUBSTRING, LEFT, RIGHT, CONCAT, CONCAT_WS, TRIM (spaces / one character), REPLACE, REVERSE, LPAD, RPAD, INSTR, LOCATE (binary collations) (2026-11-17); refused: UPPER, LOWER (case mapping differs), ASCII; not reviewed: REGEXP, SOUNDEX, FORMAT, ... |
| Date parts: YEAR, MONTH, DAYOFMONTH, DAYOFYEAR, QUARTER, DAYOFWEEK, WEEKDAY, HOUR, MINUTE, SECOND, DATE of DATE / DATETIME | done | 6b |
| EXTRACT(unit FROM ...) | done | 2026-11-17, every unit but WEEK (default_week_format) |
| Date arithmetic on columns | partial | `d +/- INTERVAL n unit`, DATE_ADD / SUB, ADDDATE / SUBDATE, DATEDIFF, TO_DAYS, LAST_DAY, DAYNAME / MONTHNAME (en_US) (2026-11-17); missing: TIMESTAMPDIFF, multi-part units |
| DATE_FORMAT, STR_TO_DATE, CURRENT_DATE / NOW() in expressions | missing | |
| Functions on TIMESTAMP columns (session time zone) | missing | |
| Bit operators (& | ^ ~ << >>) | missing | |
| JSON functions | missing | |

### Aggregates

| Feature | Status | Notes / tests |
|---|---|---|
| COUNT, SUM, AVG, MIN, MAX, with DISTINCT | done | exact SUM and AVG (6a, B); SUM / AVG of FLOAT and DOUBLE: the last digits can differ (DuckDB's parallel order); allowed by default, `duckdb_offload_float_aggregates = REFUSE` sends them to InnoDB (2026-11-18) |
| STDDEV, VARIANCE (POP / SAMP) | missing | |
| BIT_AND, BIT_OR, BIT_XOR | missing | |
| GROUP_CONCAT (LISTAGG), JSON_ARRAYAGG / JSON_OBJECTAGG | missing | |
| FILTER (WHERE ...) on aggregates, ordered-set aggregates | n/a | not in MySQL |

### Data types

| Feature | Status | Notes / tests |
|---|---|---|
| Integers (signed / unsigned), DECIMAL up to 38 digits, FLOAT, DOUBLE | done | |
| CHAR, VARCHAR, TEXT, BINARY, VARBINARY, BLOB, every character set and collation | done | weights for non-binary collations |
| DATE, DATETIME, TIMESTAMP, YEAR, TIME | done | YEAR, TIME 2026-11-16 |
| BIT, ENUM, SET | done | 2026-11-16 |
| DECIMAL above 38 digits | missing | table not loadable |
| JSON, spatial, VECTOR | missing | table not loadable |

### Tables and consistency

| Feature | Status | Notes / tests |
|---|---|---|
| Tables with a primary key | done | |
| Tables without a primary key | missing | |
| Tables with foreign keys | done | 2026-11-17, also cascading keys (binlogged with `innodb_native_foreign_keys = OFF`, the default); with ON a cascading child is refused |
| Partitioned tables | done | DROP PARTITION as archiving: missing (purge first) |
| Read your own writes | done | `duckdb_offload_wait_applied` |
| Queries in multi-statement transactions (BEGIN, autocommit = 0) | n/a | MySQL never uses a secondary engine there (InnoDB answers) |
| Isolation | done | each offloaded statement reads the last applied state, which includes every commit of the session |

## Phased plan

Status on 2026-11-17. Done items keep their phase for the record; the
open items are in the proposed working order (each step: design note for
Marco's review, implementation, MTR test, throwaway instance, then Marco's
reference instance; the SQL coverage tracker is updated).

| Phase | Scope | Status | Exit criterion |
|---|---|---|---|
| 1. POC | See POC scope | **done** (2026-11-15) | Features work end to end in MTR; measured speedup versus InnoDB: met (54x-584x) |
| 2. Correctness | Type mapping, collation weight columns, function allow list, rejection and fallback, MTR result comparison, Clang build | **mostly done**: weights, every type but JSON / spatial / VECTOR, exact decimal division, rejection rules, MTR suite. Open: the translation items of phase 2b | Identical results with use_secondary_engine OFF and FORCED across the test suite: met for everything that is offloaded |
| 2b. Translation coverage and the TPC-H proof | See "Next steps" 1-3 | **in progress** (TPC-H check: 19 of 22) | HammerDB TPROC-H runs with all 22 queries giving InnoDB's results, offloaded where translated; measured speedup at scale factor 1 and 10 |
| 3. Propagation hardening | Crash tests, binlog gap detection (done), catch-up after duckdb_enable=FALSE (done: the applier resumes from the applied position), compaction and snapshot expiry, incremental refresh after DDL, batch tuning, freshness policy (read your own writes done), apply lag metrics | partly done | Copy stays consistent through crash tests; measured apply lag under write load; disk space stable under continuous changes |
| 4. History retention complete | DROP PARTITION as archiving (tagged), DUCKDB_HISTORY_READER privilege (done), DDL on history tables (done), confirmed SECONDARY_UNLOAD (done), backup integration, encryption | partly done | Purged rows remain queryable in FULL scope; LIVE results identical to InnoDB; backup and restore preserve history |
| 5. Production readiness | Target release (9.7 or 8.4), consistency check, resource governance, backup, observability, packaging, CI, upgrade policy, wider translation by real usage (see "Production readiness" and "SQL gaps by priority" below) | not started | Agreed workload runs on a staging environment within lag and memory targets; crash and consistency tests pass; lake size stable under continuous changes |

### Next steps (proposed order, 2026-11-17, for Marco's review)

1. **TPC-H enablers, small** (`tpch/README.md`): done 2026-11-17 (LIKE
   on binary collations, grouped CHAR values in joins, EXTRACT and the
   function review: DWL-06 "Function review against DuckDB"). TPC-H 19 of
   22.
2. **Grouped string values in joins, exact by a data check**: done
   2026-11-22, as a check inside the query instead of the table check
   below: each group is verified to hold one stored value
   (`CASE WHEN count(DISTINCT col) > 1 THEN error(marker) ELSE
   any_value(col) END`), the statement runs first during optimization (as
   the DECIMAL path) and the marker is a refusal. Same snapshot as the
   result, no cache, and only the result's groups count. TPC-H Q16 / Q22
   to check again. Original plan (decided
   2026-11-17): before offloading a query that shows a grouped string of a
   PAD SPACE or non-binary collation in a join without the table's primary
   key, ask DuckDB whether the column has two different values that compare
   equal (`GROUP BY <weight or rtrim key> HAVING count(DISTINCT col) > 1
   LIMIT 1`); if none, every group is unambiguous and the query is exact;
   cache the answer per column until the applier next changes the table.
   VARCHAR and `_ci` columns with consistent data (TPC-H Q16, names stored
   in one case), and string expressions grouped under PAD SPACE (TPC-H
   Q22, `GROUP BY substr(...)`). About 1-2 days. An opt-in "any value" mode
   (`duckdb_grouped_value = EXACT | ANY`) stays a possible later option.
3. **TPC-H Q21**: equalities propagated between EXISTS and NOT EXISTS
   nests. SUBSTR is done (review). Expected with 1-3: 22 of 22.
4. **HammerDB TPROC-H on the release build**, scale factor 1 then 10: build
   with HammerDB (`autocommit = 1` in its MySQL OLAP script, otherwise MySQL
   offloads nothing), load the copies, power run with InnoDB and with the
   plugin, identical results, timings in `perf/`.
5. **Target release decision: 9.7 or 8.4 LTS**, before more code depends
   on 9.7 interfaces. For 8.4, confirm the external executor hook,
   `HTON_SECONDARY_ENGINE_SUPPORTS_DDL`, the SQL-layer foreign keys (and
   that cascades are binlogged) and the secondary-only statement mechanism
   used by the guard.
6. **Production hardening, must-have** (phases 3 and 5; detail in
   "Production readiness" below): crash tests and a built-in consistency
   check, snapshot expiry and compaction, apply lag under write load,
   resource governance, backup and restore, refresh after DDL that does not
   block the other tables.
7. **Translation by real usage** (phase 5; order in "SQL gaps by priority"
   below): first counters of refusal reasons, so a workload shows which
   gaps matter; then CAST / CONVERT, DATE_FORMAT and NOW() / CURRENT_DATE,
   functions on TIMESTAMP columns, window functions, GROUP_CONCAT, ROLLUP.
8. **Structural gaps**: tables without a primary key (needs a design: no
   row identity in the binary log; match on the full before-image or a
   hidden identity), JSON columns loaded as text (functions on them
   refused, the other columns offloaded).
9. **History**: tagged DROP PARTITION as archiving (designed: record
   partition bounds at load, archive the partition's rows at the DDL
   position; 3-4 days), backup integration (XtraBackup), encryption.
10. **General availability**: observability, packaging, security and
    compliance, upgrade policy, CI (detail below).

### TODO from 2026-11-18: limits that stop a result set

Agreed 2026-11-18, before the next steps above: what makes a query fail or
never reach DuckDB (not only run slower). Order of work: T1 to T5, then the
leftovers. Each item: design note for Marco, implementation, MTR test,
coverage tracker updated.

**T0. No more `wait_applier()`: DDL waits for its refresh, with progress**
(agreed 2026-11-19)
- [x] After a DDL on a table with a copy, the session's **next statement**
      waits until the applier has refreshed the copy (`duckdb_guard`:
      the DDL is recorded at its end, the wait is at the next statement's
      pre-parse). The DDL itself cannot wait: it holds the table's metadata
      lock until the server sends its OK, and the refresh (internal
      SECONDARY_LOAD) needs that lock; the first version deadlocked there
      (found by MTR, 2026-11-20). `duckdb_ddl_wait_refresh` (ON default) and
      `duckdb_ddl_wait_refresh_timeout` (seconds, 0 = until done); KILL
      QUERY or the timeout stop only the wait (error log line). MTR
      `ddl_wait`.
- [x] Progress while waiting: SHOW PROCESSLIST state ("Waiting for DuckDB
      copy refresh of db.t: N of M rows (P%), S s"), Performance Schema
      stages `stage/duckdb/waiting for copy refresh`, `waiting for applier`,
      `copy load` with WORK_COMPLETED / WORK_ESTIMATED,
      error log progress at info level every 10 s for long loads, and a
      line when a wait ends or stops (a note would be cleared by the
      statement that waited).
- [x] `INFORMATION_SCHEMA.DUCKDB_TABLES`: STATE 'refreshing',
      REFRESH_ROWS_DONE, REFRESH_ROWS_TOTAL, applied position.
- [x] FULL scope queries wait without the `duckdb_offload_wait_timeout_ms`
      limit (KILL / max_execution_time stop them): no InnoDB answer exists.
- [ ] Remove `wait_applier()` from the tutorial; HOWTO keeps waiting for the
      whole applier only as an operator step.

**T1. Prepared statements and variables** (check 1/2 day, then 1-2 days)
- [ ] Check server-side prepared statements with parameters (`?`): the
      translator has no case for `Item::PARAM_ITEM`, most likely refused.
      Connectors use them (JDBC `useServerPrepStmts`, PDO without emulation).
- [ ] Check stored procedure variables (`Item_splocal`) and user variables
      (`@x`): not handled, refused.
- [ ] If confirmed: translate them as constants at execution (as literals,
      typed as the parameter / variable).

**T2. DuckDB errors during execution** (1 day or a redesign: to find out)
- [ ] Today an error after optimization (out of memory, spill disk full,
      any runtime error) is the statement's error, also with
      `use_secondary_engine=ON`, where InnoDB would answer.
- [ ] Find out what MySQL allows at execution time (going back to the
      primary engine from the external executor).
- [ ] Fall back when the error comes before the first row is sent; if MySQL
      does not allow it, run blocking statements first (as the DECIMAL path).

**T3. FULL scope survival** (a refusal under FULL is an error: InnoDB has no
archived rows)
- [ ] A stale history copy (DDL not followed, applier stopped): read it as
      of its last applied state with a warning instead of refusing.
- [ ] Translate first what history reports use: CAST / CONVERT, NOW() /
      CURRENT_DATE, DATE_FORMAT, functions on TIMESTAMP columns.
- [ ] Applier: XA transactions on loaded tables, `binlog_encryption=ON`.

**T4. Loadability** (a table that cannot be loaded has no offload and no
history)
- [ ] JSON columns loaded as text, JSON functions refused (small); also
      decide spatial and VECTOR (VECTOR: exact distance first).
- [ ] Zero dates (`0000-00-00`): stored as NULL plus a hidden flag, as the
      `__n_` / `__w_` columns.
- [ ] Tables without a primary key: design first (no row identity in the
      binary log).

**T5. Concurrency** (with the HammerDB run, next step 4)
- [ ] More concurrent offloaded statements than `duckdb_executor_threads`:
      queueing, wait time, behaviour.
- [ ] Client character sets other than utf8mb4 (`character_set_results`).

**Leftovers of the 2026-11-18 review**
- [ ] B1: the DECIMAL first run materializes the whole result (memory);
      policy: run first only grouped / LIMIT statements, or bound by
      `duckdb_memory_limit` and fall back (decide).
- [ ] D1: more edge data in the shared test tables (combining marks, emoji
      modifiers, 'ß', Turkish 'İ' / 'ı', trailing spaces vs tabs,
      `_ai_ci`-equal strings, 39-digit literals); a difference is a bug or a
      refusal, never a re-recorded result.
- [ ] E1: split `duckdb_offload.cc` (about 3.6k lines) into literals,
      strings, temporal, aggregates, blocks, execution; no behaviour change.
- [ ] Document `autocommit=0` / `BEGIN` (MySQL never offloads there) in the
      HOWTO limits, with the HammerDB note.

### Production readiness

What is needed besides SQL coverage before the feature can be used by
customers. An untranslated query still gives InnoDB's result (slower); the
items below are the ones that can lose data, disk or availability.

Must-have before any customer use:

| Item | What | Why | Status |
|---|---|---|---|
| Crash safety | Kill mysqld at every applier stage (mid batch, mid DuckLake commit, during a refresh after DDL, during SECONDARY_LOAD); compare the copy with InnoDB row by row after restart | The copy must never silently diverge | missing |
| Consistency check | An operator command (checksum per table or per range, InnoDB vs DuckDB, at the applied position) | Proof for operators and support; detects divergence after incidents | missing |
| Disk space | DuckLake snapshot expiry, compaction of small files, removal of dropped copies and old versions (scheduled, with variables) | Today the lake grows forever under updates: the risk most likely to hit a user within weeks | missing |
| Apply lag under write load | Sustained OLTP load (sysbench or HammerDB TPROC-C) next to the analytics; batch tuning; decide on the deferred parallel prepare workers | Freshness target and capacity planning | missing |
| Resource governance | Memory and thread limit per offloaded query, a cap on concurrent offloaded queries, DuckDB out-of-memory becomes an error or an InnoDB fallback, never a crash | DuckDB runs inside mysqld: a runaway query affects OLTP | partial (`duckdb_memory_limit`, `duckdb_threads`, KILL, `max_execution_time`) |
| Backup and restore | XtraBackup integration, or a documented procedure that keeps the lake, its applied position and the InnoDB backup consistent | For history tables the lake is the only copy of purged rows | missing |
| Refresh after DDL | Incremental refresh (ALTER ADD / DROP COLUMN applied in place) or a refresh that does not stop the applier for other tables | Today DDL on a large table means minutes of lag for every table | missing |
| Performance proof | HammerDB TPROC-H at scale factor 1 and 10 on the release build (next step 4) | The business case | planned |

Needed for general availability:

| Item | What | Status |
|---|---|---|
| Observability | Apply lag per table, refusal reasons aggregated (not only the last), lake size | partial: lag in seconds and bytes, state code, memory and spill, offloaded query time (status variables), Performance Schema stages with progress, PMM / Grafana dashboard and custom queries (`monitoring/`, 2026-11-20) |
| Packaging | One tarball; plugin size (about 1.5 GB with debug info, stripped release); ICU symbol clash between the server (ICU 77) and DuckDB's icu extension (hidden visibility, to test) | missing |
| Security and compliance | Encryption (designed, keyring component), erasure requests against history copies (a purge keeps the personal data), privileges reviewed | designed |
| Upgrade policy | DuckDB / DuckLake version upgrades and lake format migration (DuckLake has no release tags) | missing |
| CI | MTR suite in CI, Clang build, ASAN / UBSAN runs of the applier and the offload | missing (MTR suite exists, 26 tests) |
| Documentation | Operations guide (sizing, monitoring, failure scenarios), beyond HOWTO and TUTORIAL | partial |

### SQL gaps by priority

From the SQL coverage tracker; "common" means the gap appears in typical
reporting and BI queries. Each one today sends the query to InnoDB.

| Priority | Gap | Why | Effort |
|---|---|---|---|
| Done | Grouped strings in joins, VARCHAR and non-binary collations | 2026-11-22: checked per group in the query | done |
| High | Window functions | In most modern analytic queries | large (ordering and peers must follow MySQL collations) |
| High | CAST / CONVERT | Everywhere in ORMs and BI tools | medium (MySQL conversion rules, truncation) |
| High | DATE_FORMAT, NOW() / CURRENT_DATE in expressions, TIMESTAMPDIFF | Reports format dates and filter relative to now | medium (locale-dependent formats) |
| High | Functions on TIMESTAMP columns | Every TIMESTAMP column is cut off from date functions | medium (session time zone, as for constants) |
| Medium | LIKE and string functions on `_ci` / non-binary collations | Most schemas use `utf8mb4_0900_ai_ci` | large, may stay partial |
| Medium | ROLLUP / GROUPING SETS / GROUPING() | Report subtotals | medium |
| Medium | STDDEV / VARIANCE, BIT_AND / OR / XOR, GROUP_CONCAT | Statistics; GROUP_CONCAT is very common in MySQL | small; medium for GROUP_CONCAT (separator, ORDER BY, `group_concat_max_len`) |
| Medium | `<=>`, row constructors, string compared with a number | Generated SQL | small to medium |
| Medium | TPC-H Q21 (equalities between EXISTS and NOT EXISTS) | Last TPC-H subquery gap | medium (next step 3) |
| Low | UPPER / LOWER, REGEXP, WEEK, recursive CTEs, LATERAL, JSON_TABLE, JSON functions | Rarer in analytics or semantics hard to match | medium to large each |
| Low | Unsigned subtraction, EXP / POW overflow errors | Edge cases | small |

## Risks

- The secondary engine API, the external executor hook and the LEX methods used by the guard have no open-source production user in upstream MySQL and may change between releases without notice. Not changing core code means we depend on these internal interfaces from the plugin.
- Silent wrong results from semantic gaps, mainly collations, are worse than slow results. Rejection must be the default. Mitigated: weight columns, the exactness rule for every translated construct, OFF / FORCED comparison of every test query (no wrong result found in the TPC-H check, 2026-11-17).
- A DuckDB fault or memory blowup inside mysqld affects the whole server, not only analytical sessions.
- Change propagation under heavy update or delete load may not keep up, given the DuckDB cost for single-row changes; DuckLake writes a delete file and a new data file per update, and many small commits create many small files. Space of old versions and dropped copies is not reclaimed yet (phase 3).
- A refresh after DDL reloads the whole table, and the applier waits for it: on large tables DDL means minutes of lag for all tables.
- Benchmarks and tools that run with `autocommit=0` (HammerDB does for MySQL) get no offload: MySQL never uses a secondary engine inside a multi-statement transaction.
- Binlog dependency: propagation needs binlog with ROW format, FULL image and GTIDs; binlog files purged before the applier reads them leave a gap.
- History loss: a lake directory lost without backup, or an accidental SECONDARY_UNLOAD, destroys archived data permanently.
- Compliance: an erasure request executed in a purge-tagged transaction keeps personal data in DuckDB.
- DuckDB v2.0.0 is a recent major release (2026-10-21); DuckLake has no release tags.
- Plugin size (about 1.5 GB with debug info) and duplicated libraries: ICU is bundled by both the server (ICU 77) and DuckDB's icu extension; hidden visibility must keep them apart (to test).

## Open questions

Decided during development (kept for the record):

- Write-and-read statements under FULL scope: rejected by the guard, including every UPDATE / DELETE / TRUNCATE of a history table (2026-11-16).
- `__archived_at` is visible in the lake (readable offline); MySQL users see only the table's own columns.
- After `duckdb_enable=FALSE` and back, the applier resumes from the applied position; after a binary log gap, `duckdb_apply_gap_action` decides.
- Batch defaults: 1000 transactions, 100,000 rows, 1000 ms; `duckdb_apply_workers` deferred.
- Tables with foreign keys: allowed when cascades are binlogged (`innodb_native_foreign_keys=OFF`, 2026-11-17).
- A grouped string value shown in a join without the table's primary key: CHAR under binary collations is exact (2026-11-17); VARCHAR and non-binary collations are checked per group in the query, which runs first; an ambiguous group is a refusal (2026-11-22).
- DECIMAL results declared above 38 digits (2026-11-17): computed as DECIMAL(38, scale); statements that could overflow run first; `duckdb_decimal_overflow` = FALLBACK (default, InnoDB answers), ROUND (fewer decimals with a warning), ERROR.

Open:

- How to expose the separate history name (orders_history) without core changes, if at all.
- Which release we target in the end: 9.7 (current POC base) or 8.4 LTS, where the external executor hook, `HTON_SECONDARY_ENGINE_SUPPORTS_DDL`, the SQL-layer foreign keys and the secondary-only statement mechanism must be confirmed.
- How to handle tables without a primary key (excluded for now).
- DuckLake encryption support and the crypto library before implementing encryption.
- Packaging: separate package or part of the server packages (everything in one tarball either way).

## Appendix A: How AliSQL solved the storage engine problems

AliSQL integrates DuckDB as a storage engine (ENGINE=DUCKDB) with data stored only in DuckDB. They did not solve the problems inside a loadable plugin: they changed the MySQL SQL layer and maintain a downstream DuckDB fork.

- Row-at-a-time execution: reads bypass the handler. After MySQL parses, checks privileges, opens tables, acquires MDL and resolves types, a statement that references only DuckDB tables is sent whole to DuckDB, re-expressed from the parse tree when needed, with session settings (schema, time zone, collation, sql_mode) synchronized and results converted using MySQL type metadata. Mixed DuckDB and InnoDB statements are rejected, except one-way ingestion into DuckDB. Writes use whole-statement execution for single-table UPDATE and DELETE, and an appender or a private delta table, collapsed per logical key before commit, for row writes.
- Two-phase commit: XA is rejected. An internal DuckDB table stores the confirmed binlog position in the same DuckDB transaction as the data, and DuckDB commits first. On recovery, the binlog is truncated back to the position DuckDB confirmed, and other engines follow standard recovery. The cost is a custom binlog commit path coupled to ordinary binlog code.
- Data dictionary and DDL: the MySQL dictionary stays authoritative. The SQL layer selects a logical primary key, drops secondary indexes, foreign keys and CHECK constraints with warnings, and uses native DuckDB ALTER for simple changes or MySQL COPY ALTER otherwise. The logical primary key has no physical uniqueness in DuckDB.

## Appendix B: Verified standalone build commands

These are the commands of the verification run (scripts in /opt/duckdb_tmp: `build.sh`, `extension_config.cmake`, `smoke/`). Inside the MySQL build the same options are passed by the ExternalProjects, with the MySQL compiler, C++ standard (23), build type and sanitizer settings instead of the fixed values below, and `<bld>`, `<inst>` under `${CMAKE_BINARY_DIR}/extra/duckdb`.

Add the submodules:

```bash
git submodule add https://github.com/duckdb/duckdb.git          extra/duckdb/duckdb
git -C extra/duckdb/duckdb   checkout v2.0.0
git submodule add https://github.com/duckdb/ducklake.git        extra/duckdb/ducklake
git -C extra/duckdb/ducklake checkout 55cc50cf080fa5d04e76099620b84b330d4a2ba1
git submodule add https://github.com/RoaringBitmap/CRoaring.git extra/duckdb/croaring
git -C extra/duckdb/croaring checkout v5.5.5
git config -f .gitmodules submodule.extra/duckdb/duckdb.shallow true
```

CRoaring:

```bash
cmake -S extra/duckdb/croaring -B <bld>/croaring -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_ROARING_TESTS=OFF -DROARING_USE_CPM=OFF \
  -DCMAKE_INSTALL_PREFIX=<inst>/croaring
cmake --build <bld>/croaring && cmake --install <bld>/croaring
```

DuckDB with extensions and DuckLake:

```bash
cmake -S extra/duckdb/duckdb -B <bld>/duckdb -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
  -DCMAKE_CXX_STANDARD=20 -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DBUILD_SHELL=OFF -DBUILD_UNITTESTS=OFF -DENABLE_UNITTEST_CPP_TESTS=OFF -DBUILD_BENCHMARKS=OFF \
  -DENABLE_SANITIZER=OFF -DENABLE_UBSAN=OFF \
  -DENABLE_JEMALLOC=OFF \
  -DENABLE_EXTENSION_LOAD=OFF -DENABLE_BUILTIN_HTTPLIB=OFF \
  -DOVERRIDE_GIT_DESCRIBE=v2.0.0 \
  -DDUCKDB_EXTENSION_CONFIGS=extra/duckdb/extension_config.cmake \
  -DDUCKLAKE_SOURCE_DIR=extra/duckdb/ducklake \
  -DCMAKE_PREFIX_PATH=<inst>/croaring \
  -DCMAKE_INSTALL_PREFIX=<inst>/duckdb
cmake --build <bld>/duckdb && cmake --install <bld>/duckdb
```

Note: the verification run did not yet pass `OVERRIDE_GIT_DESCRIBE`, and the smoke test reported `v2.0.0-dev1`; the option is added to fix the version string.

Extension config (`extra/duckdb/extension_config.cmake`):

```cmake
duckdb_extension_load(core_functions)
duckdb_extension_load(parquet)
duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(ducklake SOURCE_DIR ${DUCKLAKE_SOURCE_DIR})
duckdb_extension_statically_link(core_functions parquet json icu ducklake)

set(PS_DUCKDB_CXX_STANDARD 20 CACHE STRING "C++ standard forced on all DuckDB components")

function(ps_duckdb_force_std dir)
  get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(t IN LISTS targets)
    get_target_property(type ${t} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
      continue()
    endif()
    set_target_properties(${t} PROPERTIES CXX_STANDARD ${PS_DUCKDB_CXX_STANDARD}
                                          CXX_STANDARD_REQUIRED ON)
    get_target_property(opts ${t} COMPILE_OPTIONS)
    if(opts)
      list(FILTER opts EXCLUDE REGEX "^-std=(c|gnu)\\+\\+[0-9]+$")
      set_target_properties(${t} PROPERTIES COMPILE_OPTIONS "${opts}")
    endif()
  endforeach()
  get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(sd IN LISTS subdirs)
    ps_duckdb_force_std("${sd}")
  endforeach()
endfunction()

function(ps_duckdb_force_std_all)
  ps_duckdb_force_std("${CMAKE_SOURCE_DIR}")
  if(TARGET clangd_cache)
    set_target_properties(clangd_cache PROPERTIES EXCLUDE_FROM_ALL ON)
  endif()
endfunction()

cmake_language(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ps_duckdb_force_std_all)
```

Smoke test results (C++20 program, C API only, static link): the five extensions registered and loaded with autoload off; ATTACH of `ducklake:<dir>/catalog.ducklake` with a local DATA_PATH (the directory must exist); 100,000 rows appended through the Appender into `lake.test.orders`; 50,000 rows archived with `UPDATE ... SET __archived_at = now()`; LIVE returned 50,000 rows, FULL returned 100,000 with 50,000 archived; on disk one `catalog.ducklake` and Parquet data and delete files under `data/test/orders/`.

## References

- [MySQL source documentation: ha_mock.h (MOCK secondary engine)](https://dev.mysql.com/doc/dev/mysql-server/8.4.10/ha__mock_8h_source.html)
- [MySQL source documentation: ha_mock.cc (MOCK secondary engine)](https://dev.mysql.com/doc/dev/mysql-server/8.4.11/ha__mock_8cc.html)
- [ShannonBase repository (open-source Rapid secondary engine)](https://github.com/shannon-data/shannonbase)
- [Internals of ShannonBase (wiki)](https://github.com/Shannon-Data/ShannonBase/wiki/Internals-of-ShannonBase)
- [AliSQL repository (DuckDB storage engine)](https://github.com/alibaba/AliSQL)
- [Discussion and AliSQL design document: DuckDB as a columnar storage engine for MySQL](https://github.com/mysql/mysql-community/discussions/79)
- [DuckDB mysql extension](https://duckdb.org/docs/extensions/mysql.html)
- [DuckLake v1.0 announcement](https://ducklake.select/2026/04/13/ducklake-10/)
- [DuckDB repository and releases](https://github.com/duckdb/duckdb/releases)
- [DuckLake repository](https://github.com/duckdb/ducklake)
- [CRoaring repository](https://github.com/RoaringBitmap/CRoaring)
- [DuckDB 2.0 build documentation](https://duckdb.org/docs/2.0/dev/building/overview.html)
