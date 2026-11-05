# DWL#6: Query offload through the external executor

Affects: Percona Server 9.7 — Status: Design, under review

## Description

### GENERAL IDEA

Reporting queries on loaded tables run in DuckDB. The server's secondary engine
framework decides whether to try offload; our hooks decide whether the query can
run in DuckDB, translate it into DuckDB SQL, run it on the session's DuckDB
connection and return the rows to the client. MySQL's own executor is not used
for offloaded queries: we use the external executor hook
(`SecondaryEngineFlag::USE_EXTERNAL_EXECUTOR` and `JOIN::override_executor_func`),
so DuckDB runs the whole query vectorized.

### WHEN A QUERY IS OFFLOADED

- `use_secondary_engine` is ON or FORCED;
- every table in the query has `SECONDARY_ENGINE=duckdb` and is loaded (joins
  included);
- the estimated cost is above `secondary_engine_cost_threshold` (our pre-prepare
  hook), unless forced;
- every expression is on the allow list;
- the statement is a top-level SELECT returning rows to the client;
- the session has not written in the current transaction.

### HISTORY SCOPE

In LIVE scope we add `__archived_at IS NULL` for every retain_history table, so
DuckDB returns the same rows as InnoDB and fallback to InnoDB is safe. In FULL
scope the predicate is omitted, and fallback is forbidden (DWL#7).

## Requirements

### Functional Requirements

1. An eligible SELECT on loaded tables runs in DuckDB and returns the same
   columns, types and rows as on InnoDB (within the semantic limits of Phase 1).
2. Joins (inner, left, right outer, semi and anti joins from IN and EXISTS) are
   offloaded when all tables are loaded DuckDB tables.
3. Supported in the POC: column references, literals, comparison, BETWEEN, IN
   lists, IS NULL, AND, OR, NOT, arithmetic, CASE, COUNT, SUM, AVG, MIN, MAX,
   COUNT(DISTINCT), GROUP BY, HAVING, ORDER BY, LIMIT and OFFSET, DISTINCT,
   derived tables and views over loaded tables, and a first set of date
   functions (YEAR, MONTH, DAY, DATE, DATE_FORMAT subset: to define).
4. Any unsupported construct rejects the offload with a reason, visible in the
   optimizer trace and in a status counter; in LIVE scope the query then runs on
   InnoDB.
5. In LIVE scope the predicate `__archived_at IS NULL` is added for each
   retain_history table; in FULL scope it is not.
6. INSERT ... SELECT, CREATE ... SELECT, SELECT ... INTO, and subqueries inside
   DML are not offloaded (POC).
7. A query in a transaction that has already written is not offloaded (LIVE) or
   rejected (FULL).
8. With `use_secondary_engine=FORCED`, a query that cannot be offloaded fails
   with our reason.
9. EXPLAIN shows that the query is executed by DuckDB and the scope used; EXPLAIN
   FORMAT=TREE shows the generated DuckDB SQL (proposal).
10. Status variables count offloaded queries, rejections by reason, and DuckDB
    execution time.
11. A KILL QUERY on an offloaded query interrupts DuckDB execution.

### Non-Functional Requirements

1. On the reference analytical workload, offloaded queries are measurably faster
   than InnoDB execution (POC exit criterion).
2. Translation adds negligible time compared with DuckDB execution.
3. Memory used by an offloaded query is bounded by `duckdb_memory_limit`; above
   it, DuckDB spills to `tmp/` or fails the query, never mysqld.

## Dependent Tasks

- DWL#3: Secondary tables: SECONDARY_LOAD and SECONDARY_UNLOAD.
- DWL#5: History retention (scope variable and `__archived_at`).

## High Level Architecture

### Contents

1. Hooks
2. Eligibility walk
3. SQL generation
4. External executor
5. Result conversion
6. Cancellation

### Hooks

| Hook | Role |
|---|---|
| `secondary_engine_pre_prepare_hook` | Cost threshold (from the MOCK engine) |
| `prepare_secondary_engine` | Per-statement context; statement options as in the MOCK engine |
| `optimize_secondary_engine` | Eligibility walk, SQL generation, set `override_executor_func` |
| `compare_secondary_engine_cost` | Optimizer cost (as MOCK) in the POC |
| `find_secondary_engine_offload_fail_reason` | Our rejection reason for FORCED and FULL |

`secondary_engine_flags` includes `USE_EXTERNAL_EXECUTOR`, so the server does not
create iterators for offloaded statements (sql/sql_optimizer.cc).

### Eligibility walk

A recursive walk over the query expression: query blocks, table references and
join conditions, select list, WHERE, GROUP BY, HAVING, ORDER BY, LIMIT. Each Item
is checked against the allow list by type and function name.

### SQL generation

The same walk emits DuckDB SQL text. Tables become `lake.<db>.<table>` with the
scope predicate; derived tables and views become subqueries; expressions are
printed with DuckDB syntax and explicit casts where MySQL and DuckDB differ.
Generating SQL from the query block lets DuckDB choose join order and plans.

### External executor

`override_executor_func(JOIN *, Query_result *)` is called once per query block
by `Query_expression::execute` (sql/sql_union.cc). For a single-block statement
we run the full SQL there; for UNION we generate one statement for the whole
expression and run it on the first call (to verify the call pattern for set
operations).

### Result conversion

Results are fetched chunk by chunk (`duckdb_fetch_chunk`) and each value is
converted to the MySQL type of its select-list item. Two options for sending rows,
to verify:

- (a) value holder Items (one per select-list position, typed like the original
  item) filled per row, then `query_result->send_data(thd, holders)`;
- (b) direct use of the protocol when the result goes to the client.

Option (a) is preferred because it works with every `Query_result`.

### Cancellation

A watcher checks `thd->killed` between chunks and calls `duckdb_interrupt()` on
the connection.

## Low Level Design

### Files

| File | Content |
|---|---|
| `storage/duckdb/offload/hooks.cc` | Hook implementations |
| `storage/duckdb/offload/eligibility.h/.cc` | Allow list and walk |
| `storage/duckdb/offload/sqlgen.h/.cc` | DuckDB SQL generation |
| `storage/duckdb/offload/executor.h/.cc` | External executor, fetch, conversion, send |
| `storage/duckdb/duckdb_types.cc` | DuckDB to MySQL value conversion (shared) |

### Rejection reasons

Enumerated and counted: `not_loaded`, `unsupported_item:<name>`,
`unsupported_statement`, `written_in_transaction`, `scope_full_not_allowed`,
`duckdb_disabled`, `below_cost_threshold`.

### Type conversion back to MySQL

| DuckDB | MySQL item result |
|---|---|
| integer types | integer, with signedness of the item |
| DECIMAL | `my_decimal` |
| DOUBLE, FLOAT | double |
| VARCHAR | string in the item's character set |
| DATE, TIMESTAMP | `MYSQL_TIME` |
| aggregates | the item's result type; DuckDB SUM and AVG types are cast in SQL to match MySQL precision (to define per function) |

### Open points

- Value holder Items versus protocol: to verify.
- Behaviour of `override_executor_func` for UNION and set operations: to verify.
- Exact list of date functions and their DuckDB equivalents: to define.
- Collations: string comparison and grouping follow DuckDB in the POC; Phase 2
  makes them follow MySQL collations or rejects.

## Test Plan

MTR suite `duckdb`:

1. `offload_basic`: each supported construct; result compared between
   `use_secondary_engine=OFF` and `FORCED`.
2. `offload_joins`: join types on loaded tables; join with a non-loaded table not
   offloaded.
3. `offload_reject`: unsupported constructs; reason in optimizer trace and status.
4. `offload_live_scope`: archived rows hidden in LIVE.
5. `offload_full_scope`: archived rows visible in FULL.
6. `offload_transaction`: no offload after a write in the same transaction.
7. `offload_explain`: EXPLAIN output.
8. `offload_kill`: KILL QUERY on a long DuckDB query.
9. `offload_memory`: query above `duckdb_memory_limit` spills or fails cleanly.
10. Performance: reference analytical workload, InnoDB versus DuckDB timings
    (outside MTR).
