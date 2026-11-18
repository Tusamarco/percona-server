# DuckDB secondary engine: tutorial

Copyright (c) 2026-present, Marco Tusa.

A step by step guide to the DuckDB secondary engine plugin of Percona
Server 9.7 (branch `9.7-duckdb`): build it, start a server with it, create
tables with and without history, query them on InnoDB and on DuckDB, delete
with and without archiving, archive whole partitions, then every variable,
every status variable, how to debug, and what the plugin can and cannot do.

Every command block can be copied and pasted as it is. Shell blocks use the
variables of step 0; SQL blocks are pasted into the client opened with
`m tut` (step 2.5). Each SQL block sets the session variables it needs, so
the blocks can also be run alone.

For the design and the details behind each step: `HOWTO.md` (reference),
`duckdb-secondary-engine-design.md`, the worklogs `DWL-0*.md`.

Contents:

0. Variables used by the commands
1. Build
2. Install a server with the plugin
3. A table without history
4. Queries: InnoDB or DuckDB
5. Changes: how DuckDB follows InnoDB
6. A table with history
7. Delete without and with archiving
8. Archiving with partitions
9. DDL, unload, drop
10. System variables, one by one
11. Status variables, one by one
12. Debugging
13. Read the lake when MySQL is down
14. What can and cannot be done
15. Clean up

---

## 0. Variables used by the commands

Set them once in each shell you use (change the paths to yours):

```bash
export SRC=/opt/mysql_source/percona-server              # source tree, branch 9.7-duckdb
export BUILD=/opt/mysql_source/percona-server-9.7-build  # build directory
export BASE=/opt/duckdb_tutorial                         # the tutorial server
m() { "$BUILD/runtime_output_directory/mysql" -S "$BASE/mysql.sock" -uroot "$@"; }
```

`m` is the client connected to the tutorial server: `m` alone opens it,
`m tut` opens it on the database `tut`, `m -e "..."` runs one statement.

---

## 1. Build

### 1.1 Requirements

- A compiler with C++23 (the reference build uses clang 21) and CMake.
- About 10 GB of disk for the build directory. The first build compiles
  DuckDB and takes long; later builds reuse it.

### 1.2 Get the DuckDB sources (git submodules)

```bash
cd "$SRC"
git submodule update --init extra/duckdb/duckdb extra/duckdb/ducklake extra/duckdb/croaring
```

### 1.3 Configure

```bash
mkdir -p "$BUILD" && cd "$BUILD"
cmake "$SRC" \
  -DCMAKE_C_COMPILER=clang-21 -DCMAKE_CXX_COMPILER=clang++-21 \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DWITH_DUCKDB=ON \
  -DWITH_UNIT_TESTS=ON
```

