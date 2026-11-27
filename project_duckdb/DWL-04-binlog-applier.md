# DWL#4: Change propagation: binlog applier

Affects: Percona Server 9.7 — Status: 4a verified (2026-11-10); 4b implemented, under test; 4c-1 implemented, under test (2026-11-10); 4c-2 implemented, under test (2026-11-11); 4d implemented, verified on a throwaway instance and on the reference instance (2026-11-11, after the deferred-refresh fix); MTR `applier`, `super_read_only`, `binlog_reset`, `ddl_wait`, `json_geometry` in the suite

(2026-11-26, external review: a row event without every column in its
images (binlog_row_image MINIMAL / NOBLOB in the writing session) and a DML
logged as a statement on a loaded table mark that copy stale instead of
applying NULLs or losing the change; `binlog_order_commits` switched OFF
stops the applier. The guard refuses such writes at the source: DWL-07.
MTR `binlog_safety`.)

Step 4b: `duckdb_row_decoder.*` (packed row images to typed values, no THD,
links with mysys and strings only), `duckdb_changes.*` (collapse per primary
key, per transaction; per batch in 4c), status `duckdb_apply_rows_decoded` and
`duckdb_apply_changes`, temporary variable `duckdb_apply_debug_log`, unit
tests `duckdb_apply-t` (gunit). Mapping to lake columns by name is done in 4c.

Steps: 4a reader (observer, thread, files, events, transactions, filtering;
no DuckDB writes), 4b row decoder and collapse, 4c-1 batches, committer,
applied position, recovery, gap handling and repoint, 4c-2 large
transactions in pieces, 4d DDL, compressed payloads, MTR.

(2026-11-25: storage moved to a native DuckDB file, `repository.duckdb`; design
document, Storage. Each "DuckLake transaction" below is now a DuckDB
transaction on that file; catalog snapshots, inlining and Parquet files no
longer apply.)

Decisions (2026-11-09):

- Row values are decoded by our own decoder from the packed row image, using
  the Table_map column types and metadata; no THD, no opened tables (4b).
- The reader is notified by the transaction observer's `after_commit`, after
  the InnoDB commit (changed 2026-11-13; before: `after_sync`). A transaction
  is in the binary log before InnoDB commits it (flush, sync, then commit;
  with semi-sync AFTER_SYNC the commit waits for a replica, up to
  `rpl_semi_sync_source_timeout`): reading at `after_sync` showed it in
  DuckDB before InnoDB did. With `binlog_order_commits=ON` (required at
  SECONDARY_LOAD) the server commits a group in InnoDB, then calls
  `after_commit` for each transaction in binary log order
  (sql/binlog.cc, `ordered_commit`), so the reader never reads past what
  InnoDB has committed.
- SECONDARY_LOAD (and the refresh after DDL, which runs it) cannot miss a
  transaction that is in the binary log but not yet committed in InnoDB: it
  holds MDL_SHARED_NO_WRITE (sql/sql_table.cc), which waits for every
  transaction that wrote the table to end, before it reads InnoDB and takes
  the binary log position.
- Durability: `sync_binlog=1` is required to load history tables; classic
  tables get a warning. See "Durability settings".
- The load records the binary log file and position (with the GTID set).
  The applier keeps one global position; events before a table's load
  position are skipped. Before 4c the reader starts at the earliest load
  position, or at the end of the binary log. Tables loaded before 4a have no
  position and are followed from the reader start (no backfill; accepted
  for the POC).
- In the POC, SECONDARY_LOAD requires `binlog_encryption=OFF`
  (`binlog_transaction_compression=ON` is supported since 4d).

Decisions (2026-11-10, step 4c-1):

- No prepare workers in 4c-1: the reader thread builds batches and hands each
  one to the executor as one task. Workers are added later only if the
  counters show decoding is the bottleneck.
- Each transaction is collapsed on its own (a ROLLBACK discards only it), then
  merged into the batch with the same rules (`TableChanges::merge()`).
- The applied position (file, position, last GTID) is written in the same
  DuckLake transaction as the data. At start the reader resumes there; without
  one it starts at the earliest load position or the current end.
- Every committed transaction moves the position, relevant or not. Without
  changes to loaded tables the position is committed at most once a minute:
  each commit is a DuckLake catalog snapshot.
