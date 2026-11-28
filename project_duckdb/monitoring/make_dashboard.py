#!/usr/bin/env python3
# Copyright (c) 2026-present, Marco Tusa. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# Writes duckdb_pmm_dashboard.json, the Grafana / PMM dashboard of the DuckDB
# secondary engine plugin. Edit the panels here and run it again:
#   python3 make_dashboard.py
import json
import os

DS = {"type": "prometheus", "uid": "${datasource}"}
SVC = 'service_name=~"$service_name"'


def gs(name):
    """A duckdb_* status counter as PMM collects it."""
    return f"mysql_global_status_{name}{{{SVC}}}"


def gv(name):
    return f"mysql_global_variables_{name}{{{SVC}}}"


def rate(name):
    return f"rate({gs(name)}[$__rate_interval])"


_id = [0]


def nid():
    _id[0] += 1
    return _id[0]


def target(expr, legend="{{service_name}}", instant=False, fmt=None, ref="A"):
    t = {"datasource": DS, "expr": expr, "legendFormat": legend, "refId": ref}
    if instant:
        t["instant"] = True
        t["range"] = False
    if fmt:
        t["format"] = fmt
    return t


def refs(targets):
    for i, t in enumerate(targets):
        t["refId"] = chr(ord("A") + i)
    return targets


def timeseries(title, targets, x, y, w=12, h=8, unit="short", desc="",
               stack=False, bars=False, minv=0):
    return {
        "id": nid(), "type": "timeseries", "title": title, "description": desc,
        "datasource": DS, "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "targets": refs(targets),
        "fieldConfig": {
            "defaults": {
                "unit": unit, "min": minv,
                "custom": {
                    "drawStyle": "bars" if bars else "line",
                    "lineWidth": 1, "fillOpacity": 60 if bars else 10,
                    "stacking": {"mode": "normal" if stack else "none"},
                    "showPoints": "never", "spanNulls": True,
                },
            },
            "overrides": [],
        },
        "options": {"legend": {"displayMode": "table", "placement": "bottom",
                               "calcs": ["mean", "max", "lastNotNull"]},
                    "tooltip": {"mode": "multi", "sort": "desc"}},
    }


def stat(title, targets, x, y, w=4, h=4, unit="short", desc="",
         thresholds=None, mappings=None, text_mode="value", color_mode="value",
         decimals=None, graph=False):
    d = {"unit": unit,
         "thresholds": {"mode": "absolute",
                        "steps": thresholds or [{"color": "blue",
                                                 "value": None}]},
         "mappings": mappings or [], "color": {"mode": "thresholds"}}
    if decimals is not None:
        d["decimals"] = decimals
    return {
        "id": nid(), "type": "stat", "title": title, "description": desc,
        "datasource": DS, "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "targets": refs(targets),
        "fieldConfig": {"defaults": d, "overrides": []},
        "options": {"reduceOptions": {"calcs": ["lastNotNull"], "fields": "",
                                      "values": False},
                    "textMode": text_mode, "colorMode": color_mode,
                    "graphMode": "area" if graph else "none",
                    "justifyMode": "auto", "orientation": "auto"},
    }


def row(title, y, collapsed=False):
    return {"id": nid(), "type": "row", "title": title, "collapsed": collapsed,
            "gridPos": {"x": 0, "y": y, "w": 24, "h": 1}, "panels": []}


GREEN_RED = [{"color": "green", "value": None}, {"color": "red", "value": 1}]
STATE_CODES = [{"type": "value", "options": {
    "0": {"text": "idle", "color": "green", "index": 0},
    "1": {"text": "reading", "color": "blue", "index": 1},
    "2": {"text": "waiting to commit", "color": "blue", "index": 2},
    "3": {"text": "committing", "color": "blue", "index": 3},
    "4": {"text": "refreshing", "color": "purple", "index": 4},
    "5": {"text": "ERROR", "color": "red", "index": 5},
    "6": {"text": "stopped", "color": "orange", "index": 6}}}]
ON_OFF = [{"type": "value", "options": {
    "0": {"text": "OFF", "color": "red", "index": 0},
    "1": {"text": "ON", "color": "green", "index": 1}}}]

panels = []
y = 0

