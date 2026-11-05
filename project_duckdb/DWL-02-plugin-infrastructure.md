# DWL#2: Plugin infrastructure, configuration and DuckLake storage

Affects: Percona Server 9.7 — Status: Design, under review

## Description

### GENERAL IDEA

We create the `duckdb` secondary engine plugin in `storage/duckdb/`, starting
from a copy of the MOCK engine. This worklog covers what every other function
needs: plugin registration, system variables, the DuckDB instance with the
DuckLake attached, the startup sequence with directory and catalog creation and
location checks, per-session DuckDB connections, `duckdb_enable`, and the
status variable framework. It does not load data or offload queries (DWL#3,
DWL#6).

### PLUGIN LIBRARY

`ha_duckdb.so` declares two plugins in one `mysql_declare_plugin` block:
`duckdb` (MYSQL_STORAGE_ENGINE_PLUGIN, secondary engine) and `duckdb_guard`
(audit plugin, implemented in DWL#7). Both load with one `plugin-load-add`.

### SYSTEM VARIABLES

All are plugin variables of `duckdb`, so the server adds the `duckdb_` prefix.

| Variable | Type | Default | Scope | Dynamic |
|---|---|---|---|---|
| `duckdb_enable` | BOOL | TRUE | global | yes |
| `duckdb_data_path` | string | empty = `<datadir>/duckdb_data` | global | no |
| `duckdb_memory_limit` | size | none, mandatory | global | yes |
| `duckdb_threads` | int | none, mandatory | global | yes |
| `duckdb_encryption` | BOOL | OFF | global | no (not in the POC: ON fails with "not supported") |

Variables of later worklogs (`duckdb_history_scope`,
`duckdb_avoid_purge_gtid_tag`, `duckdb_apply_*`) are declared in this plugin but
specified in DWL#4, DWL#5.

### DUCKLAKE STORAGE

```
<path>/                      duckdb_data_path, default <datadir>/duckdb_data
  catalog.ducklake           DuckLake catalog (DuckDB file)
  data/                      Parquet data and delete files
  tmp/                       DuckDB spill directory
```

One in-memory DuckDB instance per mysqld with `ATTACH 'ducklake:<path>/catalog.ducklake'
AS lake (DATA_PATH '<path>/data/')`.

### CONFIGURATION EXAMPLE

```ini
[mysqld]
plugin-load-add           = ha_duckdb.so
loose-duckdb_memory_limit = 8G
loose-duckdb_threads      = 8
```

## Requirements

### Functional Requirements

1. The plugin library registers the `duckdb` secondary engine plugin and the
   `duckdb_guard` audit plugin.
2. `duckdb_memory_limit` and `duckdb_threads` must be set; if either is missing,
   plugin initialization fails with an error naming the variable and the plugin
   is not loaded. This applies also when `duckdb_enable=FALSE`.
3. Setting `duckdb_memory_limit` or `duckdb_threads` to 0 at runtime is rejected.
4. `duckdb_data_path` empty means `<datadir>/duckdb_data`; a relative value is
   resolved against the datadir; an absolute value is used as is.
5. At startup, missing `duckdb_data/`, `data/` and `tmp/` are created with mode
   0750, and a missing `catalog.ducklake` is created.
6. If `data/` contains files and `catalog.ducklake` is missing, plugin
   initialization fails with an error asking the DBA to restore the catalog.
7. Plugin initialization fails if the resolved path is the datadir itself or
   inside a schema directory, or if the directory is world-writable.
8. A warning is logged if the path is inside `secure_file_priv`, if
   `secure_file_priv` is empty, or if the directory is world-readable,
   group-writable, or not owned by the mysqld user.
9. A catalog locked by another process gives a readable error.
10. After the attach, DuckDB extension autoload and autoinstall are off and
    external access is disabled.
11. With `duckdb_enable=FALSE` at startup the plugin does not open DuckDB or
    touch the directory.
12. Setting `duckdb_enable=FALSE` at runtime makes the server behave as if the
    plugin were not loaded for new statements; running offloaded statements
    finish; lake data is never deleted.
13. Setting `duckdb_enable=TRUE` at runtime runs the full startup sequence; on
    failure the SET fails and the value stays FALSE.
14. Changing `duckdb_memory_limit` or `duckdb_threads` at runtime applies the
    value to DuckDB; if DuckDB refuses it, the SET fails and the old value stays.
15. Each session that uses DuckDB gets its own DuckDB connection, released when
    the session ends.
16. `duckdb_encryption=ON` fails plugin initialization with "not supported in
    this version" (POC).
17. Global variables require `SYSTEM_VARIABLES_ADMIN`.

### Non-Functional Requirements

1. Work for dynamic variables never runs under `LOCK_global_system_variables`.
2. Loading the plugin with `duckdb_enable=FALSE` adds no measurable overhead to
   statements on InnoDB tables.
3. DuckDB exceptions never propagate into server code.
4. DuckDB symbols are not exported from `ha_duckdb.so`.

## Dependent Tasks

- DWL#1: Build integration of DuckDB, DuckLake and CRoaring.

## High Level Architecture

### Contents

1. Plugin declaration
2. Variables
3. Startup sequence
4. Lake manager
5. Session connections
6. duckdb_enable
7. Status variables

### Plugin declaration

`storage/duckdb/ha_duckdb.cc` holds the handlerton setup copied from the MOCK
engine (`Init`, `Deinit`, `Create`), with `HTON_IS_SECONDARY_ENGINE`. In this
worklog the offload hooks are present but reject every statement; DWL#3 and DWL#6
fill them.

### Variables

Declared with `MYSQL_SYSVAR_*`. Real work for dynamic variables is done in the
check callback, which runs without `LOCK_global_system_variables` and can fail;
the update callback only stores the value (sql/sql_plugin_var.cc). An internal
mutex serializes concurrent changes.

### Startup sequence

```
Init()
 ├─ check mandatory variables          → fail if missing
 ├─ if !duckdb_enable                  → return 0 (inactive)
 └─ Lake::open()
      ├─ resolve path, realpath
      ├─ location checks                → fail or warn
      ├─ create directories
      ├─ catalog missing but data/ has files → fail
      ├─ register static extensions, open DuckDB in memory
      ├─ ATTACH ducklake, set temp_directory, memory_limit, threads
      ├─ hardening settings
      └─ reconcile with the data dictionary (DWL#3)
```

### Lake manager

A singleton that owns the DuckDB database handle and the attach state, and gives
out connections. Opened by `Init()` or by `duckdb_enable=TRUE`; closed by
`Deinit()` or `duckdb_enable=FALSE` after running offloaded statements finish.

### Session connections

A per-THD structure in the handlerton THD slot (`thd_get_ha_data` /
`thd_set_ha_data`) holds the DuckDB connection, created on first use and closed
in `hton->close_connection`.

### duckdb_enable

Every hook checks the flag itself and returns the same result as when the engine
is absent, because the secondary engine paths check whether the plugin is loaded,
not the handlerton state (for example `plugin_is_ready()` in
`secondary_engine_load_table`, sql/sql_table.cc).

### Status variables

Framework for `SHOW STATUS LIKE 'duckdb_%'`; this worklog adds `duckdb_enabled`
(effective state), `duckdb_lake_path`, `duckdb_memory_used`.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/CMakeLists.txt` | `MYSQL_ADD_PLUGIN(duckdb ... STORAGE_ENGINE MODULE_ONLY LINK_LIBRARIES ext::duckdb)`, `-fvisibility=hidden`, version script |
| `storage/duckdb/ha_duckdb.h/.cc` | Handler class `ha_duckdb` (from `ha_mock`), handlerton setup, plugin declaration |
| `storage/duckdb/duckdb_sysvars.h/.cc` | Variable declarations, check and update callbacks |
| `storage/duckdb/duckdb_lake.h/.cc` | Lake manager: path, checks, directories, open, attach, close |
| `storage/duckdb/duckdb_session.h/.cc` | Per-THD connection |
| `storage/duckdb/duckdb_status.cc` | Status variables |
| `storage/duckdb/duckdb.map` | Linker version script exporting only plugin symbols |

File names are a proposal.

### Mandatory variable check

Default 0 means "not set". `Init()` checks both and, if one is 0, logs
`DuckDB: duckdb_<name> is not set; add it to the configuration file (use the
loose- prefix). Plugin not loaded.` and returns non-zero. The check callbacks
reject 0.

### Path resolution and checks

1. Value empty → `mysql_real_data_home` + `duckdb_data`; relative → joined with
   `mysql_real_data_home`.
2. Create missing components, then `realpath()`.
3. Refuse: realpath equals the datadir realpath; realpath's parent is the datadir
   and the name is an existing schema (dictionary lookup, to verify the cheapest
   way at plugin init); `st_mode & S_IWOTH`.
4. Warn: inside `opt_secure_file_priv`; `opt_secure_file_priv` empty;
   `S_IROTH`; `S_IWGRP`; `st_uid != geteuid()`.

### DuckDB open

1. `duckdb_register_static_extension()` for core_functions, parquet, json, icu,
   ducklake (once per process).
2. `duckdb_open_ext(NULL, ...)` with config `autoload_known_extensions=false`,
   `autoinstall_known_extensions=false`, `threads`, `memory_limit`.
3. On an internal connection: `ATTACH 'ducklake:<path>/catalog.ducklake' AS lake
   (DATA_PATH '<path>/data/')`, `SET temp_directory='<path>/tmp'`,
   `SET enable_external_access=false` (to verify that it does not block DuckLake
   access to DATA_PATH).
4. Create `lake.__mysql` schema and `applied_position` table if missing (used by
   DWL#4).

### Dynamic variable callbacks

| Variable | Check callback | Update callback |
|---|---|---|
| `duckdb_enable` | ON: `Lake::open()`; OFF: stop offload, wait for running offloaded statements, `Lake::close()` | store |
| `duckdb_memory_limit` | `SET memory_limit=...` on DuckDB if open | store |
| `duckdb_threads` | range check, `SET threads=...` if open | store |

### Error handling

All DuckDB calls go through the C API; a `duckdb_state` error becomes
`my_error(ER_SECONDARY_ENGINE_PLUGIN, ...)` with the DuckDB message. A new error
code is not added (no core change).

### Open points

- Exact check for "inside a schema directory" at plugin init: to verify.
- `enable_external_access=false` after attach: to verify with DuckLake.
- Two ICU copies in one process: to test.

## Test Plan

MTR suite `duckdb`:

1. `startup_default_path`: plugin loaded with the mandatory variables; directory,
   catalog, `data/`, `tmp/` created under the datadir.
2. `startup_custom_path`: absolute and relative `duckdb_data_path`.
3. `startup_missing_vars`: plugin not loaded when a mandatory variable is missing;
   error in the log; server running.
4. `startup_lost_catalog`: files in `data/`, no catalog: plugin not loaded.
5. `startup_location_checks`: refusals (datadir, world-writable) and warnings.
6. `startup_loose_options`: `loose-duckdb_*` options without the plugin: server
   starts.
7. `enable_off_equivalence`: the same script without the plugin and with
   `duckdb_enable=FALSE` gives identical results.
8. `enable_runtime`: OFF and ON at runtime; failure of ON leaves FALSE.
9. `sysvars_dynamic`: memory limit and threads changes, invalid values, privilege
   check.
10. `encryption_not_supported`: `duckdb_encryption=ON` fails plugin init.