- Unload and the committer share `Lake::apply_mutex()`; the committer checks
  the registry under it, so a table is never written after its drop.
- A table loaded again during a batch: the batch is committed first and the
  changes for the old copy are skipped (the new copy contains them).
- Gaps (purged binary logs): if no loaded table needs the purged range the
  reader continues; otherwise `duckdb_apply_gap_action` decides (STOP by
  default). `duckdb_apply_start_position` repoints the reader by hand.
- `SAVEPOINT` / `ROLLBACK TO` query events do not end a transaction. XA
  transactions on loaded tables stop the reader (not supported yet).

Decisions (2026-11-11, step 4c-2):

- A source transaction with more than `duckdb_apply_batch_max_rows` rows of
  loaded tables is written in pieces of that size inside one DuckLake
  transaction. No separate variable: `duckdb_apply_max_transaction_rows` is
  removed.
- Before the first piece, the pending batch is committed on its own: batches
  still end at transaction boundaries.
- Each piece is collapsed on its own, relative to the rows the earlier pieces
  wrote; applying the pieces in order equals applying consecutive
  transactions (4c-1).
- The DuckLake transaction holds `apply_mutex` from the first piece to the
  commit. The transaction is already complete in the binary log (we read only
  synced positions), so nothing waits on the server; an unload waits until the
  large transaction is applied.
- A ROLLBACK, a read error, a gap or a stop in the middle rolls the DuckLake
  transaction back. The applied position does not move: the transaction is
  read again at the next start.
- Verified before the code (standalone program against the in-tree DuckDB and
  DuckLake): DELETE and UPDATE of rows inserted earlier in the same open
  transaction work, both inlined and in Parquet files, and survive a
  re-attach; a rollback leaves nothing.

Decisions (2026-11-11, step 4d):

- DDL is never blocked, locally or from replication
  (`HTON_SECONDARY_ENGINE_SUPPORTS_DDL`): a column type change in InnoDB must
  reach DuckDB.
- The copy follows a DDL by a refresh from InnoDB, not by translating the
  MySQL DDL: DuckDB gets exactly InnoDB's new shape and values (defaults,
  charsets, conversions). History tables keep their archived rows, converted
  by name.
- The refresh is the server's own SECONDARY_LOAD run in an internal session
  with `sql_log_bin=0`: nothing replicates, no GTID is created.
- A copy that cannot follow a DDL becomes stale; the applier never stops for
  a DDL.
- Verified before the code: DuckLake refresh in one transaction (spike), and
  the internal session from a plugin thread (spike on a throwaway instance).
- MTR suite postponed.

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
| `duckdb_apply_batch_max_trx` | int, 1 to 1,000,000 | 1000 | yes |
| `duckdb_apply_batch_max_rows` | int, 1,000 to 100,000,000 | 100000 | yes; also the piece size of large transactions |
| `duckdb_apply_batch_max_delay_ms` | int, 10 to 60,000 | 1000 | yes |
| `duckdb_apply_gap_action` | STOP, FIRST_AVAILABLE | STOP | yes |
| `duckdb_apply_start_position` | FIRST, CURRENT or a binlog name | "" | yes; works as a command, not persisted |
| `duckdb_apply_debug_log` | bool | OFF | yes (testing aid) |
| `duckdb_apply_workers` | int | 1 | not implemented (see decisions) |

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
5. After a crash or restart, the applier resumes from the position stored in
   `lake.__mysql.applied_position` and applies each transaction exactly once.
6. A batch is committed when it reaches `duckdb_apply_batch_max_trx` source
   transactions, `duckdb_apply_batch_max_rows` row changes, or
   `duckdb_apply_batch_max_delay_ms`, whichever comes first, always at a
   transaction boundary. A transaction larger than `duckdb_apply_batch_max_rows`
   is written in pieces inside one DuckLake transaction, so memory stays
   bounded and the transaction stays atomic in DuckDB.