# ---------------------------------------------------------------- Overview
panels.append(row("Overview", y)); y += 1
panels += [
    stat("Plugin", [target(gs("duckdb_enabled"))], 0, y, w=3,
         desc="duckdb_enable: OFF means the server behaves as if the plugin "
              "were not loaded.", mappings=ON_OFF,
         thresholds=[{"color": "red", "value": None},
                     {"color": "green", "value": 1}]),
    stat("Applier state",
         [target(gs("duckdb_apply_reader_state_code"), "state")], 3, y, w=4,
         desc="duckdb_apply_reader_state_code: idle (everything applied), "
              "reading, waiting to commit, committing, refreshing (a copy "
              "after DDL), error (the applier stopped: see the error log), "
              "stopped.",
         mappings=STATE_CODES, color_mode="background",
         thresholds=[{"color": "green", "value": None},
                     {"color": "red", "value": 5}]),
    stat("Lag",
         [target(gs("duckdb_apply_lag_seconds"), "lag")], 7, y, w=3, unit="s",
         decimals=1,
         desc="duckdb_apply_lag_seconds: age of the oldest transaction "
              "committed in MySQL and not yet in DuckDB (0 when everything "
              "is applied), as Seconds_Behind_Source.",
         thresholds=[{"color": "green", "value": None},
                     {"color": "orange", "value": 5},
                     {"color": "red", "value": 60}], graph=True),
    stat("Lag (binlog bytes)",
         [target(gs("duckdb_apply_lag_bytes"), "bytes")], 10, y, w=3,
         unit="bytes",
         desc="duckdb_apply_lag_bytes: binary log between the applied "
              "position and the end of the binary log, across files.",
         thresholds=[{"color": "green", "value": None},
                     {"color": "orange", "value": 10 * 1024 * 1024},
                     {"color": "red", "value": 100 * 1024 * 1024}]),
    stat("Avg offloaded query",
         [target(f"sum(increase({gs('duckdb_offload_time_us')}[$__range])) / "
                 f"clamp_min(sum(increase({gs('duckdb_offload_queries')}[$__range])), 1) / 1e6",
                 "avg")], 13, y, w=3, unit="s", decimals=3,
         desc="Average time in DuckDB of the queries it ran, over the time "
              "range (duckdb_offload_time_us / duckdb_offload_queries)."),
    stat("Offload ratio",
         [target(f"100 * sum(increase({gs('duckdb_offload_queries')}[$__range])) / "
                 f"clamp_min(sum(increase({gs('duckdb_offload_queries')}[$__range])) + "
                 f"sum(increase({gs('duckdb_offload_refused')}[$__range])), 1)",
                 "ratio")], 16, y, w=4, unit="percent", decimals=1,
         desc="Queries run in DuckDB / queries tried in DuckDB, over the "
              "time range. Refused queries ran on InnoDB (or failed with "
              "FORCED).",
         thresholds=[{"color": "red", "value": None},
                     {"color": "orange", "value": 50},
                     {"color": "green", "value": 90}]),
    stat("DDL failures (range)",
         [target(f"sum(increase({gs('duckdb_apply_ddl_failures')}[$__range]))",
                 "failures")], 20, y, w=4,
         desc="Copies the applier could not follow after a DDL: they are "
              "STALE now (see the table below).", thresholds=GREEN_RED,
         decimals=0),
]
y += 4
panels += [
    stat("Copies by state",
         [target(f"sum by (state) (mysql_duckdb_tables_copy{{{SVC}}})",
                 "{{state}}", instant=True)], 0, y, w=10,
         desc="DuckDB copies (INFORMATION_SCHEMA.DUCKDB_TABLES): LOADED, "
              "REFRESHING, REFRESH_PENDING, STALE.", text_mode="value_and_name",
         thresholds=[{"color": "green", "value": None}]),
    stat("History copies",
         [target(f"count(mysql_duckdb_tables_copy{{{SVC},retain_history=\"YES\"}}) or vector(0)",
                 "history")], 10, y, w=3,
         desc="Copies with retain_history: their archived rows exist only "
              "in the lake."),
    stat("Stale copies",
         [target(f"count(mysql_duckdb_tables_copy{{{SVC},state=\"STALE\"}}) or vector(0)",
                 "stale")], 13, y, w=3, thresholds=GREEN_RED,
         desc="Copies no longer applied (DROP / detach of a history table, "
              "or a failed refresh). Reattach, SECONDARY_UNLOAD or "
              "duckdb_discard_stale_copy."),
    stat("Last refusal",
         [target(f"mysql_duckdb_last_refusal_info{{{SVC}}}", "{{reason}}",
                 instant=True)], 16, y, w=8, text_mode="name",
         desc="duckdb_offload_last_refusal: why the last query did not run "
              "in DuckDB."),
]
y += 4

