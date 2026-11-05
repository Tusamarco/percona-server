# DWL#4: Change propagation: binlog applier

Affects: Percona Server 9.7 — Status: Design, under review

## Description

### GENERAL IDEA

DML always runs on InnoDB first. After commit, the changes reach DuckDB through
an applier inside the plugin that reads the binary log. We do not change core
code, so we cannot hook row changes in the handler; the binlog is the only
complete, ordered and public record of committed changes.

### APPLIER MODEL

```
binlog files ──► [reader] ──► [prepare worker 1..N] ──► [committer] ──► DuckLake
                 decode,       convert to DuckDB         one DuckLake
                 filter,       vectors, collapse         transaction per batch
                 detect tag    per primary key           + applied GTID
```

One reader thread, `duckdb_apply_workers` prepare workers, one committer. The
committer writes one DuckLake transaction per batch, in GTID order, including the
applied position, so a crash never loses or duplicates a change.

We do not use the replica parallel worker model: DuckLake commits are expensive
(new files and a catalog snapshot each), so parallel small commits would multiply
the most expensive step. We keep its principle in the prepare workers.

### VARIABLES

| Variable | Type | Default | Dynamic |
|---|---|---|---|
| `duckdb_apply_workers` | int | 1 | proposal: no in the POC |
| `duckdb_apply_batch_max_trx` | int | to be decided | yes |
| `duckdb_apply_batch_max_rows` | int | to be decided | yes |
| `duckdb_apply_batch_max_delay_ms` | int | to be decided | yes |

## Requirements

### Functional Requirements

1. Every committed INSERT, UPDATE and DELETE on a loaded table is applied to its
   DuckLake table, including changes from REPLACE, INSERT ... ON DUPLICATE KEY
   UPDATE, LOAD DATA, multi-table statements, and the replication applier on a
   replica.
2. Changes are applied in commit order; a source transaction touching several
   loaded tables is applied atomically in DuckDB.
3. Rolled back transactions never reach DuckDB.
4. Events for tables that are not loaded are ignored; events already contained in
   a table's load GTID set (DWL#3) are skipped.
5. After a crash or restart, the applier resumes from the GTID stored in
   `lake.__mysql.applied_position` and applies each transaction exactly once.
6. A batch is committed when it reaches `duckdb_apply_batch_max_trx` source
   transactions, `duckdb_apply_batch_max_rows` row changes, or
   `duckdb_apply_batch_max_delay_ms`, whichever comes first, always at a
   transaction boundary.
