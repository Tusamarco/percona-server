# DWL#6: Query offload through the external executor

Affects: Percona Server 9.7 — Status: steps 6a and 6b implemented, verified on a throwaway instance (2026-11-12) and on the reference instance (6a 2026-11-12, 6b 2026-11-13); 6c and later design

Steps (one at a time, tested by Marco before the next):

- 6a (implemented): hooks, external executor, single loaded table; column
  references, literals, comparisons, BETWEEN, IN lists, IS [NOT] NULL, AND,
  OR, NOT; COUNT, SUM, AVG, MIN, MAX; GROUP BY, HAVING, ORDER BY, LIMIT,
  OFFSET; LIVE/FULL scope; read your own writes; status and trace.
- 6b (implemented): expressions (CASE, IF, IFNULL, COALESCE, NULLIF,
  arithmetic, DIV, MOD, ABS, ROUND), LIKE, COUNT(DISTINCT), SUM/AVG
  DISTINCT, DISTINCT, date parts of DATE/DATETIME, PAD SPACE binary
  collations (`utf8mb3_bin`, `utf8mb4_bin`), TIMESTAMP constants.
- 6c: joins, semi and anti joins, derived tables, views, UNION.
- 6d: EXPLAIN with the DuckDB SQL, registry fix (cached TABLE objects),
  performance measurement.
- Weight-string columns for non-binary collations: a later step, to be
  placed after 6c (decided 2026-11-12).

Decisions (2026-11-12):

- A query is offloaded only if the generator can translate every part of it
  exactly; anything else is refused with a reason, and the server then runs
  it on InnoDB (`use_secondary_engine=ON`) or reports the reason (FORCED).
  We print our own DuckDB SQL from the resolved query; `Item::print()` is not
  used (MySQL dialect).
- Freshness: `duckdb_offload_wait_applied` (session, default ON): before a
  query runs in DuckDB, the applier must have applied the session's last
  commit (at most 5 s, otherwise InnoDB).
- Collations in the POC: binary strings and NO PAD binary collations
  (`utf8mb4_0900_bin`) are exact; PAD SPACE binary collations (`utf8mb3_bin`,
  `utf8mb4_bin`) are exact since 6b (`rtrim`, padded sort key); other
  collations are refused
  (`collation:<column>`) until weight strings.

Before step 6a the engine declined every query and the handler refused a
query forced into it (error 213); before 2026-11-12 that returned an empty
result instead. The handler still refuses any scan it is asked for: with the
external executor it is never asked.

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
5a. Collations: comparisons (`=`, `<`, `IN`, `BETWEEN`, `LIKE`), `GROUP BY`,
   `ORDER BY`, `DISTINCT`, `MIN`/`MAX` and joins on string columns whose
   collation is not binary (`_bin` or `binary`) reject the offload with the
   reason `collation`; in LIVE scope the query runs on InnoDB, in FULL scope it
   fails. String operations on binary collations, and all non-string
   operations, are offloaded. Results are converted back to the character set
   of each select-list item. Phase 2 lifts the restriction with weight-string
   columns (design doc, Semantic equivalence).
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
- DWL#4: Change propagation (offload on a copy that does not follow DML would
  return stale data).
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
`duckdb_disabled`, `below_cost_threshold`, `collation:<column>`; since 6b
also `unsupported_function:<name>`, `string_number_comparison`,
`exact_division`, `arithmetic_on_unsigned`, `round_of_double`,
`distinct_padded_string`, `date_function_argument`, `like_on_binary_string`,
`duckdb_prepare: <DuckDB error>` and others (the name says the construct).

### Type conversion back to MySQL

| DuckDB | MySQL item result |
|---|---|
| integer types | integer, with signedness of the item |
| DECIMAL | `my_decimal` |
| DOUBLE, FLOAT | double |
| VARCHAR | string in the item's character set |
| DATE, TIMESTAMP | `MYSQL_TIME` |
| aggregates | the item's result type; DuckDB SUM and AVG types are cast in SQL to match MySQL precision (to define per function) |

### Implemented in step 6a

Files: `duckdb_offload.h/.cc`.

