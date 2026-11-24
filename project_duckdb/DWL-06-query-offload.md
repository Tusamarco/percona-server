# DWL#6: Query offload through the external executor

Affects: Percona Server 9.7 — Status: steps 6a and 6b implemented, verified on the reference instance (6a 2026-11-12, 6b 2026-11-13); 6c-1, 6c-2 verified on the reference instance, 6c-3, 6d-1 and 6d-2 implemented, verified on a throwaway instance (2026-11-13); 6d-3 first release run done; types, DECIMAL overflow, LIKE, grouped CHAR and the function review implemented with MTR tests (2026-11-16/17)

Steps (one at a time, tested by Marco before the next):

- 6a (implemented): hooks, external executor, single loaded table; column
  references, literals, comparisons, BETWEEN, IN lists, IS [NOT] NULL, AND,
  OR, NOT; COUNT, SUM, AVG, MIN, MAX; GROUP BY, HAVING, ORDER BY, LIMIT,
  OFFSET; LIVE/FULL scope; read your own writes; status and trace.
- 6b (implemented): expressions (CASE, IF, IFNULL, COALESCE, NULLIF,
  arithmetic, DIV, MOD, ABS, ROUND), LIKE, COUNT(DISTINCT), SUM/AVG
  DISTINCT, DISTINCT, date parts of DATE/DATETIME, PAD SPACE binary
  collations (`utf8mb3_bin`, `utf8mb4_bin`), TIMESTAMP constants.
- 6c, in three steps (decided 2026-11-13):
  - 6c-1 (implemented): joins (inner, cross, LEFT, RIGHT), semi- and
    anti-joins (IN, EXISTS, NOT EXISTS, NOT IN as the optimizer flattens
    them), derived tables, views, non-recursive CTEs, scalar subqueries the
    server rewrites into joins;
  - 6c-2 (implemented): UNION, INTERSECT, EXCEPT (DISTINCT and ALL),
    nested and parenthesized with their own ORDER BY / LIMIT, also in
    derived tables;
  - 6c-3 (implemented): subqueries the server leaves as expressions
    (IN / NOT IN with NULLs, EXISTS, scalar subqueries in any clause,
    correlated), the server's rewrites of the others, one-row checks.
- 6d: 6d-1 (implemented) EXPLAIN with the DuckDB SQL; 6d-2 (implemented) registry fix
  for cached TABLE objects, KILL and timeout; 6d-3 performance measurement
  on a release build (separate build directory) with a production-like
  workload (decided 2026-11-13).
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
  commit (at most `duckdb_offload_wait_timeout_ms`, default 5000, otherwise
  InnoDB; counters `duckdb_offload_waits`, `duckdb_offload_wait_timeouts`,
  2026-11-18).
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
  `duckdb_offload_last_refusal` (later also `_explained`, `_waits`,
  `_wait_timeouts`, `_decimal_reruns`); optimizer trace `duckdb_offload` with the SQL
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

### Implemented in step 6c-1

Same file, `duckdb_offload.cc`; the generator works per query block.

- Aliases: each table of the query is `tN`, columns `tN."col"`; a column
  of a table outside the block is refused
  (`outer_or_other_table_column:<table>.<column>`).
- FROM from the join tree (`Query_block::m_table_nest`, stored in reverse
  order by the parser): inner joins and CROSS JOIN, LEFT JOIN with its ON
  (RIGHT JOIN arrives as LEFT JOIN), nests in parentheses.
- LIVE scope: a history table is read as `(SELECT * FROM lake.db.t WHERE
  "__archived_at" IS NULL) AS tN`, so the filter stays on the table
  wherever it is (in WHERE it would turn a LEFT JOIN into an inner join).
- Semi-join nests (IN and EXISTS subqueries flattened by the optimizer)
  become `EXISTS (SELECT 1 FROM <nest> WHERE ...)`: the semi-join condition
  was merged into WHERE, so each AND part using the nest's tables moves
  into its EXISTS (multiple equalities split into pairs anchored on an
  outer column). Anti-join nests (NOT EXISTS, NOT IN on non-nullable
  columns) become `NOT EXISTS` with their own condition. Refused: a nest
  inside an outer join or inside another nest, a condition across two
  semi-joins.
- Semi-join materialization is disabled while the server optimizes a
  statement for us (that strategy rewrites WHERE to read its temporary
  table `<subqueryN>`); the session's `optimizer_switch` is restored in the
  optimize hook and when the statement context is destroyed.
- Materialized derived tables and CTEs: `(SELECT ... AS "col", ...) AS tN`,
  generated recursively, each value cast to the DuckDB type of its MySQL
  type (DECIMAL kept as DECIMAL). Merged views and derived tables need
  nothing. Refused: recursive CTEs, LATERAL, set operations inside.
- Scalar subqueries in the select list: the server already rewrites them
  into a LEFT JOIN with a grouped derived table (`coalesce(..., 0)` for
  COUNT); they are offloaded that way.
- PAD SPACE columns in joins: grouping, ordering and comparisons are exact
  and allowed; showing a grouped PAD value or MIN/MAX of a PAD column is
  refused (`padded_group_value_in_join`, `padded_min_max_in_join`): in a
  join the row InnoDB meets first depends on the join order (decided
  2026-11-13: allowed only when exactly what InnoDB does).
- Comparisons use the comparison's collation (`compare_collation()`), not
  only the columns'.
- `compare_secondary_engine_cost` no longer keeps a statement-wide best
  cost.

### Implemented in step 6c-2

- A set operation is printed as `SELECT ... FROM ((c0) OP (c1) ...) AS tN
  [ORDER BY] [LIMIT]`: the post-processing query block of the
  `Query_term_set_op` (ORDER BY, LIMIT of the whole operation) reads the
  result table, whose columns are named c1, c2, ... (positional, so
  duplicate names such as `SELECT a, a` cannot clash).
- Operators, left-associative in MySQL and DuckDB
  (`PT_set_operation::merge_descendants`): UNION is DISTINCT up to the last
  DISTINCT operand (`last_distinct`) and ALL after it; INTERSECT is
  DISTINCT if any operator is; EXCEPT is ALL before the first DISTINCT
  operand (`first_distinct`) and DISTINCT from it on.
