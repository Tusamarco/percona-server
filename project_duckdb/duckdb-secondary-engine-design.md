# DuckDB as a MySQL Secondary Engine

**Design and Implementation Plan**

Status: draft v0.3 for review. Owner: Marco. Target: Percona Server for MySQL; the POC is built on the Percona-Server-9.7.2-2 branch (final target release still open, see Open questions).

Changes from v0.2:

- Storage is DuckLake (DuckDB catalog file plus Parquet data files) under `<datadir>/duckdb_data`, configurable with `duckdb_data_path`.
- No change to core server code: everything lives in the plugin. Change propagation therefore uses a binlog reader inside the plugin, not commit hooks.
- Purge deletes are identified by a tagged GTID (`duckdb_avoid_purge`, configurable), replacing `duckdb_purge_mode`.
- Applier model: one binlog reader, parallel prepare workers, one committer with one DuckLake commit per batch.
- Query offload uses the server's external executor hook; SQL text is generated from the query block; joins are offloaded when all tables are DuckDB tables.
- A guard plugin in the same library makes FULL scope never fall back to InnoDB.
- Full list of plugin variables, startup sequence and directory checks.
- Build integration verified: DuckDB v2.0.0, DuckLake and CRoaring built as C++20 inside one CMake run (ExternalProject), with a smoke test.
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

One DuckDB instance lives inside mysqld, in memory, with one DuckLake attached as `lake`. Each MySQL session that touches DuckDB gets its own DuckDB connection. DuckDB, its extensions, DuckLake and CRoaring are linked statically into the plugin library; DuckDB and DuckLake are MIT licensed, compatible with GPL-2.0 (third-party licenses to be confirmed for the SBOM).

The library `ha_duckdb.so` contains two plugins:

- `duckdb`: the secondary engine (load, unload, offload, applier, variables).
- `duckdb_guard`: an audit (event tracking) plugin that enforces the FULL scope rules (see Query offload).

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
| `duckdb_memory_limit` | size | none, mandatory | global | yes | Plugin init fails if not set |
| `duckdb_threads` | int | none, mandatory | global | yes | Plugin init fails if not set; range 1 to number of CPUs |
| `duckdb_encryption` | BOOL | OFF | global | no | Fixed when the catalog is created; not in the POC |
| `duckdb_history_scope` | enum LIVE, FULL | LIVE | global, session, SET_VAR hint | yes | Affects only tables with retain_history |
| `duckdb_avoid_purge_gtid_tag` | string | `duckdb_avoid_purge` | global | yes | GTID tag that marks purge transactions |
| `duckdb_apply_workers` | int | 1 | global | proposal: no in the POC | Prepare workers of the applier |
| `duckdb_apply_batch_max_trx` | int | to be decided | global | yes | Batch closes at this many source transactions |
| `duckdb_apply_batch_max_rows` | int | to be decided | global | yes | Batch closes at this many row changes |
| `duckdb_apply_batch_max_delay_ms` | int | to be decided | global | yes | Maximum wait before a batch closes; bounds lag under light load |

### Mandatory variables

`duckdb_memory_limit` and `duckdb_threads` have no usable default: the internal default 0 means "not set". If either is missing (from my.cnf, the command line or mysqld-auto.cnf), plugin init fails with an error such as `DuckDB: duckdb_memory_limit is not set; add it to the configuration file. Plugin not loaded.` They are mandatory also when `duckdb_enable=FALSE`. With the default plugin load option the server still starts and only the plugin is not loaded; with `--duckdb=FORCE` or `FORCE_PLUS_PERMANENT` the server refuses to start, which is standard MySQL behavior. At runtime, setting either to 0 is rejected.

### Dynamic variables: where the work happens

The update callback of a plugin variable runs under `LOCK_global_system_variables` and cannot fail (sql/sql_plugin_var.cc); the check callback runs before, without the lock, and can return an error. We therefore do the real work in the check callback (open or close DuckDB, `SET memory_limit`, `SET threads`) and only store the value in the update callback. An internal plugin mutex serializes concurrent changes. Known edge case: in a multi-variable `SET` where a later variable fails its check, our change is already applied; our checks are idempotent and we document it.

