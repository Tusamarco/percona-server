# DWL#3: Secondary tables: SECONDARY_LOAD and SECONDARY_UNLOAD

Affects: Percona Server 9.7 — Status: Design, under review

## Description

### GENERAL IDEA

A user declares DuckDB as the secondary engine of an InnoDB table and loads its
current data into DuckLake. We implement `load_table()` and `unload_table()` of
the handler, the preconditions checked at load, the type mapping, the registry
of loaded tables, and the reconciliation with the data dictionary at startup.

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
classic secondary engine copy.

### MAPPING

A MySQL table `db.t` becomes the DuckLake table `lake.db.t` (schema created on
first use), with the same column names and mapped types. Tables with
`retain_history` get an extra column `__archived_at TIMESTAMP`, NULL for live
rows.

## Requirements

### Functional Requirements

1. `ALTER TABLE ... SECONDARY_LOAD` copies all rows of the InnoDB table into
   `lake.<db>.<table>` in one DuckLake transaction.
2. The load is refused with an error if the table has no primary key, has
   foreign keys (referencing or referenced), or has a column of an unsupported
   type; the error names the reason or the column.
3. The load is refused if `log_bin`, `binlog_format=ROW`,
   `binlog_row_image=FULL`, `gtid_mode=ON` or `binlog_row_metadata=FULL` does
   not hold; the error names the setting.
4. Supported types in the POC: TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT (signed
   and unsigned), DECIMAL, FLOAT, DOUBLE, CHAR, VARCHAR, TEXT types, DATE,
   DATETIME, TIMESTAMP.
