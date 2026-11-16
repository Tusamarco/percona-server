# DWL#3: Secondary tables: SECONDARY_LOAD and SECONDARY_UNLOAD

Affects: Percona Server 9.7 — Status: step 3a implemented (under test), step 3b implemented, verified on a throwaway instance (2026-11-11) and on the reference instance (2026-11-12); MTR postponed

## Description

### GENERAL IDEA

A user declares DuckDB as the secondary engine of an InnoDB table and loads its
current data into DuckLake. We implement `load_table()` and `unload_table()` of
the handler, the preconditions checked at load, the type mapping, the registry
of loaded tables, and the behavior of DDL on tables with a DuckDB copy.

The work is split in two steps:

- 3a (implemented): SECONDARY_LOAD, registry, type mapping, `open()` of loaded
  tables, SECONDARY_UNLOAD of classic tables, DDL on history tables keeps the
  data and marks the copy stale.
- 3b (implemented): reattach and resync of history copies, `adopt_history_from`,
  structure check (also without loading), confirmed discard of history, the
  two-transaction load. File cleanup after unload stays in Phase 3; MTR tests
  postponed.

Decisions (2026-11-11, step 3b):

- Since DWL#4 step 4d, RENAME TABLE and ALTER are allowed and followed by the
  applier. `adopt_history_from` stays: it gives a stale history copy to a
  table with another name when the old table no longer exists (RENAME cannot
  do it).
- Taking over a history copy (adopt, reattach, resync) requires the same
  structure: the same columns by name, the same DuckDB type for each, the
  same primary key columns. Column order does not count. Any difference
  refuses the load; every difference is reported.
- The structure can be checked without loading
  (`duckdb_history_check_only`).
- History is deleted only with `duckdb_unload_history_confirm=ON`.
- Every load is two DuckLake transactions: the data under a build name, then
  the swap and the registry row under `apply_mutex` (see Load). The space use
  of a replacing load is unchanged: the new copy is built beside the old one,
  archived rows included; DuckLake frees files only at snapshot expiry
  (Phase 3). An in-place variant (no archived-row copy) was considered and
  not taken.

### USER INTERFACE

```sql
CREATE TABLE orders (
  id BIGINT PRIMARY KEY, amount DECIMAL(12,2), created_at DATETIME)
  SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';

ALTER TABLE orders SECONDARY_LOAD;
ALTER TABLE orders SECONDARY_UNLOAD;
```

`retain_history` is optional, default false. Tables without it behave as a
classic secondary engine copy. The DuckDB table is never created by hand: the
plugin derives it from the InnoDB definition at SECONDARY_LOAD.

### MAPPING

A MySQL table `db.t` becomes the DuckLake table `lake.db.t` (schema created on
first use), with the same column names and mapped types. Tables with
`retain_history` get an extra column `__archived_at TIMESTAMP`, NULL for live
rows. All identifiers are quoted in generated DuckDB SQL, so column names such
as `date` and `time` are safe.

### REFERENCE TEST TABLES

`mill1` to `mill10` (Marco's test tables: BIGINT primary key, `utf8mb3_bin`
strings, DATE, TIMESTAMP, SMALLINT UNSIGNED). `mill1`-`mill5` are classic copies,
`mill6`-`mill10` have `retain_history`. About one million rows in at least one
table for the speedup measurement.

## Requirements

### Functional Requirements

1. `ALTER TABLE ... SECONDARY_LOAD` copies all rows of the InnoDB table into
   `lake.<db>.<table>` in one DuckLake transaction, together with the registry
   row. A crash or an error leaves nothing in the lake.
2. The load is refused with an error if the table has no primary key, has
   foreign keys (referencing or referenced), or has a column of an unsupported
   type; the error names the reason or the column.
3. The load is refused if `log_bin`, `binlog_format=ROW`,
   `binlog_row_image=FULL`, `gtid_mode=ON` or `binlog_row_metadata=FULL` does
   not hold; the error names the setting. Since DWL#4 step 4a it also
   requires `binlog_transaction_compression=OFF` and `binlog_encryption=OFF`
   (POC), and `sync_binlog=1` for `retain_history` tables (warning for classic
   tables). The load records the binary log file and position.
