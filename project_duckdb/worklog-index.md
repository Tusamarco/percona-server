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
| [DWL#6](DWL-06-query-offload.md) | Query offload through the external executor | 6a (single table, external executor, read your own writes) and 6b (expressions, PAD SPACE collations, TIMESTAMP constants) implemented, verified on a throwaway instance and on the reference instance (2026-11-13); 6c-6d design | DWL#3, DWL#4, DWL#5 |
| [DWL#7](DWL-07-full-scope-guard.md) | FULL scope guard plugin | Design, under review | DWL#6 |

Development order follows the table. DWL#6 must not be enabled before DWL#4:
without the applier, DML on InnoDB does not reach DuckDB and offloaded queries
would return stale data. Its translation code can be developed earlier, with
offload kept off.

How to build, configure and use what is implemented: `HOWTO.md`.

Manual test commands for what is implemented: `manual-test-plan.md` (until the
MTR suite exists; postponed).

Outside the POC, to be written later: encryption, DROP PARTITION as purge,
backup integration, compaction and snapshot expiry, `orders_history` name.
(The freshness wait, `duckdb_offload_wait_applied`, and DDL on history
tables, by refresh with the archived rows kept, were added to the POC.)

Conventions for all worklogs:

- No core server code changes: all code lives in `storage/duckdb/`,
  `extra/duckdb/` and `cmake/duckdb.cmake`.
- Every behavior change gets a test: for now a manual one in
  `manual-test-plan.md` (MTR suite `duckdb` postponed, 2026-11-11).
- Items marked "to verify" must be confirmed in the 9.7 source or by a test
  before the code relies on them. Items marked "proposal" need Marco's decision.