- `duckdb_memory_limit`: lowering it below current usage makes DuckDB evict buffers; if that is not possible the SET fails with the DuckDB message and the old value stays.
- `duckdb_threads`: applies to the shared DuckDB instance; running queries pick it up at their next task scheduling.
- `duckdb_avoid_purge_gtid_tag`: validated as a GTID tag (letter or underscore first, then letters, digits, underscores, at most 32 characters), stored in lowercase, empty rejected. Because the applier is asynchronous, a change records the binlog position where it happened: transactions before it are matched against the old tag, transactions after it against the new one, and the switch is logged. The value must be the same on all servers of a replication topology.

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
       ├─ lake.<mysql_schema>.<table>       one per SECONDARY_ENGINE table
       └─ lake.__mysql.applied_position     applied GTID, same transaction as data
  temp_directory = <path>/tmp/              spills from large queries
```

- Catalog backend: a local DuckDB file. mysqld is the only writer, so the single-writer limit does not matter, and it adds no external dependency.
- Each MySQL database maps to a DuckLake schema with the same name, each table to a table with the same name, plus the hidden `__archived_at TIMESTAMP` column on retain_history tables.
- Default location `<datadir>/duckdb_data`. The name `duckdb_data` is reserved in the datadir when the default is used; a MySQL database with that name would fail with "database directory exists".
- Hardening after attach: `autoload_known_extensions=false`, `autoinstall_known_extensions=false` (runtime settings in DuckDB 2.0), `enable_external_access=false` so DuckDB SQL cannot read arbitrary files or reach the network (to verify that it does not block DuckLake access to DATA_PATH when set after the attach). `lock_configuration` is not used, because we change `memory_limit` and `threads` at runtime; users never send SQL to DuckDB directly and our translator never emits SET.
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

- Preconditions, checked at load; the load is refused otherwise: a primary key, no foreign keys, supported column types, `log_bin=ON`, `binlog_format=ROW`, `binlog_row_image=FULL`, `gtid_mode=ON`, `binlog_row_metadata=FULL`.
- POC types: signed and unsigned integers, DECIMAL, FLOAT and DOUBLE, CHAR, VARCHAR and TEXT, DATE, DATETIME, TIMESTAMP. Any other type fails the load with an error naming the column.
- `NOT SECONDARY` columns are honored through the table read set.
- The load records the binlog position and GTID set it represents; the applier skips events for that table up to that point.
- For a retain_history table, SECONDARY_LOAD is allowed only when the DuckDB copy is empty; afterwards InnoDB no longer holds the archived rows, so a reload would destroy history.

### Change propagation: binlog reader in the plugin

Because core code must not change, the plugin reads the binary log. It uses only public interfaces:

- Notification: a `Binlog_storage_observer` registered with `register_binlog_storage_observer()` (sql/replication.h), the API semisync uses. `after_flush` gives the binlog file and position after each group commit, so we never poll.
- Parsing: the in-tree event library (`libs/mysql/binlog/event`): `Gtid` events (transaction id and tag), `Table_map`, `Write_rows`, `Update_rows`, `Delete_rows`. Compressed transaction payloads are decoded by the same library. Events for tables that are not loaded are ignored.

```
binlog files ──► [reader] ──► [prepare worker 1..N] ──► [committer] ──► DuckLake
                 decode,       convert to DuckDB         one DuckLake
                 filter,       vectors, collapse         transaction per batch
                 detect tag    per primary key           + applied GTID