| CMake option | Meaning |
|---|---|
| `WITH_DUCKDB=ON` | Builds DuckDB, DuckLake, CRoaring and the plugin. Without it nothing DuckDB-related is built. |
| `CMAKE_BUILD_TYPE` | `RelWithDebInfo` or `Release` for speed; `Debug` for development (much slower queries and loads). |
| `DUCKDB_BUILD_TYPE` | Optional: build type of DuckDB alone (default: the server's). |
| `WITH_UNIT_TESTS=ON` | Also builds the plugin's unit tests `duckdb_apply-t`. |

### 1.4 Build

```bash
cd "$BUILD"
cmake --build . -j "$(nproc)"          # server, client, plugin
```

After a change in `storage/duckdb` only:

```bash
cd "$BUILD"
cmake --build . --target duckdb duckdb_apply-t -j "$(nproc)"
```

### 1.5 Check the build

```bash
cd "$BUILD"
ls -l plugin_output_directory/ha_duckdb.so            # the plugin library
./runtime_output_directory/duckdb_apply-t | tail -1   # [  PASSED  ] 41 tests.
```

The test suite (optional, long): see `storage/duckdb/mysql-test/duckdb/README.md`.

---

## 2. Install a server with the plugin

The plugin needs the binary log in row format with full metadata and GTIDs:
the DuckDB copy of a table is kept current by reading the binary log.

### 2.1 Directories

```bash
mkdir -p "$BASE"/{data,logs,plugins,duckdb_data}
chmod 750 "$BASE/duckdb_data"      # the plugin warns about a world-readable lake
ln -sf "$BUILD/plugin_output_directory/ha_duckdb.so" "$BASE/plugins/"
```

### 2.2 Configuration file

```bash
cat > "$BASE/my.cnf" <<EOF
[mysqld]
datadir                   = $BASE/data
socket                    = $BASE/mysql.sock
port                      = 3410
mysqlx                    = OFF
pid-file                  = $BASE/mysqld.pid
log-error                 = $BASE/error.log
plugin-dir                = $BASE/plugins
server_id                 = 1

# required by the plugin (SECONDARY_LOAD checks them)
log_bin                   = $BASE/logs/binlog
binlog_format             = ROW
binlog_row_image          = FULL
binlog_row_metadata       = FULL
gtid_mode                 = ON
enforce_gtid_consistency  = ON
binlog_order_commits      = ON
sync_binlog               = 1
innodb_flush_log_at_trx_commit = 1

# recommended
binlog_expire_logs_seconds = 604800
log_error_verbosity        = 3
innodb_buffer_pool_size    = 1G
EOF
```

### 2.3 Initialize the data directory (without the plugin)

```bash
"$BUILD/runtime_output_directory/mysqld" --defaults-file="$BASE/my.cnf" --initialize-insecure
```

### 2.4 Add the plugin and start

```bash
cat >> "$BASE/my.cnf" <<EOF

# the DuckDB plugin
plugin-load-add                = ha_duckdb.so
loose-duckdb_data_path         = $BASE/duckdb_data
loose-duckdb_memory_limit      = 2G
loose-duckdb_threads           = 2
EOF
cat > "$BASE/start" <<EOF
#!/bin/bash
nohup "$BUILD/runtime_output_directory/mysqld" --defaults-file="$BASE/my.cnf" > /dev/null 2>&1 &
for i in \$(seq 60); do "$BUILD/runtime_output_directory/mysqladmin" -S "$BASE/mysql.sock" -uroot ping > /dev/null 2>&1 && break; sleep 1; done
EOF
cat > "$BASE/stop" <<EOF
#!/bin/bash
"$BUILD/runtime_output_directory/mysqladmin" -S "$BASE/mysql.sock" -uroot shutdown
EOF
chmod +x "$BASE/start" "$BASE/stop"
"$BASE/start"
```

`duckdb_memory_limit` and `duckdb_threads` are mandatory: without them the
plugin is not loaded (the error log says why) and the server starts without
it. Keep the `loose-` prefix: without it the server refuses to start when
the plugin is missing.

### 2.5 Check that the plugin runs

```bash
m -e "SELECT PLUGIN_NAME, PLUGIN_STATUS, PLUGIN_TYPE FROM information_schema.PLUGINS WHERE PLUGIN_NAME LIKE 'duckdb%'"
m -e "SHOW GLOBAL STATUS LIKE 'duckdb_enabled'; SHOW GLOBAL STATUS LIKE 'duckdb_lake_path'"
grep -i duckdb "$BASE/error.log" | tail -5
```

Expected: `duckdb` (STORAGE ENGINE), `duckdb_guard` (AUDIT) and
`DUCKDB_TABLES` (INFORMATION SCHEMA) ACTIVE; `duckdb_enabled` ON; the error
log shows the lake path and `binlog reader started`.

The three plugins in the library:

| Plugin | What it is |
|---|---|
| `duckdb` | The secondary engine: copies, applier, query offload. |
| `duckdb_guard` | Audit plugin: keeps queries on history tables under `duckdb_history_scope=FULL` in DuckDB (section 6). |
| `DUCKDB_TABLES` | The table `INFORMATION_SCHEMA.DUCKDB_TABLES` (section 11). |

The lake (where DuckDB keeps the copies) is the directory
`duckdb_data_path`: a DuckLake catalog `catalog.ducklake` and Parquet files
under `data/`.

Create the tutorial database:

```bash
m -e "CREATE DATABASE tut"
```

---

## 3. A table without history

A **classic copy**: DuckDB holds the same rows as InnoDB. Deleted rows
disappear from both.

### 3.1 Create the table

The table lives in InnoDB as always; `SECONDARY_ENGINE = duckdb` declares
that it may have a DuckDB copy. A primary key is required.

```sql
CREATE TABLE orders (
  id        BIGINT PRIMARY KEY,
  customer  INT NOT NULL,
  region    VARCHAR(20) NOT NULL,
  status    ENUM('new','paid','shipped','returned') NOT NULL,
  amount    DECIMAL(10,2) NOT NULL,
  placed_at DATETIME NOT NULL
) SECONDARY_ENGINE = duckdb;
```

### 3.2 Fill it

```sql
SET SESSION cte_max_recursion_depth = 1000000;
INSERT INTO orders
WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 300000)
SELECT n, n % 5000, ELT(n % 4 + 1, 'north', 'south', 'east', 'west'),
       n % 4 + 1, (n % 1000) / 10, TIMESTAMP '2024-01-01 00:00:00' + INTERVAL n MINUTE
FROM s;
```

### 3.3 Load the copy

```sql
ALTER TABLE orders SECONDARY_LOAD;
```

The load copies the InnoDB rows into the lake. From then on the plugin's
applier keeps the copy current from the binary log. Check:

```sql
SELECT TABLE_NAME, STATE, RETAIN_HISTORY, PRIMARY_KEY, WEIGHT_COLUMNS
  FROM information_schema.DUCKDB_TABLES WHERE TABLE_SCHEMA = 'tut';
SELECT TABLE_NAME, CREATE_OPTIONS FROM information_schema.TABLES
 WHERE TABLE_SCHEMA = 'tut' AND TABLE_NAME = 'orders';   -- SECONDARY_LOAD="1"
```

`WEIGHT_COLUMNS` lists `region`, `status`: columns with a collation DuckDB
cannot compare itself (`utf8mb4_0900_ai_ci`, the default) get a hidden
column with MySQL's sort weight, so results follow MySQL's collation.

A table can also get the engine later: `ALTER TABLE t SECONDARY_ENGINE =
duckdb;` then `ALTER TABLE t SECONDARY_LOAD;`.

---

## 4. Queries: InnoDB or DuckDB

### 4.1 The three modes of `use_secondary_engine`

```sql
SET SESSION use_secondary_engine = OFF;      -- always InnoDB
SELECT region, COUNT(*), SUM(amount) FROM orders GROUP BY region ORDER BY region;

SET SESSION use_secondary_engine = FORCED;   -- always DuckDB, or an error
SELECT region, COUNT(*), SUM(amount) FROM orders GROUP BY region ORDER BY region;

SET SESSION use_secondary_engine = ON;       -- default: DuckDB when worth it
SELECT region, COUNT(*), SUM(amount) FROM orders GROUP BY region ORDER BY region;
```

The results are identical; the client shows the time of each. To see which
engine answered, look at the counter before and after:

```sql
SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries';
```

### 4.2 When ON sends a query to DuckDB

With ON a query runs in DuckDB when its tables all have a loaded copy, the
plugin can translate it exactly, and one of these holds:

- MySQL's estimated cost is above `secondary_engine_cost_threshold`
  (default 100000);
- MySQL's plan reads a loaded table of at least `duckdb_offload_min_rows`
  rows (default 1,000,000) by a full scan, full index scan or non-unique
  index lookup.

The table has 300,000 rows, below the default `duckdb_offload_min_rows`.
Lower it (or the cost threshold) for this session:

```sql
SET SESSION use_secondary_engine = ON;
SET SESSION duckdb_offload_min_rows = 100000;
SELECT status, COUNT(*), AVG(amount) FROM orders GROUP BY status ORDER BY status;
SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries';
SET SESSION duckdb_offload_min_rows = DEFAULT;
```

Primary key lookups and small index ranges stay on InnoDB, where they are
fast.

### 4.3 See the plan

```sql
SET SESSION use_secondary_engine = FORCED;
EXPLAIN FORMAT=TREE SELECT region, SUM(amount) FROM orders WHERE customer < 100 GROUP BY region;
SHOW WARNINGS;
```

The notes give the DuckDB SQL the plugin generated and DuckDB's own plan.

### 4.4 When a query is not offloaded

```sql
SET SESSION use_secondary_engine = FORCED;
SELECT region, GROUP_CONCAT(id) FROM orders WHERE id < 10 GROUP BY region;
```

With FORCED the error says why (here `unsupported_aggregate`). With ON the
same query runs on InnoDB, and the reason is kept:

```sql
SET SESSION use_secondary_engine = ON;
SET SESSION secondary_engine_cost_threshold = 0;
SELECT region, GROUP_CONCAT(id) FROM orders WHERE id < 10 GROUP BY region;
SHOW GLOBAL STATUS LIKE 'duckdb_offload_last_refusal';
SHOW GLOBAL STATUS LIKE 'duckdb_offload_refused';
SET SESSION secondary_engine_cost_threshold = DEFAULT;
```

The optimizer trace has the generated SQL or the reason too:

```sql
SET SESSION use_secondary_engine = FORCED, optimizer_trace = 'enabled=on';
SELECT COUNT(*) FROM orders WHERE region = 'NORTH';
SELECT JSON_PRETTY(JSON_EXTRACT(TRACE, '$**.duckdb_offload')) AS duckdb FROM information_schema.OPTIMIZER_TRACE;
SET SESSION optimizer_trace = 'enabled=off';
```

Read the trace right after the query: the next statement (even a `SET`)
replaces it. `region = 'NORTH'` matched `north`: the comparison follows the
column's case-insensitive collation, as on InnoDB; the trace shows how, a
comparison on the hidden weight column `__w_region`.

### 4.5 Change when MySQL uses DuckDB

With ON, MySQL estimates what the query would cost on InnoDB. It sends the
query to DuckDB when that cost is above `secondary_engine_cost_threshold`
(default 100000), or when the plan scans a loaded table of at least
`duckdb_offload_min_rows` rows (default 1,000,000). See the estimate and the
decision:

```sql
SET SESSION use_secondary_engine = OFF;
SELECT customer, SUM(amount) FROM orders GROUP BY customer ORDER BY 2 DESC LIMIT 3;
SHOW SESSION STATUS LIKE 'Last_query_cost';      -- InnoDB's estimated cost

SET SESSION use_secondary_engine = ON, optimizer_trace = 'enabled=on';
SELECT customer, SUM(amount) FROM orders GROUP BY customer ORDER BY 2 DESC LIMIT 3;
SELECT JSON_EXTRACT(TRACE, '$**.secondary_engine_not_used') AS not_used FROM information_schema.OPTIMIZER_TRACE;
SET SESSION optimizer_trace = 'enabled=off';
```

`not_used` shows the cost, the threshold and the reason the query stayed
on InnoDB (empty when it went to DuckDB). Lower the limits where they
should apply:

```sql
-- this session
SET SESSION secondary_engine_cost_threshold = 10000;
SELECT customer, SUM(amount) FROM orders GROUP BY customer ORDER BY 2 DESC LIMIT 3;
SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries';
SET SESSION secondary_engine_cost_threshold = DEFAULT;

-- one query, with an optimizer hint
SELECT /*+ SET_VAR(secondary_engine_cost_threshold = 0) */ customer, SUM(amount)
  FROM orders GROUP BY customer ORDER BY 2 DESC LIMIT 3;
SELECT /*+ SET_VAR(duckdb_offload_min_rows = 100000) */ customer, SUM(amount)
  FROM orders GROUP BY customer ORDER BY 2 DESC LIMIT 3;
SELECT /*+ SET_VAR(use_secondary_engine = FORCED) */ customer, SUM(amount)
  FROM orders GROUP BY customer ORDER BY 2 DESC LIMIT 3;
SHOW GLOBAL STATUS LIKE 'duckdb_offload_queries';
```

For every new session, and after restarts (`SET PERSIST` writes
`mysqld-auto.cnf`):

```sql
SET PERSIST secondary_engine_cost_threshold = 10000;
SET PERSIST duckdb_offload_min_rows = 100000;
-- back to the defaults
RESET PERSIST secondary_engine_cost_threshold;
RESET PERSIST duckdb_offload_min_rows;
SET GLOBAL secondary_engine_cost_threshold = DEFAULT, duckdb_offload_min_rows = DEFAULT;
```

Guidelines: 0 sends every query the plugin can translate to DuckDB (each
pays about a millisecond to start in DuckDB); primary key lookups and short
index ranges are fast on InnoDB and stay there with the defaults;
`duckdb_offload_min_rows = 0` turns its rule off.

---

## 5. Changes: how DuckDB follows InnoDB

Write to the table as always; InnoDB is the only place data is written.
After the commit, the applier reads the transaction from the binary log and
applies it to the copy, usually within a second.

```sql
UPDATE orders SET amount = amount + 1 WHERE id <= 1000;
DELETE FROM orders WHERE id BETWEEN 1001 AND 2000;
INSERT INTO orders VALUES (900001, 1, 'north', 'new', 5.00, NOW());

SET SESSION use_secondary_engine = FORCED;
SELECT COUNT(*), SUM(amount) FROM orders;
SET SESSION use_secondary_engine = OFF;
SELECT COUNT(*), SUM(amount) FROM orders;
```

Same numbers. A query sent to DuckDB right after your own commit waits until
the applier has applied it (`duckdb_offload_wait_applied`, at most
`duckdb_offload_wait_timeout_ms`, 5 s by default).
Inside a transaction that has written, queries run on InnoDB (they must see
your uncommitted rows).

What the applier did:

```sql
SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status
 WHERE VARIABLE_NAME IN ('duckdb_apply_reader_state', 'duckdb_apply_transactions_read',
   'duckdb_apply_changes', 'duckdb_apply_batches', 'duckdb_apply_rows_written',
   'duckdb_applied_gtid');
```

`duckdb_apply_reader_state` = `idle` means everything in the binary log is
in DuckDB.

### 5.1 A helper: wait for the applier

Most changes reach DuckDB within a second, but a DDL makes the applier
reload the table (section 9), which takes as long as a SECONDARY_LOAD. The
tutorial calls this procedure after such steps; it returns when the applier
is idle again:

```sql
DROP PROCEDURE IF EXISTS wait_applier;
DELIMITER //
CREATE PROCEDURE wait_applier()
BEGIN
  DECLARE st VARCHAR(200) DEFAULT '';
  -- performance_schema has no DuckDB copy: read it on InnoDB
  SET @wait_applier_mode = @@SESSION.use_secondary_engine;
  SET SESSION use_secondary_engine = OFF;
  DO SLEEP(1);                       -- let the applier pick up the last commit
  REPEAT
    SELECT VARIABLE_VALUE INTO st FROM performance_schema.global_status
     WHERE VARIABLE_NAME = 'duckdb_apply_reader_state';
    IF st <> 'idle' THEN DO SLEEP(0.5); END IF;
  UNTIL st = 'idle' END REPEAT;
  SET SESSION use_secondary_engine = @wait_applier_mode;
END //
DELIMITER ;
CALL wait_applier();
```

---

## 6. A table with history

A **history copy** keeps, next to the live rows, the rows you purge from
InnoDB: InnoDB stays small, DuckDB keeps everything for analysis.

### 6.1 Create and load

```sql
CREATE TABLE readings (
  id       BIGINT PRIMARY KEY,
  sensor   INT NOT NULL,
  read_at  DATETIME NOT NULL,
  value    DOUBLE NOT NULL
) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}';

SET SESSION cte_max_recursion_depth = 1000000;
INSERT INTO readings
WITH RECURSIVE s(n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM s WHERE n < 100000)
SELECT n, n % 50, TIMESTAMP '2024-01-01 00:00:00' + INTERVAL n MINUTE, (n % 997) / 10 FROM s;

ALTER TABLE readings SECONDARY_LOAD;
SELECT TABLE_NAME, STATE, RETAIN_HISTORY FROM information_schema.DUCKDB_TABLES WHERE TABLE_SCHEMA = 'tut';
```

### 6.2 Live rows and full history

`duckdb_history_scope` decides what a query sees in a history table:

- `LIVE` (default): the rows InnoDB has. Results are the same on both
  engines.
- `FULL`: live rows and archived rows. Only DuckDB has archived rows, so
  under FULL a query on a history table always runs in DuckDB, or fails
  with the reason (the `duckdb_guard` plugin sees to it).

FULL needs the privilege `DUCKDB_HISTORY_READER` (nobody has it until it is
granted, root included). Grant it, then reconnect (`m tut` again):

```sql
GRANT DUCKDB_HISTORY_READER ON *.* TO root@localhost;
```

---

## 7. Delete without and with archiving

### 7.1 Plain DELETE: the rows are gone everywhere

```sql
DELETE FROM readings WHERE id <= 100;

SET SESSION duckdb_history_scope = FULL;
SELECT COUNT(*) FROM readings;          -- 99900: deleted rows are not kept
SET SESSION duckdb_history_scope = LIVE;
```

A plain DELETE is a real delete, also on a history table.

### 7.2 Purge: delete from InnoDB, archive in DuckDB

A purge is a delete in a transaction whose GTID carries the tag
`duckdb_avoid_purge`. The rows leave InnoDB and stay in DuckDB as archived
rows (with the time of the purge). Setting a GTID tag needs the
`TRANSACTION_GTID_TAG` privilege.

```sql
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM readings WHERE read_at < '2024-02-01';
SET gtid_next = 'AUTOMATIC';
CALL wait_applier();
```

The tag covers exactly one transaction: the statement above, or everything
between `BEGIN` and `COMMIT` after the `SET`. Set it back to `AUTOMATIC`
right after, as above.

```sql
SET SESSION use_secondary_engine = OFF;
SELECT COUNT(*), MIN(read_at) FROM readings;                -- InnoDB: live rows only

SET SESSION use_secondary_engine = ON, duckdb_history_scope = LIVE;
SELECT COUNT(*), MIN(read_at) FROM readings;                -- the same

SET SESSION duckdb_history_scope = FULL;
SELECT COUNT(*), MIN(read_at) FROM readings;                -- live + archived
SELECT DATE(read_at) AS day, COUNT(*), AVG(value)
  FROM readings GROUP BY day ORDER BY day LIMIT 5;          -- history of January

SELECT /*+ SET_VAR(duckdb_history_scope = FULL) */ COUNT(*) FROM readings;   -- per query
SET SESSION duckdb_history_scope = LIVE;
SHOW GLOBAL STATUS LIKE 'duckdb_apply_purge_transactions';
SHOW GLOBAL STATUS LIKE 'duckdb_apply_archived_rows';
```

Under FULL, what cannot run in DuckDB is stopped instead of running on
InnoDB with only the live rows:

```sql
SET SESSION duckdb_history_scope = FULL, use_secondary_engine = OFF;
SELECT COUNT(*) FROM readings;          -- error: FULL requires DuckDB execution
SET SESSION use_secondary_engine = ON;
UPDATE readings SET value = 0 WHERE id = 5000;   -- error: FULL is a read scope
SET SESSION duckdb_history_scope = LIVE;
```

Updating an archived row is not possible: archived rows are history.

---

## 8. Archiving with partitions

A partitioned table can have a copy; DuckDB stores it as one table. The
usual way to retire old data, `ALTER TABLE ... DROP PARTITION`, is **not**
an archive operation: the applier sees a DDL, refreshes the copy from
InnoDB, and the rows of the dropped partition are gone from DuckDB too.

To archive a partition, **purge its rows first, then drop it**:

### 8.1 A partitioned history table

```sql
CREATE TABLE sales (
  id       BIGINT NOT NULL,
  sold_at  DATE NOT NULL,
  region   VARCHAR(20) NOT NULL,
  amount   DECIMAL(10,2) NOT NULL,
  PRIMARY KEY (id, sold_at)
) SECONDARY_ENGINE = duckdb
  SECONDARY_ENGINE_ATTRIBUTE = '{"retain_history": true}'
PARTITION BY RANGE COLUMNS (sold_at) (
  PARTITION p2023 VALUES LESS THAN ('2024-01-01'),
  PARTITION p2024 VALUES LESS THAN ('2025-01-01'),
  PARTITION pmax  VALUES LESS THAN (MAXVALUE));

INSERT INTO sales VALUES (1, '2023-03-01', 'north', 10.00), (2, '2023-07-01', 'south', 20.00),
  (3, '2024-02-01', 'north', 30.00), (4, '2024-09-01', 'south', 40.00), (5, '2025-01-15', 'north', 50.00);
ALTER TABLE sales SECONDARY_LOAD;
```

### 8.2 Archive the partition p2023

```sql
-- 1. purge the partition's rows: archived in DuckDB
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM sales PARTITION (p2023);
SET gtid_next = 'AUTOMATIC';

-- 2. drop the partition, now empty
ALTER TABLE sales DROP PARTITION p2023;
CALL wait_applier();              -- the DDL refreshes the copy
```

`DELETE ... PARTITION (p)` deletes only that partition, without scanning the
others. Once the refresh is done (under FULL a query cannot fall back to
InnoDB: during the refresh it fails with `applier_behind`):

```sql
SET SESSION use_secondary_engine = OFF;
SELECT id, sold_at FROM sales ORDER BY id;          -- InnoDB: 3, 4, 5

SET SESSION use_secondary_engine = ON, duckdb_history_scope = FULL;
SELECT id, sold_at FROM sales ORDER BY id;          -- DuckDB: 1, 2 (archived), 3, 4, 5
SET SESSION duckdb_history_scope = LIVE;
```

### 8.3 What not to do

```sql
ALTER TABLE sales DROP PARTITION p2024;             -- no purge first
CALL wait_applier();

SET SESSION duckdb_history_scope = FULL;
SELECT id, sold_at FROM sales ORDER BY id;          -- 1, 2, 5: rows 3 and 4 are lost
SET SESSION duckdb_history_scope = LIVE;
```

The same holds for `ALTER TABLE ... TRUNCATE PARTITION`.

### 8.4 A monthly routine

Add the next period before it starts, by splitting the last partition:

```sql
ALTER TABLE sales REORGANIZE PARTITION pmax INTO (
  PARTITION p2025 VALUES LESS THAN ('2026-01-01'),
  PARTITION pmax  VALUES LESS THAN (MAXVALUE));
CALL wait_applier();
SELECT id, sold_at FROM sales PARTITION (p2025);              -- row 5 moved there

SET SESSION use_secondary_engine = ON, duckdb_history_scope = FULL;
SELECT id, sold_at FROM sales ORDER BY id;                    -- archived 1, 2 still there
SET SESSION duckdb_history_scope = LIVE;
```

Retire the oldest one as in 8.2. A template (replace `p_oldest` with its
name):

```text
SET gtid_next = 'AUTOMATIC:duckdb_avoid_purge';
DELETE FROM sales PARTITION (p_oldest);
SET gtid_next = 'AUTOMATIC';
ALTER TABLE sales DROP PARTITION p_oldest;
CALL wait_applier();
```

Notes:

- Each partition DDL (DROP, ADD, REORGANIZE) refreshes the copy: the live
  rows are loaded again from InnoDB and the archived rows are kept. On a
  large table this takes as long as a SECONDARY_LOAD, and the applier waits
  for it (changes of all tables are applied after the refresh).
- For a very large partition, purge it in chunks (several tagged DELETE
  transactions with `LIMIT`) to keep transactions small; the applier applies
  a transaction larger than `duckdb_apply_batch_max_rows` in pieces anyway.

---

## 9. DDL, unload, drop

### 9.1 DDL on a table with a copy

```sql
ALTER TABLE orders ADD COLUMN channel VARCHAR(10) NOT NULL DEFAULT 'web';
CALL wait_applier();              -- the copy is reloaded with the new column
RENAME TABLE orders TO orders2;
CALL wait_applier();
SELECT TABLE_NAME, STATE FROM information_schema.DUCKDB_TABLES WHERE TABLE_SCHEMA = 'tut';
RENAME TABLE orders2 TO orders;
CALL wait_applier();
```

DDL never blocks: the applier refreshes the copy from InnoDB after column
changes, TRUNCATE and partition DDL (history tables keep their archived
rows, converted to the new column types), follows RENAME TABLE, and ignores
what does not change the data (indexes, comments). A copy that cannot
follow is marked `STALE` (shown in `DUCKDB_TABLES`, reason in the error
log); queries then run on InnoDB.

### 9.2 Unload

```sql
ALTER TABLE orders SECONDARY_UNLOAD;      -- classic copy: removed from the lake
ALTER TABLE orders SECONDARY_LOAD;        -- and back
```

A history copy holds rows that exist nowhere else, so unloading it needs a
confirmation:

```sql
SET SESSION duckdb_unload_history_confirm = ON;
-- ALTER TABLE readings SECONDARY_UNLOAD;   -- would delete the archived rows
SET SESSION duckdb_unload_history_confirm = OFF;
```

### 9.3 Drop

`DROP TABLE` of a classic table removes its copy. `DROP TABLE` of a history
table keeps the copy (`STALE`, with a warning): creating the table again
with the same structure and `SECONDARY_LOAD` reattaches the history;
`"adopt_history_from": "db.table"` in `SECONDARY_ENGINE_ATTRIBUTE` gives it
to a table with another name. To delete it for good:

```sql
SET SESSION duckdb_unload_history_confirm = ON;
-- SET GLOBAL duckdb_discard_stale_copy = 'tut.old_table';
SET SESSION duckdb_unload_history_confirm = OFF;
```

---

## 10. System variables, one by one

List them with `SHOW GLOBAL VARIABLES LIKE 'duckdb%';`. Global variables
marked "my.cnf" go in the configuration file with the `loose-` prefix.

### Lake and resources

**`duckdb_enable`** (global, dynamic, default ON). Opens or closes the lake
and starts or stops the applier. OFF: the server behaves as if the plugin
were not loaded (queries run on InnoDB, loads are refused); nothing is
deleted. Back to ON, the applier continues where it stopped.

```sql
SET GLOBAL duckdb_enable = OFF;
SET GLOBAL duckdb_enable = ON;
```

**`duckdb_data_path`** (global, my.cnf only, default empty =
`<datadir>/duckdb_data`). The lake directory. Not the datadir itself, not
inside a schema directory, not world-writable.

**`duckdb_lake_relocate`** (global, dynamic, default ON). When the lake was
moved or restored to another path: ON checks that every file is under the
new `duckdb_data_path` and updates the path stored in the catalog (warning
in the error log); OFF refuses to open the lake. Used when the lake opens.

**`duckdb_memory_limit`** (global, dynamic, mandatory, at least 256M).
Memory DuckDB may use. Changes apply at once.

```sql
SET GLOBAL duckdb_memory_limit = 4 * 1024 * 1024 * 1024;
```

**`duckdb_threads`** (global, dynamic, mandatory, 1 to the number of
cores). DuckDB's own worker threads for one query.

```sql
SET GLOBAL duckdb_threads = 4;
```

**`duckdb_executor_threads`** (global, dynamic, default 4). Threads that run
DuckDB calls (loads, queries, applier commits): at most this many DuckDB
operations at the same time; others wait in a queue
(`duckdb_executor_queue_length`).

**`duckdb_executor_stack_size`** (global, my.cnf only, default 8M, 2M to
64M). Stack of those threads.

**`duckdb_encryption`** (global, my.cnf only, default OFF). Not supported in
this version: ON keeps the plugin from loading.

### Applier

**`duckdb_apply_batch_max_trx`** (global, dynamic, default 1000).
**`duckdb_apply_batch_max_rows`** (global, dynamic, default 100000).
**`duckdb_apply_batch_max_delay_ms`** (global, dynamic, default 1000).
The applier groups transactions into one DuckDB commit (a batch) until one
limit is reached. Larger batches apply faster; the delay bounds how late
DuckDB can be under light load. A transaction with more rows than
`duckdb_apply_batch_max_rows` is applied in pieces inside one DuckDB
transaction.

```sql
SET GLOBAL duckdb_apply_batch_max_delay_ms = 200;     -- fresher, more commits
```

**`duckdb_apply_gap_action`** (global, dynamic, default STOP). When binary
logs the applier still needed were purged (server down too long, short
`binlog_expire_logs_seconds`): STOP stops the applier in error state;
FIRST_AVAILABLE warns and continues at the first available binary log (the
tables named in the error log then miss changes: reload them).

**`duckdb_apply_start_position`** (global, dynamic, a command). Restarts the
applier at the start of a binary log: a file name, FIRST or CURRENT. Used to
repair after a gap (section 12.4). It reads back as empty.

```sql
-- SET GLOBAL duckdb_apply_start_position = 'FIRST';
```

**`duckdb_apply_debug_log`** (global, dynamic, default OFF). Testing aid:
one error log line per change applied, with DuckDB row counts (needs
`log_error_verbosity = 3`). Do not leave it on with real traffic.

### Queries

**`duckdb_offload_min_rows`** (session, global or `SET_VAR` hint, default
1000000; section 4.5). Also
try DuckDB when MySQL's plan reads a loaded table of at least this many
rows by a full scan, full index scan or non-unique index lookup, whatever
the estimated cost. 0: only `secondary_engine_cost_threshold` decides.

**`duckdb_decimal_overflow`** (session, global or `SET_VAR` hint, default
FALLBACK). MySQL declares DECIMAL results up to 65 digits, DuckDB computes up
to 38: such results are computed as DECIMAL(38, scale), exact while the
values fit, and the statement runs in DuckDB first. If a value does not fit:
FALLBACK runs the query on InnoDB (identical results; FORCED: an error),
ROUND runs it again in DuckDB with fewer decimals and a warning, ERROR fails
with DuckDB's error.

```sql
SET SESSION duckdb_decimal_overflow = ROUND;
SET SESSION duckdb_decimal_overflow = DEFAULT;
```

**`duckdb_offload_wait_applied`** (session or `SET_VAR` hint, default ON).
Before a query
runs in DuckDB, wait until the applier has applied this connection's last
commit (at most `duckdb_offload_wait_timeout_ms`; after that the query runs
on InnoDB). OFF: a query may miss a commit made less than about a second
before.

**`duckdb_offload_wait_timeout_ms`** (session or `SET_VAR` hint, default
5000, 0 to 3600000). How long that wait lasts, in milliseconds.

```sql
SELECT /*+ SET_VAR(duckdb_offload_wait_timeout_ms = 20000) */ COUNT(*) FROM orders;
```

**`duckdb_offload_float_aggregates`** (session, global or `SET_VAR` hint,
default APPROXIMATE). SUM and AVG of FLOAT and DOUBLE values: DuckDB adds
them in parallel in no fixed order, so the last digits can differ from
InnoDB's (and from one run to the next). APPROXIMATE: they run in DuckDB.
REFUSE: such queries run on InnoDB (under `duckdb_history_scope = FULL` they
fail). SUM and AVG of integers and DECIMAL are always exact.

```sql
SELECT /*+ SET_VAR(duckdb_offload_float_aggregates = REFUSE) */ sensor, AVG(value)
  FROM readings GROUP BY sensor ORDER BY sensor LIMIT 3;   -- InnoDB's digits
```

**`duckdb_history_scope`** (session, global, `SET_VAR` hint; default LIVE).
LIVE or FULL (section 7). FULL needs `DUCKDB_HISTORY_READER` and the active
`duckdb_guard` plugin.

Server variables that decide offload: **`use_secondary_engine`** (OFF, ON,
FORCED; section 4) and **`secondary_engine_cost_threshold`** (default
100000; section 4.5). Both also work as `SET_VAR` hints.

### History copies

**`duckdb_unload_history_confirm`** (session, default OFF). Must be ON to
unload a history copy or discard a stale history copy (the archived rows
are deleted).

**`duckdb_discard_stale_copy`** (global, a command). `'db.table'`: drops
the stale copy of a table that no longer exists. Reads back as empty.

**`duckdb_history_check_only`** (session, default OFF). ON: SECONDARY_LOAD
of a history table only checks whether it can take over an existing
history copy (reattach, resync, adopt) and ends with an error that gives
the result; nothing is loaded.

```sql
SET SESSION duckdb_history_check_only = ON;
ALTER TABLE readings SECONDARY_LOAD;    -- error with the check result
SET SESSION duckdb_history_check_only = OFF;
```

### Server variables the plugin requires

`log_bin`, `binlog_format=ROW`, `binlog_row_image=FULL`,
`binlog_row_metadata=FULL`, `gtid_mode=ON`, `binlog_order_commits=ON`;
`sync_binlog=1` for history tables (recommended for all) and
`innodb_flush_log_at_trx_commit=1` (recommended); `binlog_encryption=OFF`.
SECONDARY_LOAD refuses with a message naming the setting.

---

## 11. Status variables, one by one

All at once:

```sql
SELECT VARIABLE_NAME, VARIABLE_VALUE FROM performance_schema.global_status
 WHERE VARIABLE_NAME LIKE 'duckdb%' ORDER BY VARIABLE_NAME;
```

Counters restart at 0 when the server starts.

### State

| Variable | Meaning | Normal |
|---|---|---|
| `duckdb_enabled` | The lake is open. | ON |
| `duckdb_lake_path` | Lake directory in use. | your path |

### Executor (DuckDB calls)

| Variable | Meaning | Normal |
|---|---|---|
| `duckdb_executor_threads_busy` | Executor threads running a DuckDB call now. | up to `duckdb_executor_threads` |
| `duckdb_executor_queue_length` | Calls waiting for a thread. | 0; often above 0: raise `duckdb_executor_threads` |
| `duckdb_executor_tasks` | Calls run since start. | grows |

### Applier

| Variable | Meaning | Normal |
|---|---|---|
| `duckdb_apply_reader_state` | `idle` (all applied), `reading`, `waiting to commit` (a batch waits for its delay), `committing`, `refreshing db.t` (a refresh after DDL), `error: <reason>`. | `idle` most of the time |
| `duckdb_apply_reader_file`, `duckdb_apply_reader_position` | Next binary log position to read. | the end of the binary log (`SHOW BINARY LOG STATUS`) |
| `duckdb_applied_file`, `duckdb_applied_position`, `duckdb_applied_gtid` | What DuckDB contains: the last applied position and GTID. | close to the reader position |
| `duckdb_apply_last_gtid` | Last transaction read that changes a loaded table. | |
| `duckdb_apply_transactions_read` | Transactions read that change loaded tables. | grows with writes |
| `duckdb_apply_row_events_read`, `duckdb_apply_rows_decoded` | Row events and rows of loaded tables decoded. | |
| `duckdb_apply_changes` | Net changes after collapsing the changes of one batch per primary key. | ≤ rows decoded |
| `duckdb_apply_batches`, `duckdb_apply_rows_written` | DuckDB commits made, rows written. | |
| `duckdb_apply_large_transactions`, `duckdb_apply_pieces` | Transactions applied in pieces, pieces written. | |
| `duckdb_apply_purge_transactions`, `duckdb_apply_archived_rows` | Tagged purges on loaded tables; rows kept as archived rows. | grows with purges |
| `duckdb_apply_ddl_refreshes` | Copies refreshed after a DDL. | |
| `duckdb_apply_ddl_renames` | Copies renamed after RENAME TABLE. | |
| `duckdb_apply_ddl_deferred` | Refreshes waiting for a later RENAME (a table renamed right after its DDL). | |
| `duckdb_apply_ddl_failures` | Copies marked stale by a DDL. | 0; above 0: see the error log |

How late is DuckDB? Compare the reader with the binary log end:

```sql
SHOW BINARY LOG STATUS;
SHOW GLOBAL STATUS LIKE 'duckdb_apply_reader%';
SHOW GLOBAL STATUS LIKE 'duckdb_applied%';
```

### Queries and the guard

| Variable | Meaning | Normal |
|---|---|---|
| `duckdb_offload_queries` | Queries run in DuckDB to the end (EXPLAIN and statements that failed during execution are not counted). | grows with analytic queries |
| `duckdb_offload_explained` | EXPLAIN of statements that would run in DuckDB. | |
| `duckdb_offload_waits` / `duckdb_offload_wait_timeouts` | Waits for the connection's last commit (`duckdb_offload_wait_applied`), and those that reached `duckdb_offload_wait_timeout_ms` (the query ran on InnoDB). | timeouts near 0; growing: the applier is behind |
| `duckdb_offload_decimal_reruns` | Runs again with fewer decimals (`duckdb_decimal_overflow = ROUND`). | |
| `duckdb_offload_refused` | Queries the plugin refused (they ran on InnoDB, or failed with FORCED). | |
| `duckdb_offload_last_refusal` | Reason of the last refusal (section 12.3). | |
| `duckdb_guard_forced` | Statements on history tables under FULL sent to DuckDB only. | |
| `duckdb_guard_rejected` | Statements under FULL stopped (OFF, not a SELECT, stale copy...). | |

### Tables

`INFORMATION_SCHEMA.DUCKDB_TABLES` (needs the PROCESS privilege) lists every
copy in the lake:

```sql
SELECT * FROM information_schema.DUCKDB_TABLES;
```

| Column | Meaning |
|---|---|
| `TABLE_SCHEMA`, `TABLE_NAME` | The MySQL table. |
| `STATE` | `LOADED` (kept current), `STALE` (no longer updated: a dropped history table, or a DDL the copy could not follow), `REFRESH_PENDING` (a refresh waits for a later RENAME). |
| `RETAIN_HISTORY` | YES for history copies. |
| `PRIMARY_KEY`, `WEIGHT_COLUMNS` | Key columns; columns with a hidden weight column. |
| `LOAD_BINLOG_FILE`, `LOAD_BINLOG_POSITION`, `LOAD_GTID_SET` | Where the load was taken: the applier starts from there for this table. |

---

## 12. Debugging

### 12.1 The plugin does not load

```bash
grep -i -E "duckdb|plugin" "$BASE/error.log" | tail -20
```

Common causes: `duckdb_memory_limit` or `duckdb_threads` missing ("is not
set ... Plugin not loaded"); `plugin-dir` or the `.so` link wrong; a
`duckdb_*` option without `loose-` while the plugin is missing (the server
does not start); `duckdb_encryption=ON`; a lake moved with
`duckdb_lake_relocate=OFF`.

### 12.2 SECONDARY_LOAD is refused

The error names the reason. Typical: a server setting from section 10
missing; no primary key; a column type not supported (JSON, spatial,
VECTOR); a cascading foreign key with `innodb_native_foreign_keys = ON`;
DECIMAL with more than 38 digits; zero dates; a
history table whose structure differs from its stale copy (`SHOW WARNINGS`
lists every difference; `duckdb_history_check_only` checks without
loading).

### 12.3 A query does not run in DuckDB

1. Run it with `use_secondary_engine = FORCED`: the error gives the reason.
2. Or `SHOW GLOBAL STATUS LIKE 'duckdb_offload_last_refusal'` after it ran
   with ON.
3. With ON and no refusal, it was not even tried: its cost is under
   `secondary_engine_cost_threshold` and it reads no table of
   `duckdb_offload_min_rows` rows by a scan. Lower one of them in the
   session.
4. `table not loaded` / error 3889: a table of the query has no copy.
5. Inside a transaction that has written: InnoDB by design.

Common reasons:

| Reason | Meaning |
|---|---|
| `unsupported_function:<name>`, `unsupported_aggregate:<name>` | Not translated yet: InnoDB answers. |
| `collation:<column> (...)` | An operation on a string whose collation DuckDB cannot reproduce exactly (e.g. LIKE on `_ci`). |
| `grouped_string_value_in_join` | A grouped `_ci` or PAD SPACE VARCHAR string shown in a join without the table's primary key in GROUP BY (InnoDB's value depends on its join order). CHAR under a binary collation is offloaded. |
| `arithmetic_on_unsigned` | Unsigned arithmetic that could leave 0..2^64-1 (MySQL error 1690). |
| `column_type_in_function:<name>` | A TIME, ENUM, SET, YEAR or BIT column inside a function the plugin does not translate for that type. |
| `applier_behind` | The copy is not yet current for this connection's last commit (5 s wait expired). |
| `not_loaded:<table>` | That table has no copy. |
| `window`, `unsupported_item:...` | Window functions and other constructs not translated. |

### 12.4 The applier is behind or stopped

```sql
SHOW GLOBAL STATUS LIKE 'duckdb_apply_reader_state';
```

- `refreshing db.t` for long: a refresh after DDL on a large table; wait.
- `error: <reason>`: look at the error log
  (`grep -i "binlog applier" "$BASE/error.log" | tail`).
- Binary logs purged before the applier read them: the error log names the
  tables that miss changes. Reload them and restart the applier:

```sql
-- ALTER TABLE <each named classic table> SECONDARY_UNLOAD;
-- ALTER TABLE <each named classic table> SECONDARY_LOAD;
-- ALTER TABLE <each named history table> SECONDARY_LOAD;   -- resync, history kept
-- SET GLOBAL duckdb_apply_start_position = 'FIRST';
```

### 12.5 A copy is STALE

```sql
SELECT TABLE_SCHEMA, TABLE_NAME, STATE FROM information_schema.DUCKDB_TABLES WHERE STATE <> 'LOADED';
```

The error log tells why (`refresh after DDL failed`, `kept and marked
stale`). Classic table: `SECONDARY_UNLOAD` + `SECONDARY_LOAD`. History
table: `SECONDARY_LOAD` reattaches if the structure still matches; a table
dropped for good: `duckdb_discard_stale_copy` (with confirmation).

### 12.6 Compare InnoDB and DuckDB for a query

```sql
SET SESSION use_secondary_engine = OFF;
SELECT region, COUNT(*), SUM(amount) FROM orders GROUP BY region ORDER BY region;
SET SESSION use_secondary_engine = FORCED;
SELECT region, COUNT(*), SUM(amount) FROM orders GROUP BY region ORDER BY region;
```

For many queries: `project_duckdb/duckdb_offload_compare.sh` runs a file of
queries both ways and reports the differences.

### 12.7 More detail

- `log_error_verbosity = 3`: the plugin's notes (loads, refreshes, applier
  start and stop).