7. Changes to the same primary key inside a batch are collapsed: insert then
   updates becomes one insert of the final row; insert then delete becomes
   nothing; several updates become one update with the final image (purge rules:
   DWL#5).
8. DDL on a table with `SECONDARY_ENGINE=duckdb` is never refused, whether it
   is run locally or arrives through replication. When the reader reaches the
   statement in the binary log, it brings the copy in line: a renamed table is
   renamed in the lake; a table whose rows or columns changed (ALTER, TRUNCATE,
   a dropped primary key) is refreshed from InnoDB, keeping the archived rows
   of history tables. If that fails, the copy is marked stale and the reader
   goes on. DROP TABLE and `SECONDARY_ENGINE=NULL` unload the copy as before
   (history tables: stale).
9. If binlog files the applier still needs have been purged, the affected
   tables (loaded before the end of the purged range) are named in the error
   log. With `duckdb_apply_gap_action=STOP` the reader stops in error state;
   with FIRST_AVAILABLE it continues at the next available file. If no loaded
   table is affected it always continues. `SET GLOBAL
   duckdb_apply_start_position` restarts the reader at FIRST, CURRENT or a
   named file; a position before the applied position is refused.
10. Compressed transaction payloads (`binlog_transaction_compression=ON`) are
    supported; `binlog_encryption=ON` makes SECONDARY_LOAD refuse with an
    error (POC).
11. Status variables report apply lag and progress (Low Level Design).
12. `duckdb_enable=FALSE` stops the applier; on TRUE it resumes from the applied
    position if the binlog is still available, otherwise FR 9 applies.
13. A warning (error log at reader start, SHOW WARNINGS at SECONDARY_LOAD)
    when `binlog_expire_logs_seconds` is below one day with auto purge on.

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

A `Trans_observer` registered with `register_trans_observer()`
(sql/replication.h), the API used by semisync. Its `after_commit` callback
gives the binlog file name and position of each transaction after its InnoDB
commit (before 2026-11-13: `Binlog_storage_observer::after_sync`, before the
InnoDB commit). The callback only
stores the position and wakes the reader; it does no work.

### Durability settings

After a crash, InnoDB commits every prepared transaction found in the binary
log, so InnoDB and DuckDB agree, provided both are durable:

- `sync_binlog=1`: otherwise an operating system crash or power loss can lose
  the end of the binary log that the applier already applied; InnoDB then
  rolls those transactions back, and the copy keeps them.
- `innodb_flush_log_at_trx_commit=1`: otherwise InnoDB can lose committed
  transactions that the binary log (and the copy) still have.

A crash of mysqld alone (not of the machine) loses nothing either way. With
other settings, after an operating system crash a classic copy is repaired
with SECONDARY_UNLOAD + SECONDARY_LOAD; a history copy cannot be repaired
(its archived rows exist only in DuckDB), which is why `sync_binlog=1` is
required for it. `innodb_flush_log_at_trx_commit` is documented, not
checked, in the POC.

Known limitation: when the reader is started at runtime (`duckdb_enable`
OFF then ON, `duckdb_apply_start_position`), it takes the current end of the
binary log as committed; a transaction between its binary log sync and its
InnoDB commit at that very moment can be shown by DuckDB before InnoDB. At
server startup crash recovery has committed everything first.

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

### Replicas

On a replica the reader reads the replica's own binary log, not the relay
log. With `log_replica_updates=ON` (the default) the replication applier
writes every transaction it commits there, through the same group commit, so
`after_sync` notifies us exactly as for a local commit. The relay log is not
used: it holds what was received, not what was committed (a transaction may
still fail, be skipped or filtered), its order is not the commit order with a
multi-threaded applier, and it is purged as soon as it is applied.

Requirements on the replica itself (its own settings apply to what it writes:
`Table_map_log_event::init_metadata_fields()` reads the writing server's
`binlog_row_metadata`, and the applier marks columns with the replica's
`binlog_row_image`): `log_bin`, `log_replica_updates=ON`,
`binlog_row_metadata=FULL`, `binlog_row_image=FULL`.

Errant GTIDs: `ALTER TABLE ... SECONDARY_ENGINE=duckdb`, `SECONDARY_LOAD` and
`SECONDARY_UNLOAD` are written to the binary log (sql/sql_table.cc, "write
binlog to maintain replication consistency"). Run on a replica they create
GTIDs the source does not have. Either run them on the replica with
`SET sql_log_bin = 0`, or define `SECONDARY_ENGINE` on the source (allowed
without the plugin installed there). The plugin's own internal statements
(refresh after DDL, step 4d) always run with `sql_log_bin = 0`.
The same applies to `GRANT DUCKDB_HISTORY_READER` on a replica whose source
has no plugin (the source does not know the privilege, so the grant cannot
come from there).

`read_only` / `super_read_only` (usual on replicas): the applier's DML
writes only to DuckDB and is not affected. The refresh after DDL runs
`ALTER TABLE ... SECONDARY_LOAD` in an internal session, which the server's
read-only check (`check_readonly()`, sql/auth/sql_authorization.cc) refuses
with error 1290 unless the THD has `skip_readonly_check`, the flag the
replication applier's internal table writes use. The server clears that
flag at the start of every statement (`THD::reset_for_next_command`), so it
cannot be set once on the session. The `duckdb_guard` audit plugin sets it
at the pre-parse event, which comes after the reset and before the check,
for THDs registered by `run_internal_statements()` only
(`is_internal_session()`; one atomic load for every other statement).
Session service THDs are created with auditing off (sql/srv_session.cc), so
the internal session turns it on (`mysql_audit_enable_auditing()`): audit
plugins see its statements. Consequence: with the guard disabled, a refresh
on a `super_read_only` server fails and the copy becomes stale. Verified on
2026-11-16 (MTR `super_read_only`, test plan section 18): MODIFY, ADD
COLUMN, RENAME, TRUNCATE refreshed on a classic and a history table, user
statements still refused.

Verified on 2026-11-15 (test plan section 18): source without the plugin,
replica with it, DML, transactions in pieces, replicated DDL, 4 parallel
workers, replica stop and restart during writes, tagged purges; DuckDB equal
to InnoDB and no errant GTIDs.

Before 4d, DDL replicated on a table with `SECONDARY_ENGINE=duckdb` stopped
the replica (`ER_SECONDARY_ENGINE_DDL`). Since 4d the engine sets
`HTON_SECONDARY_ENGINE_SUPPORTS_DDL` and the reader refreshes the copy, in
binlog order, on the source and on the replica alike.

## Low Level Design

### Files

| File | Content |
|---|---|
| `duckdb_binlog_reader.h/.cc` | Thread, file reading, events, transactions, batches, gaps, repoint |
| `duckdb_row_decoder.h/.cc` | Row images to typed values |
| `duckdb_changes.h/.cc` | Collapse per key, per transaction; merge into a batch |
| `duckdb_applier.h/.cc` | Batch, committer, applied position |
| `duckdb_ddl.h/.cc` | DDL statements from query events: tables to refresh, table and column renames (4d) |
| `duckdb_session.h/.cc` | Internal server session for the refresh: `skip_grants`, `sql_log_bin=0` (4d) |
| `duckdb_load.cc` | Load; refresh, rename and stale marking after DDL (4d) |
| `ha_duckdb.cc` | Observer, variables, status, `BFN_BINLOG_END`, DDL flag |

### Stability rules (review of 2026-11-10)

- The reader never keeps a registry pointer: it uses `Registry::snapshot()`
  copies, because an unload may remove the entry at any time.
- No C++ exception may leave the reader thread or reach the commit path:
  the thread body and `after_sync` catch everything; the reader goes to
  `error:` state, a commit is never affected.
- Every decoded event is checked with `header()->get_is_valid()`.
- Memory per transaction is bounded by `duckdb_apply_batch_max_rows`: larger
  transactions are written in pieces (4c-2). The 4b limit
  `duckdb_apply_max_transaction_rows` is removed.
- An open DuckLake transaction of a large transaction holds `apply_mutex`: it
  is rolled back on every exit path (error, gap, stop, exception) before the
  reader waits in `error:` state, so an unload never waits on a stopped
  reader.

### Lessons from step 4a (implemented)

- A storage engine's `init()` receives the handlerton, not the plugin. The
  binlog storage observer must be registered with our `st_plugin_int`, found
  with `plugin_lock_by_name("duckdb", MYSQL_STORAGE_ENGINE_PLUGIN)` (as the NDB
  engine does); with the handlerton the server silently skips the observer.
- `after_sync` passes the binary log name without directory
  (`THD::set_trans_pos` strips it). We keep names without directory everywhere
  and build the full path from the current log's directory only to open a
  file.
- Plugins are initialized before the server opens the binary log: the reader
  checks `opt_bin_log` at start and waits for `mysql_bin_log.is_open()`.
- The Table_map carries `Field::binlog_type()`, which defaults to `type()`, not
  `real_type()`: DATE columns arrive as `MYSQL_TYPE_DATE` (10), not
  `MYSQL_TYPE_NEWDATE` (14), with the same 3-byte value. DATETIME and TIMESTAMP
  override it (`DATETIME2`, `TIMESTAMP2`). Unit tests must use the binlog type.
- The event library appends one zero byte to `Rows_event::row` after the row
  data (`libs/mysql/binlog/event/rows_event.cpp`); the decoder must stop one
  byte before the end, or it starts a phantom row. Unit tests that call
  `decode_row()` directly do not see this: only an end-to-end test does.
- `Binlog_file_reader::open(file, 0)` does not read the file's
  Format_description_event (it reads it only when the offset is after it), and
  `read_event_data()` does not record it. The reader must record it itself;
  otherwise events are decoded without stripping the checksum, and the event
  library asserts on the Table_map optional metadata in debug builds. This
  shows only when reading starts at the beginning of a file (after a
  rotation or a restart). Events decoded with an undefined checksum algorithm
  are now an error.
- Server headers used here (`sql/binlog.h`, `sql/rpl_log_encryption.h`)
  require `MYSQL_SERVER` and the `extra::unordered_dense` include path.

### Lessons from step 4c-1

- `MYSQL_BIN_LOG::get_log_index()` ends with `LOG_INFO_EOF` (-1) on success:
  it lists until `find_next_log()` fails. Treating non-zero as an error made
  every index lookup fail.
- The server logs `SAVEPOINT` inside the transaction: ending the transaction
  there would put the applied position mid-transaction and lose rows at the
  next start.
- A short `binlog_expire_logs_seconds` (2000 s on the test instance) purged the
  file of the earliest load position while the server was down.
- `clean_up()` (sql/mysqld.cc) destroys `mysql_bin_log` (`cleanup()`) before
  `plugin_shutdown()`, on every shutdown and on a startup abort. Stopping the
  reader at plugin deinit is too late: it can lock destroyed binlog mutexes.
  The reader stops at `BFN_BINLOG_END` (`hton->binlog_func`, called by
  `ha_binlog_end()` just before), still commits its pending batch, and cannot
  be started again. Found on 2026-11-11 by an aborted startup (assertion in
  `get_current_log()`).

### Reading binlog files

Verified in 4a: `Binlog_file_reader` (sql/binlog_reader.h) is used from the
plugin.

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

### Committer (4c-1, implemented)

One DuckLake transaction per batch, on the reader's own connection (executor
thread, not killable). Per table, after a registry check (unloaded, stale or
reloaded tables are skipped):

1. lake columns read with `duckdb_columns()` and mapped to binlog columns by
   name (a lake column missing from the binlog is an error);
2. deleted keys: `CREATE OR REPLACE TEMP TABLE __duckdb_del` with the lake key
   types, filled by the appender, then `DELETE ... WHERE EXISTS` (history
   tables: only `__archived_at IS NULL`);
3. archived keys: `__duckdb_arc` with `__ts`, then `UPDATE ... SET
   __archived_at = k.__ts FROM __duckdb_arc k ... AND t.__archived_at IS NULL`;
4. inserted rows (archived rows with their time, then live rows) through the
   appender, value by value (DuckDB converts to the column type; out of range
   is an error);

then `lake.__mysql.applied_position` (DELETE + INSERT) and COMMIT. Any error:
ROLLBACK, the position does not move, reader in `error:` state. Temporary
tables survive the commit, hence `CREATE OR REPLACE`. DuckLake inlines inserts
of up to 10 rows in the catalog (no Parquet file) and retries commit conflicts
itself (10 retries).

`OpenTransaction` (`duckdb_applier.h`) holds the DuckLake transaction and
`apply_mutex`: `begin()`, `apply()` (once per batch, or once per piece),
`commit()` (position and COMMIT), `rollback()`. Any error rolls back and
releases the mutex. `commit_batch()` is `begin` + `apply` + `commit`.

### Large transactions (4c-2, implemented)

In `handle_rows()`, after each whole rows event, when the current
transaction holds `duckdb_apply_batch_max_rows` rows or more, `write_piece()`:

1. on the first piece: commits the pending batch, opens the DuckLake
   transaction (`OpenTransaction::begin`), counts a large transaction;
2. applies the collected changes (same `apply_table()` as batches);
3. clears them: the next piece is collapsed on its own.

At XID/COMMIT, `commit_large()` applies the last piece, writes the applied
position and commits; the large transaction counts as one batch. A ROLLBACK
event, a read error, a gap, a stop or an exception calls `abandon_large()`
(ROLLBACK, warning in the error log). A GTID event while a large transaction
is open is an error (missing end event).

### Compressed payloads (4d, implemented)

A `Transaction_payload` event holds every event of a transaction except its
GTID event. The reader decompresses it with `Payload_event_buffer_istream`, as
the replica applier does (`Transaction_payload_log_event::do_apply_event`),
and handles the inner events with a copy of the format description that has
checksums off (inner events carry none). Their position is the position after
the payload event. Pieces (4c-2) work the same inside a payload.

### DDL: refresh after DDL (4d, implemented)

The engine sets `HTON_SECONDARY_ENGINE_SUPPORTS_DDL`: the server allows any
DDL and does nothing more for us, so the reader handles it from the binary
log, in order.

1. Detection (`parse_ddl()`, unit-tested): a tokenizer (quotes, comments,
   `/*!NNNNN */` versioned comments as code, the event's default database,
   `lower_case_table_names`) reads ALTER TABLE, TRUNCATE, RENAME TABLE and
   `DROP INDEX PRIMARY`. ALTER clauses are classified: index-only and option
   clauses (ADD INDEX, COMMENT, ALGORITHM, SET DEFAULT, ...) change nothing;
   unknown clauses count as changes. `SECONDARY_LOAD` / `SECONDARY_UNLOAD`
   (written to the binary log by the server) are ignored. Column renames are
   taken from `RENAME COLUMN a TO b` and `CHANGE a b`.
2. A table loaded or refreshed after the statement (load position after it)
   is skipped. A stale copy is renamed with its table but never refreshed: it
   comes back only through an explicit SECONDARY_LOAD, which checks the
   structure (DWL#3 step 3b; fixed 2026-11-12, the ALTER that sets the engine
   back could otherwise revive it unchecked). Otherwise the pending batch is committed first, then renames,
   then refreshes; the reader waits for each.
3. Rename (`rename_in_lake()`, under `apply_mutex`): `ALTER TABLE ... RENAME
   TO` in the same schema; across schemas `CREATE TABLE ... AS SELECT` and
   DROP (DuckLake renames within a schema). Registry row and entry follow.
4. Refresh: the reader sets a refresh request and runs
   `ALTER TABLE db.t SECONDARY_LOAD` in an internal session
   (`run_internal_statements()`: `srv_session` service, `skip_grants()` as the
   replica applier without PRIVILEGE_CHECKS_USER, `sql_log_bin=0`). The load
   sees the request and builds `t__duckdb_refresh` from InnoDB; for history
   tables it copies the archived rows of the old copy by column name
   (following renames) with `CAST` to the new types, NULL for new columns;
   then, in the same DuckLake transaction, drops the old table, renames the
   new one and writes the registry row with the new load position. The
   registry entry is updated in place (handlers hold its lock).
5. Refresh by name and lag: the load finds the table by its current name.
   When the reader is behind, the table may have been renamed after the
   statement (online schema change tools do ALTER then RENAME); the refresh
   then gets ER_NO_SUCH_TABLE. It is deferred, not failed: the copy is
   marked `refresh_pending` (registry and `lake.__mysql.tables`, with the
   column renames to apply, `pending_renames`; kept across restarts), its
   changes are skipped, and when the reader reaches the RENAME it renames the
   copy and refreshes the new name. Several DDLs before the RENAME chain their
   column renames (a->b, b->c gives a->c). A DROP unloads the copy when it
   runs, so nothing stays pending. Counted in `duckdb_apply_ddl_deferred`.
   A table swap (RENAME t TO t_old, t_new TO t) works by name: a refresh made
   after the swap loads the new `t`, and the earlier RENAME is skipped (its
   position is before the new load position).
6. Failure (no primary key, unsupported type, a value CAST cannot convert,
   retain_history removed, session unavailable): the table is marked stale
   (registry and lake), error log, `duckdb_apply_ddl_failures`; the reader
   goes on. Removing retain_history by DDL is refused: the archived rows
   would be lost.
   Since DWL#3 step 3b the refresh is a load in two DuckLake transactions
   (data, then swap and registry under `apply_mutex`), like every load.
7. `ALTER TABLE ... DROP PARTITION` calls the unload like DROP TABLE; the
   unload now keeps the copy for it (the refresh follows).

### Lessons from step 4d

- `srv_session_init_thread()` records the stack start and
  `srv_session_deinit_thread()` calls `my_thread_end()`: the reader attaches
  once at the top of its thread and detaches in place of `my_thread_end()`,
  never per statement.
- No `thread_local` in the plugin: it is built with
  `-ftls-model=initial-exec` and the static DuckDB libraries use the static
  TLS space a dlopen'ed library gets; one more `thread_local` made the load
  fail ("cannot allocate memory in static TLS block").
- Plugin service pointers (`srv_session_service`, `command_service`) must be
  exported from the plugin (they are, through `MYSQL_ADD_PLUGIN` and
  mysqlservices) even with VISIBILITY_HIDDEN.
- The first manual test (2026-11-11) ran TRUNCATE and RENAME back to back:
  the reader reached the TRUNCATE after the rename and the refresh found no
  table, which marked the copy stale. Fixed by the deferred refresh (item 5).
- The refresh needs no user account: `mysql.session` has no table
  privileges, and the security context service can add only SUPER and
  EXECUTE.

### JSON and spatial columns (2026-11-23)

- GEOMETRY rows: length prefix and bytes, as a BLOB.
- JSON rows: MySQL's binary JSON, turned into the text MySQL prints with the
  server's own code (`json_binary::parse_binary`, `Json_wrapper::to_string`
  with quoting, as `Field_json::val_str`), so the applier writes the text
  the load wrote (`duckdb_json.cc`, kept out of the row decoder, which the
  unit tests link without the server: `set_json_to_text`).
