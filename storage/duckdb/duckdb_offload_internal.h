/* Copyright (c) 2026-present, Marco Tusa. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

// Query offload: declarations shared by the files of the translator
// (duckdb_offload*.cc).

#ifndef STORAGE_DUCKDB_DUCKDB_OFFLOAD_INTERNAL_H
#define STORAGE_DUCKDB_DUCKDB_OFFLOAD_INTERNAL_H

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "include/mem_root_deque.h"
#include "mysql/strings/m_ctype.h"
#include "sql/item.h"
#include "sql/item_cmpfunc.h"
#include "sql/item_func.h"
#include "sql/item_subselect.h"
#include "sql/item_sum.h"
#include "sql/query_term.h"
#include "sql/sql_lex.h"
#include "sql/table.h"
#include "storage/duckdb/duckdb_offload.h"

namespace duckdb_se {
namespace offload {

// The error a grouped string check raises in DuckDB (grouped_value).
inline constexpr char kGroupedAmbiguous[] =
    "duckdb_offload_grouped_value_ambiguous";

// How a select-list value comes back from DuckDB (the SQL casts it).
enum class Kind {
  INT,        // BIGINT
  UINT,       // UBIGINT
  DOUBLE,     // DOUBLE
  DECIMAL,    // DECIMAL(p,s) as VARCHAR text
  STRING,     // VARCHAR (UTF-8), converted to the item's character set
  BLOB,       // BLOB (bytes)
  DATE,       // DATE
  DATETIME,   // TIMESTAMP holding a DATETIME (no time zone)
  TIMESTAMP,  // TIMESTAMP holding a UTC instant (MySQL TIMESTAMP)
  BIT,        // UBIGINT holding a BIT(n) value, sent as ceil(n/8) bytes
  TIME,       // BIGINT holding a TIME as signed microseconds
};

// Our own error for a scalar subquery with more than one row (the one-row
// check of a derived table made from a scalar subquery); DuckDB's own
// message for a scalar subquery is matched too.
inline constexpr const char *kDuckDBMultipleRows =
    "More than one row returned by a subquery used as an expression";

// Columns whose lake value is not what MySQL uses in every context: TIME
// (signed microseconds; MySQL's string and number forms differ), ENUM and
// SET (value plus hidden index), YEAR and BIT (numbers that MySQL shows as
// '0000' or as bytes in string contexts).
// JSON and GEOMETRY (2026-11-23, OPAQUE): stored as MySQL prints them (JSON
// text, SRID + WKB bytes), so selecting the column, IS [NOT] NULL and COUNT
// give MySQL's result; comparisons, functions, grouping, ordering, DISTINCT
// and other aggregates follow JSON / spatial rules DuckDB does not share:
// refused.
enum class SpecialColumn { NONE, NUMBER, OTHER, OPAQUE };

// How strings of a column compare in DuckDB, where strings are UTF-8 bytes
// compared without padding:
// - RAW: binary strings and NO PAD binary collations (utf8mb4_0900_bin):
//   byte order is the collation order;
// - PAD: PAD SPACE binary collations of utf8mb3/utf8mb4 (utf8mb3_bin,
//   utf8mb4_bin): code point order with trailing spaces ignored, reproduced
//   with rtrim (equality) and a padded sort key (ordering);
// - WEIGHT: any other collation: the copy has MySQL's weight of each value
//   in "__w_<column>" (DWL#6 step A); constants are weighted when we print
//   the SQL; other expressions are refused;
// - REFUSE: not used (kept for a collation without weights).
enum class StrMode { RAW, PAD, WEIGHT, REFUSE };

// duckdb_offload_literals.cc
bool is_constant(const Item *item);

// duckdb_offload_strings.cc
bool is_string(const Item *item);
StrMode str_mode_cs(const CHARSET_INFO *cs);
StrMode str_mode(const Item *item);
bool exact_text(const Item *item, bool ordering);
bool is_char_column(const Item *item);
std::string pad_equal_key(const std::string &e);

// duckdb_offload_functions.cc
SpecialColumn special_column(const Item *item);
bool date_format_sql(const std::string &fmt, const std::string &x,
                     std::string *out, bool *fixed_numeric);
bool date_format_format(const Item_func *fn, std::string *fmt);
bool is_date_or_datetime(enum_field_types t);

class Generator {
 public:
  Generator(THD *thd, std::string *reason) : m_thd(thd), m_reason(reason) {}

  bool generate(LEX *lex, std::string *sql, std::vector<Kind> *kinds);

  // DECIMAL values MySQL declares with more than 38 digits: computed in
  // DuckDB as DECIMAL(38, scale), exact while the values fit (OffloadContext
  // runs such statements first, duckdb_decimal_overflow).
  bool decimal_wide() const { return m_decimal_wide; }
  /** A grouped string is shown with an in-query check (grouped_value):
      the statement runs first, during optimization. */
  bool grouped_check() const { return m_grouped_check; }
  // duckdb_decimal_overflow=ROUND, after an overflow: DECIMAL results keep
  // at most this many decimals (-1: all).
  void set_decimal_scale_cap(int cap) { m_scale_cap = cap; }
  int decimal_max_scale() const { return m_max_scale; }

 private:
  bool refuse(const std::string &why) {
    *m_reason = why;
    return true;
  }
  // DECIMAL(38, scale) for a DECIMAL result, with the ROUND cap.
  std::string dec38(unsigned scale) {
    m_max_scale = std::max(m_max_scale, static_cast<int>(scale));
    const unsigned s = m_scale_cap >= 0
                           ? std::min(scale, static_cast<unsigned>(m_scale_cap))
                           : scale;
    return "DECIMAL(38," + std::to_string(s) + ")";
  }
  bool m_decimal_wide{false};
  bool m_grouped_check{false};
  int m_scale_cap{-1};
  int m_max_scale{0};
  bool refuse_collation(const Item *item) {
    const char *name = item->type() == Item::FIELD_ITEM
                           ? down_cast<const Item_field *>(item)->field_name
                           : "expression";
    return refuse(std::string("collation:") + name + " (" +
                  item->collation.collation->m_coll_name + ")");
  }
  bool expr(Item *item, std::string *out);
  bool function(Item_func *fn, std::string *out);
  bool aggregate(Item_sum *s, std::string *out);
  bool operands(Item_func *fn, bool ordering, unsigned from, unsigned to,
                std::vector<std::string> *ops);
  bool numeric(Item *item, std::string *out, bool in_decimal = false);
  bool unsigned_arith(Item *left, Item *right, char op, std::string *out);
  bool time_literal(Item *item, std::string *out);
  bool special_args(Item_func *fn);
  int division_frac(int f1, int f2) const;
  bool internal_frac(Item *item, int *frac);
  bool exact_division(const std::string &a, int f1, const std::string &b,
                      int f2, int round_to, int a_digits, std::string *out);
  bool temporal_arg(Item *item, std::string *out);
  bool local_timestamp(Item *a, std::string *out);
  bool cast(Item_func *fn, std::string *out);
  bool order_key(Item *item, std::string *out);
  bool enum_index(Item *item, std::string *out);
  bool weight_expr(Item *item, const CHARSET_INFO *cs, std::string *out);
  bool string_key(Item *item, const CHARSET_INFO *cs, bool ordering,
                  size_t max_bytes, std::string *out);
  bool literal(Item *item, std::string *out);
  bool typed_literal(Item *item, enum_field_types type, std::string *out);
  bool output_kind(Item *item, Kind *kind, std::string *cast);

  // An AND part of a condition and the tables it uses.
  struct Conj {
    std::string sql;
    table_map tables;
    // A pair of a multiple equality (equality propagation): implied by the
    // other pairs, so it may be left out where one of its columns is not
    // visible (another semi-join's table).
    bool implied{false};
    Item_multi_eq *eq{nullptr};  // the multiple equality of an implied pair
    // A plain column = column equality: its two columns.
    Item_field *eq_a{nullptr};
    Item_field *eq_b{nullptr};
  };
  bool table_ref(Table_ref *tr, std::string *out);
  bool from_list(mem_root_deque<Table_ref *> &list, bool under_outer,
                 bool in_semi, std::string *out);
  bool conjuncts(Item *cond, table_map avoid, std::vector<Conj> *out);
  bool multi_eq(Item_multi_eq *eq, table_map avoid, std::vector<Conj> *out);
  bool comparable(Item *a, Item *b);
  bool block_sql(Query_block *qb, const std::vector<std::string> *names,
                 std::vector<Kind> *kinds, std::string *sql,
                 const std::vector<Item *> *targets = nullptr,
                 const std::string *from_sql = nullptr,
                 std::set<std::string> *weights_out = nullptr,
                 bool weights_required = false);
  bool setop_sql(Query_term_set_op *t, const std::vector<std::string> *names,
                 const std::vector<Item *> *targets, std::vector<Kind> *kinds,
                 std::string *sql, std::set<std::string> *weights_out = nullptr,
                 bool weights_required = false);
  bool subquery_sql(Query_expression *unit, std::string *sql);
  bool subquery(Item_subselect *sub, std::string *out);
  std::string truth(const std::string &e, Item::Bool_test test, bool *ok);

  // A table of the query: its alias and primary key (base tables).
  struct TableInfo {
    std::string alias;
    std::string pk;          // the key, or row(...) of its columns
    std::string pk_parts;    // its columns
    std::string pk_negated;  // negated (empty unless all integers)
    bool derived{false};     // a materialized derived table (our SQL)
    bool positional{false};  // columns named c1, c2, ... (set operations)
    bool scalar{false};      // a scalar subquery the server made a table:
                             // exactly one row (or none)
    std::set<std::string> weights;  // columns with a weight column
    std::vector<const Field *> pk_fields;  // base tables
  };
  // What is being printed in one query block.
  struct Block {
    Block *parent{nullptr};    // the enclosing block of a subquery
    Query_block *qb{nullptr};  // the query block printed
    std::set<const TABLE *> tables;  // the tables its columns may use
    bool post_group{false};          // select list, HAVING, ORDER BY
    int agg_depth{0};
    // PAD SPACE columns in GROUP BY: grouped on rtrim(); outside an
    // aggregate shown as the value of the row with the smallest primary
    // key, the row InnoDB meets first (single table only).
    std::vector<const Field *> group_pad;
    // Grouped string expressions under PAD SPACE (grouped on rtrim()):
    // shown with the per-group check (grouped_expression()).
    std::vector<Item *> group_pad_exprs;
    std::set<const Field *> grouped;  // GROUP BY columns
    struct Semi {
      Table_ref *nest;
      bool anti;
      std::string from;
      table_map tables{0};  // the tables of the nest (or derived table)
      Item *cond{nullptr};  // an anti-join's own condition (ON)
    };
    std::vector<Semi> semi;  // semi- and anti-join nests at the top
    // WHERE parts printed elsewhere: the IS NULL of a derived anti-join
    // (derived_marker()).
    std::set<const Item *> skip_where;
    // [NOT] EXISTS printed in place of a derived table's IS [NOT] NULL
    // marker inside OR (derived_marker()).
    std::map<const Item *, std::string> marker_sql;
  };
  bool grouped_value(const Field *field, const TableInfo &ti,
                     const std::string &col, std::string *out);

  THD *m_thd;
  Item_field *source_column(Item_field *f);
  bool value_literal(Item *item, std::string *out);
  // While an anti-join's condition is printed: the tables of the other
  // semi-join nests (not visible there) and of this nest.
  table_map m_anti_others{0};
  table_map m_anti_mine{0};
  Item_field *outer_equal(Item_field *f);
  Item *derived_marker(Table_ref *e, bool *anti, bool *top);
  bool unique_match(Table_ref *e);
  bool lateral_keys(Table_ref *e,
                    std::vector<std::pair<Item *, std::string>> *keys);
  // Tables a derived table printed now may reference as LATERAL (the
  // tables before it in its FROM list; in an EXISTS also the outer
  // query's), and the correlation for the next derived block printed.
  table_map m_lateral_ok{0};
  table_map m_main_visible{0};
  std::vector<std::pair<Item *, std::string>> m_lateral_keys;
  // Set while the derived table of a derived anti-join is printed: its
  // block leaves DISTINCT out (NOT EXISTS does not need it).
  bool m_drop_distinct{false};
  bool grouped_expression(Item *item, std::string *out);
  std::string *m_reason;
  bool m_full{false};
  std::map<const TABLE *, TableInfo> m_info;
  int m_next_alias{0};
  Block *m_block{nullptr};
  // While printing the operands of a comparison with a DATE, DATETIME or
  // TIMESTAMP column: constants are values of that type, as MySQL compares.
  enum_field_types m_temporal{MYSQL_TYPE_NULL};
};

}  // namespace offload
}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_DUCKDB_OFFLOAD_INTERNAL_H
