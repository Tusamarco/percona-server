# MTR suite `duckdb`

Copyright (c) 2026-present, Marco Tusa. GPL v2.

Tests of the DuckDB secondary engine (`storage/duckdb`). The suite lives in
the plugin directory (no files in the server's `mysql-test/`); MTR finds it
by path.

## Run

From the build directory (debug or release):

```bash
cd <build>/mysql-test
MTR_SHUTDOWN_TIMEOUT=120 ./mtr --suite=<source>/storage/duckdb/mysql-test/duckdb --parallel=2
./mtr --suite=<source>/storage/duckdb/mysql-test/duckdb guard history     # some tests
```

`t/suite.opt` loads `ha_duckdb.so` from `<build>/plugin_output_directory`
(`$MTR_BINDIR`, set by the build's `mtr` wrapper); `my.cnf` sets the binlog
options the engine needs and a small DuckDB (512M, 2 threads). A debug build
needs about 1.5 hours for the suite with `--parallel=2` (an HDD machine),
mostly loads and DuckDB commits. On a busy machine a test can exceed MTR's
default 15-minute limit: add `--testcase-timeout=45`.

## Layout

| File | Content |
|---|---|
| `my.cnf`, `t/suite.opt` | server options, plugin |
| `include/have_duckdb.inc` | skips without the plugin; suppressions for the plugin's expected warnings |
| `include/wait_applier.inc` | waits until the applier is idle at the binary log end (a debug build's DuckDB commit can exceed the freshness wait) |
| `include/duckdb_compare.inc` | `$query` with OFF and FORCED: results compared (`--diff_files`), the FORCED run must have run in DuckDB |
| `include/duckdb_refused.inc` | `$query` refused with FORCED (reason recorded), ON gives the InnoDB rows |
| `include/tables_*.inc` | test tables (as `project_duckdb/offload_queries/duckdb_offload_setup.sql`) |
| `include/cleanup.inc` | drops the test tables, discards stale history copies |
| `include/duckdb_plan_shape.inc` | the shape of the DuckDB statement for `$query` (EXPLAIN notes): cross product, DISTINCT derived table, NOT EXISTS, LATERAL |

## Tests

| Test | Covers (manual-test-plan.md) |
|---|---|
| `duckdb_plugin` | plugins, engine, variables, load / unload, refused load (sections 1-10) |
| `applier` | DML of every type, large transaction in pieces, DDL refresh, RENAME, TRUNCATE (12-17) |
| `history` | purge with the tag, LIVE / FULL, unload confirmation, stale copy, structure check, check only, reattach (19, 20) |
| `guard` | FULL scope guard, UPDATE / DELETE / TRUNCATE under FULL stopped (34) |
| `i_s_tables` | INFORMATION_SCHEMA.DUCKDB_TABLES: states, RENAME, stale history copy, unload, PROCESS privilege, lake closed (35) |
| `super_read_only` | refresh after DDL with `super_read_only=ON` (a replica), user statements refused (18) |
| `cached_tables` | cached secondary tables across unload / reload, duckdb_enable, DROP (27) |
| `decimal_overflow` | DECIMAL results declared above 38 digits: run first, FALLBACK / ROUND / ERROR, ON and FORCED |
| `foreign_keys` | tables with foreign keys: ON DELETE / ON UPDATE CASCADE, SET NULL, RESTRICT; tagged purge archives cascaded history rows |
| `transactions` | autocommit=0 and BEGIN: no secondary engine, FORCED gives the reason (was a debug assertion) |
| `offload_rules` | cost threshold, duckdb_offload_min_rows, read your own writes, transaction that wrote, EXPLAIN notes, max_execution_time (21, 27, 28, 30) |
| `offload_6a`, `offload_6b`, `offload_6c1`, `offload_6c2`, `offload_6c3`, `offload_division`, `offload_weights` | the query lists `project_duckdb/offload_queries/duckdb_offload_*_queries.sql`, InnoDB vs DuckDB |
| `offload_time` | TIME: constants in every form, negative and > 24 h values, fractions, refusals (37) |
| `offload_enum` | ENUM and SET: value and index contexts, invalid value, history copy across a definition change, refusals (38) |
| `offload_like` | LIKE on binary collations (latin1_bin, utf8mb4_bin): trailing spaces, case, accents, escapes, NO_BACKSLASH_ESCAPES; LIKE on _ci refused (40) |
| `offload_grouped_char` | grouped CHAR under binary collations shown in a join (exact); VARCHAR and _ci refused; PAD_CHAR_TO_FULL_LENGTH refused (41) |
| `offload_functions` | functions reviewed against DuckDB: date, number and string functions with edge values; the refused ones (42) |
| `offload_yb` | YEAR and BIT: load, applier, results (YEAR columns zero-filled, BIT as bytes), unsigned arithmetic, refusals (36) |
| `offload_refusals` | the refusal lists `project_duckdb/offload_queries/duckdb_offload_*_refusals.sql` |
| `offload_counters` | SUM / AVG of FLOAT and DOUBLE (`duckdb_offload_float_aggregates`), FLOAT sent as FLOAT, the offload counters (runs, EXPLAIN, waits), `duckdb_offload_wait_timeout_ms`, the user's `optimizer_switch` kept around offload |

| `binlog_safety` | writes the binary log would not carry in full refused on tables with a copy (sql_log_bin = 0, binlog_format STATEMENT / MIXED, binlog_row_image MINIMAL / NOBLOB); with the guard skipped (debug point) the applier marks the copy stale; binlog_order_commits switched OFF stops the applier (2026-11-26) |
| `binlog_reset` | RESET BINARY LOGS AND GTIDS while copies are loaded; repoint to FIRST |
| `ddl_wait` | T0: the statement after a DDL waits for the copy refresh, with progress; timeout and KILL |
| `json_geometry` | JSON and spatial columns: load, applier (binary JSON as text), select / IS NULL / COUNT offloaded, functions refused |
| `offload_params` | T1: prepared statement parameters, stored program and user variables |
| `offload_exec_errors` | T2: a DuckDB error at the first chunk falls back to InnoDB (debug point) |
| `offload_now`, `offload_cast`, `offload_date_format` | T3: NOW() family, CAST / CONVERT, DATE_FORMAT |
| `offload_join_distinct`, `offload_group_expr` | grouped strings in joins and grouped string expressions, checked per group |
| `offload_exists_pair` | EXISTS and NOT EXISTS on the same column (TPC-H Q21), NOT IN; the DuckDB plan checked (no cross product, anti join) |
| `offload_lateral` | correlated scalar / ALL / ANY subqueries as LATERAL; uncorrelated IN with GROUP BY left as it is |
| `offload_char_or` | CHAR compared without rtrim (load under PAD_CHAR_TO_FULL_LENGTH), constant SUBSTRING, EXISTS / IN / NOT EXISTS inside OR |
| `offload_timestamp_fn` | functions, DATE_FORMAT, arithmetic and CAST on TIMESTAMP columns under UTC and fixed-offset session zones; a zone with daylight saving refused (2026-11-26) |
| `offload_edge_strings` | D1: edge strings (combining marks, emoji modifiers, ß, Turkish i, trailing spaces vs tabs) under 8 collations: equality, ordering, grouping, distinct, MIN / MAX, joins |
| `generated_columns` | VIRTUAL and STORED generated columns, expression defaults: load, applier, queries, DDL refresh |
| `client_charsets` | results to clients with latin1, utf8mb3, cp1251 and no conversion |
| `query_slots` | `duckdb_max_concurrent_queries = 1`: a query held in its slot (debug sync), a second waits (processlist state) and KILL QUERY ends it with `use_secondary_engine = ON` (no InnoDB answer), a third runs when the slot frees; counters and stage (2026-11-27) |

Not in MTR (manual-test-plan.md): restarts with special options and long
runs: startup failures (11), binlog gaps (15), replica with a source (18), the applier
waiting for the InnoDB commit with semi-sync (24), lake relocation (31),
performance (29).

## Adding a test

- Every test starts with `--source ../include/have_duckdb.inc`, ends with
  `--source ../include/cleanup.inc`, and calls `wait_applier.inc` after
  loads and DML that a FORCED query then reads.
- A query whose InnoDB and DuckDB results must be the same:
  `--let $query= SELECT ...` then `--source ../include/duckdb_compare.inc`
  (one line, no trailing `;`). Results are not recorded, only the query.
- Record with `--record`, after checking the results with
  `project_duckdb/duckdb_offload_compare.sh`.
- The comparison tests were generated from the query lists; a query added to
  a list goes into the matching `offload_*.test` as a `--let` / `--source`
  pair.