- `SET GLOBAL duckdb_apply_debug_log = ON`: one line per applied change
  (testing only).
- The optimizer trace (section 4.4) for the generated DuckDB SQL.
- With the server stopped, read the lake itself with `duckq` (section 13).

### 12.8 Replicas

Load copies on a replica with `SET sql_log_bin = 0` (and `super_read_only =
OFF` for the operation), grant `DUCKDB_HISTORY_READER` the same way: these
statements are written to the binary log by the server and would otherwise
be errant GTIDs. Read `HOWTO.md` section 2, "Replicas", before using a
replica.

---

## 13. Read the lake when MySQL is down

The copies are an ordinary DuckLake lake: with MySQL stopped, DuckDB reads
them directly, for example to get data out while the server cannot start.
The tool `duckq` (`project_duckdb/tools/duckq.c`) is built from the same
DuckDB and DuckLake libraries as the plugin, so the versions match.

### 13.1 Build duckq (once)

```bash
L="$BUILD/extra/duckdb/install/lib"
clang++-21 -x c "$SRC/project_duckdb/tools/duckq.c" -x none \
  -I"$BUILD/extra/duckdb/install/include" -o "$BASE/duckq" \
  -Wl,--start-group $(ls "$L"/lib*.a) -Wl,--end-group -lpthread -ldl
```