- Each operand's columns are cast to the result types MySQL computed
  (`types_array()`). Mixing kinds is refused (`set_operation_mixed_types`,
  decided 2026-11-13): numbers combine with numbers, DATE with DATETIME, a
  NULL literal with anything; string with number, TIMESTAMP with DATETIME,
  binary with non-binary string are refused.
- Operations that compare rows (UNION DISTINCT, INTERSECT, EXCEPT, also
  ALL) need byte-comparable strings: PAD SPACE and other collations are
  refused there; UNION ALL accepts any collation.
- An operand without tables (`SELECT 1`) has no FROM.
- The external executor is set on every query block; the first call runs
  the statement, the rows go to the first block's count (FOUND_ROWS() as
  InnoDB). The value holders follow the query expression's field list
  (`get_field_list()`), the result columns for a set operation.
- Derived tables and CTEs with a set operation are offloaded the same way.

### Implemented in step 6c-3

For a secondary engine the server rewrites most subqueries into joins with
derived tables before our hook (`subquery_to_derived`): IN / EXISTS under
OR, scalar subqueries in WHERE and the select list, EXISTS / IN in the
select list, ANY and ALL (MAX, COUNT and `Item_bool_if`). They are
offloaded through 6c-1, plus:

- `Item_bool_if` (internal IF with constant branches) as CASE; cached
  expressions (`Item_cache`) as the expression they cache.
- A derived table made from a scalar subquery (`m_was_scalar_subquery`) has
  one row: its columns are wrapped in `any_value()` in the select list and
  HAVING of a grouped query (DuckDB requires an aggregate).
- `m_reject_multiple_rows` (the server's rewrite of an uncorrelated scalar
  subquery): MySQL raises ER_SUBQUERY_NO_1_ROW if the table has more than
  one row. We add `count(*) OVER ()` and `error(...)` when it is above 1.
  Before this step the check was missing: `WHERE grp = (SELECT grp FROM p6
  WHERE id < 10)` returned rows instead of error 1242 (found 2026-11-13).
- The correlated scalar subquery without aggregate is rewritten with
  `any_value` (MySQL then checks per key): refused
  (`correlated_scalar_subquery_without_aggregate`).

Subqueries left as expressions are printed as subqueries:

- `x IN (SELECT ...)` / NOT IN (`<in_optimizer>` wraps the predicate; it is
  found among its arguments), `EXISTS (...)`, scalar `(SELECT ...)` in any
  clause, with MySQL's truth test (NOT, IS [NOT] TRUE / FALSE as coalesce).
  DuckDB applies the same three-valued logic to IN and NOT IN.
- The in-to-exists strategy's additions are left out: conditions marked
  `created_by_in2exists()` in WHERE and HAVING, and the `LIMIT 1` marked
  `m_internal_limit` (it cut an IN list to one value).
- Correlated subqueries: a column may belong to an enclosing block (aliases
  are unique in the statement); an aggregate of an outer query inside a
  subquery is refused.
- IN compares like MySQL only for byte strings (RAW collations) and
  comparable kinds; others, row subqueries (`(a, b) IN`) and remaining
  ALL / ANY predicates are refused.