# ----------------------------------------------------------- Query offload
panels.append(row("Query offload", y)); y += 1
panels += [
    timeseries("Queries", [
        target(f"sum({rate('duckdb_offload_queries')})", "run in DuckDB"),
        target(f"sum({rate('duckdb_offload_refused')})", "refused (InnoDB)"),
        target(f"sum({rate('duckdb_offload_explained')})", "EXPLAIN")],
        0, y, unit="reqps",
        desc="duckdb_offload_queries (run to the end), duckdb_offload_refused, "
             "duckdb_offload_explained."),
    timeseries("Offload ratio", [
        target(f"100 * sum({rate('duckdb_offload_queries')}) / clamp_min("
               f"sum({rate('duckdb_offload_queries')}) + "
               f"sum({rate('duckdb_offload_refused')}), 1e-9)", "ratio")],
        12, y, w=6, unit="percent",
        desc="Share of the queries tried in DuckDB that ran there."),
    timeseries("Average offloaded query time", [
        target(f"sum({rate('duckdb_offload_time_us')}) / clamp_min("
               f"sum({rate('duckdb_offload_queries')}), 1e-9) / 1e6", "avg")],
        18, y, w=6, unit="s",
        desc="duckdb_offload_time_us / duckdb_offload_queries: time in DuckDB "
             "(execution and fetch, plus a first run during optimization) "
             "per query run there."),
]
y += 8
panels += [
    timeseries("Read your own writes", [
        target(f"sum({rate('duckdb_offload_waits')})", "waits"),
        target(f"sum({rate('duckdb_offload_wait_timeouts')})", "timeouts (ran on InnoDB)")],
        0, y, w=8, unit="reqps",
        desc="Queries that waited for the session's last commit "
             "(duckdb_offload_wait_applied) and those that hit "
             "duckdb_offload_wait_timeout_ms. Timeouts growing: the applier "
             "is behind."),
    timeseries("FULL scope guard", [
        target(f"sum({rate('duckdb_guard_forced')})", "sent to DuckDB only"),
        target(f"sum({rate('duckdb_guard_rejected')})", "rejected")],
        8, y, w=8, unit="reqps",
        desc="duckdb_guard: statements on history tables under FULL scope."),
    timeseries("DECIMAL reruns", [
        target(f"sum({rate('duckdb_offload_decimal_reruns')})", "reruns")],
        16, y, w=8, unit="reqps",
        desc="Runs again with fewer decimals (duckdb_decimal_overflow=ROUND)."),
]
y += 8

# ----------------------------------------------------------- Binlog applier
panels.append(row("Binlog applier", y)); y += 1
panels += [
    timeseries("Read from the binary log", [
        target(f"sum({rate('duckdb_apply_transactions_read')})", "transactions"),
        target(f"sum({rate('duckdb_apply_row_events_read')})", "row events")],
        0, y, w=8, unit="ops",
        desc="Transactions and row events read (all tables)."),
    timeseries("Rows", [
        target(f"sum({rate('duckdb_apply_rows_decoded')})", "decoded"),
        target(f"sum({rate('duckdb_apply_changes')})", "changes (after collapse)"),
        target(f"sum({rate('duckdb_apply_rows_written')})", "written to DuckDB")],
        8, y, w=8, unit="ops",
        desc="Rows of loaded tables decoded, changes after collapsing per "
             "primary key, rows written to DuckDB."),
    timeseries("Batches", [
        target(f"sum({rate('duckdb_apply_batches')})", "batches (DuckLake commits)"),
        target(f"sum({rate('duckdb_apply_large_transactions')})", "large transactions"),
        target(f"sum({rate('duckdb_apply_pieces')})", "pieces")],
        16, y, w=8, unit="ops",
        desc="One DuckLake commit per batch; large transactions are applied "
             "in pieces (duckdb_apply_batch_max_rows)."),
]
y += 8
panels += [
    timeseries("Applier lag (seconds)", [
        target(gs("duckdb_apply_lag_seconds"), "{{service_name}}")],
        0, y, unit="s",
        desc="duckdb_apply_lag_seconds: age of the oldest transaction "
             "committed in MySQL and not yet in DuckDB (0 when caught up)."),
    timeseries("Applier lag (binlog bytes)", [
        target(gs("duckdb_apply_lag_bytes"), "{{service_name}}")],
        12, y, unit="bytes",
        desc="duckdb_apply_lag_bytes: binary log not yet applied, across "
             "files (it does not drop when a file rotates)."),
]
y += 8

