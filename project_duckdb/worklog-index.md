# DuckDB secondary engine: worklogs

Worklogs for the DuckDB secondary engine POC, one per function, in the style of
MySQL worklogs (Description, Requirements, Dependent Tasks, High Level
Architecture, Low Level Design), plus a Test Plan. The overall design is in
`duckdb-secondary-engine-design.md` (v0.3); each worklog refers to it instead of
repeating it, and the design document wins in case of conflict.

Numbering uses the local prefix DWL (DuckDB worklog) to avoid confusion with
MySQL WL numbers.

| ID | Title | Status | Depends on |
|---|---|---|---|
| [DWL#1](DWL-01-build-integration.md) | Build integration of DuckDB, DuckLake and CRoaring | Implemented, verified | none |
| [DWL#2](DWL-02-plugin-infrastructure.md) | Plugin infrastructure, configuration and DuckLake storage | Implemented (2a, executor pool), under test | DWL#1 |
| [DWL#3](DWL-03-secondary-load-unload.md) | Secondary tables: SECONDARY_LOAD and SECONDARY_UNLOAD | 3a implemented, under test; 3b (reattach, resync, adopt_history_from, structure check, confirmed discard, two-transaction load) implemented, verified on a throwaway instance; MTR postponed | DWL#2 |
| [DWL#4](DWL-04-binlog-applier.md) | Change propagation: binlog applier | 4a verified; 4b implemented, under test; 4c-1 implemented, under test (gaps and repoint verified); 4c-2 implemented, under test; 4d (DDL refresh, compressed payloads) implemented, verified on a throwaway instance; MTR postponed | DWL#3 |
| [DWL#5](DWL-05-history-retention.md) | History retention: purge marker and archive marks | Implemented (fixed purge tag, counters, `duckdb_history_scope` and `DUCKDB_HISTORY_READER`), verified on a throwaway instance; MTR postponed | DWL#4 |
| [DWL#6](DWL-06-query-offload.md) | Query offload through the external executor | 6a (single table, external executor, read your own writes) and 6b (expressions, PAD SPACE collations, TIMESTAMP constants) verified on the reference instance; 6c-1 (joins, semi/anti joins, derived tables, views, CTEs) and 6c-2 (UNION, INTERSECT, EXCEPT) verified on the reference instance; 6c-3 (subqueries) implemented, verified on a throwaway instance; types, DECIMAL overflow, LIKE, grouped CHAR, function review implemented, MTR tests | DWL#3, DWL#4, DWL#5 |
| [DWL#7](DWL-07-full-scope-guard.md) | FULL scope guard plugin | Implemented (duckdb_guard audit plugin), verified on a throwaway instance | DWL#6 |

Done 2026-11-15/16: replica test (test plan section 18, with the guard and
super_read_only fixes), `INFORMATION_SCHEMA.DUCKDB_TABLES` (the registry as a
table: copies with their state, history flag, primary key, weight columns,
load position; section 35), the types YEAR, BIT, TIME, ENUM and SET
(DWL-03, DWL-06; sections 36-38).

Done 2026-11-17: tutorial (`TUTORIAL.md`), foreign keys allowed, DECIMAL
beyond 38 digits (`duckdb_decimal_overflow`), LIKE on binary collations,
grouped CHAR in joins, function review (dates, numbers, strings; DWL-06),
TPC-H check 19/22 (sections 39-42, `tpch/README.md`). Next (design doc
plan): data check for grouped strings (Q16, Q22), Q21, HammerDB run on the
release build, the 9.7 / 8.4 decision, production hardening (crash and
consistency tests, compaction, lag, resource governance, backup), then
translations by real usage (design doc "Production readiness" and "SQL
gaps by priority").

Done 2026-11-18: review fixes (REVERSE by code points, FLOAT sent as FLOAT,
literals over 38 digits refused, DECIMAL overflow by error type),
`duckdb_offload_float_aggregates` (default APPROXIMATE),
`duckdb_offload_wait_timeout_ms`, offload counters (MTR `offload_counters`,
26 tests). **TODO from now**: design doc "TODO from 2026-11-18: limits that
stop a result set" (T1 prepared statements and variables, T2 DuckDB errors
during execution, T3 FULL scope survival, T4 loadability, T5 concurrency,
leftovers B1, D1, E1).

Development order follows the table. DWL#6 must not be enabled before DWL#4:
without the applier, DML on InnoDB does not reach DuckDB and offloaded queries
would return stale data. Its translation code can be developed earlier, with
offload kept off.

How to build, configure and use what is implemented: `HOWTO.md` (reference)
and `TUTORIAL.md` (step by step guide).

What offloaded queries can contain, against the SQL standard: the **SQL
coverage tracker** in `duckdb-secondary-engine-design.md` (update it with
every step that changes coverage). TPC-H / HammerDB readiness and its plan:
`tpch/README.md` (19 of 22 queries offloaded on 2026-11-17).

Manual test commands for what is implemented: `manual-test-plan.md` (and the MTR suite
`storage/duckdb/mysql-test/duckdb`).

Outside the POC, to be written later: encryption, DROP PARTITION as purge,
backup integration, compaction and snapshot expiry, `orders_history` name.
(The freshness wait, `duckdb_offload_wait_applied`, and DDL on history
tables, by refresh with the archived rows kept, were added to the POC.)

Conventions for all worklogs:

- No core server code changes: all code lives in `storage/duckdb/`,
  `extra/duckdb/` and `cmake/duckdb.cmake`.
- Every behavior change gets a test in the MTR suite
  `storage/duckdb/mysql-test/duckdb` (since 2026-11-15), or in
  `manual-test-plan.md` when it needs restarts, a replica or long runs.
- Items marked "to verify" must be confirmed in the 9.7 source or by a test
  before the code relies on them. Items marked "proposal" need Marco's decision.
