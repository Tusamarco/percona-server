# DWL#7: FULL scope guard plugin

Affects: Percona Server 9.7 — Status: Design, under review

## Description

### GENERAL IDEA

Under `duckdb_history_scope=FULL`, a query on a retain_history table must see
live and archived rows. Archived rows exist only in DuckDB, so a FULL query that
runs on InnoDB silently returns live rows only: a wrong answer. When
`use_secondary_engine=OFF`, or when the server decides not to try offload, the
engine hooks are never called and cannot prevent this.

The `duckdb_guard` audit plugin, in the same library as the engine, closes the
gap without core changes. On every table access read event it checks whether the
statement reads a history table under FULL scope and, if so, forces the
statement to run in the secondary engine or fail.

### SERVER MECHANISMS USED

- Table access READ events are raised in `open_tables()` (sql/sql_base.cc),
  before `open_secondary_engine_tables()` decides between primary and secondary
  execution, and before any row is read.
- A statement can be marked as executable only in the secondary engine with
  `lex->set_execute_only_in_secondary_engine(true, reason)` (sql/sql_lex.h), the
  mechanism used for CUBE, TABLESAMPLE and GROUPING SETS. With
  `use_secondary_engine=OFF` such a statement fails; otherwise it is treated as
  FORCED, so a failed offload is an error, never a fallback.

## Requirements

### Functional Requirements

1. A statement that reads a retain_history table under FULL scope (session value
   or SET_VAR) never runs on InnoDB.
2. With `use_secondary_engine=OFF`, such a statement fails with: `duckdb_history_scope=FULL
   requires DuckDB execution, but use_secondary_engine=OFF`.
3. If a history table in the statement is not loaded, or `duckdb_enable=FALSE`,
   the statement fails with an error saying so.
4. Otherwise the statement is marked secondary-only; if offload then fails, the
   error carries the engine's rejection reason (DWL#6).
5. Statements that write and also read a history table under FULL scope
   (INSERT ... SELECT, UPDATE or DELETE with subquery, CREATE ... SELECT) fail
   with an error (POC).
6. Statements under LIVE scope, and statements on tables without retain_history,
   are not affected.
7. The engine refuses `duckdb_history_scope=FULL` when the guard plugin is not
   active.
8. Statements inside stored routines are checked when each statement runs; views
   are checked through their underlying tables.
9. Replication applier threads are not affected.
10. A status counter reports guard rejections by reason.

### Non-Functional Requirements

1. The guard adds negligible overhead to statements: for tables that are not
   history tables it does one in-memory lookup.
2. The guard never changes the result of statements outside FULL scope.

## Dependent Tasks

- DWL#6: Query offload through the external executor.

## High Level Architecture

### Contents

1. Plugin declaration
2. Event handling
3. Decision
4. Interaction with the engine

### Plugin declaration

`duckdb_guard` is a `MYSQL_AUDIT_PLUGIN` declared in the same
`mysql_declare_plugin` block as `duckdb`, subscribed to the table access class,
READ subclass. To verify: that audit plugins receive table access events in the
9.7 event tracking dispatch (sql/sql_audit.cc, `event_tracking_dispatch_error`),
and how a plugin returns an error that aborts the statement.

### Event handling

For each READ event (one per table reference, in `next_global` order), the guard
looks up (database, table) in the engine's registry cache (DWL#3).

### Decision

```
table is retain_history?            no  → continue
effective scope is FULL?            no  → continue
statement writes?                   yes → error (FR 5)
use_secondary_engine = OFF?         yes → error (FR 2)
table loaded and duckdb_enable?     no  → error (FR 3)
otherwise                               → lex->set_execute_only_in_secondary_engine()
```

### Interaction with the engine

The engine's `find_secondary_engine_offload_fail_reason` hook returns the real
rejection reason, so FORCED-style failures show our message. The reason value
passed to `set_execute_only_in_secondary_engine()` comes from the server's fixed
list (no DuckDB entry); its text is shown only if the server raises its own
error, which our checks precede.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/guard/guard.cc` | Audit plugin descriptor, event callback, decision |
| `storage/duckdb/ha_duckdb.cc` | Second entry in `mysql_declare_plugin`; FULL refused without the guard |

### Effective scope

The guard reads the session value of `duckdb_history_scope` through the plugin
variable accessor (`THDVAR`). To verify: that `SET_VAR` hints are applied before
`open_tables()` raises the events, so the hint value is visible.

### Statement kind

Read from `thd->lex->sql_command`: SELECT is a read; INSERT_SELECT,
REPLACE_SELECT, UPDATE_MULTI, DELETE_MULTI, CREATE_TABLE with a select, and
UPDATE or DELETE with subqueries are "writes and reads" (FR 5).

### Reason value

Proposal: `TEMPORARY_TABLE_USAGE` or the value with the fewest side effects; to
verify that the reason affects only the error message.

### Open points

- Audit plugin delivery of table access events in 9.7: to verify first; if it
  does not work, the fallback is to document that FULL scope requires
  `use_secondary_engine=FORCED` (option b of the design discussion).
- Timing of SET_VAR application: to verify.

## Test Plan

MTR suite `duckdb`:

1. `guard_off`: FULL scope with `use_secondary_engine=OFF` fails with our error.
2. `guard_on_fallback`: FULL scope, ON, query with an unsupported construct fails
   with the engine reason instead of running on InnoDB.
3. `guard_not_loaded`: FULL scope on a history table that is stale or unloaded.
4. `guard_disabled`: `duckdb_enable=FALSE` under FULL.
5. `guard_hint`: `SET_VAR(duckdb_history_scope='FULL')` on one statement.
6. `guard_write_read`: INSERT ... SELECT from a history table under FULL fails.
7. `guard_live_unaffected`: LIVE scope queries fall back normally.
8. `guard_routine_view`: history table read through a view and a stored
   procedure.
9. `guard_missing`: engine loaded without the guard; FULL refused.
10. `guard_replica`: replication applier not affected.
