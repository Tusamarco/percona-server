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
mostly loads and DuckLake commits. On a busy machine a test can exceed MTR's
default 15-minute limit: add `--testcase-timeout=45`.

## Layout

| File | Content |
|---|---|
| `my.cnf`, `t/suite.opt` | server options, plugin |
| `include/have_duckdb.inc` | skips without the plugin; suppressions for the plugin's expected warnings |
| `include/wait_applier.inc` | waits until the applier is idle at the binary log end (a debug build's DuckLake commit can exceed the freshness wait) |
| `include/duckdb_compare.inc` | `$query` with OFF and FORCED: results compared (`--diff_files`), the FORCED run must have run in DuckDB |
| `include/duckdb_refused.inc` | `$query` refused with FORCED (reason recorded), ON gives the InnoDB rows |
| `include/tables_*.inc` | test tables (as `project_duckdb/duckdb_offload_setup.sql`) |
| `include/cleanup.inc` | drops the test tables, discards stale history copies |

## Tests

| Test | Covers (manual-test-plan.md) |
|---|---|
| `duckdb_plugin` | plugins, engine, variables, load / unload, refused load (sections 1-10) |
| `applier` | DML of every type, large transaction in pieces, DDL refresh, RENAME, TRUNCATE (12-17) |
| `history` | purge with the tag, LIVE / FULL, unload confirmation, stale copy, structure check, check only, reattach (19, 20) |
| `guard` | FULL scope guard, UPDATE / DELETE / TRUNCATE under FULL stopped (34) |
| `super_read_only` | refresh after DDL with `super_read_only=ON` (a replica), user statements refused (18) |
| `cached_tables` | cached secondary tables across unload / reload, duckdb_enable, DROP (27) |
| `offload_rules` | cost threshold, duckdb_offload_min_rows, read your own writes, transaction that wrote, EXPLAIN notes, max_execution_time (21, 27, 28, 30) |
| `offload_6a`, `offload_6b`, `offload_6c1`, `offload_6c2`, `offload_6c3`, `offload_division`, `offload_weights` | the query lists `project_duckdb/duckdb_offload_*_queries.sql`, InnoDB vs DuckDB |
| `offload_refusals` | the refusal lists `duckdb_offload_*_refusals.sql` |

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
