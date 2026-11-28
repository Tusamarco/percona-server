# DWL#8: History maintenance: duckdb_delete and duckdb_update

Affects: Percona Server 9.7 — Status: design, to review (2026-11-27)

Copyright (c) 2026-present, Marco Tusa.

## Description

### GENERAL IDEA

Tables with `retain_history` keep in DuckDB the rows that the purge job
removed from InnoDB (archived rows, `__archived_at` set; DWL#5). Today
nothing can change or remove them: InnoDB has no such rows, so no DML
reaches them, and DuckDB is written only by the load and the applier.
History needs maintenance: obsolete history removed (retention, legal
erasure), a historical flag corrected.

Two procedures take a DELETE or UPDATE statement as text. The call writes
one row in an InnoDB control table; that row is in the binary log and
replicates like any data. The applier meets it at its binary log position,
in order with the other changes, and runs the statement on the DuckDB copy
of the source and of every replica with the plugin.

### USER INTERFACE

```sql
CALL duckdb.duckdb_delete('DELETE FROM shop.orders WHERE created_at < ''2020-01-01''', FALSE);
CALL duckdb.duckdb_update('UPDATE shop.orders SET obsolete = 1 WHERE created_at < ''2021-01-01''', FALSE);
```

- Second argument `force`:
  - FALSE (the normal case): only archived rows (`__archived_at IS NOT
    NULL`). The live rows, which InnoDB also has, are never touched, so the
    copy stays identical to InnoDB for LIVE queries.
  - TRUE: also the live rows of the DuckDB copy. InnoDB keeps them, so the
    copy then differs from InnoDB: the user's choice (Marco, 2026-11-27),
    with a reload to align it again (see "Force and exactness").
- A statement without WHERE is never executed (Marco, 2026-11-27). A WHERE
  that is constant (`WHERE 1`, `WHERE 1 = 1`, `WHERE TRUE`) counts as none.
- Through `CALL` only: a `SELECT duckdb_delete(...)` would not be in the
  binary log (Marco, 2026-11-27).
- The call returns when the applier has run the statement (like read your
  own writes, `duckdb_offload_wait_timeout_ms` bounds the wait), with one
  row: request id, rows affected, state (`done`, `failed: <reason>`,
  `pending` after the timeout).

`CALL duckdb_delete(...)` without a schema works only when the current
database is `duckdb`: MySQL looks up an unqualified procedure in the
current database, and a plugin cannot add procedures everywhere without a
server change. Hence `duckdb.duckdb_delete`. Open point 1.

## Requirements

### Functional Requirements

1. `duckdb.duckdb_delete(stmt, force)` and `duckdb.duckdb_update(stmt,
   force)` accept one single-table DELETE / UPDATE on a table with a DuckDB
   copy, with a WHERE clause.
2. Refused at call time, nothing written: not exactly one statement of the
   procedure's kind; more than one table (joins, multi-table DELETE /
   UPDATE); no WHERE or a constant WHERE; ORDER BY / LIMIT; a table without
   a loaded copy; `force = FALSE` on a table without `retain_history`
   (nothing to act on); SET on the primary key or on a hidden column
   (`__archived_at`, ...); a WHERE or SET the offload translator refuses
   (same rules and reasons as queries); `sql_log_bin = 0` (the request would
   not reach the applier; like the guard); a missing privilege (FR 7).
3. Accepted: a row in `duckdb.maintenance` (InnoDB, row-logged), committed
   by the call.
4. The applier, at that row's position: commits its pending batch, then
   runs the statement on the copy with the force filter, in one DuckDB
   transaction with the applied position (crash-safe: after a restart the
   request is either done or run again, never twice).
5. A request whose table is not loaded at that point (unloaded, stale,
   dropped meanwhile) is skipped with state `failed`, the applier continues.
6. Results: `lake.__mysql.maintenance` (request id, GTID, table, kind,
   force, rows affected, state, time), shown in
   `INFORMATION_SCHEMA.DUCKDB_MAINTENANCE`; status
   `duckdb_maintenance_requests`, `_rows`, `_failures`.
7. Privilege: the dynamic privilege `DUCKDB_HISTORY_ADMIN` plus DELETE
   (UPDATE) on the target table.
8. Replicas with the plugin apply the same requests at the same position;
   a replica without the plugin only stores the control row.

### Non-Functional Requirements

1. The applier never writes InnoDB and never creates a GTID (as today):
   results stay in DuckDB.
2. Same exactness as the offload: the WHERE / SET are MySQL expressions
   evaluated with MySQL's semantics (collations, PAD SPACE, DECIMAL,
   TIMESTAMP zones), through the offload translator.

## Dependent Tasks

DWL#4 (applier), DWL#5 (history), DWL#6 (translator), DWL#7 (guard).

## High Level Architecture

### Pieces

| Piece | Where | Role |
|---|---|---|
| Schema `duckdb`: table `maintenance`, procedures `duckdb_delete`, `duckdb_update` | install script `duckdb_maintenance.sql` (open point 2) | the user interface, the binary log carrier |
| Loadable functions `duckdb_maintenance_check(kind, stmt, force)` and `duckdb_maintenance_wait(id)` | plugin, `udf_registration` service | validation and translation at call time; the wait for the applier |
| Request apply | applier (`duckdb_binlog_reader.cc`, `duckdb_applier.cc`) | recognises row events of `duckdb.maintenance`, runs the request |
| Results | `lake.__mysql.maintenance`, I_S table, status | what happened |

### The call

