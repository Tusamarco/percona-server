<!-- Copyright (c) 2026-present, Marco Tusa. All rights reserved. -->

# Monitoring the DuckDB plugin with PMM / Grafana

| File | What |
|---|---|
| `duckdb_pmm_dashboard.json` | Grafana dashboard "MySQL DuckDB Secondary Engine" (import into PMM 3 or any Grafana with a Prometheus / VictoriaMetrics source) |
| `queries-duckdb-medium.yml` | PMM custom queries: applier state and lag, copies, refresh progress, last refusal |
| `make_dashboard.py` | Generates the JSON; edit panels there and run `python3 make_dashboard.py` |

## Where the metrics come from

1. **Status counters: nothing to do.** PMM's `mysqld_exporter` already
   collects `SHOW GLOBAL STATUS` and `SHOW GLOBAL VARIABLES`. Every numeric
   `duckdb_*` status variable becomes `mysql_global_status_duckdb_*` (for
   example `mysql_global_status_duckdb_offload_queries`), every numeric
   variable `mysql_global_variables_duckdb_*`. `duckdb_enabled` (ON/OFF)
   becomes 1/0.
2. **Text values, per-table state, progress: custom queries.** The applier
   state, the binary log positions (as numbers, with the lag), the
   `DUCKDB_TABLES` rows, the Performance Schema stages and the last refusal
   are not numbers in `SHOW STATUS`: `queries-duckdb-medium.yml` turns them
   into metrics.

## Install

### 1. MySQL user for PMM

The standard PMM grants are enough (PROCESS for
`INFORMATION_SCHEMA.DUCKDB_TABLES`, BACKUP_ADMIN for
`performance_schema.log_status`, used for the lag):

```sql
GRANT SELECT, PROCESS, REPLICATION CLIENT, RELOAD, BACKUP_ADMIN ON *.* TO 'pmm'@'127.0.0.1';
```

### 2. Performance Schema stages (load and refresh progress)

Optional, for the "Refresh / load progress" panel (stage rows). In `my.cnf`:

```ini
[mysqld]
performance-schema-instrument            = 'stage/duckdb/%=ON'
performance-schema-consumer-events-stages-current = ON
```

or at runtime:

```sql
UPDATE performance_schema.setup_instruments SET ENABLED = 'YES', TIMED = 'YES'
 WHERE NAME LIKE 'stage/duckdb/%';
UPDATE performance_schema.setup_consumers SET ENABLED = 'YES'
 WHERE NAME = 'events_stages_current';
```

The refresh progress of `DUCKDB_TABLES` works without them.

### 3. Custom queries (on the MySQL host)

```bash
sudo cp queries-duckdb-medium.yml \
  /usr/local/percona/pmm/collectors/custom-queries/mysql/medium-resolution/
sudo systemctl restart pmm-agent
```

Check that the metrics arrive (PMM: Explore, or on the host):

```bash
curl -s -u pmm:<agent id> http://127.0.0.1:<exporter port>/metrics | grep -E 'mysql_duckdb_|global_status_duckdb'
```

(`pmm-admin list` shows the exporter's port and agent id.) If they do not,
the exporter logs why: `journalctl -u pmm-agent | grep -i custom`.

### 4. Dashboard

PMM 3: **Dashboards > New > Import**, upload `duckdb_pmm_dashboard.json`,
pick a folder (for example "MySQL"). The data source variable defaults to
PMM's `Metrics`; in another Grafana choose your Prometheus source.

## What the dashboard shows

| Row | Panels | Metrics |
|---|---|---|
| Overview | plugin ON/OFF, applier state, applier stopped, binlog files / bytes behind, offload ratio, DDL failures, copies by state, history copies, stale copies, last refusal | `duckdb_enabled`, `mysql_duckdb_applier_state_info`, `mysql_duckdb_applier_lag_*`, `duckdb_offload_queries` / `_refused`, `duckdb_apply_ddl_failures`, `mysql_duckdb_tables_copy`, `mysql_duckdb_last_refusal_info` |
| Query offload | queries run / refused / EXPLAIN per second, offload ratio, read-your-own-writes waits and timeouts, FULL scope guard, DECIMAL reruns | `duckdb_offload_*`, `duckdb_guard_*` |
| Binlog applier | transactions and row events read, rows decoded / changes / written, batches / large transactions / pieces, lag in bytes and files | `duckdb_apply_*`, `mysql_duckdb_applier_*` |
| History and DDL | archived rows, purge transactions, DDL refreshes / renames / deferred / failures | `duckdb_apply_archived_rows`, `_purge_transactions`, `_ddl_*` |
| Copies and progress | table of every copy (state colored), progress bars of refreshes, loads and DDL waits | `mysql_duckdb_tables_*`, `mysql_duckdb_stages_*` |
| Executor | busy threads against `duckdb_executor_threads`, queued tasks, tasks per second | `duckdb_executor_*` |
| Configuration | memory limit, threads, batch settings, offload thresholds | `mysql_global_variables_duckdb_*` |

Alert ideas (Grafana alerting / PMM alert rules):

| Alert | Expression |
|---|---|
| Applier stopped | `mysql_duckdb_applier_state_info{state="error"} == 1` |
| Applier behind | `mysql_duckdb_applier_lag_files > 0` for 10 min |
| A copy went stale | `increase(mysql_global_status_duckdb_apply_ddl_failures[10m]) > 0` |
| Queries fall back | `rate(mysql_global_status_duckdb_offload_wait_timeouts[5m]) > 0` |
| Executor saturated | `mysql_global_status_duckdb_executor_queue_length > 0` for 5 min |

## Not covered yet

- Lake size on disk: not exposed by the plugin; node_exporter shows the
  file system of `duckdb_data_path`, not the directory (design doc,
  "Production readiness", observability).
- Apply lag in seconds: the lag is in binary log bytes and files; seconds
  need the commit time of the last applied transaction (to add to the
  plugin).
- Not yet run against a live PMM: the queries and the dashboard are written
  from the plugin's status variables and PMM's documented formats; check
  them on the first install (step 3).

## Older plugin builds (before 2026-11-19)

`DUCKDB_TABLES` has no `REFRESH_ROWS_*` columns before T0. Replace the
`mysql_duckdb_tables` query with:

```yaml
mysql_duckdb_tables:
  query: "SELECT TABLE_SCHEMA AS table_schema, TABLE_NAME AS table_name, STATE AS state, RETAIN_HISTORY AS retain_history, 1 AS copy FROM information_schema.DUCKDB_TABLES"
  metrics:
    - table_schema: {usage: "LABEL", description: "Schema"}
    - table_name: {usage: "LABEL", description: "Table"}
    - state: {usage: "LABEL", description: "LOADED, STALE, REFRESH_PENDING"}
    - retain_history: {usage: "LABEL", description: "YES for history copies"}
    - copy: {usage: "GAUGE", description: "Always 1"}
```

and drop `mysql_duckdb_stages` (no stages before T0).