# ----------------------------------------------------------- History & DDL
panels.append(row("History and DDL", y)); y += 1
panels += [
    timeseries("History", [
        target(f"sum({rate('duckdb_apply_archived_rows')})", "rows archived"),
        target(f"sum({rate('duckdb_apply_purge_transactions')})", "purge transactions")],
        0, y, unit="ops",
        desc="Deletes in transactions tagged duckdb_avoid_purge become "
             "archived rows in DuckDB."),
    timeseries("DDL handled by the applier", [
        target(f"sum(increase({gs('duckdb_apply_ddl_refreshes')}[$__interval]))", "refreshes"),
        target(f"sum(increase({gs('duckdb_apply_ddl_renames')}[$__interval]))", "renames"),
        target(f"sum(increase({gs('duckdb_apply_ddl_deferred')}[$__interval]))", "deferred"),
        target(f"sum(increase({gs('duckdb_apply_ddl_failures')}[$__interval]))", "failures")],
        12, y, bars=True, stack=True,
        desc="Refreshes after DDL, RENAMEs followed, refreshes deferred to a "
             "later rename, failures (copy marked STALE)."),
]
y += 8

# ------------------------------------------------ Copies and refresh progress
panels.append(row("Copies and progress", y)); y += 1
panels.append({
    "id": nid(), "type": "table", "title": "DuckDB copies",
    "description": "INFORMATION_SCHEMA.DUCKDB_TABLES (custom query).",
    "datasource": DS, "gridPos": {"x": 0, "y": y, "w": 12, "h": 10},
    "targets": refs([target(f"mysql_duckdb_tables_copy{{{SVC}}}", "",
                            instant=True, fmt="table")]),
    "transformations": [{"id": "organize", "options": {
        "excludeByName": {"Time": True, "Value": True, "__name__": True,
                          "agent_id": True, "agent_type": True,
                          "environment": True, "instance": True, "job": True,
                          "machine_id": True, "node_id": True,
                          "node_name": True, "node_type": True,
                          "service_id": True, "service_type": True,
                          "cluster": True, "replication_set": True,
                          "region": True, "az": True, "node_model": True,
                          "custom_label": True},
        "renameByName": {"table_schema": "Schema", "table_name": "Table",
                         "state": "State", "retain_history": "History",
                         "service_name": "Service"}}}],
    "fieldConfig": {"defaults": {}, "overrides": [{
        "matcher": {"id": "byName", "options": "State"},
        "properties": [{"id": "custom.cellOptions",
                        "value": {"type": "color-background"}},
                       {"id": "mappings", "value": [{"type": "value", "options": {
                           "LOADED": {"color": "green", "index": 0},
                           "REFRESHING": {"color": "blue", "index": 1},
                           "REFRESH_PENDING": {"color": "orange", "index": 2},
                           "STALE": {"color": "red", "index": 3}}}]}]}]},
    "options": {"showHeader": True, "sortBy": [{"displayName": "Table"}]},
})
panels.append({
    "id": nid(), "type": "bargauge", "title": "Refresh / load progress",
    "description": "Copies being refreshed after DDL (DUCKDB_TABLES) and "
                   "loads or waits in progress (Performance Schema stages "
                   "stage/duckdb/%, README.md). Rows done / InnoDB's estimate.",
    "datasource": DS, "gridPos": {"x": 12, "y": y, "w": 12, "h": 10},
    "targets": refs([
        target(f"100 * mysql_duckdb_tables_refresh_rows_done{{{SVC}}} / "
               f"(mysql_duckdb_tables_refresh_rows_total{{{SVC}}} > 0)",
               "refresh {{table_schema}}.{{table_name}}", instant=True),
        target(f"100 * mysql_duckdb_stages_work_completed{{{SVC}}} / "
               f"(mysql_duckdb_stages_work_estimated{{{SVC}}} > 0)",
               "{{stage}}", instant=True)]),
    "fieldConfig": {"defaults": {"unit": "percent", "min": 0, "max": 100,
                                 "thresholds": {"mode": "absolute", "steps": [
                                     {"color": "blue", "value": None}]}},
                    "overrides": []},
    "options": {"displayMode": "gradient", "orientation": "horizontal",
                "showUnfilled": True,
                "reduceOptions": {"calcs": ["lastNotNull"], "values": False}},
})
y += 10

# -------------------------------------------------------------- DuckDB engine
panels.append(row("DuckDB engine", y)); y += 1
panels += [
    timeseries("DuckDB memory", [
        target(gs("duckdb_memory_used_bytes"), "used"),
        target(gv("duckdb_memory_limit"), "limit (duckdb_memory_limit)")],
        0, y, unit="bytes",
        desc="duckdb_memory_used_bytes (duckdb_memory(), sampled about every "
             "5 s) against duckdb_memory_limit."),
    timeseries("Spill to disk", [
        target(gs("duckdb_temp_spill_bytes"), "temporary storage")],
        12, y, unit="bytes",
        desc="duckdb_temp_spill_bytes: data DuckDB moved to its temporary "
             "directory (tmp/ in the lake) because of the memory limit."),
]
y += 8