4. Supported types in the POC: TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT (signed
   and unsigned), DECIMAL (precision up to 38), FLOAT, DOUBLE, CHAR, VARCHAR,
   TEXT types, BINARY, VARBINARY, BLOB types, DATE, DATETIME, TIMESTAMP.
5. Text columns in any character set and any collation are loaded; values are
   stored as UTF-8 (converted with the server's own conversion when the
   character set is not UTF-8 compatible). Binary strings are stored as BLOB.
   Collation semantics are enforced at query time (DWL#6) and made exact in
   Phase 2.
6. Zero dates and zero timestamps make the load fail, naming the column.
7. `SECONDARY_ENGINE_ATTRIBUTE` must be a JSON object with `retain_history`
   (boolean) and `adopt_history_from` ("db.table" or "table", 3b; requires
   `retain_history`). Any other key is an error.
8. Columns marked `NOT SECONDARY` are not loaded.
9. The load records the GTID set executed at load time with the table, so the
   applier (DWL#4) skips events already contained in the load.
10. Before committing, the load compares the DuckDB row count with the number of
    InnoDB rows read; a mismatch fails the load.
11. SECONDARY_LOAD of a classic table that is already in the lake is refused:
    the user runs SECONDARY_UNLOAD first. This also covers a leftover copy after
    a rolled back load (the server calls our unload even when the dictionary
    says the table is not loaded).
12. SECONDARY_LOAD of a `retain_history` table that is already in the lake
    reattaches a stale copy or resyncs a loaded one (FR 17).
13. `SECONDARY_UNLOAD` on a table without `retain_history` drops the DuckLake
    table and its registry row. Its Parquet files stay on disk until snapshot
    expiry and file cleanup (Phase 3).
14. `SECONDARY_UNLOAD` on a table with `retain_history` requires
    `SET SESSION duckdb_unload_history_confirm = ON` (FR 20).
15. DDL that unloads a table (DROP TABLE, `SECONDARY_ENGINE = NULL`): a
    classic copy is dropped; a `retain_history` copy is kept and marked stale,
    with a warning. Other DDL (ALTER, TRUNCATE, RENAME TABLE, DROP PARTITION)
    is allowed (`HTON_SECONDARY_ENGINE_SUPPORTS_DDL`) and followed by the
    binlog applier (DWL#4 step 4d).
16. Reconciliation is lazy: a table the dictionary marks as loaded but that is
    missing from the lake, or is stale, fails `open()` in the secondary engine,
    so queries run on InnoDB (LIVE) and FULL scope errors (DWL#7).
17. (3b) SECONDARY_LOAD on a `retain_history` table that has its own copy:
    a stale copy is reattached, a loaded one is resynced (after a binlog gap,
    for example). Archived rows are kept, live rows are replaced with the
    InnoDB rows. A resync waits until the applier has committed every change
    before the load position (the archived rows come from the old copy); it
    warns that purges in a binary log range the applier did not read cannot
    be recovered.
18. (3b) `adopt_history_from`: a table without a copy takes over the stale
    history copy of another table (which no longer exists, or no longer has
    the secondary engine). The source must exist, have retain_history and be
    stale; a copy still used by its table is refused (RENAME TABLE is the way
    then). The attribute applies only while the table has no copy of its own.
19. (3b) Structure check: adopt, reattach and resync require the same columns
    by name, the same DuckDB type for each, and the same primary key columns
    (recorded at load in `pk_columns`; for copies loaded before 3b the
    applier records it at their first change, and a copy without it is
    refused). Every difference is a warning; the load fails with "the
    structure does not match ... nothing loaded". With
    `SET SESSION duckdb_history_check_only = ON`, SECONDARY_LOAD only runs the
    checks and always ends with an error giving the result (a success would
    make the server record the table as loaded).
20. (3b) Discarding history (SECONDARY_UNLOAD of a history table, or
    `SET GLOBAL duckdb_discard_stale_copy = 'db.table'` for a stale copy whose
    table no longer exists) requires `SET SESSION
    duckdb_unload_history_confirm = ON`. Lake data is never deleted
    implicitly. The discard is refused for a copy that is not stale or whose
    table exists.
21. Loaded tables survive a server restart: after restart they are loaded
    without a new SECONDARY_LOAD.
22. With `duckdb_enable=FALSE`, SECONDARY_LOAD gives the same warning as when the
    engine is not available and leaves the dictionary unchanged, and
    SECONDARY_UNLOAD never deletes lake data.

### Non-Functional Requirements

1. Load throughput is bounded by the InnoDB scan, not by DuckDB: rows are written
   through the DuckDB Appender, never row by row INSERT.
2. Memory used by the load is bounded by DuckDB's `memory_limit`, independent of
   table size.
3. `duckdb_enable=FALSE` waits for running loads to finish before DuckDB closes.

## Dependent Tasks

- DWL#2: Plugin infrastructure, configuration and DuckLake storage.

## High Level Architecture

### Contents

1. Load flow
2. Type mapping
3. Table registry
4. Unload and DDL
5. Reconciliation

### Load flow

The server opens and locks the table with `open_and_lock_tables()`, holding
`MDL_SHARED_NO_WRITE`, at READ COMMITTED (sql/sql_table.cc), and fills the read
set with the columns to load. Writes are blocked for the whole load and a plain
scan is consistent.

```
load_table(TABLE &t)
 ├─ preconditions (PK, FK, binlog settings, attributes, not already in the lake)
 ├─ column types from the read set
 ├─ read executed GTID set
 ├─ BEGIN; CREATE SCHEMA IF NOT EXISTS lake.<db>; CREATE TABLE lake.<db>.<t>
 ├─ appender on lake.<db>.<t> (it writes inside the open transaction)
 ├─ scan primary handler: rnd_init / rnd_next → convert row → append
 ├─ close appender; check count(*) against rows read
 ├─ registry row with GTID set
 └─ COMMIT (one DuckLake snapshot)
```

### Type mapping

| MySQL | DuckDB |
|---|---|
| TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT | TINYINT, SMALLINT, INTEGER, INTEGER, BIGINT |
| same, UNSIGNED | UTINYINT, USMALLINT, UINTEGER, UINTEGER, UBIGINT |
| DECIMAL(p,s), p ≤ 38 | DECIMAL(p,s); p > 38 refused |
| FLOAT, DOUBLE | FLOAT, DOUBLE |
| CHAR, VARCHAR, TEXT types (any character set) | VARCHAR, UTF-8; CHAR without trailing spaces |
| BINARY, VARBINARY, BLOB types | BLOB |
| DATE | DATE (zero dates refused) |
| DATETIME(n) | TIMESTAMP (microseconds) |
| TIMESTAMP(n) | TIMESTAMP, value in UTC (time zone semantics: Phase 2) |
| YEAR | USMALLINT: 0 (0000) or the year (2026-11-16) |
| BIT(n) | UBIGINT: the value (2026-11-16) |
| TIME(n) | BIGINT: signed microseconds (2026-11-16) |
| ENUM, SET | VARCHAR value (UTF-8, weight column if needed) + hidden `__n_<col>`: ENUM index USMALLINT, SET bitmask UBIGINT (2026-11-16) |
| other types (JSON, spatial, VECTOR) | refused in the POC |

#### YEAR, BIT, TIME, ENUM, SET (done 2026-11-16)

Nothing structural prevented them: each layer (load type mapping in
`duckdb_types.cc`, binlog decoder in `duckdb_row_decoder.cc`, offload result
types and expressions in `duckdb_offload.cc`) lacked the cases. The binlog
values are documented and `binlog_row_metadata=FULL` (already required)
carries the ENUM/SET value lists (`Optional_metadata_fields::m_enum_str_value`
/ `m_set_str_value`). JSON, spatial and VECTOR stay refused.

| MySQL | DuckDB | Notes |
|---|---|---|
| YEAR | USMALLINT | 0 = 0000, else 1900 + byte; result holder displays as YEAR (`0000`) |
| BIT(n) | UBIGINT | binlog: big-endian bytes; result sent as the ceil(n/8)-byte binary string |
| TIME(n) | BIGINT, signed microseconds | DuckDB TIME covers 00:00-24:00 only, MySQL -838:59:59..838:59:59; constants converted at SQL generation; SUM/AVG of TIME refused (MySQL sums HHMMSS numbers) |
| ENUM | VARCHAR value + hidden `__n_<col>` USMALLINT index | string contexts (=, <, LIKE, MIN/MAX: MySQL compares the string with the column collation, weights for _ci) use the value; ORDER BY, `e = 2`, `e + 0` use the index (chosen from MySQL's own comparator type); index 0 = invalid value `''` |
| SET | VARCHAR value + hidden `__n_<col>` UBIGINT bitmask | as ENUM |

DuckDB's own ENUM type was considered for space and rejected: DuckLake
refuses it ("Failed to convert DuckDB type to DuckLake - unsupported type
ENUM"), and on disk it would not save anything: Parquet dictionary-encodes
low-cardinality strings (measured, 1M rows, 3 values: VARCHAR 256,675 bytes,
UTINYINT 256,219 bytes). The hidden index column costs about the same again
for such a column. The structure check of history copies ignores `__n_`
columns as it does `__w_`.

Implemented in that order (lowest risk first) on 2026-11-16; offload rules
in DWL-06 ("YEAR and BIT", "TIME", "ENUM and SET").

- Load (`duckdb_types.cc`): YEAR `Field_year::val_int`, BIT `Field_bit::val_int`,
  TIME `Field_time::val_time` → `Time_val::to_microseconds`, ENUM / SET the
  string (`val_str`, converted to UTF-8, weighted like VARCHAR) and
  `val_int` into `__n_<col>` (after the weight columns).
- Binlog decoder: YEAR one byte; BIT big-endian bytes (metadata: bits % 8,
  bytes); TIME2 `Time_val::load_time` (as `log_event.cc`); ENUM / SET the
  index or bitmask little-endian (1-2 / 1-8 bytes), the value rebuilt from
  the Table_map element list in the column charset (`m_enum_and_set_*`
  charsets), then converted and weighted like a string; the applier writes
  the number into `__n_<col>`.
- History copies rebuilt after DDL (or reattached, resynced, adopted):
  archived ENUM / SET rows get their index recomputed from their value with
  the current definition (a value no longer in it: index 0, the text is
  kept); archived SET text is written again in the current element order
  ('x,z' becomes 'z,x' after SET('z','y','x')), so text and bitmask agree.

### Table registry

`lake.__mysql.tables` keeps one row per table in the lake: `db`, `tbl`,
`retain_history`, `state` (loaded, stale, refresh_pending), `load_gtid_set`,
`loaded_at`, `load_binlog_file` / `load_binlog_pos`, `pending_renames`,
`pk_columns` (3b). It is
the source of truth for "is loaded" and is cached in memory (with the table's
THR_LOCK) for the handler, the applier and the guard.

### Unload and DDL

| Operation | Classic table | `retain_history` table |
|---|---|---|
| SECONDARY_UNLOAD | drop copy | only with `duckdb_unload_history_confirm=ON` |
| DROP TABLE, `SECONDARY_ENGINE = NULL` | drop copy | keep copy, mark stale, warning |
| ALTER, TRUNCATE, RENAME TABLE, DROP PARTITION | copy refreshed or renamed by the applier (DWL#4 4d) | same, archived rows kept |
| SECONDARY_LOAD when already in the lake | refused: unload first | stale: reattach; loaded: resync (same structure) |
| SECONDARY_LOAD with `adopt_history_from` | n/a | takes over a stale copy (same structure) |
| `duckdb_discard_stale_copy` | drops a stale copy whose table is gone | same, with confirmation |

### Load: two DuckLake transactions (3b)

T1 builds the copy as `<table>__duckdb_build` (create, append, count check)
and commits, with no registry write. T2, under `apply_mutex`: for a replacing
load the archived rows of the old copy (followed by column renames for a
refresh after DDL), DROP of the old copy (of the adopted one for
`adopt_history_from`), RENAME of the build table, the registry row (with
`pk_columns`), COMMIT. Every registry writer (unload, stale marking, renames,
the applier's batch) holds `apply_mutex` for its own short transaction, so T2
never overlaps one; a single long transaction did, and failed at its commit
("another transaction has deleted inlined data"). A failure in T2 drops the
build table. A crash between T1 and T2 leaves only the build table: the lake
drops every `__duckdb_build` table when it opens (the registry never refers
to one).

The server passes `error_if_not_loaded=true` only for SECONDARY_UNLOAD
(sql/sql_table.cc:12903); all DDL paths pass false (DROP :3004, ALTER :14660,
partitions :18511, temporary tables :1931). We use it to tell them apart.

### Reconciliation

Lazy (FR 16). No scan of the whole dictionary at startup.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/ha_duckdb.cc` | `open()` uses the registry; `load_table()`, `unload_table()` call `duckdb_load` |
| `storage/duckdb/duckdb_load.h/.cc` | Preconditions, scan and append, unload |
| `storage/duckdb/duckdb_types.h/.cc` | Type mapping, value conversion, identifier and literal quoting (shared with DWL#4 and DWL#6) |
| `storage/duckdb/duckdb_registry.h/.cc` | Registry table and in-memory cache |
| `storage/duckdb/duckdb_lake.h/.cc` | Per-operation `Lake::Connection`; `close()` waits for open connections; registry table, `enable_external_access=false` and registry reload when the lake opens |

### Preconditions

- Primary key: `table.s->primary_key != MAX_KEY`.
- Foreign keys: `table.s->foreign_keys` and `table.s->foreign_key_parents`.
- Binlog settings: `opt_bin_log`, `global_system_variables.binlog_format` and
  `.binlog_row_image`, `binlog_row_metadata`, `global_gtid_mode`.
- Attributes: `table.s->secondary_engine_attribute` parsed with rapidjson.

### Scan and values

`table.file` is the primary (InnoDB) handler. We call `ha_rnd_init(true)` and
`ha_rnd_next(table.record[0])` until `HA_ERR_END_OF_FILE`. Values: integers with
`val_int()` (the appender casts to the column type), FLOAT and DOUBLE with
`val_real()`, DECIMAL as text (DuckDB casts to the exact DECIMAL), strings with
`val_str()` and `String::copy()` to `utf8mb4` when needed, DATE with
`val_date()`, DATETIME with `val_datetime()`, TIMESTAMP with `get_timestamp()`
(UTC microseconds).

### GTID set at load

`gtid_state->get_executed_gtids()->to_string()` under `global_tsid_lock`, read
before the scan. Since writes on the table are blocked during the load, every
transaction that touched the table before the load is in the set. To verify:
ordering between binlog group commit and engine commit for transactions in
flight at load start.

### Open points

- DECIMAL loaded through text: to verify with the MTR type test (the `mill`
  tables have no DECIMAL column).
- `enable_external_access=false`: verified by loading `mill1` (DuckLake must
  still write Parquet files to `data/`).
- File cleanup after unload and discard: Phase 3 (snapshot expiry).
- MTR suite: postponed.
- A resync waits at most 60 s for the applier; on a timeout it fails ("try
  again").

## Test Plan

Manual commands for step 3a: `manual-test-plan.md`, sections 5 to 9.

MTR suite `duckdb` (3b), using include files that create the `mill` table
definition and fill it with deterministic data:

1. `load_basic`: load, check the log line and row count, unload, load again.
2. `load_preconditions`: no PK, FK, unsupported type, each binlog setting wrong,
   invalid attributes.
3. `load_types`: every supported type with boundary values and NULLs; latin1
   and binary columns; zero dates refused.
4. `load_not_secondary`: `NOT SECONDARY` columns absent in DuckDB.
5. `load_twice`: classic table refused until unloaded; history table refused.
6. `unload_history`: refused without confirmation; allowed with it (3b).
7. `ddl_history`: DROP TABLE and `SECONDARY_ENGINE = NULL` on a history table
   keep the copy and mark it stale; RENAME and ALTER refused by the server.
8. `reattach_history`: drop, recreate and load reattaches the stale copy (3b).
9. `rename_history`: rename through `adopt_history_from` (3b).
10. `restart_loaded`: table still loaded after restart.
11. `enable_off_during_load`: `duckdb_enable=OFF` waits for a running load.
