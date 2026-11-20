<!-- Copyright (c) 2026-present, Marco Tusa. All rights reserved. -->

# Monitoring the DuckDB plugin with PMM / Grafana

| File | What |
|---|---|
| `duckdb_pmm_dashboard.json` | Grafana dashboard "MySQL DuckDB Secondary Engine" (import into PMM 3 or any Grafana with a Prometheus / VictoriaMetrics source) |
| `queries-duckdb-medium.yml` | PMM custom queries: copies, load / refresh progress, last refusal code |
| `make_dashboard.py` | Generates the JSON; edit panels there and run `python3 make_dashboard.py` |

## Where the metrics come from

1. **Status variables: nothing to do.** PMM's `mysqld_exporter` already
   collects `SHOW GLOBAL STATUS` and `SHOW GLOBAL VARIABLES`. Every numeric
   `duckdb_*` status variable becomes `mysql_global_status_duckdb_*` (for
   example `mysql_global_status_duckdb_offload_queries`), every numeric
   variable `mysql_global_variables_duckdb_*`. `duckdb_enabled` (ON/OFF)
   becomes 1/0. Since 2026-11-20 the plugin gives the monitoring numbers
   directly:

   | Status variable | Meaning |
   |---|---|
   | `duckdb_apply_lag_seconds` | age of the oldest transaction committed in MySQL and not yet in DuckDB (its `immediate_commit_timestamp`); 0 when everything is applied |
   | `duckdb_apply_lag_bytes` | binary log between the applied position and the end of the binary log, across files |
   | `duckdb_apply_reader_state_code` | 0 idle, 1 reading, 2 waiting to commit, 3 committing, 4 refreshing a copy, 5 error, 6 stopped |
   | `duckdb_memory_used_bytes` / `duckdb_temp_spill_bytes` | DuckDB memory and temporary storage (`duckdb_memory()`, sampled about every 5 s by the applier thread) |
   | `duckdb_offload_time_us` | total time in DuckDB of the queries counted in `duckdb_offload_queries`: `rate(time) / rate(queries)` is the average offloaded query time |

2. **Per-table state, progress, refusal reason: custom queries.**
   `queries-duckdb-medium.yml` reads `INFORMATION_SCHEMA.DUCKDB_TABLES` (one
   series per copy), the Performance Schema stages `stage/duckdb/%`
   (aggregated by stage, never per connection) and the reason code of the
   last refusal (`unsupported_function`, not the function name). It takes
   no server-wide lock (no `performance_schema.log_status`).

## Install

### 1. MySQL user for PMM

The standard PMM grants are enough (PROCESS for
`INFORMATION_SCHEMA.DUCKDB_TABLES`):

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
| Overview | plugin ON/OFF, applier state, lag (seconds, binlog bytes), average offloaded query time, offload ratio, DDL failures, copies by state, history copies, stale copies, last refusal code | `duckdb_enabled`, `duckdb_apply_reader_state_code`, `duckdb_apply_lag_*`, `duckdb_offload_time_us`, `duckdb_offload_queries` / `_refused`, `duckdb_apply_ddl_failures`, `mysql_duckdb_tables_copy`, `mysql_duckdb_last_refusal_info` |
| Query offload | queries run / refused / EXPLAIN per second, offload ratio, average offloaded query time, read-your-own-writes waits and timeouts, FULL scope guard, DECIMAL reruns | `duckdb_offload_*`, `duckdb_guard_*` |
| Binlog applier | transactions and row events read, rows decoded / changes / written, batches / large transactions / pieces, lag in seconds and bytes | `duckdb_apply_*` |
| History and DDL | archived rows, purge transactions, DDL refreshes / renames / deferred / failures | `duckdb_apply_archived_rows`, `_purge_transactions`, `_ddl_*` |
| Copies and progress | table of every copy (state colored), progress bars of refreshes and of loads / DDL waits by stage | `mysql_duckdb_tables_*`, `mysql_duckdb_stages_*` |
| DuckDB engine | memory used against `duckdb_memory_limit`, spill to disk | `duckdb_memory_used_bytes`, `duckdb_temp_spill_bytes` |
| Executor | busy threads against `duckdb_executor_threads`, queued tasks, tasks per second | `duckdb_executor_*` |
| Configuration | memory limit, threads, batch settings, offload thresholds | `mysql_global_variables_duckdb_*` |

Alert ideas (Grafana alerting / PMM alert rules):

| Alert | Expression |
|---|---|
| Applier stopped | `mysql_global_status_duckdb_apply_reader_state_code == 5` |
| Applier behind | `mysql_global_status_duckdb_apply_lag_seconds > 60` for 10 min |
| A copy went stale | `increase(mysql_global_status_duckdb_apply_ddl_failures[10m]) > 0` |
| Queries fall back | `rate(mysql_global_status_duckdb_offload_wait_timeouts[5m]) > 0` |
| DuckDB spills | `mysql_global_status_duckdb_temp_spill_bytes > 0` for 10 min |
| DuckDB memory near the limit | `mysql_global_status_duckdb_memory_used_bytes / mysql_global_variables_duckdb_memory_limit > 0.9` |
| Executor saturated | `mysql_global_status_duckdb_executor_queue_length > 0` for 5 min |

## Not covered yet

- Lake size on disk: not exposed by the plugin; node_exporter shows the
  file system of `duckdb_data_path`, not the directory (design doc,
  "Production readiness", observability).
- Not yet run against a live PMM: the queries and the dashboard are written
  from the plugin's status variables and PMM's documented formats; check
  them on the first install (step 3).

## Older plugin builds

Before 2026-11-20 the lag, state code, memory and query time status
variables do not exist: those panels stay empty. Before 2026-11-19 (T0)
`DUCKDB_TABLES` has no `REFRESH_ROWS_*` columns and there are no
`stage/duckdb/%` stages: replace the `mysql_duckdb_tables` query with

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

and drop `mysql_duckdb_stages`.