- Hooks: `secondary_engine_pre_prepare_hook` (lake open, cost above
  `secondary_engine_cost_threshold`), `prepare_secondary_engine` (context,
  `OPTION_NO_CONST_TABLES | OPTION_NO_SUBQUERY_DURING_OPTIMIZATION`),
  `optimize_secondary_engine` (checks, SQL, freshness wait, sets
  `override_executor_func`), `compare_secondary_engine_cost` (as MOCK),
  `find_secondary_engine_offload_fail_reason`. Flags: `SUPPORTS_HASH_JOIN`,
  `SUPPORTS_NESTED_LOOP_JOIN`, `USE_EXTERNAL_EXECUTOR`. The optimize hook runs
  after `Query_expression::optimize()` (sql/sql_select.cc), so it sees the
  optimized query block; a refusal (error) makes the server re-run the
  statement on InnoDB unless forced (sql/sql_prepare.cc).
- SQL: `SELECT CAST(<expr> AS <type>), ... FROM lake.<db>.<table> AS t
  [WHERE t."__archived_at" IS NULL AND ...] [GROUP BY] [HAVING]
  [ORDER BY ... NULLS FIRST/LAST] [LIMIT] [OFFSET]`. Each select-list value
  is cast to the DuckDB type matching the MySQL item (BIGINT, UBIGINT,
  DOUBLE, DECIMAL(p,s) read as text, VARCHAR, BLOB, DATE, TIMESTAMP). AVG of
  integers and decimals is computed as an exact DECIMAL sum divided by the
  count (DuckDB's AVG is DOUBLE). ORDER BY follows MySQL's NULL order.
  Constants compared with DATE or DATETIME columns are converted the MySQL
  way and printed as typed literals; the optimizer's `Item_multi_eq` is
  printed as equalities.
- Results: fetched chunk by chunk (`duckdb_execute_prepared_streaming`,
  `duckdb_fetch_chunk`); each column's DuckDB type is checked against the
  expected one before reading; values go into `Item_cache_*` holders typed
  like the select-list items, sent with `Query_result::send_data()`.
  TIMESTAMP (UTC in the lake) is converted to the session time zone; strings
  to the item's character set.
- Read your own writes: a transaction observer (`after_commit`, as semi-sync)
  records the last commit position of each connection that has sent a query
  to DuckDB; `close_connection` forgets it. A connection's first offloaded
  query waits for the current binlog end. `THD::get_trans_pos()` cannot be
  used: the server resets it at every statement (sql/sql_parse.cc).
- Refused: other statements, set operations, more than one table, not a
  base table, not loaded/stale, SQL_CALC_FOUND_ROWS, ROLLUP, DISTINCT,
  windows, subqueries, unsupported items/functions/aggregates, string
  comparisons/grouping/ordering/MIN/MAX on collations other than binary NO
  PAD, comparisons with TIMESTAMP, unsupported result types, DECIMAL above
  38, FULL scope without `DUCKDB_HISTORY_READER`, a write in the open
  transaction (the server also refuses multi-statement transactions, error
  3889), applier behind (5 s) or not running.
- Status: `duckdb_offload_queries`, `duckdb_offload_refused`,
  `duckdb_offload_last_refusal`; optimizer trace `duckdb_offload` with the SQL
  or the refusal reason.

### Lessons from step 6a

- With `USE_EXTERNAL_EXECUTOR` the server sends the result metadata itself
  and calls `override_executor_func` once per query block.
- A wrong cast (a DECIMAL vector read as VARCHAR) crashed the server: each
  column's type is now checked before reading a chunk.
- The optimizer turns string literals compared with temporal columns into
  `date_literal` / `datetime_literal` functions before our hook: constant
  temporal expressions are evaluated and printed as typed literals.
- `Item_cache_date::store_value()` requires no example item (`setup()` is not
  called for it); `Item_cache_datetime::store_value()` requires
  `MYSQL_TYPE_DATETIME` (also used for TIMESTAMP results).

### Implemented in step 6b

Same file, `duckdb_offload.cc`.

- Strings, by collation of the column: RAW (binary, NO PAD binary) compared
  as is; PAD (`utf8mb3_bin`, `utf8mb4_bin`): equality and IN on
  `rtrim(x, ' ')`, ordering (`<`, BETWEEN, ORDER BY, MIN/MAX) on the sort key
  `rtrim(x, ' ') || repeat(' ', N)` with N above the longest operand, which
  orders like PAD SPACE also for characters below a space (`'a' > 'a\t'`);
  any other collation refused (`collation:<column> (<collation>)`). LIKE
  keeps trailing spaces significant in both (no rtrim); LIKE on binary
  strings is refused (DuckDB has no LIKE on BLOB).
- GROUP BY a PAD column groups on `rtrim`; the column in the select list,
  HAVING or ORDER BY (outside aggregates) is `arg_min(t.col, <primary key>)`,
  the row a table scan meets first. MIN/MAX of a PAD column:
  `arg_min/arg_max(x, row(sort key, PK)) FILTER (WHERE x IS NOT NULL)`, the
  PK negated for MAX (ties such as `''` and `' '` give the smallest PK, as
  MySQL keeps the first value met); refused for a non-integer PK. DISTINCT
  on a PAD or other non-RAW string is refused; COUNT(DISTINCT) of a PAD
  column counts `rtrim` values.
- Comparisons: constants compared with DATE/DATETIME become typed
  literals; with a TIMESTAMP column, MySQL's value in the session time zone
  converted to UTC (refused in a DST gap). Refused: TIMESTAMP with another
  temporal, string or number operand; DATE/DATETIME with a string or number
  column; a string column with a number (MySQL compares as DOUBLE; DuckDB
  would cast the string). A number column with a string constant: the
  constant as DOUBLE.
- Arithmetic: `+ - *` with integers widened to BIGINT; unsigned columns
  refused; DECIMAL results above 38 digits refused. `/` only when MySQL's
  result is DOUBLE (`NULLIF` divisor: NULL for zero); exact division (DECIMAL
  result) refused. `DIV` of integers `//`, `MOD` `%`, both with a `NULLIF`
  divisor (truncation toward zero and the dividend's sign in both). ROUND of
  exact numbers (half away from zero in both); ROUND of DOUBLE (MySQL: half
  to even) and TRUNCATE refused.
- CASE (searched; simple CASE only on numbers and RAW strings), IF, IFNULL,
  COALESCE, NULLIF (PAD-aware comparison, returns the original value).
- Date parts of DATE/DATETIME: YEAR, MONTH, DAYOFMONTH, DAYOFYEAR, QUARTER,
  DAYOFWEEK (DuckDB + 1), WEEKDAY (isodow - 1), HOUR, MINUTE, SECOND, DATE.
  On TIMESTAMP refused (UTC in the lake: time-zone dependent).
- Aggregates: COUNT(DISTINCT x), SUM(DISTINCT), AVG(DISTINCT) (exact
  decimal like AVG). Output DECIMAL is read as at most DECIMAL(38,s): MySQL
  widens SUM to 65 digits, DuckDB computes in 38 and raises an error on
  overflow, never a wrong value.
- The DuckDB statement is prepared in the optimize hook: a DuckDB bind
  error is a refusal (`duckdb_prepare: ...`) and the query runs on InnoDB;
  the prepared statement and its connection live in the statement context.

### Lessons from step 6b

- The server sends the result metadata from `JOIN::fields`, which with
  GROUP BY can be temporary-table items of another type (BIGINT for an INT
  expression): the value holders must be typed from that list, not from the
  query block's select list (a debug assertion in `Protocol_text` crashed
  the server on `GROUP BY DAYOFWEEK(dt)`).
- `YEAR()` has result type `MYSQL_TYPE_YEAR` (sent as a short); `/` and
  `DIV` share `DIV_FUNC`.
- A literal's type matters in comparisons: `b4 = 1` must be refused at plan
  time, not fail in DuckDB at execution.
- Known difference (accepted): with a PAD column in GROUP BY, MySQL shows one
  value of each group (values equal but for trailing spaces). With a
  temporary table it is the first row met (smallest PK in a scan), which we
  reproduce; when MySQL groups by sorting (e.g. with COUNT(DISTINCT)) it is
  the first row after filesort, which depends on the plan: the shown value
  can then differ in trailing spaces only. MySQL does not define which value
  is shown.

### Open points

- FULL scope with `use_secondary_engine=ON`: a refused query (or one below
  the cost threshold) runs on InnoDB with live rows only, without an error.
  The rule "FULL never falls back" needs the DWL#7 guard; until then the
  HOWTO says to use FORCED with FULL.
- KILL QUERY during a long DuckDB query: implemented (killed checks, the
  executor interrupts the task), not yet tested with a query long enough.
- Registry entries and cached tables: from this worklog the server may keep
  secondary `TABLE` objects cached, holding a pointer to the registry entry's
  THR_LOCK. `Registry::reload()` must then mark entries absent instead of
  erasing them (today it erases entries no longer in the lake, which is safe
  only because no secondary table stays open).

- Value holder Items versus protocol: to verify.
- Behaviour of `override_executor_func` for UNION and set operations: to verify.
- More functions (string functions, DATE_FORMAT, date arithmetic,
  TIMESTAMP date parts in the session time zone, exact division): later.
- Collations: the POC rejects collation-dependent operations on non-binary
  collations (FR 5a); Phase 2 adds weight-string columns for exact semantics.

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