- `Json_wrapper::to_string` reads `current_thd` (`max_allowed_packet`): the
  reader thread now has an internal THD (`create_internal_thd()`, as the
  server's background threads), installed again when an internal session
  (refresh after DDL) leaves the thread without one. The first version
  crashed the reader there (MTR `json_geometry`).
- A partial JSON update (`PARTIAL_UPDATE_ROWS_EVENT`, written when a session
  sets `binlog_row_value_options = PARTIAL_JSON`) used to stop the whole
  applier; now that table's copy becomes stale, as after a failed refresh,
  and the applier goes on.

### Status variables

Implemented: `duckdb_apply_reader_state`, `duckdb_apply_reader_file`,
`duckdb_apply_reader_position`, `duckdb_apply_transactions_read`,
`duckdb_apply_row_events_read`, `duckdb_apply_rows_decoded`,
`duckdb_apply_changes`, `duckdb_apply_last_gtid`, `duckdb_apply_batches`,
`duckdb_apply_rows_written`, `duckdb_apply_large_transactions`,
`duckdb_apply_pieces`, `duckdb_apply_ddl_refreshes`,
`duckdb_apply_ddl_renames`, `duckdb_apply_ddl_failures`,
`duckdb_apply_ddl_deferred`,
`duckdb_applied_file`,
`duckdb_applied_position`, `duckdb_applied_gtid`.
Later: lag (seconds, transactions) and time per phase.
Added 2026-11-20 (monitoring): `duckdb_apply_lag_seconds`,
`duckdb_apply_lag_bytes`, `duckdb_apply_reader_state_code`,
`duckdb_memory_used_bytes`, `duckdb_temp_spill_bytes`. DWL#5 adds
`duckdb_apply_archived_rows`, `duckdb_apply_purge_transactions`.

### Open points

- Gap on a retain_history table: it cannot be reloaded until 3b. (3b
  implemented: resync, DWL#3 FR 17.)
- A refresh stops the applier for the other tables while it runs (it is a
  full reload of one table). Acceptable for the POC.
- DWL#6: no query may be offloaded to a table while it is refreshed or
  stale.
- Replica: not tested yet (manual test plan section 18, after 4d). (Tested
  2026-11-15/16, see Replicas.)
- MTR suite: postponed (2026-11-11); the Test Plan below lists the cases.
  (Since 2026-11-16: `applier`, `super_read_only`; later `binlog_reset`,
  `ddl_wait`, `json_geometry`.)
- XA transactions on loaded tables: not supported (reader stops).
- Lag status variables. (Done 2026-11-20, see Status variables.)

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
