# DWL#5: History retention: purge marker and archive marks

Affects: Percona Server 9.7 — Status: Design, under review

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
| `duckdb_avoid_purge_gtid_tag` | string | `duckdb_avoid_purge` | global | yes |
| `duckdb_history_scope` | enum LIVE, FULL | LIVE | global, session, SET_VAR hint | yes |

`duckdb_history_scope` is declared here and used by DWL#6 and DWL#7.

### WHY A GTID TAG

Row events in the binlog do not carry session variables, so a session switch
such as `duckdb_purge_mode` cannot reach the applier without core changes. A
tagged GTID is written in the same transaction, replicates to replicas, and
setting it already requires a server privilege.

## Requirements

### Functional Requirements

1. A delete on a retain_history table in a transaction whose GTID tag equals
   `duckdb_avoid_purge_gtid_tag` sets `__archived_at` to the source commit time
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
7. `duckdb_avoid_purge_gtid_tag` accepts only valid GTID tags (letter or
   underscore first, then letters, digits, underscores, at most 32 characters),
   stores them in lowercase, and rejects an empty value.
8. Changing `duckdb_avoid_purge_gtid_tag` at runtime: transactions before the
   change position use the old tag, transactions after it the new tag; the switch
   is written to the error log.
9. `duckdb_history_scope` accepts LIVE and FULL at global and session level and
   through `SET_VAR`; switching to FULL requires the `DUCKDB_HISTORY_READER`
   dynamic privilege (registered by the plugin).
10. Status variables count archived rows and purge transactions.

### Non-Functional Requirements

1. Archiving costs no more than a delete in DuckDB (both write a delete file and
   new data).
2. The tag must be the same on all servers of a replication topology;
   documentation states it.

## Dependent Tasks

- DWL#4: Change propagation: binlog applier.

## High Level Architecture

### Contents

1. Tag detection
2. Delete classification
3. Live row targeting
4. Tag change handling
5. Scope variable and privilege

### Tag detection

The reader (DWL#4) decodes the GTID event of each transaction, including the tag
of tagged GTIDs, and marks the transaction as purge when the tag equals the
active value.

### Delete classification

In the prepare workers, a delete row event on a retain_history table becomes an
"archive" operation in a purge transaction and a "delete" otherwise.

### Live row targeting

All writes by key on retain_history tables add `__archived_at IS NULL`, because
the key is unique only among live rows.

### Tag change handling

The check callback records the binlog position at the moment of the change. The
reader keeps the old tag active for transactions before that position.

### Scope variable and privilege

`duckdb_history_scope` is a plugin variable with `PLUGIN_VAR_HINTUPDATEABLE`, so
`SET_VAR` works without a server change. `DUCKDB_HISTORY_READER` is registered
through the dynamic privilege service at plugin init; the check callback refuses
FULL without it.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/applier/reader.cc` | Tag extraction from GTID events |
| `storage/duckdb/applier/batch.cc` | Archive operation in collapse rules |
| `storage/duckdb/applier/committer.cc` | Archive write |
| `storage/duckdb/duckdb_sysvars.cc` | `duckdb_avoid_purge_gtid_tag`, `duckdb_history_scope` |
| `storage/duckdb/duckdb_privileges.cc` | `DUCKDB_HISTORY_READER` registration |

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

### Tag validation

Same rules as the server's GTID tag parser; to verify whether the event library
or `libs/mysql/gtid` exposes a reusable validator.

### Status variables

`duckdb_archived_rows`, `duckdb_purge_transactions`, `duckdb_active_purge_tag`.

### Open points

- Visibility of `__archived_at` to users in FULL scope: open question.
- Source of the archive timestamp: original commit timestamp, to verify.

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