```sql
CREATE PROCEDURE duckdb.duckdb_delete(IN stmt LONGTEXT, IN force_live BOOLEAN)
  SQL SECURITY INVOKER
BEGIN
  -- validates, checks privileges, translates; error if refused
  SET @duckdb_sql = duckdb_maintenance_check('DELETE', stmt, force_live);
  INSERT INTO duckdb.maintenance (kind, stmt, duckdb_sql, force_live, requested_by)
    VALUES ('DELETE', stmt, @duckdb_sql, force_live, CURRENT_USER());
  SELECT duckdb_maintenance_wait(LAST_INSERT_ID()) AS result;
END
```

The INSERT commits on its own (autocommit) before the wait. Inside an open
transaction the call is refused (the request would not be in the binary log
until COMMIT, and the wait would never end).

### Translation at call time

`duckdb_maintenance_check` parses the statement with the server's parser
(as `parse_dml_targets` does today for the reader) and prepares `SELECT 1
FROM db.t WHERE <cond>` on the InnoDB table: the WHERE (and the SET values)
become Items, which the offload translator turns into DuckDB SQL exactly as
for a query (collation keys, PAD SPACE, constants). The translated DuckDB
statement is stored in the control row with the text, so the applier on
every server runs the same SQL, made with the source's rules, and never
parses MySQL syntax itself.

Alternative refused: the statement in DuckDB's dialect. Simpler, but
`WHERE status = 'Obsolete'` on a `_ci` column would match other rows than
MySQL would (case, trailing spaces), the kind of difference the offload
exists to avoid.

### The force filter

- FALSE: `... WHERE (<cond>) AND __archived_at IS NOT NULL`.
- TRUE: `... WHERE (<cond>)`; no filter.

### Force and exactness

After a `force = TRUE` request that changed live rows, the copy differs
from InnoDB, so LIVE offloaded queries would answer differently from
InnoDB. Proposal: the copy goes to state `diverged`: queries are not
offloaded (ON: InnoDB answers; FORCED: error with the reason), the applier
keeps applying, `DUCKDB_TABLES` shows it, and `ALTER TABLE ... SECONDARY_LOAD`
(reload; history kept as with `resync`) brings it back. Rows affected 0
among live rows: no divergence. Open point 3.

### Ordering and crash safety

The request is one row event in one transaction. The applier commits its
pending batch before it, so every earlier change is in DuckDB, then runs the
request and writes the applied position (the request's GTID) and the
result in the same DuckDB transaction. A crash before that commit runs it
again from the binary log; after it, never again.

## Low Level Design

### Control table

```sql
CREATE TABLE duckdb.maintenance (
  id           BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  kind         ENUM('DELETE','UPDATE') NOT NULL,
  stmt         LONGTEXT NOT NULL,
  duckdb_sql   LONGTEXT NOT NULL,
  force_live   BOOLEAN NOT NULL,
  requested_by VARCHAR(288) NOT NULL,
  requested_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE = InnoDB;
```

The guard refuses other writes to it (only INSERT through the procedures;
no UPDATE / DELETE except a cleanup of old rows, which the applier ignores).
The applier reads only its insert events.

### Files

| File | Change |
|---|---|
| `duckdb_maintenance.cc/.h` (new) | the two loadable functions, the validation, the result store |
| `duckdb_binlog_reader.cc` | insert events of `duckdb.maintenance` become requests in the batch stream |
| `duckdb_applier.cc` | runs a request: batch commit, statement, result, position |
| `duckdb_registry.cc` | `lake.__mysql.maintenance`, the `diverged` state |
| `ha_duckdb.cc` | privilege `DUCKDB_HISTORY_ADMIN`, status variables, I_S table |
| `duckdb_guard.cc` | writes to `duckdb.maintenance` other than the procedures' INSERT |
| `duckdb_maintenance.sql` (new) | schema, table, procedures |

### Open points (for Marco)

1. Name: `CALL duckdb.duckdb_delete(...)` (schema `duckdb`), or shorter
   `CALL duckdb.delete_history(...)` / `duckdb.update_history(...)`.
2. Who creates schema, table and procedures: an install script run once
   (as `sys`), or the plugin at the first `SECONDARY_LOAD` of a
   `retain_history` table (DDL from inside the plugin: binary logged,
   replicates; more code). Proposal: the script.
3. `force = TRUE` on live rows: `diverged` state with offload refused until
   a reload (proposal, exact), or keep offloading with a warning in
   `DUCKDB_TABLES` (the user's choice, results may differ from InnoDB).
4. Size limit of a request: proposal none (DuckDB deletes and updates by
   predicate are fast), with progress in the stage of the applier.

## Test Plan

MTR suite `duckdb`:

1. `maintenance_refusals`: no WHERE, constant WHERE, two tables, ORDER BY /
   LIMIT, SET on the key or a hidden column, table without copy, FALSE on a
   table without history, `sql_log_bin = 0`, inside a transaction, missing
   privilege: errors, nothing written.
2. `maintenance_history`: purge rows (tagged), `duckdb_delete` FALSE removes
   archived rows only; `duckdb_update` FALSE changes a flag on archived
   rows; FULL scope shows it, LIVE results identical to InnoDB.
3. `maintenance_force`: TRUE on live rows: rows gone in DuckDB, copy
   `diverged`, offload refused, reload restores it.
4. `maintenance_collation`: WHERE on a `_ci` column with trailing spaces:
   the same rows as InnoDB would match.
5. `maintenance_order`: a change, a request, a change in the same batch:
   applied in order.
6. Manual: replica with the plugin applies the request; crash between
   request and commit (debug point) runs it once.