- Errors (decided 2026-11-13): when DuckDB fails with an error MySQL has
  too, the client gets MySQL's code with both messages, e.g. `ERROR 1242:
  Subquery returns more than 1 row (DuckDB error: Invalid Input Error: More
  than one row returned by a subquery ...)`; other DuckDB errors are 3877
  `DuckDB error: ...`. Unlike MySQL, DuckDB may raise the scalar subquery
  error for a row whose value MySQL would not evaluate (a branch of CASE not
  taken): an error where MySQL returns rows, never a wrong value.

### Lessons from step 6c-1

- A statement-wide "best cost" in `compare_secondary_engine_cost` made
  every plan of a second JOIN (anti-join) "not cheaper": the planner had no
  plan and asserted (`sql_planner.cc` consider_plan). The server's own
  comparison per JOIN must stand. Latent since 6a (one table, one plan).
- Semi-join materialization substitutes the subquery's columns in WHERE
  with columns of `<subqueryN>`: reading the optimized WHERE needs the
  strategy off.
- A simple derived table is merged: its columns are references to the
  inner expressions (`real_item()`), not fields.

### After the first performance run (decided 2026-11-14)

Order: C (cost rule) and B (exact division) first, then A (weight strings
for `_ci` collations, design under review).

C, `duckdb_offload_min_rows` (session, default 1,000,000, 0 disables): the
pre-prepare hook also tries a query in DuckDB when MySQL's InnoDB plan reads
a loaded table of at least that many rows by a full scan, a full index scan
or a non-unique index lookup, the access types whose row estimates come from
sampled statistics (the 382-instead-of-52,560 case). Primary key and unique
lookups and index ranges (index dives) keep MySQL's estimate, so they stay on
InnoDB under the cost threshold. The table size is InnoDB's table estimate
(free during planning; the error was per key, not per table). The decision
is in the optimizer trace (`duckdb_offload_candidate`).

B, exact decimal division (`/` of integers and decimals, and AVG), as MySQL
computes it (mysys/decimal.cc do_div_mod, Item_func_numhybrid):

- operand scales are rounded up to 9-digit words; div_precision_increment is
  reduced by that padding; the quotient is truncated at
  F = 9 * ceil((frac1 + frac2 + incr) / 9) digits;
- `/` then rounds half up to its scale (operand scale + increment, at most
  30): every arithmetic function's value is rounded to its scale, so later
  expressions see the rounded value; AVG keeps the F digits (only the output
  rounds);
- translation: the operands as exact integers (integer part and fraction
  separately), R = |A| 10^(F - f1 + f2) // |B| in HUGEINT, the sign of
  A * B, DECIMAL(38, F), then a cast to the scale (DuckDB rounds half away
  from zero, as MySQL's HALF_UP); NULL for a zero divisor. DECIMAL `+ - *`
  results are cast to MySQL's scale too (it matters with AVG operands).
- refused: operand scales above 18, F above 36. A value too large for 128
  bits is a DuckDB error, never a wrong value.
- AVG of integers and decimals used DuckDB's `/` (DOUBLE) since 6a: exact
  now (the tests had not hit a difference).
- Tests: `offload_queries/duckdb_offload_div_queries.sql` (table dv): increments 0, 4, 9,
  negatives, zero, NULL, nested divisions, ROUND of a division, AVG in
  expressions; the first version rounded where MySQL truncates (increment
  0 and 9) and used F digits where MySQL uses the rounded value (`a / b *
  2`): both found by these tests.

### Step A: weight strings for non-binary collations (decided 2026-11-14)

Decisions: weights for every string column whose collation DuckDB cannot
compare natively, automatically; results exactly as InnoDB (in a join, a
grouped `_ci` value is shown only when GROUP BY contains that table's primary
key); existing copies get weights at their next load (history copies: the
archived rows' weights are computed by the plugin at load / resync /
reattach / refresh).

A1, storage (implemented 2026-11-14):

- `duckdb_collation.h/.cc`: `needs_weight()` (non-binary collations except
  the binary-sort utf8mb3/utf8mb4 ones, which DuckDB compares on UTF-8
  bytes; latin1_bin and other non-UTF-8 binary collations need weights),
  `collation_weight()` = MySQL's `strnxfrm` of the value in its own
  character set with num_codepoints = its own length (no padding); for PAD
  SPACE collations the trailing spaces are removed first (`lengthsp`), so
  equal values have equal weights; ordering will append the space weight at
  query time (A2). Weights compare as bytes.
- The copy gets `"__w_<column>" BLOB` after the other columns (and
  `__archived_at`). The loader computes it from the InnoDB value; the binlog
  decoder from the row image bytes and the column's collation id (before the
  UTF-8 conversion); the applier writes it (`kWeightBase - index` in the
  column map).
- Archived rows (refresh after DDL, resync, reattach, adopt): copied without
  weights, then `backfill_archived_weights()` computes one weight per
  distinct value (UTF-8 converted back to the column's character set) and
  updates by value; this also covers a collation change and copies made
  before A1.
- The history structure check ignores `__w_` columns.
- `project_duckdb/tools/duckq.c`: runs SQL on a lake with the plugin's
  extensions, to inspect it while the server is stopped.
- Verified (throwaway instance): 0900_ai_ci, general_ci, latin1_swedish_ci
  columns get weights, utf8mb4_bin none; distinct weights = MySQL's COUNT
  (DISTINCT) (5, 4, 4); groups follow the collation (apple/Apple/ápple/APPLE
  one group, 'apple ' another with NO PAD); applier inserts and updates have
  weights; history copy after purge + resync: archived weights present;
  ALTER ... COLLATE utf8mb4_0900_as_cs recomputes them (6 distinct);
  reattach passes the structure check; unit test DuckdbCollation.Weights;
  all offload lists pass with weight columns in every copy.

A2, queries (implemented 2026-11-14):

- The registry records each copy's weight columns (`weight_columns`,
  migration; empty for copies loaded before A1: their `_ci` columns are
  refused, "copy without weights: reload it").
- String mode WEIGHT (any collation needing weights). `string_key()` gives
  the key a string compares by: bytes (RAW), rtrim / padded key (PAD), or
  the weight: the column's `__w_` column (its collation must be the
  comparison's), or a constant's weight computed at generation with
  MySQL's collation code (a BLOB literal); for ordering a PAD SPACE weight
  gets the space weight appended up to the longest value. Used by
  comparisons, IN lists, BETWEEN, NULLIF, multiple equalities and joins,
  ORDER BY, GROUP BY, COUNT(DISTINCT), MIN/MAX (ties broken on the primary
  key, single table).
- Grouped values (decided 2026-11-14: exactly as InnoDB), for PAD and
  weight collations: when GROUP BY contains all primary key columns of the
  column's table every row of the group has the same value (any_value);
  otherwise on a single table the row with the smallest primary key
  (arg_min); in a join (2026-11-22) any value, checked per group in the
  query: `CASE WHEN count(DISTINCT col) > 1 THEN error(marker) ELSE
  any_value(col) END`; such statements run first during optimization and
  the marker becomes the refusal `grouped_string_value_in_join` (a group
  with values equal under the collation but stored differently, whose
  shown value InnoDB picks by its join order). With duckdb_decimal_overflow
  = ERROR and a DECIMAL overflow in such a statement, the overflow is a
  refusal (InnoDB answers) rather than the statement's error.
- Derived tables and set operations carry `__w_<column>` next to the value
  (set operations: every operand, for the result's weight-collation
  columns), so the outer query groups and orders on them (UNION ALL of
  string literals with ORDER BY).
- Refused: LIKE on weight collations, functions over them compared or
  grouped (UPPER(c) = ...), SELECT DISTINCT, UNION DISTINCT / INTERSECT /
  EXCEPT, IN subqueries on them, MIN/MAX in joins.
- Tests: `offload_queries/duckdb_offload_a2_queries.sql` (32: cw with 0900_ai_ci,
  general_ci, latin1_swedish_ci, 0900_as_cs; cwd join; dim, p6, g) and
  `offload_queries/duckdb_offload_a2_refusals.sql` (7); three earlier refusals on `_ci`
  columns moved to the A2 list. All passed at the first run.

### Step 6d-3: performance (in progress)

- Release build in its own directory (`../percona-server-9.7-release`,
  `CMAKE_BUILD_TYPE=RelWithDebInfo`, `DUCKDB_BUILD_TYPE=RelWithDebInfo`;
  targets mysqld, duckdb, mysql, mysqladmin); the debug build is unchanged.
- Workload (`project_duckdb/perf/`): a wind farm monitoring schema close to
  production — region, site, turbine (names in the server's default
  collation), reading (one row per turbine every 10 minutes, indexed on
  (turbine_id, ts) and ts), alarm, maintenance (history table purged with
  the tag); 200 turbines x 365 days = 10.5 million readings by default,
  deterministic data. 16 queries: dashboards and reports expected in DuckDB,
  and four expected on InnoDB (primary key lookup, short index range,
  grouping on a `_ci` name, a window function).
- `duckdb_perf_run.sh` runs each query RUNS times on InnoDB (OFF) and with
  ON and the default cost threshold, as in production; it reports where the
  query ran, the median times, the speedup, and whether the results match.

First run, 2026-11-13 (release build, throwaway instance on the HDD
machine, 8 cores; buffer pool 4G, duckdb_memory_limit 4G, duckdb_threads 6;
`perf/results-2026-11-13-release.txt`):

- Setup 185 s; SECONDARY_LOAD of 10,512,000 readings 15.4 s, small tables
  0.1-0.2 s (the slow loads seen before were the debug build). DuckDB copy
  58 MB of Parquet.
- All results identical. Offloaded queries, median of 3: q02 31.9 s ->
  0.055 s (584x), q03 13.2 -> 0.118 (112x), q07 65.8 -> 0.140 (472x), q08
  2.45 -> 0.045 (55x), q09 21.4 -> 0.120 (179x), q11 4.08 -> 0.054 (76x).
  q13 (primary key) and q14 (short index range) stay on InnoDB by cost, as
  intended; q16 (window function) is refused.
- Not offloaded although heavy, three causes:
  1. `_ci` collations: GROUP BY a dimension name (q01 site name, q04
     turbine model, q15), and string literals, which carry the connection
     collation (q10 `'h1' AS p` in ORDER BY). With numbers instead, q10 runs
     in 0.104 s instead of 64.0 s (615x).
  2. Exact division: `SUM(decimal) / 6`, `SUM(a) / SUM(b)` (q01, q04) are
     refused (`exact_division`, MySQL returns an exact DECIMAL).
  3. MySQL's cost estimate: InnoDB estimated 382 readings per turbine (real
     52,560) even after ANALYZE TABLE, so q04's join cost (26,754) stayed
     below secondary_engine_cost_threshold (100,000) and was never offered
     to the engine.
- q12 (FULL scope, small table) ran on InnoDB below the threshold and
  returned live rows only: the known FULL fallback gap (DWL#7).

### Implemented in step 6d-1

- EXPLAIN (any format) of an offloaded query adds notes (code 3877): the
  DuckDB SQL, and DuckDB's own plan (`EXPLAIN <sql>` in DuckDB), which is
  the plan that runs; MySQL's printed plan only shows that the query goes
  to the secondary engine (note 1003). SHOW WARNINGS shows at most 512
  bytes per message, so long text is split in numbered notes
  ("DuckDB plan (2/7)"). A refused query (ON) has no notes; the reason is
  in `duckdb_offload_last_refusal` and the optimizer trace.
- EXPLAIN ANALYZE is refused by the server for any secondary engine
  (error 1235).

### Implemented in step 6d-2

- Registry entries are never freed: a table that leaves the lake (unload,
  DROP, discard, RENAME to another name, absent at reload) is marked not
  present. Secondary TABLE objects the server keeps in its table cache
  point to the entry's THR_LOCK, and nothing guarantees they are evicted
  first (SECONDARY_UNLOAD does not evict them, nor do our applier's DDL
  handling or `duckdb_discard_stale_copy`).
- KILL QUERY interrupts DuckDB (about 1 s on a 200,000 x 20,000 inequality
  join) and frees the executor thread. Errors as MySQL's: KILL QUERY gives
  1317, `max_execution_time` 3024, each with DuckDB's message.
- Verified: an offloaded query before and after SECONDARY_UNLOAD +
  SECONDARY_LOAD, applier RENAME and back, DROP + re-create, duckdb_enable
  OFF/ON, DROP of a history table + discard + re-create; two sessions
  offloading every 0.3 s during six unload/load cycles (about 680 queries:
  each either the right result or a clean "not loaded"; no crash).
- Seen: after RENAME TABLE the copy is under the new name only once the
  applier has read the RENAME (seconds under load); a query in between
  finds no copy (ON: InnoDB; FORCED: "Table has not been loaded").

### Lessons from step 6c-3

- Most subqueries never reach us as subqueries: the server's rewrites for
  secondary engines turn them into joins; reading its optimizer trace
  (`expanded_query`) shows what we receive.
- A rewrite can carry runtime checks (`m_reject_multiple_rows`): a derived
  table is not always "just a table".
- `Item_in_optimizer` is built with one argument; the left operand comes
  later: positions must not be assumed.
- The in-to-exists LIMIT 1 is invisible in the printed query but changes an
  IN list.

### Lessons from step 6c-2

- `Item::is_null()` on a column reads the value in the current row buffer:
  used to recognize a NULL literal it skipped the kind check at random, and
  `ts UNION ALL dt` returned TIMESTAMP values in UTC instead of the session
  time zone (one hour off). A NULL literal is recognized by its item type.

### YEAR and BIT (2026-11-16)

Load and applier: DWL-03 (YEAR as USMALLINT, BIT as UBIGINT). Offload:

- Column references need nothing new (they are numbers). Constants in
  comparisons come as MySQL resolved them (`y = 5` is 2005, `y = '0'` 2000,
  `y = 0` 0000), so translating the resolved item gives MySQL's result.
- YEAR results: MySQL sends a YEAR **column** zero-filled (`0000`,
  `Field_year::send_to_protocol`), also through GROUP BY, DISTINCT, derived
  tables and views, but a YEAR **expression** (`MAX(y)`, `IFNULL(y, 1)`) as a
  plain number (`0`, `Item::send`). A column gets `Year_column_holder`
  (`store_short(nr, 4)`), an expression the integer holder. A YEAR column of
  a set operation (UNION) is refused (`unsupported_result_type`).
- BIT results (a column, MIN/MAX of one): `Kind::BIT`, read as UBIGINT and
  sent as its ceil(n/8) bytes, big-endian, as `Field_bit::val_str`.
- Unsigned arithmetic: MySQL raises error 1690 when an unsigned result
  leaves 0..2^64-1, and DuckDB may evaluate other rows than MySQL (filter
  order, TopN), so only `+` and `*` that cannot leave the range are
  translated: operands are unsigned columns (BIT, YEAR, unsigned integers)
  or non-negative integer constants, and the type maximums keep the result
  within 64 bits (`b + 0`, `y + 1`, `u * 3` of an INT UNSIGNED). Subtraction,
  possible overflow, signed operands and DECIMAL context stay refused
  (`arithmetic_on_unsigned`).
- Fixed: an unsigned BIGINT result from 2^63 was sent as a negative number
  (`Item_cache_int::store_value` copies `unsigned_flag` from the item it is
  given; the holder now keeps it set).
- Tests: `offload_queries/duckdb_offload_yb_queries.sql` (33, identical and offloaded),
  `offload_queries/duckdb_offload_yb_refusals.sql` (5), MTR `offload_yb`; the 6b refusal
  `u + 1` (now exact) was replaced by `u - 1`.

### TIME (2026-11-16)

- Lake value: signed microseconds (BIGINT). `Kind::TIME`: read as BIGINT,
  returned through `Item_cache_time` (`Time_val::make_time`).
- Comparisons (`operands()`, multiple equalities): a TIME column only with
  TIME columns and constants; each constant is converted with
  `Item::val_time` (MySQL compares TIME with a constant as TIME), so
  `'12:30:45'`, `123045`, `12`, `'1 02:00:00'`, `12.5` give MySQL's rows.
  TIME with DATETIME, strings or numeric columns: refused
  (`mixed_time_comparison`).
- Results: a TIME column, MIN/MAX of one. TIME expressions (IF, CASE,
  IFNULL) are refused. SUM/AVG (MySQL sums HHMMSS numbers) and arithmetic
  were already refused for temporal arguments.

### ENUM and SET (2026-11-16)

- Lake: the string value (weighted like VARCHAR) and `__n_<col>` (index /
  bitmask). The generator knows every ENUM / SET column of a loaded table has
  `__n_<col>` (no older copies exist: these types were refused before).
- String contexts use the value: comparisons with strings (collation and
  weights as for VARCHAR), IN, BETWEEN, LIKE, MIN/MAX (MySQL compares ENUM
  and SET by string value in MIN/MAX, verified), GROUP BY, DISTINCT,
  COUNT(DISTINCT), the result (data type STRING).
- The index (`enum_index()`): `ORDER BY e` (MySQL sorts on the element
  position; `min(__n_e)` in the grouped part of a block: one index per
  value, elements being distinct under the collation), comparisons with
  number constants (`e = 2`: MySQL compares as DOUBLE, `val_real` is the
  index), arithmetic (`e + 0` is DOUBLE). Columns of materialized derived
  tables and set operations carry no index: ORDER BY / numeric use refused.

### Columns with a special lake value (2026-11-16)

TIME (microseconds), ENUM / SET (value + index), YEAR and BIT (numbers MySQL
shows as `0000` or bytes in string contexts) are translated only where the
generator knows their meaning: comparisons, IN, BETWEEN, IS [NOT] NULL,
arithmetic (each with its own rules above), LIKE for ENUM / SET.
`special_args()` refuses them as arguments of other functions
(`column_type_in_function:<name>`: LIKE on TIME, IFNULL, CASE, CONCAT,
FIND_IN_SET, HOUR, ...), except YEAR and BIT where the function's result is
an integer (`IFNULL(y, 1999)`). Without this, `t LIKE '12%'` would have
matched the microseconds.

Tests: `duckdb_offload_tt_*` (24 / 7), `duckdb_offload_et_*` (28 / 6),
`duckdb_offload_yb_*` (33 / 7); MTR `offload_time`, `offload_enum` (with a
history table across an ENUM / SET definition change), `offload_yb`; unit
tests for the decoder (41 in all).

### DECIMAL results above 38 digits (2026-11-17, decided with Marco)

MySQL declares DECIMAL results up to 65 digits; DuckDB's DECIMAL holds 38
(a 128-bit integer) and raises an error on overflow, never a wrong value.
DuckDB's arbitrary-size `BIGNUM` was tested and is not usable: its `*`, `/`
return DOUBLE and `sum` returns NULL.

- Results declared above 38 digits are computed as `DECIMAL(38, scale)`
  (MySQL's scale, at most 30): exact while the integer part has at most
  38 - scale digits. The refusal `decimal_precision_above_38` is gone.
- A statement is "wide" when a DECIMAL result is declared above 38 digits,
  or an exact division's dividend scaled by its shift could exceed 38
  digits (the dividend's precision + shift > 38: the HUGEINT intermediate
  can overflow although the result fits; AVG of integers counts 19 digits
  for the row count). Wide statements run in DuckDB during optimization
  (`duckdb_execute_prepared`, materialized); `execute_offloaded()` sends the
  kept result.
- `duckdb_decimal_overflow` (session, global, SET_VAR): FALLBACK (default)
  turns an overflow into a refusal (ON: InnoDB, FORCED: error with the
  reason); ROUND rebuilds the SQL with a scale cap (1, 2, 4, 8 ... decimals
  fewer, exact division computed with one digit more than kept, then
  rounded) and pushes a warning; when the overflow is in DuckDB's own
  arithmetic (SUM accumulator, multiplication) fewer decimals do not help
  and it ends as FALLBACK; ERROR does not run first: an overflow is the
  statement's error, also with ON. EXPLAIN never runs the statement.
- An overflow is recognised by DuckDB's error type (OUT_OF_RANGE for
  arithmetic, CONVERSION for a cast to DECIMAL(38, s)) and a DECIMAL /
  HUGEINT / INT128 value in the message; any other error is a plain
  refusal, never retried with fewer decimals (2026-11-18). Each ROUND rerun
  is counted in `duckdb_offload_decimal_reruns`.
- Literals of more than 38 digits are refused (DuckDB reads them as DOUBLE
  or HUGEINT).
- Tests: MTR `decimal_overflow`; TPC-H Q11 now offloaded (8 of 22).

### Join with GROUP BY and a DISTINCT aggregate (2026-11-22)

- Reported on a WooCommerce schema: `SELECT YEAR(o.d), ..., COUNT(DISTINCT
  o.id) FROM orders o JOIN addresses a ON ... GROUP BY ...` was refused as
  `outer_or_other_table_column`. For a join with GROUP BY and a DISTINCT
  aggregate the server's plan materializes the join into a temporary table,
  and `create_tmp_table()` replaces the aggregate's column arguments with
  columns of that table (`set_arg`, not recorded for rollback in a regular
  statement). The generator maps such a column back to the statement's
  column through what the temporary column keeps (`orig_db_name`,
  `orig_table_name`, the column name; `Field::new_field`). The same table
  twice in the query (ambiguous) or a DISTINCT over an expression stays
  refused.
- Tests: MTR `offload_join_distinct`.

### DuckDB errors during execution (2026-11-23, T2)

- MySQL retries a statement on InnoDB when the secondary engine fails during
  preparation or optimization (`check_secondary_engine_statement`,
  sql_parse.cc; not with FORCED, not for a killed statement), but not once
  execution started: `Sql_cmd_dml::execute_inner` calls
  `set_exec_completed()` after `create_iterators()`, before our executor. An
  error in `execute_offloaded()` (out of memory, temporary disk full, any
  runtime error) therefore reached the client even with ON.
- `optimize()` now starts the statement (`duckdb_execute_prepared_streaming`)
  and fetches its first chunk. DuckDB executes the blocking parts of a plan
  (hash join builds, aggregates, sorts, window partitions) before it returns
  a first chunk, so that is where memory and disk errors happen. An error
  there is a refusal, `duckdb_execution: <DuckDB error>`, and MySQL runs the
  statement on InnoDB (ON) or reports the reason (FORCED). A killed
  statement (KILL QUERY, max_execution_time) and a scalar subquery with more
  than one row keep MySQL's error (1317 / 3024 / 1242; InnoDB would give the
  same 1242).
- `execute_offloaded()` takes over the stream and the first chunk and
  fetches the rest as before: nothing is materialized (unlike the DECIMAL
  first run). An error after the first chunk (in a streaming pipeline:
  rare) still reaches the client: rows may have been sent.
- Statements run first for the DECIMAL or grouped-string checks already
  failed during optimization; they are unchanged. EXPLAIN runs nothing.
- Exception: a statement with DECIMAL values above 38 digits under
  `duckdb_decimal_overflow = ERROR` does not start early: there an
  overflow must be the statement's error, also with ON, and any error
  during optimization makes MySQL run the statement on InnoDB (found by
  MTR `decimal_overflow`).
- Tests: MTR `offload_exec_errors` (results longer than a chunk, empty
  result, an out-of-memory error injected at the first chunk with the debug
  point `duckdb_offload_fail_first_chunk`: InnoDB answers with ON, the
  reason with FORCED; error 1242 kept).

### Parameters and variables (2026-11-23, T1)

- Checked: a numeric prepared statement parameter was refused
  (`unsupported_item:20`, `Item::PARAM_ITEM`; a date parameter compared with
  a DATE column already reached us as a literal, folded by MySQL), stored
  program variables and parameters too (`unsupported_item:21`,
  `Item_splocal`), user variables as `unsupported_function:get_user_var`.
  Connectors with server-side prepared statements (JDBC
  `useServerPrepStmts=true`, PDO without emulation, the binary protocol of
  most drivers) and reports in stored procedures never reached DuckDB.
- Each is constant while the statement runs, and the DuckDB SQL is
  generated again at every EXECUTE / CALL: `value_literal()` prints the
  current value, typed by its kind as MySQL compares it (integers,
  unsigned included; DECIMAL exactly, more than 38 digits refused; DOUBLE
  as `CAST(... AS DOUBLE)`; strings converted to UTF-8, their collation
  the item's, as for a string literal; NULL). In a temporal context the
  value goes through the typed literal path, as literals do.
- `is_constant()` replaces `const_item()` in the translator's constant
  checks (comparisons through weight columns, padded keys, IN lists, LIKE
  patterns, function arguments, LIMIT / OFFSET): MySQL does not call these
  items constant (their value differs between executions), but within one
  execution they are.
- `@x := ...` in the statement (the value changes row by row) stays
  refused (the assignment function is not translated).
- Tests: MTR `offload_params` (EXECUTE twice with new values; integer,
  NULL, DECIMAL, DOUBLE, BIGINT UNSIGNED maximum, date, strings on
  `_ai_ci` and `_bin`, `LIMIT ?`; user variables; a procedure with
  parameters and a declared variable; the assignment refused).

### JSON and spatial columns (2026-11-23)

- New special column kind `OPAQUE` (JSON, GEOMETRY): the column as a result
  (JSON sent as its text with the JSON type of the metadata, GEOMETRY as
  its bytes), `IS [NOT] NULL` and `COUNT(col)` are translated; any other
  function or comparison (`json_or_geometry_in_function`), aggregate
  (`json_or_geometry_in_aggregate`), GROUP BY, ORDER BY, DISTINCT and set
  operations are refused: JSON comparison and spatial semantics are not
  DuckDB's. Expressions of JSON / GEOMETRY type are refused
  (`unsupported_result_type`).
- Tests: MTR `json_geometry` (load and binary log paths, objects, arrays,
  scalars, JSON null and SQL NULL, unicode and escapes, points, lines,
  polygons with SRID; joins and derived tables; nine refusals).

### EXISTS and NOT EXISTS on the same column (2026-11-23, TPC-H Q21)

- `EXISTS (... l2 ... l2.k = l1.k) AND NOT EXISTS (... l3 ... l3.k = l1.k)`
  was refused as `outer_or_other_table_column:l2.l_orderkey`. MySQL
  propagates `l1.k = l2.k = l3.k`, turns the NOT EXISTS into a LEFT JOIN
  with a derived table (`derived_1_3`) and writes its ON with the "best"
  equal field, `l2.k`: a column of the EXISTS nest, which we print in
  WHERE, so not visible in FROM.
- In a FROM join condition, and in an anti-join's condition, a column of
  another semi-join nest is replaced by an equal outer column
  (`outer_equal()`): the multiple equality the column belongs to holds for
  every row that passes WHERE (the semi-join's condition is part of it), so
  `l1.k` stands for `l2.k`. The members come from the column's own multiple
  equality and the one across nests MySQL keeps for hash join to undo the
  same substitution (`item_equal_all_join_nests`). Never a column of the
  same nest (`l3.k = l3.k`); none available: refused
  (`condition_across_semijoins`).
- An anti-join's condition is split in AND parts; a pair of a multiple
  equality that ties in another nest's column is left out when the
  equality also holds an outer column (each nest prints its own pair with
  it).
- Tests: MTR `offload_exists_pair` (Q21's shape with every case of its
  logic, without GROUP BY, NOT EXISTS before EXISTS, EXISTS + EXISTS). TPC-H
  22 of 22.

### Grouped string expressions (2026-11-22)

- `GROUP BY <string expression>` under a PAD SPACE collation (TPC-H Q22:
  `GROUP BY SUBSTR(c_phone, 1, 2)` under `latin1_bin`) was refused
  (`group_by_expression`). It is grouped on `rtrim()` of the expression,
  and where the same expression is shown after grouping (select list,
  ORDER BY, HAVING) it gets the per-group check of grouped strings
  (`count(DISTINCT e) > 1` raises the marker; the statement runs first).
- `latin1` and `ascii` binary PAD SPACE collations (handled with weights for
  columns) key an expression on the value itself (`string_key`): equality
  is the bytes without trailing spaces and their order is the code point
  order, the order of the UTF-8 text in the lake. Other weight collations
  (`_ci`, `utf8mb4_0900_*`): an expression has no weight column, still
  refused.
- Tests: MTR `offload_group_expr` (Q22's shape with real rows, LEFT() under
  `utf8mb4_bin` and `latin1_bin`, ORDER BY, HAVING, a join; 'ab' / 'ab '
  in one group refused). TPC-H 21 of 22.

### FLOAT and DOUBLE (2026-11-18)

- SUM and AVG of FLOAT / DOUBLE: DuckDB adds in parallel in no fixed order;
  on 3 million rows `sum()` gave different last digits on each run, and
  `fsum()` (compensated) differs too and is not MySQL's row-order sum.
  `duckdb_offload_float_aggregates` (session, global, SET_VAR):
  APPROXIMATE (default, decided 2026-11-18: runs in DuckDB; under FULL
  scope no InnoDB answer exists anyway), REFUSE (reason
  `sum_or_avg_of_float`).
  MIN, MAX, COUNT of FLOAT / DOUBLE and every aggregate of exact numbers are
  not affected.
- FLOAT values are sent with a FLOAT holder (the metadata type): before,
  the DOUBLE holder printed 0.10000000149011612 where InnoDB prints 0.1 (a
  debug server asserted in `Protocol_text::store_double`).
- Counters: `duckdb_offload_queries` now counts statements run to the end
  (in `execute_offloaded`), EXPLAIN goes to `duckdb_offload_explained`.
- Tests: MTR `offload_counters` (also the user's `optimizer_switch` kept
  around offload with a `SET_VAR(optimizer_switch=...)` hint).

### LIKE on binary collations (2026-11-17)

- LIKE compares characters and never pads (`'a ' LIKE 'a'` is false also
  under PAD SPACE). Under a binary collation of any character set
  (`MY_CS_BINSORT`: `latin1_bin`, `utf8mb4_bin`, `gbk_bin` ...) a character
  matches only itself, and the copy holds each value converted to UTF-8
  character by character, so DuckDB's LIKE on the stored value is MySQL's.
  Before, only RAW / PAD string modes were accepted, and `latin1_bin` (a
  weight collation for ordering: latin1 is cp1252, its byte order is not
  the UTF-8 order) was refused. The collation is the LIKE's own
  (`compare_collation()`: a COLLATE clause can make it `_ci`, refused).
- Fixed: the escape character. MySQL uses `\` when there is no ESCAPE
  clause (none under NO_BACKSLASH_ESCAPES); DuckDB's LIKE has no default
  escape, so `LIKE 'a\_b'` matched differently for every collation. We pass
  `Item_func_like::escape()` (MySQL's evaluated escape) as ESCAPE always.
- Tests: `offload_queries/duckdb_offload_like_queries.sql` (19), `_refusals.sql`, MTR
  `offload_like`; TPC-H 12 of 22 (Q2, Q13, Q14, Q20).

### Grouped CHAR values in joins (2026-11-17)

- MySQL shows a grouped PAD / `_ci` string from the first row it meets in
  the group; in a join that depends on the join order, so such values were
  refused unless GROUP BY has the table's primary key (decided 2026-11-14).
- Exact exception: a CHAR column under a binary collation (latin1_bin,
  utf8mb4_bin ...). MySQL reads CHAR without trailing spaces, the load and
  the binary log carry it so (`Field_string::pack` strips them), so values
  equal under the PAD SPACE binary comparison are byte-identical:
  `any_value(col)` shows InnoDB's value whatever the join order.
- `PAD_CHAR_TO_FULL_LENGTH` (deprecated sql_mode): MySQL returns CHAR padded
  to its length; the copy has no padding: such statements are refused
  (`sql_mode:PAD_CHAR_TO_FULL_LENGTH`; an older gap for every CHAR result).
- Next (decided): VARCHAR and non-binary collations after a data check
  (design document, Next steps 2).
- Tests: MTR `offload_grouped_char`; TPC-H 16 of 22 (Q4, Q5, Q10, Q12).

### Function review against DuckDB (2026-11-17)

Each MySQL function was compared with DuckDB's on edge values (empty and
NULL strings, multibyte characters, spaces, latin1; negative, zero and NULL
numbers; month ends, Feb 29, year 0 and 9999, microseconds) and translated
only where the results match, with the guards listed. Tests:
`offload_queries/duckdb_offload_fn_queries.sql` (28, identical and offloaded),
`offload_queries/duckdb_offload_fn_refusals.sql` (11), MTR `offload_functions`.

| MySQL | Status | DuckDB / guard |
|---|---|---|
| `EXTRACT(unit FROM d)`, all units but WEEK | translated | `date_part`; composite units built as MySQL does (`YEAR_MONTH` = year * 100 + month, ...); MICROSECOND = microseconds % 1000000 (DuckDB includes the seconds) |
| `EXTRACT(WEEK ...)` | refused | MySQL follows `default_week_format`, DuckDB ISO weeks |
| `DATE_ADD`, `DATE_SUB`, `ADDDATE`, `SUBDATE`, `d +/- INTERVAL n unit` | translated | units YEAR .. MICROSECOND with an integer count; months clamp to the month end in both; DATE result cast to DATE; NULL outside year 0 .. 9999 as MySQL; units with several parts (`'1:30' HOUR_MINUTE`) refused |
| `TO_DAYS`, `DATEDIFF` (MySQL builds it from TO_DAYS) | translated | `date_diff('day', DATE '0001-01-01', d) + 366`; of a constant: MySQL's value |
| `LAST_DAY` | translated | `last_day` |
| `DAYNAME`, `MONTHNAME` | translated | English names, only with `lc_time_names = en_US` |
| `FLOOR`, `CEILING` / `CEIL`, `SIGN` | translated | same |
| `TRUNCATE(x, d)` | translated | exact numbers, constant d (`trunc`); of DOUBLE refused |
| `SQRT`, `LN`, `LOG(x)`, `LOG2`, `LOG10` | translated | NULL outside the domain (DuckDB: NaN, -inf) |
| `EXP`, `POW` / `POWER` | refused | overflow is error 1690 in MySQL, inf in DuckDB |
| `GREATEST`, `LEAST` | translated | numbers, compared in the function's type; NULL if any argument is NULL (DuckDB skips NULLs); strings and dates not yet |
| `CHAR_LENGTH` | translated | characters (`length`) |
| `LENGTH` / `OCTET_LENGTH` | translated | bytes: utf8mb4 / utf8mb3 / ascii only (the copy is UTF-8); latin1 refused |
| `SUBSTRING` / `SUBSTR` / `MID`, `LEFT`, `RIGHT` | translated | position 0, a length below 1, a negative position beyond the start: '' as MySQL (DuckDB returns characters) |
| `CONCAT` | translated | strings, integers, DECIMAL, DATE, DATETIME(0); NULL with any NULL (`\|\|`); DOUBLE, DATETIME(n) refused (formatting) |
| `CONCAT_WS` | translated | NULL values skipped in both; NULL separator gives NULL |
| `TRIM`, `LTRIM`, `RTRIM` | translated | spaces, or one character; `TRIM('xy' FROM s)` refused (MySQL removes the string, DuckDB any of the characters) |
| `REPLACE`, `REVERSE` | translated | REPLACE matches bytes in MySQL under any collation, as DuckDB; REVERSE reverses code points (`list_reverse(string_split(s, ''))`): DuckDB's `reverse()` keeps grapheme clusters together (fixed 2026-11-18) |
| DECIMAL literals over 38 digits | refused | DuckDB reads them as DOUBLE or HUGEINT (`decimal_literal_over_38_digits`, 2026-11-18) |
| `LPAD`, `RPAD` | translated | NULL for a negative length; an empty pad returns the value (truncated if longer) as MySQL (DuckDB: error) |
| `INSTR`, `LOCATE(sub, s)` | translated | binary collations only (a search under `_ci` ignores case); the parser builds LOCATE with the string first; `LOCATE` with a start position not yet |
| `UPPER`, `LOWER` | refused | case mappings differ (DuckDB `upper('ß')` = 'ẞ', MySQL 'ß') |
| `ASCII` | refused | MySQL: first byte; DuckDB: code point |
| `NOW()` / `CURRENT_TIMESTAMP(n)` / `LOCALTIME`, `CURDATE()` / `CURRENT_DATE`, `CURTIME()`, `UTC_TIMESTAMP`, `UTC_DATE`, `UTC_TIME`, `UNIX_TIMESTAMP()` | translated (2026-11-24, T3) | MySQL fixes them at the start of the statement; we send that value as a typed literal (`TIMESTAMP '2024-11-24 10:00:00.250'`, `DATE ...`), so `SET TIMESTAMP` and the session time zone give MySQL's value. Compared with a DATE column, NOW() makes the comparison DATETIME (the date at midnight) as in MySQL; with a TIMESTAMP column the literal is converted as the column's other constants. The optimizer's cached copy (`Item_cache`) of such a value counts as a constant. MTR `offload_now` |
| `SYSDATE()` | refused | its value changes during the statement |
| `CAST` / `CONVERT(x, type)` to SIGNED | translated (2026-11-24, T3) | integers; DECIMAL rounded half away from zero (both) when its integer part has at most 18 digits (MySQL clips an overflow with a warning, DuckDB errors); BIGINT UNSIGNED refused (MySQL reinterprets the bits) |
| ... to UNSIGNED | translated | integers; a negative value wraps to 2^64 + x as in MySQL; DECIMAL refused |
| ... to DECIMAL(M, D) | translated | integers and DECIMAL whose integer part always fits M - D (otherwise MySQL clips to 99.9 with a warning: refused); a smaller scale rounds half away from zero in both |
| ... to DOUBLE / REAL / FLOAT | translated | integers (both round to the nearest value); DECIMAL refused (MySQL converts through the decimal string) |
| ... to CHAR[(N)] | translated | integers, DECIMAL (printed with the scale MySQL gives the source: an expression is cast to MySQL's DECIMAL(p, s) first), DATE, DATETIME(0); N truncates with `left` (MySQL also warns). The result has the connection's collation: LIKE and comparisons on it follow the collation rules (refused for `_ci`). DATETIME(n > 0) refused (DuckDB drops trailing zeros) |
| ... to DATETIME(n) | translated | DATE, and DATETIME(m) with m <= n; fewer digits round in MySQL: refused |
| ... to DATE | translated | DATE / DATETIME (earlier) |
| CAST from strings, DOUBLE, TIMESTAMP, TIME; to BINARY, TIME, YEAR, JSON; `CONVERT(x USING cs)` | refused | MySQL's string-to-number rules (prefix, warnings) and DOUBLE printing differ; TIMESTAMP depends on the session time zone (T3 step 4) |
| not reviewed yet | refused | DATE_FORMAT, STR_TO_DATE, TIMESTAMPDIFF, bit operators, REGEXP, JSON, STDDEV / VARIANCE, GROUP_CONCAT, window functions |

Also fixed during the review: string constants of these functions'
arguments no longer take the date type of an enclosing comparison
(`dt + INTERVAL 30 DAY > '2024-03-01'` was refused); =, <>, IN under a
binary collation of another character set (latin1_bin) compare characters
without the weight column, so expressions work there (TPC-H Q22's
`substr(c_phone, 1, 2) IN (...)`); ordering keeps the weights.

TPC-H after the review: 19 of 22 (Q7, Q8, Q9 added). Q22 now stops at
grouping by `substr(...)` under PAD SPACE (the shown value is ambiguous:
next step, the data check).

### Open points

- FULL scope fallback to InnoDB: closed by the DWL#7 guard (2026-11-15).
- After RENAME TABLE, a FORCED query fails until the applier has renamed
  the copy (the handler is opened before the freshness wait).

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