# ----------------------------------------------------------------- Executor
panels.append(row("Executor (threads that run DuckDB calls)", y)); y += 1
panels += [
    timeseries("Executor threads", [
        target(gs("duckdb_executor_threads_busy"), "busy"),
        target(gv("duckdb_executor_threads"), "configured"),
        target(gs("duckdb_executor_queue_length"), "queued tasks")],
        0, y, unit="short",
        desc="Busy executor threads against duckdb_executor_threads, and "
             "tasks waiting. A queue that stays above 0: raise "
             "duckdb_executor_threads."),
    timeseries("Executor tasks", [
        target(f"sum({rate('duckdb_executor_tasks')})", "tasks")],
        12, y, unit="ops", desc="DuckDB calls run by the executor."),
]
y += 8
panels += [
    timeseries("Query slots", [
        target(gs("duckdb_queries_running"), "running"),
        target(gv("duckdb_max_concurrent_queries"), "configured (0: max(2, cores / 4))")],
        0, y, unit="short",
        desc="Offloaded queries holding a slot against "
             "duckdb_max_concurrent_queries (0 means the default, "
             "max(2, cores / 4))."),
    timeseries("Query slot waits", [
        target(f"sum({rate('duckdb_query_slot_waits')})", "waits"),
        target(f"sum({rate('duckdb_query_slot_wait_us')}) / 1000000",
               "seconds waited per second")],
        12, y, unit="short",
        desc="Queries that waited for a slot. Many waits with little spill: "
             "raise duckdb_max_concurrent_queries; spill with waits: keep "
             "it or lower it."),
]
y += 8

# ------------------------------------------------------------ Configuration
panels.append(row("Configuration", y)); y += 1
cfg = [("Memory limit", "duckdb_memory_limit", "bytes"),
       ("DuckDB threads", "duckdb_threads", "short"),
       ("Executor threads", "duckdb_executor_threads", "short"),
       ("Max concurrent queries", "duckdb_max_concurrent_queries", "short"),
       ("Batch max trx", "duckdb_apply_batch_max_trx", "short"),
       ("Batch max rows", "duckdb_apply_batch_max_rows", "short"),
       ("Batch max delay", "duckdb_apply_batch_max_delay_ms", "ms"),
       ("Offload min rows", "duckdb_offload_min_rows", "short"),
       ("Wait timeout", "duckdb_offload_wait_timeout_ms", "ms")]
for i, (title, var, unit) in enumerate(cfg):
    panels.append(stat(title, [target(gv(var))], (i % 8) * 3, y + (i // 8) * 3,
                       w=3, h=3, unit=unit, desc=f"{var} (global value)."))
y += 3 * ((len(cfg) + 7) // 8)

dashboard = {
    "title": "MySQL DuckDB Secondary Engine",
    "uid": "mysql-duckdb-secondary-engine",
    "description": "DuckDB secondary engine plugin: query offload, binlog "
                   "applier, copies, history, executor. Status counters from "
                   "PMM's mysqld_exporter; applier lag, copies and progress "
                   "from the custom queries in queries-duckdb-medium.yml (copies, stages, last refusal code).",
    "tags": ["Percona", "MySQL", "DuckDB"],
    "editable": True, "graphTooltip": 1, "schemaVersion": 39,
    "time": {"from": "now-3h", "to": "now"}, "refresh": "30s",
    "timezone": "browser",
    "templating": {"list": [
        {"name": "datasource", "label": "Data source", "type": "datasource",
         "query": "prometheus", "current": {"text": "Metrics", "value": "Metrics"},
         "hide": 0},
        {"name": "service_name", "label": "Service", "type": "query",
         "datasource": DS,
         "query": {"query": "label_values(mysql_global_status_duckdb_enabled, service_name)",
                   "refId": "svc"},
         "definition": "label_values(mysql_global_status_duckdb_enabled, service_name)",
         "refresh": 2, "multi": True, "includeAll": True,
         "current": {"text": "All", "value": "$__all"}, "sort": 1, "hide": 0},
    ]},
    "annotations": {"list": []},
    "panels": panels,
}

out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "duckdb_pmm_dashboard.json")
with open(out, "w") as f:
    json.dump(dashboard, f, indent=2)
    f.write("\n")
print(f"wrote {out}: {len(panels)} panels")
