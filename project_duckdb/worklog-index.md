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
| [DWL#1](DWL-01-build-integration.md) | Build integration of DuckDB, DuckLake and CRoaring | Design, under review | none |
| [DWL#2](DWL-02-plugin-infrastructure.md) | Plugin infrastructure, configuration and DuckLake storage | Design, under review | DWL#1 |
| [DWL#3](DWL-03-secondary-load-unload.md) | Secondary tables: SECONDARY_LOAD and SECONDARY_UNLOAD | Design, under review | DWL#2 |
| [DWL#4](DWL-04-binlog-applier.md) | Change propagation: binlog applier | Design, under review | DWL#3 |
| [DWL#5](DWL-05-history-retention.md) | History retention: purge marker and archive marks | Design, under review | DWL#4 |
| [DWL#6](DWL-06-query-offload.md) | Query offload through the external executor | Design, under review | DWL#3, DWL#5 |
| [DWL#7](DWL-07-full-scope-guard.md) | FULL scope guard plugin | Design, under review | DWL#6 |

Development order follows the table. DWL#6 can start in parallel with DWL#4 and
DWL#5 for LIVE scope on static data; the history scope predicate is added when
DWL#5 is done.

Outside the POC, to be written later: encryption, DROP PARTITION as purge,
in-place DDL for history tables, backup integration, compaction and snapshot
expiry, freshness wait policy, `orders_history` name.

Conventions for all worklogs:

- No core server code changes: all code lives in `storage/duckdb/`,
  `extra/duckdb/` and `cmake/duckdb.cmake`.
- Every behavior change gets an MTR test in the `duckdb` suite.
- Items marked "to verify" must be confirmed in the 9.7 source or by a test
  before the code relies on them. Items marked "proposal" need Marco's decision.