5. Columns marked `NOT SECONDARY` are not loaded.
6. The load records the GTID set executed at load time with the table, so the
   applier (DWL#4) skips events already contained in the load.
7. For a table with `retain_history`, SECONDARY_LOAD is refused if the DuckLake
   table already contains rows.
8. `SECONDARY_UNLOAD` on a table without `retain_history` drops the DuckLake
   table and removes its files.
9. `SECONDARY_UNLOAD` on a table with `retain_history` is refused unless the
   session confirms it (proposal: session variable
   `duckdb_unload_history_confirm=ON`, to be decided).
10. When the server unloads a table because of DDL, a `retain_history` table is
    not dropped in DuckLake: it is marked stale and the warning says so.
11. At startup, tables marked loaded in the data dictionary but missing from the
    lake are treated as unloaded, with a warning in the error log.
12. Loaded tables survive a server restart: after restart they are loaded
    without a new SECONDARY_LOAD.
13. With `duckdb_enable=FALSE`, SECONDARY_LOAD gives the same warning as when the
    engine is not available, and SECONDARY_UNLOAD never deletes lake data.

### Non-Functional Requirements

1. Load throughput is bounded by the InnoDB scan, not by DuckDB: rows are written
   through the DuckDB Appender, never row by row INSERT.
2. Memory used by the load is bounded by DuckDB's `memory_limit`, independent of
   table size.

## Dependent Tasks

- DWL#2: Plugin infrastructure, configuration and DuckLake storage.

## High Level Architecture

### Contents

1. Load flow
2. Type mapping
3. Table registry
4. Unload flow
5. Reconciliation

### Load flow

The server calls `ha_load_table()` on a new secondary handler holding
`MDL_SHARED_NO_WRITE` on the table (sql/sql_table.cc), so writes are blocked for
the whole load and a plain scan is consistent.

```
load_table(TABLE &t)
 ├─ preconditions (PK, FK, types, binlog settings, retain_history empty)
 ├─ read executed GTID set
 ├─ CREATE SCHEMA IF NOT EXISTS lake.<db>; CREATE TABLE lake.<db>.<t> (...)
 ├─ appender on lake.<db>.<t>
 ├─ scan primary handler: rnd_init / rnd_next → convert row → append
 ├─ close appender, insert registry row with GTID set (same transaction)
 └─ COMMIT
```

### Type mapping

| MySQL | DuckDB |
|---|---|
| TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT | TINYINT, SMALLINT, INTEGER, INTEGER, BIGINT |
| same, UNSIGNED | UTINYINT, USMALLINT, UINTEGER, UINTEGER, UBIGINT |
| DECIMAL(p,s), p ≤ 38 | DECIMAL(p,s); p > 38 refused |
| FLOAT, DOUBLE | FLOAT, DOUBLE |
| CHAR, VARCHAR, TEXT types | VARCHAR (collation semantics: Phase 2) |
| DATE | DATE |
| DATETIME(n) | TIMESTAMP (microseconds) |
| TIMESTAMP(n) | TIMESTAMP, value in UTC (time zone semantics: Phase 2) |

### Table registry

`lake.__mysql.tables` keeps one row per loaded table: db, table, DD table id,
`retain_history`, load GTID set, state (loaded, stale). It is the source of truth
for "is loaded" and is cached in memory for the hooks and the guard.

### Unload flow

`unload_table(db, table, error_if_not_loaded)`: if not registered, error or no-op
as requested. Without `retain_history`: `DROP TABLE`, registry row deleted, file
cleanup. With `retain_history`: see Functional Requirements 9 and 10.

### Reconciliation

At startup, after the attach, the registry is compared with the dictionary:
tables loaded in the dictionary and missing from the registry are reported and
treated as unloaded; registry rows for dropped MySQL tables are marked stale and
reported, never dropped automatically.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/ha_duckdb.cc` | `load_table()`, `unload_table()`, `open()` uses the registry |
| `storage/duckdb/duckdb_load.h/.cc` | Preconditions, scan and append |
| `storage/duckdb/duckdb_types.h/.cc` | Type mapping and value conversion (shared with DWL#4 and DWL#6) |
| `storage/duckdb/duckdb_registry.h/.cc` | Registry table and in-memory cache |

### Preconditions

- Primary key: `table.s->primary_key != MAX_KEY`.
- Foreign keys: from the dictionary table object (`dd::Table::foreign_keys()`
  and referencing keys); to verify the cheapest access from the handler.
- Binlog settings: read the server variables at load time.
- `retain_history`: parse `table.s->secondary_engine_attribute` as JSON
  (rapidjson, already linked by the MOCK engine).

### Scan

`table.file` is the primary (InnoDB) handler. We call `ha_rnd_init(true)` and
`ha_rnd_next(table.record[0])` until `HA_ERR_END_OF_FILE`, converting each field
of the read set with `duckdb_types` into appender calls (`duckdb_append_*`).
Errors abort the DuckDB transaction and return `HA_ERR_GENERIC` with the message.

### GTID set at load

The executed GTID set read at load start is stored as text in the registry. Since
writes on the table are blocked during the load, every transaction that touched
the table before the load is in the set. To verify: ordering between binlog
group commit and engine commit for transactions in flight at load start.

### Server-initiated unload

To verify which DDL paths call `unload_table()` (for example
sql/sql_table.cc around `secondary_engine_unload_table_inner`). For those paths a
`retain_history` table is marked stale instead of dropped.

### Open points

- Confirmation mechanism for unloading history tables: proposal above.
- Primary key types with collation (string keys): semantic equivalence in Phase 2.

## Test Plan

MTR suite `duckdb`:

1. `load_basic`: load, check row count and checksum in DuckDB through a debug
   query path, unload.
2. `load_preconditions`: no PK, FK, unsupported type, each binlog setting wrong.
3. `load_types`: every supported type with boundary values and NULLs.
4. `load_not_secondary`: `NOT SECONDARY` columns absent in DuckDB.
5. `load_history_once`: second load of a non-empty retain_history table refused.
6. `unload_history`: refused without confirmation; allowed with it.
7. `ddl_unload`: DDL on a loaded table; retain_history table marked stale, not
   dropped.
8. `restart_loaded`: table still loaded after restart.
9. `reconcile`: catalog removed between restarts; tables reported unloaded.