### 13.2 Stop MySQL and read

While mysqld runs it holds a lock on the catalog, and the attach fails with
`Could not set lock on file ... Conflicting lock is held in .../mysqld`.

```bash
"$BASE/stop"
LAKE="$BASE/duckdb_data"
A="ATTACH 'ducklake:$LAKE/catalog.ducklake' AS lake (DATA_PATH '$LAKE/data/', READ_ONLY)"

# the tables in the lake and their state
"$BASE/duckq" :memory: "$A" \
  "SELECT db, tbl, state, retain_history FROM lake.__mysql.tables ORDER BY 1, 2"

# a query: each argument is one statement, results are tab-separated
"$BASE/duckq" :memory: "$A" \
  "SELECT region, COUNT(*), SUM(amount) FROM lake.tut.orders GROUP BY region ORDER BY region"

# a history table: live rows (what InnoDB has) and archived rows
"$BASE/duckq" :memory: "$A" \
  "SELECT COUNT(*) FILTER (WHERE \"__archived_at\" IS NULL) AS live,
          COUNT(*) FILTER (WHERE \"__archived_at\" IS NOT NULL) AS archived
     FROM lake.tut.readings"

# export to Parquet or CSV
"$BASE/duckq" :memory: "$A" \
  "COPY (SELECT * FROM lake.tut.readings) TO '$BASE/readings.parquet' (FORMAT parquet)" \
  "COPY (SELECT id, region, amount FROM lake.tut.orders) TO '$BASE/orders.csv' (HEADER)"
"$BASE/start"
```

