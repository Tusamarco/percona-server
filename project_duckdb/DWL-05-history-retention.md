# DWL#5: History retention: purge marker and archive marks

Affects: Percona Server 9.7 — Status: implemented, verified on a throwaway instance (2026-11-12); MTR `history` in the suite

FR 1 to 6 were implemented with the applier (DWL#4 steps 4b and 4c); FR 9
and 10 on 2026-11-12. FR 7 and 8 were dropped (see the decisions).

Decisions (2026-11-12):

- The purge tag is fixed: `duckdb_avoid_purge`. A configurable tag
  (`duckdb_avoid_purge_gtid_tag`, implemented and removed the same day) put
  the tag in two places, the server variable and the purge job's SQL; any
  mismatch (a `SET GLOBAL` lost at restart, a change on one side only, a
  replica configured differently) silently turned purges into deletes and
  lost history. With a fixed tag the job's SQL is the only place. Lakes that
  have its table get it dropped at open (`lake.__mysql.purge_tags`).
- The tag belongs to one transaction of one session: `gtid_next` is a
  session variable and each transaction has its own GTID event, so the
  applier classifies every transaction separately. A purge in one session
  does not change the deletes of other sessions, even when they commit
  together (verified: a tagged purge open for 3 s and an untagged delete
  committed meanwhile in another session; the first archived, the second
  deleted).
- `duckdb_history_scope` and the `DUCKDB_HISTORY_READER` privilege are
  implemented now; nothing reads the scope until DWL#6.

## Description

### GENERAL IDEA

For tables with `retain_history`, InnoDB keeps only live data while DuckDB keeps
the full history. A purge job deletes old rows from InnoDB; those deletes must
not delete the rows in DuckDB. The applier recognises purge transactions by a
GTID tag and turns their deletes into archive marks: the row stays in DuckDB and
its hidden `__archived_at` column is set.

### USER INTERFACE

```sql
-- purge job (requires the TRANSACTION_GTID_TAG privilege)
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM orders WHERE created_at < NOW() - INTERVAL 90 DAY;
SET gtid_next = 'AUTOMATIC';
```

Any delete in a transaction without the tag is a business delete (cancellation,
legal erasure) and also removes the live row from DuckDB.

### VARIABLES

| Variable | Type | Default | Scope | Dynamic |
|---|---|---|---|---|
| `duckdb_history_scope` | enum LIVE, FULL | LIVE | global, session, SET_VAR hint | yes |

The purge tag is fixed: `duckdb_avoid_purge` (no variable).

`duckdb_history_scope` is declared here and used by DWL#6 and DWL#7.

### WHY A GTID TAG

Row events in the binlog do not carry session variables, so a session switch
such as `duckdb_purge_mode` cannot reach the applier without core changes. A
tagged GTID is written in the same transaction, replicates to replicas, and
setting it already requires a server privilege.

## Requirements

### Functional Requirements

1. A delete on a retain_history table in a transaction whose GTID tag is
   `duckdb_avoid_purge` sets `__archived_at` to the source commit time
   on the matching live row in DuckDB, instead of deleting it.
2. A delete on a retain_history table in any other transaction deletes the
   matching live row (`__archived_at IS NULL`) in DuckDB.
3. On tables without retain_history, all deletes are applied as deletes,
   whatever the tag.
4. Inserts and updates in a tagged transaction are applied normally.
5. A primary key archived and then inserted again gives one archived row and one
   live row; updates and deletes always target the live row only.
6. Collapse inside a batch: insert then tagged delete becomes an insert with
   `__archived_at` set.
7. (Dropped 2026-11-12: configurable tag.)
8. (Dropped 2026-11-12: tag change handling.)
9. `duckdb_history_scope` accepts LIVE and FULL at global and session level and
   through `SET_VAR`; switching to FULL requires the `DUCKDB_HISTORY_READER`
   dynamic privilege (registered by the plugin).
10. Status variables count archived rows and purge transactions.

### Non-Functional Requirements

1. Archiving costs no more than a delete in DuckDB (both write a delete file and
   new data).
2. The tag is the same on every server (fixed).

## Dependent Tasks

- DWL#4: Change propagation: binlog applier.

## High Level Architecture

### Contents

1. Tag detection
2. Delete classification
3. Live row targeting
4. Scope variable and privilege

### Tag detection

The reader (DWL#4) decodes the GTID event of each transaction, including the tag
of tagged GTIDs, and marks the transaction as purge when the tag is
`duckdb_avoid_purge`.

### Delete classification

In the prepare workers, a delete row event on a retain_history table becomes an
"archive" operation in a purge transaction and a "delete" otherwise.

### Live row targeting

All writes by key on retain_history tables add `__archived_at IS NULL`, because
the key is unique only among live rows.

### Scope variable and privilege

`duckdb_history_scope` is a plugin variable with `PLUGIN_VAR_HINTUPDATEABLE`, so
`SET_VAR` works without a server change. `DUCKDB_HISTORY_READER` is registered
through the dynamic privilege service at plugin init; the check callback refuses
FULL without it.

## Low Level Design

### Files

| File | Content |
|---|---|
| `duckdb_binlog_reader.cc` | Tag extraction from GTID events (`kAvoidPurgeTag`), counters |
| `duckdb_changes.cc` | Archive operation in the collapse rules |
| `duckdb_applier.cc` | Archive write, archived rows counted |
| `ha_duckdb.cc` | `duckdb_history_scope`, `DUCKDB_HISTORY_READER` registration (`dynamic_privilege_register` service), status variables |

### Archive write

Per table and batch:
`UPDATE lake.db.t SET __archived_at = <commit time> WHERE __archived_at IS NULL
AND pk IN (...)`. The commit time comes from the source transaction (original
commit timestamp in the GTID event, to verify its availability and precision).

### Collapse additions

| Sequence in batch | Result |
|---|---|
| insert, archive | insert with `__archived_at` set |
| update(s), archive | update final image with `__archived_at` set |
| archive, insert (same key) | archive old live row + insert new live row |

### Status variables

`duckdb_apply_archived_rows` (archive marks plus rows inserted and purged in
the same batch), `duckdb_apply_purge_transactions` (tagged transactions that
change loaded tables).

### Scope variable and privilege (implemented)

`duckdb_history_scope` is a session enum (`PLUGIN_VAR_HINTUPDATEABLE`, so
`SET_VAR` works). Its check refuses FULL without `DUCKDB_HISTORY_READER`
(`Security_context::has_global_grant`), at session and global level and in a
hint (a hint gives a warning and is ignored). The privilege is registered at
plugin init through the `dynamic_privilege_register` service and removed at
deinit. Existing accounts, root included, do not have it: `GRANT
DUCKDB_HISTORY_READER ON *.* TO ...`. DWL#6 must check it again at query time
for a session that inherits FULL from the global value.

### Open points

- Visibility of `__archived_at` to users in FULL scope: decided in DWL#6.
- Archive timestamp: resolved, the original commit timestamp of the GTID
  event (the current time when it is 0).
- A purge job that forgets the tag deletes history: inherent to the design.
  The purge job must reset `gtid_next` to `AUTOMATIC` right after the purge
  (the tag stays in effect for the session until then).

## Test Plan

MTR suite `duckdb`:

1. `purge_tagged`: tagged delete; rows remain in DuckDB with `__archived_at` set;
   InnoDB rows gone.
2. `purge_untagged`: untagged delete removes rows in DuckDB.
3. `purge_no_history`: tagged delete on a table without retain_history deletes.
4. `purge_reinsert`: purge then insert of the same key; one archived and one live
   row; later update touches only the live row.
5. `purge_collapse`: insert and tagged delete in one batch.
6. `purge_tag_change`: change the tag while the applier lags; old and new tags
   classified correctly.
7. `purge_tag_validation`: invalid tags rejected; lowercase normalisation.
8. `purge_replica`: tagged purge on the source archives on the replica.
9. `history_scope_privilege`: FULL refused without `DUCKDB_HISTORY_READER`.
