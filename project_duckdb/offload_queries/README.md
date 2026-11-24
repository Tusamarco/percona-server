<!-- Copyright (c) 2026-present, Marco Tusa. All rights reserved. -->

# Offload query lists

Manual checks of the query translation (DWL#6), run against a server with
the plugin. The MTR suite (`storage/duckdb/mysql-test/duckdb`) covers the
same queries automatically.

| File | Content |
|---|---|
| `duckdb_offload_setup.sql` | Creates and loads every test table (blocks per feature: g, p6, dim, dv, cw, cwd, hist, yb, tt, et, lt, fx ...) |
| `duckdb_offload_<step>_queries.sql` | Queries that must run in DuckDB with InnoDB's result, one per line |
| `duckdb_offload_<step>_refusals.sql` | Queries that must be refused (InnoDB answers with ON, an error with the reason with FORCED) |

Steps: `6a`, `6b`, `6c`, `6c2`, `6c3` (query offload steps), `div` (exact
division), `a2` (collation weights), `yb` (YEAR, BIT), `tt` (TIME), `et`
(ENUM, SET), `like` (LIKE), `fn` (function review).

Run from `project_duckdb/`, with the comparison script there:

```bash
MYSQL="mysql -S <socket> -uroot <database>"
$MYSQL < offload_queries/duckdb_offload_setup.sql
MYSQL="$MYSQL" ./duckdb_offload_compare.sh offload_queries/duckdb_offload_6a_queries.sql
EXPECT=refused MYSQL="$MYSQL" ./duckdb_offload_compare.sh offload_queries/duckdb_offload_6b_refusals.sql
```

Results and expected counts are in `manual-test-plan.md` (sections 21 to 42).