### 13.3 What you see

| In the lake | Meaning |
|---|---|
| `lake.<db>.<table>` | The copy of MySQL table `<db>.<table>`. |
| `lake.__mysql.tables` | The registry: one row per copy (state, history flag, load position). |
| `__archived_at` | History tables: NULL for live rows, the purge time for archived rows. |
| `__w_<column>` | MySQL's sort weight of a string column (collations DuckDB cannot compare). |
| `__n_<column>` | ENUM index or SET bitmask. |
| strings | UTF-8; CHAR without trailing spaces. |
| TIMESTAMP | UTC. |
| TIME | Signed microseconds (`t / 1000000` = seconds). |
| YEAR, BIT | Numbers. |

Always attach with `READ_ONLY`: a change made directly in the lake is not
in InnoDB and not known to the applier, and the copy would no longer match
the table. To read while MySQL runs, query through MySQL, or copy the whole
`duckdb_data` directory while MySQL is stopped and read the copy.

---

## 14. What can and cannot be done

### Can

- Keep a DuckDB copy of InnoDB tables, current within about a second,
  through the binary log; on a source or on a replica (the source does not
  need the plugin).
- Run analytic SELECTs in DuckDB with exactly InnoDB's results: filters,
  joins (inner, LEFT, RIGHT, semi and anti joins), subqueries (IN, EXISTS,
  scalar, correlated), derived tables, views, CTEs, UNION / INTERSECT /
  EXCEPT, GROUP BY, HAVING, ORDER BY, LIMIT, COUNT / SUM / AVG / MIN / MAX
  (also DISTINCT), arithmetic and exact decimal division, CASE / IF /
  IFNULL / COALESCE / NULLIF, LIKE (binary collations), EXTRACT and date
  parts, DATE_ADD / DATE_SUB / INTERVAL arithmetic, DATEDIFF, TO_DAYS,
  LAST_DAY, FLOOR / CEILING / SIGN / TRUNCATE / SQRT / LN / LOG,
  GREATEST / LEAST, CONCAT / CONCAT_WS, SUBSTRING / LEFT / RIGHT, TRIM,
  REPLACE, REVERSE, LPAD / RPAD, INSTR / LOCATE, CHAR_LENGTH / LENGTH.