7. Changes to the same primary key inside a batch are collapsed: insert then
   updates becomes one insert of the final row; insert then delete becomes
   nothing; several updates become one update with the final image (purge rules:
   DWL#5).
8. DDL on a loaded table (seen as a query event) invalidates the copy: tables
   without retain_history are unloaded with a warning; retain_history tables are
   marked stale.
9. If binlog files the applier still needs have been purged, the affected tables
   are marked stale and an error is logged.
10. Compressed transaction payloads are supported; `binlog_encryption=ON` makes
    the applier refuse to start with an error (POC).
11. Status variables report apply lag and progress (Low Level Design).
12. `duckdb_enable=FALSE` stops the applier; on TRUE it resumes from the applied
    position if the binlog is still available, otherwise FR 9 applies.

### Non-Functional Requirements

1. The applier never blocks or slows down the InnoDB commit path beyond the cost
   of the binlog observer callback.
2. Under the reference write workload, the apply lag stays bounded (target set
   after the first measurements).
3. Batches are large enough that DuckLake does not create one file per source
   transaction under sustained load.

## Dependent Tasks

- DWL#3: Secondary tables: SECONDARY_LOAD and SECONDARY_UNLOAD.

## High Level Architecture

### Contents

1. Notification
2. Reader
3. Prepare workers
4. Committer
5. Applied position and recovery
6. Invalidation and gaps

### Notification

A `Binlog_storage_observer` registered with `register_binlog_storage_observer()`
(sql/replication.h), the API used by semisync. Its `after_flush` callback gives
the binlog file name and position after each group commit. The callback only
stores the position and wakes the reader; it does no work.

### Reader

Reads binlog files from the applied position up to the last notified position,
following rotations. Decodes events with the in-tree library
(`libs/mysql/binlog/event`): GTID events (including the tag, DWL#5),
`Table_map`, `Write_rows`, `Update_rows`, `Delete_rows`, transaction payload
events (decompressed), query events (DDL detection), XID and rotate events.
Groups events into source transactions and drops those that touch no loaded
table.

### Prepare workers

Receive the row changes of a batch split by table or by hash of the primary key,
so changes to one key always go to the same worker and stay in order. They decode
row images with `binlog_row_metadata=FULL` column information, convert values
with the shared type module (DWL#3), and collapse per key.

### Committer

Merges the prepared parts and, in one DuckLake transaction: deletes and updates
by primary key, appends inserts through the Appender, updates the applied
position. Then commits. Batches are committed strictly in GTID order.

### Applied position and recovery

`lake.__mysql.applied_position` holds the last applied GTID and binlog
coordinates, written in the same transaction as the data. At startup the reader
starts after that position.

### Invalidation and gaps

DDL query events on loaded tables trigger FR 8. A missing binlog file at the
needed position triggers FR 9.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/applier/observer.cc` | Binlog storage observer registration and callback |
| `storage/duckdb/applier/reader.h/.cc` | Binlog file reading and event decoding |
| `storage/duckdb/applier/batch.h/.cc` | Batch structure, collapse per key |
| `storage/duckdb/applier/worker.h/.cc` | Prepare workers |
| `storage/duckdb/applier/committer.h/.cc` | DuckLake writes and applied position |
| `storage/duckdb/applier/applier.h/.cc` | Thread lifecycle, start, stop, status |

### Reading binlog files

To verify: whether the server's binlog file reader classes (sql/binlog_reader.h)
can be used from the plugin, or whether we read files directly and use only the
event library for decoding. Binlog index and current file come from the server's
binary log object (internal interface, not core change).

### Row image decoding

`Table_map` gives the table id and column types; `binlog_row_metadata=FULL`
adds column names, signedness, charsets and primary key columns. We map columns
by name to the DuckLake table, so column order changes do not corrupt data. A
mismatch (unknown column) invalidates the table (FR 8).

### Batch and collapse

Per table, a map from primary key to the net operation:

| Sequence in batch | Result |
|---|---|
| insert | insert |
| insert, update(s) | insert final image |
| insert, delete | nothing |
| update(s) | update final image (delete old key + insert if the key changed) |
| update, delete | delete original key |
| delete, insert | delete + insert (replace) |

### DuckLake writes

Per table and batch: `DELETE FROM lake.db.t WHERE pk IN (...)` for deletes and
for the old image of updates, then the Appender for inserts and new images of
updates. DuckLake writes an update as a delete file plus new data, so
delete-plus-insert is no more expensive than UPDATE.

### Status variables

`duckdb_apply_lag_seconds`, `duckdb_apply_lag_transactions`,
`duckdb_applied_gtid`, `duckdb_apply_batches`, `duckdb_apply_rows_collapsed`,
`duckdb_apply_time_decode_us`, `duckdb_apply_time_prepare_us`,
`duckdb_apply_time_commit_us`, `duckdb_apply_state` (running, stopped, error).

### Open points

- Batch variable defaults, dynamic `duckdb_apply_workers`: to be decided.
- Use of server binlog reader classes from the plugin: to verify.
- Catch-up after a binlog gap: replay is impossible; marking stale requires a
  reload (and for retain_history tables a reload is not allowed): to decide.

## Test Plan

MTR suite `duckdb`:

1. `apply_dml`: INSERT, UPDATE, DELETE, REPLACE, ON DUPLICATE KEY UPDATE, LOAD
   DATA on a loaded table; DuckDB content equals InnoDB content.
2. `apply_multi_table`: transaction on two loaded tables applied atomically.
3. `apply_rollback`: rolled back transactions not applied.
4. `apply_collapse`: each row of the collapse table, checked through status
   counters and final content.
5. `apply_restart`: restart with pending changes; no loss, no duplicates.
6. `apply_crash`: debug crash points in the committer before and after commit;
   recovery gives exactly-once.
7. `apply_ddl_invalidate`: ALTER TABLE on loaded tables with and without
   retain_history.
8. `apply_binlog_gap`: binlog purged while the applier is stopped; tables marked
   stale.
9. `apply_compressed`: `binlog_transaction_compression=ON`.
10. `apply_replica`: changes arriving through replication on a replica.
11. `apply_lag_status`: status variables move as expected.
