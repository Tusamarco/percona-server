# TPC-H with the DuckDB secondary engine

Copyright (c) 2026-present, Marco Tusa.

Can the plugin run a TPC-H benchmark such as HammerDB's TPROC-H today, and
what is missing? This directory has a quick check (HammerDB's MySQL schema,
small generated data, the 22 queries in HammerDB's MySQL form) and the
results of 2026-11-17.

## Files

| File | Content |
|---|---|
| `tpch_schema.sql` | The schema as HammerDB creates it for MySQL (`src/mysql/mysqlolap.tcl`): database `latin1`, every character column `BINARY` (so `latin1_bin`), the same primary keys, indexes and foreign keys; plus `SECONDARY_ENGINE = duckdb`. |
| `tpch_data.sql` | About scale factor 0.01 (LINEITEM 60,000 rows), deterministic, with TPC-H value domains so every query finds rows; loads the copies; creates the view of Q15. Not a dbgen replacement. |
| `tpch_queries.sql` | The 22 queries in HammerDB's MySQL syntax (`date '...' + interval '3' month`, `extract(year from ...)`, `substr(...)`), validation parameters, one per line. |

## Run

```bash
M="mysql -S <socket> -uroot"
$M < tpch_schema.sql && $M < tpch_data.sql          # ~3.5 min on a debug build
MYSQL="$M tpch" ../duckdb_offload_compare.sh tpch_queries.sql
```

The compare script runs each query with `use_secondary_engine = OFF`
(InnoDB) and `FORCED` (DuckDB) and reports identical results or the refusal
reason.

## Results (2026-11-17, debug build)

**22 of 22 offloaded with results identical to InnoDB on 2026-11-23** (Q16 after the per-group check of grouped strings, Q22 after grouped string expressions, Q21 after the equal outer column in join conditions). History: **7 of 22 offloaded with results identical to InnoDB** (8 after the DECIMAL change of the same day, Q11; 12 after LIKE on binary collations: Q2, Q13, Q14, Q20; 16 after grouped CHAR in joins: Q4, Q5, Q10, Q12; 19 after EXTRACT: Q7, Q8, Q9). None gave a wrong
result: the other 15 were refused, and with `use_secondary_engine = ON` run
on InnoDB.

| Query | Result | Missing |
|---|---|---|
| Q1 | identical, offloaded | |
| Q2 | identical, offloaded (since 2026-11-17, LIKE on binary collations) | |
| Q3 | identical, offloaded | |
| Q4 | identical, offloaded (since 2026-11-17, grouped CHAR in joins) | |
| Q5 | identical, offloaded (since 2026-11-17, grouped CHAR in joins) | |
| Q6 | identical, offloaded | |
| Q7 | identical, offloaded (since 2026-11-17, EXTRACT) | |
| Q8 | identical, offloaded (since 2026-11-17, EXTRACT) | |
| Q9 | identical, offloaded (since 2026-11-17, EXTRACT) | |
| Q10 | identical, offloaded (since 2026-11-17, grouped CHAR in joins) | |
| Q11 | identical, offloaded (since 2026-11-17, `duckdb_decimal_overflow`) | |
| Q12 | identical, offloaded (since 2026-11-17, grouped CHAR in joins) | |
| Q13 | identical, offloaded (since 2026-11-17, LIKE on binary collations) | |
| Q14 | identical, offloaded (since 2026-11-17) | |
| Q15 | identical, offloaded (view) | |
| Q16 | identical, offloaded (since 2026-11-22, grouped strings in joins checked per group) | |
| Q17 | identical, offloaded | |
| Q18 | identical, offloaded | |
| Q19 | identical, offloaded | |
| Q20 | identical, offloaded (since 2026-11-17, LIKE on binary collations) | |
| Q21 | identical, offloaded (since 2026-11-23: in a join condition, a column of the EXISTS nest replaced by an equal outer column) | |
| Q22 | identical, offloaded (since 2026-11-22, grouped string expressions under PAD SPACE checked per group; real rows in MTR `offload_group_expr`, the quick check's data gives Q22 no rows) | |

## What a HammerDB run needs

### Outside the query translation

1. **Foreign keys** (done 2026-11-17). HammerDB creates foreign keys (all
   tables but PART and REGION). The plugin used to refuse tables with
   foreign keys, against cascaded changes InnoDB does not write to the
   binary log. In 9.7, with `innodb_native_foreign_keys = OFF` (the
   default), MySQL runs foreign key checks and cascades in the SQL layer and
   writes the cascaded rows to the binary log (verified for ON DELETE / ON
   UPDATE CASCADE and SET NULL). Tables with foreign keys now load; with ON,
   only a child of a cascading key is refused (MTR `foreign_keys`).
2. **autocommit.** HammerDB sets `autocommit = 0` on MySQL unless it detects
   HeatWave. MySQL never runs a query of a multi-statement transaction in a
   secondary engine (core rule), so nothing would be offloaded. HammerDB's
   `mysqlolap.tcl` must use `autocommit = 1` for this engine (a one-line
   change in the HammerDB script, or a HammerDB option for DuckDB). The
   plugin now gives the reason instead of failing (it crashed a debug server
   with FORCED and `autocommit = 0`; fixed 2026-11-17).
3. **Loading the copies.** After HammerDB builds the schema:
   `ALTER TABLE <t> SECONDARY_ENGINE = duckdb; ALTER TABLE <t> SECONDARY_LOAD;`
   for the eight tables (HammerDB does this itself for HeatWave).
4. **Routing.** With `use_secondary_engine = ON` and the default thresholds
   the TPC-H queries qualify on size; HammerDB's HeatWave path adds
   `/*+ set_var(use_secondary_engine=forced) */`, which would turn any
   refusal into an error.

### Query translation (15 queries)

| Item | Queries | Size |
|---|---|---|
| LIKE on binary collations: done 2026-11-17 (also MySQL's default escape `\`) | Q2, Q9, Q13, Q14, Q16, Q20 | done |
| Grouped CHAR values in joins under binary collations: done 2026-11-17 (equal values are identical) | Q4, Q5, Q9, Q10, Q12 | done |
| `EXTRACT(unit FROM date)`: done 2026-11-17 (function review) | Q7, Q8, Q9 | done |
| DECIMAL results declared above 38 digits: done 2026-11-17 (DECIMAL(38, s), run first, `duckdb_decimal_overflow`) | Q11, Q14 | done |
| SUBSTR: done 2026-11-17; grouping by it under PAD SPACE needs the data check | Q22 | with the data check |
| Equalities MySQL propagates between a semi-join (EXISTS) and an anti-join (NOT EXISTS) nest: print the propagated equality with a column visible in each subquery | Q21 | medium |
| Grouped VARCHAR value in a join without the primary key (`p_type`): exact after a data check (no two different values equal under the collation), decided as the next step | Q16 | medium |

Done so far: foreign keys, DECIMAL above 38 digits, LIKE on binary
collations, grouped CHAR in joins, EXTRACT and the function review (19 of
22). With the data check for grouped strings 21 (Q16, Q22); with the Q21
item 22.

## Plan to prove it with HammerDB

1. The items above (the three small translation items, then
   DECIMAL, SUBSTR, Q21), each with its MTR test; the quick check here
   after each.
2. HammerDB TPROC-H on the release build, scale factor 1 then 10: build the
   schema with HammerDB, load the copies, run the power test with InnoDB
   (`use_secondary_engine = OFF`) and with the plugin, same results, compare
   the times. `autocommit = 1` in HammerDB's MySQL OLAP script.
3. Record the runs in `project_duckdb/perf/` as the other performance runs.