- DECIMAL results beyond 38 digits: InnoDB answers, or rounded with a
  warning, or an error (`duckdb_decimal_overflow`).
- Follow every collation in comparisons, grouping and ordering (`_ci`,
  `_as_cs`, `general_ci`, latin1 ...).
- Column types: integers, DECIMAL up to 38 digits, FLOAT, DOUBLE, CHAR,
  VARCHAR, TEXT, BINARY, VARBINARY, BLOB, DATE, DATETIME, TIMESTAMP, YEAR,
  BIT, TIME, ENUM, SET.
- Keep history: purge rows from InnoDB and keep them in DuckDB; query live
  or full history; archive partitions by purging them before dropping them.
- Survive DDL: ALTER, RENAME, TRUNCATE, partition changes without stopping
  replication or the applier.
- Read your own writes; KILL QUERY and `max_execution_time` stop a DuckDB
  query.

### Cannot (in this version)

- Write to DuckDB directly: all writes go to InnoDB.
- Offload everything: window functions, UPPER / LOWER, DATE_FORMAT, CAST,
  LIKE and string functions on non-binary collations, functions on
  TIMESTAMP columns, a grouped VARCHAR in a join, GROUP_CONCAT, JSON
  functions, recursive CTEs, LATERAL. They run on InnoDB.
- Column types JSON, spatial, VECTOR: the table cannot be loaded.
- Tables without a primary key, with zero dates; with
  `innodb_native_foreign_keys = ON`, children of cascading foreign keys
  (with the default OFF every foreign key works).
- `DROP PARTITION` / `TRUNCATE PARTITION` as archiving (purge first).
- Update or delete archived rows (they are history); UPDATE / DELETE of a
  history table under FULL scope.
- `binlog_encryption=ON`, `duckdb_encryption`, XA transactions on loaded
  tables.
- Reclaim the space of dropped copies and old versions (later phase).
- Refresh after DDL incrementally: the whole table is loaded again.

---

## 15. Clean up

First remove the history copies: dropping the database would leave them in
the lake as stale copies (that is their purpose).

```sql
SET SESSION duckdb_unload_history_confirm = ON;
ALTER TABLE tut.readings SECONDARY_UNLOAD;
ALTER TABLE tut.sales SECONDARY_UNLOAD;
SET SESSION duckdb_unload_history_confirm = OFF;
```

Then drop the database and stop the server:

```bash
m -e "DROP DATABASE tut"
"$BASE/stop"
# rm -rf "$BASE"        # everything, including the lake
```