```

- Reader thread: reads and decodes the binlog, filters loaded tables, detects the purge tag.
- Prepare workers (`duckdb_apply_workers`): convert values and collapse changes per primary key inside the batch. Work is split by table or by hash of the primary key, so changes to the same key stay in order without dependency tracking.
- Committer: merges the prepared parts and commits once per batch, in GTID order. Cross-table source transactions stay atomic in DuckDB, there is a single applied position, and DuckDB parallelizes the commit work with its own `duckdb_threads`.
- Collapse rules within a batch: insert then updates becomes one insert of the final row; insert then a real delete becomes nothing; insert then a purge-tagged delete becomes an insert with `__archived_at` set; several updates become one update with the final image.
- Batch boundaries are always at transaction boundaries; a batch closes at the first of `duckdb_apply_batch_max_trx`, `duckdb_apply_batch_max_rows`, `duckdb_apply_batch_max_delay_ms`.
- Crash safety: each batch stores its last GTID in `lake.__mysql.applied_position` in the same DuckLake transaction as the data; on restart the reader resumes from there.
- DDL on a loaded table invalidates the copy: the table is unloaded with a warning (retain_history tables: see Durability of the history).

Why not the replica model (`replica_parallel_workers`): its gain comes from many small commits in parallel, which suits InnoDB, where a commit is cheap and row operations dominate. In DuckLake each commit writes new files and a catalog snapshot, commits serialize on the catalog, and concurrency control is optimistic, so parallel small commits would multiply the most expensive step and create many small files. We keep its principle (independent work in parallel, order where it matters) in the prepare workers. The dependency information (`last_committed`, `sequence_number`) in the Gtid events stays available if a future per-table commit mode is ever needed.

Lag is visible through status variables; queries see data up to the last applied transaction (Freshness and consistency).

### Delete classification

For tables with history retention, the applier distinguishes two kinds of delete:

- Purge delete: a delete in a transaction whose GTID carries the tag named by `duckdb_avoid_purge_gtid_tag` (default `duckdb_avoid_purge`). The row is kept in DuckDB and `__archived_at` is set to the commit time. The tag is written in the same transaction, it replicates to replicas, and setting it requires the server's existing `TRANSACTION_GTID_TAG` privilege, because a mistaken purge keeps data that was meant to disappear. `ALTER TABLE ... DROP PARTITION`, visible in the binlog as a DDL event, is a later addition.
- Business delete: any other delete, including cancellations and legal erasure requests. The live row is removed from DuckDB (`WHERE pk = ? AND __archived_at IS NULL`).

A primary key purged and inserted again gives one archived row and one live row with the same key; the key is unique only among live rows.

### Query offload and plan translation

The server decides whether to try offload: `use_secondary_engine` is ON or FORCED, every table in the query has `SECONDARY_ENGINE=duckdb` and is loaded, and our pre-prepare hook finds the estimated cost above `secondary_engine_cost_threshold`. Then:

1. Prepare hook: creates our per-statement context.
2. Optimize hook: we walk the query block (select list, FROM and joins, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT). Every item must be on the allow list (columns, literals, comparison and boolean operators, arithmetic, SUM, COUNT, AVG, MIN, MAX, a first set of date functions). If all is supported, we generate DuckDB SQL text and set `join->override_executor_func`, with the `USE_EXTERNAL_EXECUTOR` flag so the server builds no iterators. Otherwise we reject with a reason (optimizer trace, status counter), and in LIVE scope the query runs on InnoDB.
3. Executor function: runs the SQL on the session's DuckDB connection, reads results chunk by chunk, converts each value to the MySQL type of its select-list item, and sends rows to the client.

- Joins are offloaded when all tables in the query are DuckDB tables and loaded; generating SQL from the query block lets DuckDB apply its own optimizer, including join order.
- LIVE scope adds `__archived_at IS NULL` for each history table; FULL omits it.
- POC limits: only top-level SELECT results sent to the client are offloaded; INSERT ... SELECT, CREATE ... SELECT, SELECT ... INTO and subqueries inside DML run on InnoDB. No offload inside a transaction that has already written, because DuckDB does not have those writes yet.
- The framework provides the hooks; the translation is entirely our work.

### FULL scope guard (duckdb_guard)

Under FULL scope a query that ends up on InnoDB would return only live rows, silently. When `use_secondary_engine=OFF`, or when the server decides not to try offload, our engine hooks are never called, so the engine alone cannot prevent it. The guard closes the gap without core changes:

- The server raises table access READ events in `open_tables()` (sql/sql_base.cc), before `open_secondary_engine_tables()` decides between primary and secondary, and before any row is read.
- The server already supports statements that can execute only in the secondary engine (`lex->set_execute_only_in_secondary_engine()`, used for CUBE, TABLESAMPLE, GROUPING SETS). With `use_secondary_engine=OFF` such a statement fails; otherwise it is treated as FORCED, so failing to offload is an error and never a fallback.

On each READ event the guard:

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
- Collations. GROUP BY, DISTINCT, ORDER BY and comparisons on strings must follow the column collation. Under utf8mb4_0900_ai_ci, "José" and "jose" are equal; under the DuckDB default comparison they are not. This is the largest source of silent wrong results.
- NULL handling and MySQL implicit type conversions.
- Function mapping, one function at a time, with an allow list. Anything not on the list causes rejection.

### Freshness and consistency

Propagation is asynchronous, so a query on DuckDB sees data up to the last applied transaction. In the POC we expose the lag through status variables. Later we need a policy, configurable per session: accept a bounded lag, wait until DuckDB has applied the GTID the session needs (similar to WAIT_FOR_EXECUTED_GTID_SET), or, in LIVE scope only, fall back to InnoDB when the lag is above a threshold.

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
- DDL: ALTER TABLE on the base table can no longer mean unload and reload. We apply the change to the DuckDB table in place, with archived rows taking the column default.
- Unload: SECONDARY_UNLOAD on a table with history must require an explicit confirmation option, because it destroys data that exists nowhere else.
- Binlog retention: if binlog files are purged before the applier has read them, the copy has a gap. The plugin cannot block binlog purge without core changes, so it detects the gap, marks the affected tables stale and logs an error.

## Operations

- Resource limits: `duckdb_memory_limit` and `duckdb_threads` are mandatory and dynamic.
- DDL on a base table without history invalidates the secondary copy and requires a reload; with history, see Durability of the history.
- Observability: status variables and Performance Schema tables for apply lag (`duckdb_apply_lag_seconds`, `duckdb_apply_lag_transactions`, `duckdb_applied_gtid`), batches and collapsed rows (`duckdb_apply_batches`, `duckdb_apply_rows_collapsed`), time split between decode, prepare and commit, offloaded and rejected query counts, rejection reasons, archived row counts and DuckDB memory use.
- Testing: MTR suite `duckdb`; every query runs with use_secondary_engine OFF and FORCED and results are compared in LIVE scope; FULL scope tests check archived rows and rejections.

## Build and source integration

The whole feature builds in one CMake run together with the server, with the same compiler and C++ standard (C++20). This was verified on 2026-11-05 with GCC 15.3 on Fedora 43: all 480 C++ files of DuckDB, its extensions and DuckLake compiled with `-std=c++20` and no errors, and a statically linked C++20 smoke test passed (Appendix B). Clang is not yet verified.

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

- `extra/duckdb/extension_config.cmake`: loads the five extensions, DuckLake from our submodule, links them statically, forces C++20 on every target (DuckLake sets `CMAKE_CXX_STANDARD 17` and DuckDB's bundled re2 adds `-std=c++17`; at the end of configuration we set the standard on all targets and drop those flags, without touching upstream files), and keeps DuckDB's `clangd_cache` helper out of the build.
- `cmake/duckdb.cmake` and `extra/duckdb/CMakeLists.txt`: two ExternalProjects, `croaring_ext` then `duckdb_ext`, and imported static targets.
- `storage/duckdb/`: the plugin.

### ExternalProject wiring

- At configure time nothing is built; at build time the plugin target depends on `duckdb_ext`, which depends on `croaring_ext`, so everything builds in the same `make` or `ninja` run. `BUILD_ALWAYS ON` makes edits in the submodules picked up incrementally.
- Forwarded from the MySQL build: C and C++ compilers, `CMAKE_CXX_STANDARD=20`, build type (cache variable `DUCKDB_BUILD_TYPE`, default the MySQL build type, overridable because DuckDB debug assertions are slow), sanitizer flags (DuckDB's `ENABLE_SANITIZER` and `ENABLE_UBSAN` follow `WITH_ASAN` and `WITH_UBSAN`; they default to ON upstream), PIC, the compiler launcher (ccache).
- DuckDB options: `BUILD_SHELL=OFF`, `BUILD_UNITTESTS=OFF`, `ENABLE_UNITTEST_CPP_TESTS=OFF`, `BUILD_BENCHMARKS=OFF`, `ENABLE_JEMALLOC=OFF` (no second allocator in mysqld), `ENABLE_EXTENSION_LOAD=OFF`, `ENABLE_BUILTIN_HTTPLIB=OFF`, `DUCKDB_EXTENSION_CONFIGS=<our config>` (the CMake name in DuckDB 2.0; `EXTENSION_CONFIGS` is the Makefile name and is ignored by CMake), `DUCKLAKE_SOURCE_DIR`, `CMAKE_PREFIX_PATH=<croaring install>`.
- DuckDB warnings stay DuckDB's own: the server's `-Werror` maintainer flags do not apply to DuckDB sources. C++20 gives deprecation warnings in DuckDB headers (for example `std::atomic_load` on `shared_ptr`), warnings only.
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

Out of the POC: encryption, `binlog_encryption`, DROP PARTITION as purge, the separate `orders_history` name, the freshness wait policy, in-place DDL for history tables, XtraBackup integration, compaction scheduling, write-and-read statements under FULL (rejected).

No core code changes: everything lives in `storage/duckdb/`, `extra/duckdb/` and `cmake/duckdb.cmake`.

POC tests (MTR suite `duckdb`):

- Startup: default and custom `duckdb_data_path`, creation of directory and catalog, refusal when data files exist without catalog, permission refusals and warnings, missing mandatory variables, `loose-` options without the plugin.
- `duckdb_enable`: same output as without the plugin; runtime OFF and ON.
- Load: preconditions (PK, FK, types, binlog settings), retain_history load once.
- Propagation: INSERT, UPDATE, DELETE, REPLACE, INSERT ... ON DUPLICATE KEY UPDATE, LOAD DATA; collapse cases; restart and crash recovery from the applied position.
- Purge: tagged and untagged deletes; purge then reinsert of the same key; tag change at runtime.
- Offload: OFF versus FORCED result comparison in LIVE; joins; rejections and fallback; FULL errors from the guard.

Exit criterion: the five features work end to end in MTR, and a reference analytical workload shows a measured speedup versus InnoDB.

## Phased plan

| Phase | Scope | Exit criterion |
|---|---|---|
| 1. POC | See POC scope | Features work end to end in MTR; measured speedup on a reference analytical workload versus InnoDB |
| 2. Correctness | Type and collation mapping, function allow list, rejection and fallback, MTR result comparison, Clang build | Identical results with use_secondary_engine OFF and FORCED across the test suite |
| 3. Propagation hardening | Crash tests, binlog gap detection, catch-up after duckdb_enable=FALSE, compaction and snapshot expiry, batch tuning, freshness policy | Copy stays consistent through crash tests; measured apply lag under write load |
| 4. History retention complete | DROP PARTITION as purge, DUCKDB_HISTORY_READER privilege, in-place DDL, confirmed SECONDARY_UNLOAD, backup integration, encryption | Purged rows remain queryable in FULL scope; LIVE results still identical to InnoDB; backup and restore preserve history |
| 5. Production readiness | Wider plan translation, resource governance, observability, packaging | Agreed workload runs on a staging environment within lag and memory targets |

## Risks

- The secondary engine API, the external executor hook and the LEX methods used by the guard have no open-source production user in upstream MySQL and may change between releases without notice. Not changing core code means we depend on these internal interfaces from the plugin.
- Silent wrong results from semantic gaps, mainly collations, are worse than slow results. Rejection must be the default.
- A DuckDB fault or memory blowup inside mysqld affects the whole server, not only analytical sessions.
- Change propagation under heavy update or delete load may not keep up, given the DuckDB cost for single-row changes; DuckLake writes a delete file and a new data file per update, and many small commits create many small files.
- Binlog dependency: propagation needs binlog with ROW format, FULL image and GTIDs; binlog files purged before the applier reads them leave a gap.
- History loss: a lake directory lost without backup, or an accidental SECONDARY_UNLOAD, destroys archived data permanently.
- Compliance: an erasure request executed in a purge-tagged transaction keeps personal data in DuckDB.
- DuckDB v2.0.0 is a recent major release (2026-10-21); DuckLake has no release tags.
- Plugin size (about 1.5 GB with debug info) and duplicated libraries: ICU is bundled by both the server (ICU 77) and DuckDB's icu extension; hidden visibility must keep them apart (to test).

## Open questions

- Under FULL scope, what statements that write and also read a history table do: read live data only, read the full history, or be rejected. The POC rejects them.
- How to expose the separate history name (orders_history) without core changes, if at all.
- Which release we target in the end: 9.7 (current POC base) or 8.4 LTS, where the external executor hook and the secondary-only statement mechanism must be confirmed.
- Whether the archive mark column (__archived_at) is visible to users in FULL scope.
- How to handle tables without a primary key (excluded for now).
- After `duckdb_enable=FALSE` and back to TRUE, or after a binlog gap: catch up from the applied position or mark tables stale and require a reload.
- Defaults for `duckdb_apply_batch_max_trx`, `_max_rows`, `_max_delay_ms`, and whether `duckdb_apply_workers` is dynamic.
- DuckLake encryption support and the crypto library before implementing encryption.
- Packaging: separate package or part of the server packages (everything in one tarball either way).

## Appendix A: How AliSQL solved the storage engine problems

AliSQL integrates DuckDB as a storage engine (ENGINE=DUCKDB) with data stored only in DuckDB. They did not solve the problems inside a loadable plugin: they changed the MySQL SQL layer and maintain a downstream DuckDB fork.

- Row-at-a-time execution: reads bypass the handler. After MySQL parses, checks privileges, opens tables, acquires MDL and resolves types, a statement that references only DuckDB tables is sent whole to DuckDB, re-expressed from the parse tree when needed, with session settings (schema, time zone, collation, sql_mode) synchronized and results converted using MySQL type metadata. Mixed DuckDB and InnoDB statements are rejected, except one-way ingestion into DuckDB. Writes use whole-statement execution for single-table UPDATE and DELETE, and an appender or a private delta table, collapsed per logical key before commit, for row writes.
- Two-phase commit: XA is rejected. An internal DuckDB table stores the confirmed binlog position in the same DuckDB transaction as the data, and DuckDB commits first. On recovery, the binlog is truncated back to the position DuckDB confirmed, and other engines follow standard recovery. The cost is a custom binlog commit path coupled to ordinary binlog code.
- Data dictionary and DDL: the MySQL dictionary stays authoritative. The SQL layer selects a logical primary key, drops secondary indexes, foreign keys and CHECK constraints with warnings, and uses native DuckDB ALTER for simple changes or MySQL COPY ALTER otherwise. The logical primary key has no physical uniqueness in DuckDB.

## Appendix B: Verified standalone build commands

These are the commands of the verification run (scripts in /opt/duckdb_tmp: `build.sh`, `extension_config.cmake`, `smoke/`). Inside the MySQL build the same options are passed by the ExternalProjects, with the MySQL compiler, build type and sanitizer settings instead of the fixed values below, and `<bld>`, `<inst>` under `${CMAKE_BINARY_DIR}/extra/duckdb`.

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
