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

// Query offload, blocks: expressions, FROM lists, joins and semi-joins,
// conditions, query blocks, subqueries, set operations, Generator::generate.

#include "storage/duckdb/duckdb_offload_internal.h"

#include <cstring>
#include <string>
#include <vector>

#include "mysqld_error.h"
#include "sql/sql_class.h"
#include "sql/sql_optimizer.h"
#include "storage/duckdb/duckdb_collation.h"
#include "storage/duckdb/duckdb_load.h"  // history_scope_full
#include "storage/duckdb/duckdb_registry.h"
#include "storage/duckdb/duckdb_types.h"  // quote_ident

namespace duckdb_se {
namespace offload {

namespace {

std::vector<std::string> split_list(const std::string &joined) {
  std::vector<std::string> out;
  size_t start = 0;
  while (!joined.empty() && start <= joined.size()) {
    size_t end = joined.find('\x1f', start);
    if (end == std::string::npos) end = joined.size();
    out.push_back(joined.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

// Kinds a set operation may combine: numbers with numbers, DATE with
// DATETIME; otherwise the same kind.
int kind_class(Kind k) {
  switch (k) {
    case Kind::INT:
    case Kind::UINT:
    case Kind::DOUBLE:
    case Kind::DECIMAL:
      return 0;
    case Kind::STRING:
      return 1;
    case Kind::BLOB:
      return 2;
    case Kind::DATE:
    case Kind::DATETIME:
      return 3;
    case Kind::TIMESTAMP:
      return 4;
    case Kind::BIT:
      return 5;
    case Kind::TIME:
      return 6;
  }
  return -1;
}

bool same_class(Kind a, Kind b) { return kind_class(a) == kind_class(b); }

}  // namespace

// With a join, GROUP BY and a DISTINCT aggregate the server's plan
// materializes the join into a temporary table first, and create_tmp_table()
// replaces the aggregate's column arguments with columns of that table
// (sum_item->set_arg(Item_field(<temporary>.col)); not recorded for later
// rollback in a regular statement). We translate the statement, not that
// plan: a temporary column made from a table column keeps that column's
// database, table and name (Field::new_field), and we find the column again
// among the tables of this block and the enclosing ones. Ambiguous (the same
// table twice in the query) or not a plain column: nullptr, and the query
// is refused as before.
Item_field *Generator::source_column(Item_field *f) {
  const Field *tf = f->field;
  if (tf == nullptr || tf->table == nullptr ||
      tf->table->s->tmp_table == NO_TMP_TABLE || tf->orig_table_name == nullptr ||
      tf->orig_db_name == nullptr || tf->field_name == nullptr)
    return nullptr;
  Field *found = nullptr;
  for (const Block *b = m_block; b != nullptr; b = b->parent) {
    for (const TABLE *t : b->tables) {
      const Table_ref *tr = t->pos_in_table_list;
      if (tr == nullptr || tr->db == nullptr || tr->table_name == nullptr ||
          strcmp(tr->db, tf->orig_db_name) != 0 ||
          strcmp(tr->table_name, tf->orig_table_name) != 0)
        continue;
      for (Field **fp = t->field; *fp != nullptr; fp++) {
        if (my_strcasecmp(system_charset_info, (*fp)->field_name,
                          tf->field_name) != 0)
          continue;
        if (found != nullptr) return nullptr;  // the table twice: ambiguous
        found = *fp;
      }
    }
  }
  if (found == nullptr) return nullptr;
  return new (m_thd->mem_root) Item_field(found);
}

// TPC-H Q21: EXISTS (... l2 ... l2.k = l1.k) AND NOT EXISTS (... l3 ...
// l3.k = l1.k). The optimizer propagates l1.k = l2.k = l3.k and rewrites the
// anti-join's condition with the "best" equal field: l3.k = l2.k, a column
// of the other nest, not visible in our NOT EXISTS. That multiple equality
// holds for every row that passes WHERE (the semi-join's condition is part
// of it), so an equal column of the outer tables can stand for it. The
// Item_field keeps its multiple equalities (the one of its nest, and the
// one across nests MySQL keeps for hash join to undo this same
// substitution). Never a column of this nest (l3.k = l3.k) or of another
// nest; nullptr when there is no outer one.
Item_field *Generator::outer_equal(Item_field *f) {
  const table_map hidden = m_anti_others | m_anti_mine;
  for (Item_multi_eq *eq : {f->multi_equality(), f->item_equal_all_join_nests}) {
    if (eq == nullptr) continue;
    for (Item_field &m : eq->get_fields()) {
      if (m.field == nullptr || (m.used_tables() & hidden) != 0) continue;
      const Block *owner = m_block;
      while (owner != nullptr && owner->tables.count(m.field->table) == 0)
        owner = owner->parent;
      if (owner != nullptr && comparable(f, &m)) return &m;
    }
  }
  return nullptr;
}

// A grouped PAD SPACE string expression shown after grouping: the group
// holds values equal under the collation ('13' and '13 '); MySQL shows the
// first row's. As for a grouped column in a join (grouped_value()), we
// check per group that only one stored value is present: the statement
// runs first and a group with two becomes a refusal.
bool Generator::grouped_expression(Item *item, std::string *out) {
  m_block->agg_depth++;
  struct Leave {
    int &d;
    ~Leave() { d--; }
  } leave{m_block->agg_depth};
  std::string e;
  if (expr(item, &e)) return true;
  m_grouped_check = true;
  *out = "CASE WHEN count(DISTINCT " + e + ") > 1 THEN error('" +
         kGroupedAmbiguous + "') ELSE any_value(" + e + ") END";
  return false;
}

bool Generator::expr(Item *item, std::string *out) {
  item = item->real_item();
  if (!m_block->marker_sql.empty()) {
    const auto m = m_block->marker_sql.find(item);
    if (m != m_block->marker_sql.end()) {
      *out = m->second;
      return false;
    }
  }
  if (m_block->post_group && m_block->agg_depth == 0 &&
      item->type() != Item::FIELD_ITEM) {
    for (Item *g : m_block->group_pad_exprs)
      if (item == g || item->eq(g)) return grouped_expression(item, out);
  }
  switch (item->type()) {
    case Item::SUBQUERY_ITEM:
      return subquery(down_cast<Item_subselect *>(item), out);
    case Item::CACHE_ITEM: {
      // A cached copy of an expression (the server's rewrites use them).
      Item *example = down_cast<Item_cache *>(item)->get_example();
      if (example == nullptr) return refuse("cache_without_expression");
      return expr(example, out);
    }
    case Item::FIELD_ITEM: {
      auto *f = down_cast<Item_field *>(item);
      // In an anti-join's condition, a column of another semi-join nest
      // (MySQL's best-equal-field substitution, TPC-H Q21): an equal outer
      // column instead (outer_equal()).
      if (m_anti_others != 0 && f->field != nullptr &&
          (f->used_tables() & m_anti_others) != 0) {
        Item_field *sub = outer_equal(f);
        if (sub == nullptr) return refuse("condition_across_semijoins");
        return expr(sub, out);
      }
      // A column of this block or, in a subquery, of an enclosing block (a
      // correlated reference: aliases are unique in the statement).
      const Block *owner = m_block;
      while (owner != nullptr && f->field != nullptr &&
             owner->tables.count(f->field->table) == 0)
        owner = owner->parent;
      if (f->field != nullptr && owner == nullptr) {
        // A column of a temporary table of the server's plan, made from a
        // column of this statement: that column.
        if (Item_field *src = source_column(f); src != nullptr)
          return expr(src, out);
      }
      if (f->field == nullptr || owner == nullptr)
        return refuse(std::string("outer_or_other_table_column:") +
                      (f->table_name != nullptr ? f->table_name : "") + "." +
                      (f->field_name != nullptr ? f->field_name : ""));
      if (f->field->is_flag_set(NOT_SECONDARY_FLAG))
        return refuse(std::string("not_secondary_column:") + f->field_name);
      const TableInfo &ti = m_info[f->field->table];
      const std::string col =
          ti.positional
              ? ti.alias + ".c" +
                    std::to_string(f->field->field_index() -
                                   f->field->table->hidden_field_count + 1)
              : ti.alias + "." + quote_ident(f->field->field_name);
      if (owner != m_block) {
        *out = col;
        return false;
      }
      // A table made from a scalar subquery has one row: its columns are
      // the same for the whole group (MySQL allows them in HAVING and the
      // select list; DuckDB wants an aggregate).
      if (ti.scalar && m_block->post_group && m_block->agg_depth == 0) {
        *out = "any_value(" + col + ")";
        return false;
      }
      bool group_pad = false;
      for (const Field *g : m_block->group_pad)
        group_pad = group_pad || g == f->field;
      if (m_block->post_group && m_block->agg_depth == 0 && group_pad)
        return grouped_value(f->field, ti, col, out);
      *out = col;
      return false;
    }
    case Item::PARAM_ITEM:
    case Item::ROUTINE_FIELD_ITEM:
    case Item::INT_ITEM:
    case Item::DECIMAL_ITEM:
    case Item::REAL_ITEM:
    case Item::STRING_ITEM:
    case Item::NULL_ITEM:
      if (m_temporal != MYSQL_TYPE_NULL && !item->is_null())
        return typed_literal(item, m_temporal, out);
      return literal(item, out);
    case Item::SUM_FUNC_ITEM:
      return aggregate(down_cast<Item_sum *>(item), out);
    case Item::FUNC_ITEM:
    case Item::COND_ITEM:
      // A constant date or datetime (the optimizer turns string literals
      // compared with temporal columns into date_literal / datetime_literal):
      // MySQL's own value, printed as a typed literal.
      if (item->type() == Item::FUNC_ITEM && item->const_item() &&
          is_date_or_datetime(item->data_type()))
        return typed_literal(item, m_temporal == MYSQL_TYPE_TIMESTAMP
                                       ? MYSQL_TYPE_TIMESTAMP
                                       : item->data_type(),
                             out);
      return function(down_cast<Item_func *>(item), out);
    default:
      return refuse("unsupported_item:" +
                    std::to_string(static_cast<int>(item->type())));
  }
}

// The DuckDB type each select-list value is cast to, matching the MySQL type
// of the item, so the conversion back is one rule per kind.
bool Generator::output_kind(Item *item, Kind *kind, std::string *cast) {
  switch (item->data_type()) {
    case MYSQL_TYPE_TIMESTAMP:
      *kind = Kind::TIMESTAMP;
      *cast = "TIMESTAMP";
      return false;
    case MYSQL_TYPE_DATETIME:
      *kind = Kind::DATETIME;
      *cast = "TIMESTAMP";
      return false;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      *kind = Kind::DATE;
      *cast = "DATE";
      return false;
    case MYSQL_TYPE_YEAR: {
      // The year number (0 for 0000). A YEAR column (base table, derived
      // table, view) is sent zero-filled, "0000" (Field_year::
      // send_to_protocol), a YEAR expression (MAX(y), YEAR(d)) as a plain
      // integer, "0" (Item::send): make_holder() picks the holder.
      const Item::Type t = item->real_item()->type();
      if (t != Item::FIELD_ITEM && t != Item::FUNC_ITEM &&
          t != Item::SUM_FUNC_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::INT;
      *cast = "BIGINT";
      return false;
    }
    case MYSQL_TYPE_BIT: {
      // A BIT column or MIN/MAX of one: MySQL sends the value's bytes,
      // big-endian, ceil(n/8) of them (max_length is n).
      const Item::Type t = item->real_item()->type();
      if ((t != Item::FIELD_ITEM && t != Item::SUM_FUNC_ITEM) ||
          item->max_length < 1 || item->max_length > 64)
        return refuse("unsupported_result_type");
      *kind = Kind::BIT;
      *cast = "UBIGINT";
      return false;
    }
    case MYSQL_TYPE_TIME: {
      // A TIME column or MIN/MAX of one (signed microseconds in the lake).
      // Other TIME expressions (IF, CASE, functions) are not translated.
      const Item::Type t = item->real_item()->type();
      if (t != Item::FIELD_ITEM && t != Item::SUM_FUNC_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::TIME;
      *cast = "BIGINT";
      return false;
    }
    case MYSQL_TYPE_JSON:
      // A JSON column: the text MySQL prints, as stored (sent with the
      // JSON type of the metadata). JSON expressions are not translated.
      if (item->real_item()->type() != Item::FIELD_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::STRING;
      *cast = "VARCHAR";
      return false;
    case MYSQL_TYPE_GEOMETRY:
      // A GEOMETRY column: SRID + WKB, MySQL's bytes.
      if (item->real_item()->type() != Item::FIELD_ITEM)
        return refuse("unsupported_result_type");
      *kind = Kind::BLOB;
      *cast = "BLOB";
      return false;
    case MYSQL_TYPE_ENUM:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_VECTOR:
      return refuse("unsupported_result_type");
    default:
      break;
  }
  switch (item->result_type()) {
    case INT_RESULT:
      *kind = item->unsigned_flag ? Kind::UINT : Kind::INT;
      *cast = item->unsigned_flag ? "UBIGINT" : "BIGINT";
      return false;
    case REAL_RESULT:
      *kind = Kind::DOUBLE;
      *cast = "DOUBLE";
      return false;
    case DECIMAL_RESULT: {
      // MySQL widens SUM and arithmetic results up to 65 digits; DuckDB
      // computes up to 38 and raises an error on overflow (never a wrong
      // value): the result is read as at most DECIMAL(38,s).
      unsigned scale = item->decimals;
      if (scale > 37) return refuse("decimal_scale_above_37");
      if (item->decimal_precision() > 38) m_decimal_wide = true;
      m_max_scale = std::max(m_max_scale, static_cast<int>(scale));
      if (m_scale_cap >= 0)
        scale = std::min(scale, static_cast<unsigned>(m_scale_cap));
      unsigned precision = std::min(item->decimal_precision(), 38U);
      if (precision < scale) precision = scale;
      if (precision == 0) precision = 1;
      *kind = Kind::DECIMAL;
      // Read back as exact text: CAST(CAST(e AS DECIMAL(p,s)) AS VARCHAR).
      *cast = "DECIMAL(" + std::to_string(precision) + "," +
              std::to_string(scale) + ")) AS VARCHAR";
      return false;
    }
    case STRING_RESULT:
      if (item->collation.collation == &my_charset_bin) {
        *kind = Kind::BLOB;
        *cast = "BLOB";
      } else {
        *kind = Kind::STRING;
        *cast = "VARCHAR";
      }
      return false;
    default:
      return refuse("unsupported_result_type");
  }
}

// A base or materialized derived table of the FROM clause: its alias, and
// its primary key columns (base tables) for the PAD SPACE tie-break.
bool Generator::table_ref(Table_ref *tr, std::string *out) {
  if (tr->is_table_function()) return refuse("table_function");
  if (tr->is_recursive_reference()) return refuse("recursive_cte");
  TABLE *table = tr->table;
  if (table == nullptr) return refuse("table_not_open");
  TableInfo ti;
  ti.alias = "t" + std::to_string(m_next_alias++);
  if (tr->is_view_or_derived()) {
    // Merged views and derived tables are join nests (from_list); this one
    // is materialized: a subquery in FROM, columns named as MySQL names them.
    Query_expression *unit = tr->derived_query_expression();
    if (unit == nullptr) return refuse("derived_without_query");
    if (unit->is_recursive()) return refuse("recursive_cte");
    if (unit->m_lateral_deps != 0) return refuse("lateral_derived_table");
    std::vector<std::string> names;
    for (uint i = 0; i < table->visible_field_count(); i++)
      names.emplace_back(table->visible_field_ptr()[i]->field_name);
    // A grouped derived table joined on its GROUP BY columns (MySQL's
    // rewrite of correlated scalar / ALL / ANY subqueries for secondary
    // engines): LATERAL with the correlation put back (lateral_keys()).
    std::vector<std::pair<Item *, std::string>> keys;
    if (lateral_keys(tr, &keys)) return true;
    Block inner;
    Block *outer = m_block;
    const enum_field_types temporal = m_temporal;
    m_block = &inner;
    m_temporal = MYSQL_TYPE_NULL;
    m_lateral_keys = keys;
    std::string sql;
    // Columns of weight collations carry their weights when they can.
    const bool failed =
        unit->is_simple()
            ? block_sql(unit->first_query_block(), &names, nullptr, &sql,
                        nullptr, nullptr, &ti.weights, false)
            : setop_sql(unit->set_operation(), &names, nullptr, nullptr, &sql,
                        &ti.weights, false);
    m_block = outer;
    m_temporal = temporal;
    if (failed) return true;
    if (unit->m_reject_multiple_rows)
      // A scalar subquery the server made a table: MySQL raises
      // ER_SUBQUERY_NO_1_ROW when it has more than one row (the executor
      // maps DuckDB's error back to it).
      sql = "SELECT * EXCLUDE (\"__duckdb_rows\") FROM (SELECT *, count(*) "
            "OVER () AS \"__duckdb_rows\" FROM (" +
            sql +
            ") AS r) AS r WHERE CASE WHEN \"__duckdb_rows\" > 1 THEN "
            "error('" +
            std::string(kDuckDBMultipleRows) + "') ELSE TRUE END";
    *out = std::string(keys.empty() ? "" : "LATERAL ") + "(" + sql + ") AS " +
           ti.alias;
    ti.derived = true;
    ti.scalar = tr->m_was_scalar_subquery;
  } else {
    const std::string db = table->s->db.str;
    const std::string name = table->s->table_name.str;
    TableState state;
    if (!Registry::instance().snapshot(db, name, &state) || !state.usable())
      return refuse("not_loaded:" + db + "." + name);
    if (table->s->primary_key == MAX_KEY) return refuse("no_primary_key");
    const KEY &pk = table->key_info[table->s->primary_key];
    std::string negated;
    bool integer_key = true;
    for (const std::string &w : split_list(state.weight_columns))
      ti.weights.insert(w);
    for (unsigned i = 0; i < pk.user_defined_key_parts; i++) {
      const Field *f = pk.key_part[i].field;
      ti.pk_fields.push_back(f);
      const std::string c = ti.alias + "." + quote_ident(f->field_name);
      ti.pk_parts += (i ? ", " : "") + c;
      // HUGEINT: negating an unsigned BIGINT stays unsigned in DuckDB.
      negated += (i ? ", " : "") + ("-CAST(" + c + " AS HUGEINT)");
      integer_key = integer_key && f->result_type() == INT_RESULT;
    }
    ti.pk = pk.user_defined_key_parts == 1 ? ti.pk_parts
                                           : "row(" + ti.pk_parts + ")";
    if (integer_key) ti.pk_negated = negated;
    const std::string lake =
        "lake." + quote_ident(db) + "." + quote_ident(name);
    // LIVE scope hides archived rows. Filtering in a subquery keeps the
    // filter on the table itself, wherever it is in the join (in WHERE it
    // would turn a LEFT JOIN into an inner join).
    *out = state.retain_history && !m_full
               ? "(SELECT * FROM " + lake +
                     " WHERE \"__archived_at\" IS NULL) AS " + ti.alias
               : lake + " AS " + ti.alias;
  }
  m_info[table] = ti;
  m_block->tables.insert(table);
  return false;
}

// MySQL plans a correlated scalar subquery (and ALL / ANY) for a secondary
// engine as a derived table grouped on the correlation columns, joined on
// them (Query_block::transform_scalar_subqueries_to_join_with_derived):
//   FROM ot JOIN (SELECT agg(..), it.k FROM it GROUP BY it.k) AS d
//   ON / WHERE d.k = ot.k
// DuckDB then aggregates the whole inner table (TPC-H Q17: 600M rows into
// 20M groups at SF100) before the join can reduce it. Filtering the
// derived block on it.k = ot.k removes whole groups that the join drops
// anyway, so the result is the same; as LATERAL, DuckDB decorrelates it
// and aggregates only the keys it needs (2026-11-25). The equalities come
// from d's ON condition and, for an inner join, the block's WHERE (also
// multiple equalities); the outer column must be of a table d may
// reference (m_lateral_ok), the types such that DuckDB's = is MySQL's.
// keys: (d's select-list item for the key, the outer column's SQL).
bool Generator::lateral_keys(
    Table_ref *e, std::vector<std::pair<Item *, std::string>> *keys) {
  if (m_lateral_ok == 0 || e->table == nullptr) return false;
  Query_expression *unit = e->derived_query_expression();
  if (unit == nullptr || !unit->is_simple() || unit->m_reject_multiple_rows ||
      unit->m_lateral_deps != 0)
    return false;
  // Only what was a correlated subquery in the statement, as AliSQL and
  // MariaDB send it: a scalar one (m_was_scalar_subquery), or ALL / ANY
  // (a derived table MySQL made, "derived_N_M", whose select list has the
  // aggregates, grouped on the correlation columns). A grouped view, or an
  // uncorrelated IN (SELECT k ... GROUP BY k HAVING ...) whose select list
  // is the key (TPC-H Q15, Q18), is better as it is: LATERAL makes DuckDB
  // first collect all outer keys. (LATERAL is exact either way.)
  if (!e->m_was_scalar_subquery) {
    if (e->alias == nullptr || strncmp(e->alias, "derived_", 8) != 0)
      return false;
    bool aggregates = false;
    for (Item *v : unit->first_query_block()->visible_fields())
      aggregates = aggregates || v->has_aggregation();
    if (!aggregates) return false;
  }
  Query_block *dq = unit->first_query_block();
  if (dq->group_list.elements == 0 || dq->olap != UNSPECIFIED_OLAP_TYPE ||
      dq->has_windows() || dq->select_limit != nullptr ||
      dq->offset_limit != nullptr)
    return false;
  // d's column -> its select-list item, if it is a GROUP BY item.
  auto group_item = [&](const Item_field *f) -> Item * {
    if (f->field == nullptr || f->field->table != e->table) return nullptr;
    const uint idx = f->field->field_index();
    uint k = 0;
    Item *src = nullptr;
    for (Item *v : dq->visible_fields())
      if (k++ == idx) {
        src = v;
        break;
      }
    if (src == nullptr) return nullptr;
    for (ORDER *o = dq->group_list.first; o != nullptr; o = o->next)
      if ((*o->item)->real_item() == src->real_item() ||
          (*o->item)->real_item()->eq(src->real_item()))
        return src;
    return nullptr;
  };
  auto usable = [&](Item *x, Item *y) {
    const bool strings = is_string(x) || is_string(y);
    if (strings)
      return is_string(x) && is_string(y) && str_mode(x) == StrMode::RAW &&
             str_mode(y) == StrMode::RAW;
    return x->result_type() == y->result_type() &&
           x->result_type() != REAL_RESULT && !x->is_temporal() &&
           !y->is_temporal();
  };
  std::set<const Item *> done;
  auto consider = [&](Item_field *a, Item_field *b) -> bool {
    Item *src = group_item(a);
    if (src == nullptr || done.count(src->real_item()) != 0) return false;
    if (b->field == nullptr || b->field->table == e->table) return false;
    const table_map bm = b->used_tables() & ~PSEUDO_TABLE_BITS;
    if (bm == 0 || (bm & ~m_lateral_ok) != 0) return false;
    if (!usable(src->real_item(), b)) return false;
    std::string o;
    if (expr(b, &o)) return true;
    done.insert(src->real_item());
    keys->emplace_back(src, o);
    return false;
  };
  std::vector<Item *> parts;
  auto split = [&](Item *c) {
    if (c == nullptr) return;
    c = c->real_item();
    if (c->type() == Item::COND_ITEM &&
        down_cast<Item_func *>(c)->functype() == Item_func::COND_AND_FUNC) {
      for (Item &a : *down_cast<Item_cond *>(c)->argument_list())
        parts.push_back(a.real_item());
    } else {
      parts.push_back(c);
    }
  };
  split(e->join_cond());
  if (!e->outer_join && m_block->qb != nullptr)
    split(m_block->qb->where_cond());
  for (Item *c : parts) {
    if (c->type() != Item::FUNC_ITEM) continue;
    auto *fn = down_cast<Item_func *>(c);
    if (fn->functype() == Item_func::EQ_FUNC) {
      Item *l = fn->arguments()[0]->real_item();
      Item *r = fn->arguments()[1]->real_item();
      if (l->type() != Item::FIELD_ITEM || r->type() != Item::FIELD_ITEM)
        continue;
      auto *lf = down_cast<Item_field *>(l), *rf = down_cast<Item_field *>(r);
      if (consider(lf, rf) || consider(rf, lf)) return true;
    } else if (fn->functype() == Item_func::MULTI_EQ_FUNC) {
      std::vector<Item_field *> fields;
      for (Item_field &f : down_cast<Item_multi_eq *>(fn)->get_fields())
        fields.push_back(&f);
      for (Item_field *a : fields)
        for (Item_field *b : fields)
          if (a != b && consider(a, b)) return true;
    }
  }
  return false;
}

// MySQL plans NOT EXISTS / NOT IN for a secondary engine as a derived
// table (Item_exists_subselect::is_derived_candidate, cannot_do_antijoin):
//   FROM ot LEFT JOIN (SELECT DISTINCT 1 AS c, ie ...) AS d ON <cond>
//   WHERE d.c IS NULL
// which is exactly NOT EXISTS (SELECT 1 FROM (SELECT ...) AS d WHERE
// <cond>) when d.c is never NULL in d and d is used nowhere else. DuckDB
// plans that as an anti join; the LEFT JOIN made it compute the DISTINCT
// derived table first (TPC-H Q21 at SF100: hundreds of millions of rows,
// spilled to disk). Returns the IS NULL part of WHERE, or nullptr when e
// is not such a table.
Item *Generator::derived_marker(Table_ref *e, bool *anti, bool *top) {
  if (!e->outer_join || !e->is_view_or_derived() || e->table == nullptr ||
      e->m_was_scalar_subquery)
    return nullptr;
  Query_expression *unit = e->derived_query_expression();
  Query_block *qb = m_block->qb;
  if (unit == nullptr || !unit->is_simple() || unit->is_recursive() ||
      unit->m_reject_multiple_rows || unit->m_lateral_deps != 0 ||
      qb == nullptr || qb->where_cond() == nullptr)
    return nullptr;
  Query_block *dq = unit->first_query_block();
  if (dq->select_limit != nullptr || dq->offset_limit != nullptr)
    return nullptr;
  auto uses_d = [&](Item *item) {
    return item != nullptr &&
           WalkItem(item, enum_walk::PREFIX, [&](Item *i) {
             const Item *r = i->real_item();
             return r->type() == Item::FIELD_ITEM &&
                    down_cast<const Item_field *>(r)->field != nullptr &&
                    down_cast<const Item_field *>(r)->field->table == e->table;
           });
  };
  // d's column idx is in an equality (EQ or multiple equality) of ON.
  auto on_equated = [&](uint idx) {
    if (e->join_cond() == nullptr) return false;
    bool found = false;
    WalkItem(e->join_cond(), enum_walk::PREFIX, [&](Item *i) {
      if (i->type() != Item::FUNC_ITEM) return false;
      auto *fn = down_cast<Item_func *>(i);
      auto is_d = [&](Item *x) {
        x = x->real_item();
        return x->type() == Item::FIELD_ITEM &&
               down_cast<Item_field *>(x)->field != nullptr &&
               down_cast<Item_field *>(x)->field->table == e->table &&
               down_cast<Item_field *>(x)->field->field_index() == idx;
      };
      if (fn->functype() == Item_func::EQ_FUNC &&
          (is_d(fn->arguments()[0]) || is_d(fn->arguments()[1])))
        found = true;
      if (fn->functype() == Item_func::MULTI_EQ_FUNC)
        for (Item_field &f : down_cast<Item_multi_eq *>(fn)->get_fields())
          if (is_d(&f)) found = true;
      return found;
    });
    return found;
  };
  // IS [NOT] NULL on a column of d that is never NULL in d, or that ON
  // equates.
  auto is_marker = [&](Item *p, bool *is_anti) {
    if (p->type() != Item::FUNC_ITEM) return false;
    const auto ft = down_cast<Item_func *>(p)->functype();
    if (ft != Item_func::ISNULL_FUNC && ft != Item_func::ISNOTNULL_FUNC)
      return false;
    Item *a = down_cast<Item_func *>(p)->arguments()[0]->real_item();
    if (a->type() != Item::FIELD_ITEM ||
        down_cast<Item_field *>(a)->field == nullptr ||
        down_cast<Item_field *>(a)->field->table != e->table)
      return false;
    const uint idx = down_cast<Item_field *>(a)->field->field_index();
    uint k = 0;
    Item *src = nullptr;
    for (Item *f : dq->visible_fields())
      if (k++ == idx) {
        src = f;
        break;
      }
    if (src == nullptr) return false;
    // A nullable column (the key of IN) is a marker too when ON equates
    // it: a matched row then never has NULL there.
    if (src->is_nullable() && !on_equated(idx)) return false;
    *is_anti = ft == Item_func::ISNULL_FUNC;
    return true;
  };
  // Walk AND / OR: d may appear only in one marker.
  Item *marker = nullptr;
  bool bad = false;
  Item *where = qb->where_cond()->real_item();
  std::function<void(Item *, bool)> walk = [&](Item *c, bool at_top) {
    c = c->real_item();
    if (c->type() == Item::COND_ITEM) {
      const bool is_and = down_cast<Item_func *>(c)->functype() ==
                          Item_func::COND_AND_FUNC;
      for (Item &a : *down_cast<Item_cond *>(c)->argument_list())
        walk(&a, at_top && is_and);
      return;
    }
    if (!uses_d(c)) return;
    bool a = false;
    if (marker == nullptr && is_marker(c, &a)) {
      marker = c;
      *anti = a;
      *top = at_top;
      return;
    }
    bad = true;
  };
  walk(where, true);
  if (bad || marker == nullptr) return nullptr;
  // d nowhere else: select list, GROUP BY, HAVING, ORDER BY, other joins.
  // (used_tables() is not enough: COUNT(*) reports every table.)
  for (Item *f : qb->fields)
    if (uses_d(f)) return nullptr;
  if (uses_d(qb->having_cond())) return nullptr;
  for (ORDER *o = qb->group_list.first; o != nullptr; o = o->next)
    if (uses_d(*o->item)) return nullptr;
  for (ORDER *o = qb->order_list.first; o != nullptr; o = o->next)
    if (uses_d(*o->item)) return nullptr;
  std::function<bool(mem_root_deque<Table_ref *> &)> other_use =
      [&](mem_root_deque<Table_ref *> &list) {
        for (Table_ref *t : list) {
          if (t != e && uses_d(t->join_cond())) return true;
          if (t->nested_join != nullptr &&
              other_use(t->nested_join->m_tables))
            return true;
        }
        return false;
      };
  if (other_use(qb->m_table_nest)) return nullptr;
  return marker;
}

// d (a DISTINCT derived table) matches each outer row at most once: its ON
// condition is only equalities, together fixing every column of d that is
// not a constant (the EXISTS marker). Then LEFT JOIN d ... d.c IS [NOT] NULL
// never duplicated a row and [NOT] EXISTS gives the same rows anywhere in
// WHERE.
bool Generator::unique_match(Table_ref *e) {
  Query_expression *unit = e->derived_query_expression();
  if (unit == nullptr || e->join_cond() == nullptr) return false;
  Query_block *dq = unit->first_query_block();
  if (!dq->is_distinct()) return false;
  std::set<uint> fixed;
  auto d_field = [&](Item *i) -> const Item_field * {
    i = i->real_item();
    if (i->type() != Item::FIELD_ITEM) return nullptr;
    const auto *f = down_cast<const Item_field *>(i);
    return f->field != nullptr && f->field->table == e->table ? f : nullptr;
  };
  auto uses_d = [&](Item *item) {
    return WalkItem(item, enum_walk::PREFIX,
                    [&](Item *i) { return d_field(i) != nullptr; });
  };
  std::vector<Item *> parts;
  Item *c = e->join_cond()->real_item();
  if (c->type() == Item::COND_ITEM &&
      down_cast<Item_func *>(c)->functype() == Item_func::COND_AND_FUNC) {
    for (Item &a : *down_cast<Item_cond *>(c)->argument_list())
      parts.push_back(a.real_item());
  } else {
    parts.push_back(c);
  }
  for (Item *p : parts) {
    if (p->type() != Item::FUNC_ITEM) return false;
    auto *fn = down_cast<Item_func *>(p);
    if (fn->functype() == Item_func::EQ_FUNC) {
      Item *l = fn->arguments()[0], *r = fn->arguments()[1];
      const Item_field *dl = d_field(l), *dr = d_field(r);
      if (dl != nullptr && !uses_d(r))
        fixed.insert(dl->field->field_index());
      else if (dr != nullptr && !uses_d(l))
        fixed.insert(dr->field->field_index());
      else
        return false;
    } else if (fn->functype() == Item_func::MULTI_EQ_FUNC) {
      bool outer = false;
      std::vector<uint> mine;
      for (Item_field &f : down_cast<Item_multi_eq *>(fn)->get_fields()) {
        if (d_field(&f) != nullptr)
          mine.push_back(f.field->field_index());
        else
          outer = true;
      }
      if (!outer) return false;
      fixed.insert(mine.begin(), mine.end());
    } else {
      return false;
    }
  }
  uint k = 0;
  for (Item *f : dq->visible_fields()) {
    if (fixed.count(k) == 0 && !f->const_item()) return false;
    k++;
  }
  return true;
}

// A join list (stored in reverse order by the parser). Inner joins, CROSS
// JOIN and LEFT JOIN (RIGHT JOIN is already a LEFT JOIN) are printed as
// joins; semi-join nests (IN / EXISTS subqueries the optimizer flattened)
// become EXISTS and anti-join nests (NOT EXISTS, NOT IN) NOT EXISTS in the
// block's WHERE, so only at the top of the block.
bool Generator::from_list(mem_root_deque<Table_ref *> &list, bool under_outer,
                          bool in_semi, std::string *out) {
  std::string from;
  table_map seen = 0;  // tables of this list printed so far
  // The tables of this list's semi- and anti-join nests (printed as EXISTS
  // / NOT EXISTS in WHERE, not visible in FROM).
  table_map semi_all = 0;
  if (!in_semi)
    for (Table_ref *e : list)
      if (e->is_sj_or_aj_nest()) semi_all |= e->nested_join->used_tables;
  // The tables an EXISTS of this block can see: the block's tables outside
  // its semi- and anti-join nests.
  if (!in_semi && !under_outer && m_block->qb != nullptr &&
      &list == &m_block->qb->m_table_nest)
    m_main_visible = m_block->qb->all_tables_map() & ~semi_all;
  for (auto it = list.rbegin(); it != list.rend(); ++it) {
    Table_ref *e = *it;
    if (e->is_sj_or_aj_nest()) {
      if (under_outer || in_semi) return refuse("semijoin_not_at_top_level");
      Block::Semi s;
      s.nest = e;
      s.anti = e->is_aj_nest();
      s.tables = e->nested_join->used_tables;
      s.cond = e->join_cond();
      if (from_list(e->nested_join->m_tables, false, true, &s.from))
        return true;
      if (s.from.empty()) return refuse("empty_semijoin_nest");
      m_block->semi.push_back(s);
      continue;
    }
    // NOT EXISTS / NOT IN made a derived table by MySQL (2026-11-24):
    // printed back as NOT EXISTS (derived_marker()).
    bool anti = false, top = false;
    Item *marker = !in_semi && !under_outer && !from.empty() &&
                           e->nested_join == nullptr
                       ? derived_marker(e, &anti, &top)
                       : nullptr;
    if (marker != nullptr && !(anti && top) && unique_match(e)) {
      // Inside OR (or IS NOT NULL): [NOT] EXISTS in place of the marker;
      // each outer row matches at most one row of d (unique_match()), so
      // the LEFT JOIN never duplicated rows (2026-11-26).
      std::string sub;
      m_drop_distinct = true;
      bool failed = table_ref(e, &sub);
      m_drop_distinct = false;
      if (failed) return true;
      std::string on = "TRUE";
      if (e->join_cond() != nullptr) {
        const table_map saved_others = m_anti_others;
        const table_map saved_mine = m_anti_mine;
        m_anti_others = semi_all;
        m_anti_mine = e->map();
        failed = expr(e->join_cond(), &on);
        m_anti_others = saved_others;
        m_anti_mine = saved_mine;
        if (failed) return true;
      }
      m_block->marker_sql[marker] = std::string(anti ? "(NOT EXISTS (" : "(EXISTS (") +
                                    "SELECT 1 FROM " + sub + " WHERE " + on +
                                    "))";
      continue;
    }
    if (marker != nullptr && anti && top) {
      {
        Block::Semi s;
        s.nest = e;
        s.anti = true;
        s.tables = e->map();
        s.cond = e->join_cond();
        m_drop_distinct = true;
        const bool failed = table_ref(e, &s.from);
        m_drop_distinct = false;
        if (failed) return true;
        m_block->skip_where.insert(marker);
        m_block->semi.push_back(s);
        continue;
      }
    }
    std::string item;
    if (e->nested_join != nullptr) {
      std::string nest;
      if (from_list(e->nested_join->m_tables, under_outer || e->outer_join,
                    in_semi, &nest))
        return true;
      if (nest.empty()) return refuse("empty_join_nest");
      item = "(" + nest + ")";
    } else {
      // LATERAL only at the top of a block or of an EXISTS (not inside a
      // parenthesized join).
      m_lateral_ok =
          under_outer ? 0 : (seen | (in_semi ? m_main_visible : 0));
      const bool failed = table_ref(e, &item);
      m_lateral_ok = 0;
      if (failed) return true;
    }
    seen |= e->nested_join != nullptr ? e->nested_join->used_tables
                                      : (e->table != nullptr ? e->map() : 0);
    std::string on;
    if (e->join_cond() != nullptr) {
      // A join condition may name a column of a semi-join nest of this
      // block (TPC-H Q21: MySQL turns NOT EXISTS into LEFT JOIN derived ON
      // derived.k = l2.k, l2 in the EXISTS we print in WHERE, not visible
      // here): an equal outer column stands for it (outer_equal()).
      const table_map saved_others = m_anti_others;
      const table_map saved_mine = m_anti_mine;
      if (semi_all != 0) {
        m_anti_others = semi_all;
        m_anti_mine = e->nested_join != nullptr
                          ? e->nested_join->used_tables
                          : (e->table != nullptr ? e->map() : 0);
      }
      const bool failed = expr(e->join_cond(), &on);
      m_anti_others = saved_others;
      m_anti_mine = saved_mine;
      if (failed) return true;
    }
    if (from.empty()) {
      if (e->outer_join || !on.empty())
        return refuse("join_condition_on_first_table");
      from = item;
    } else if (e->outer_join) {
      from += " LEFT JOIN " + item + " ON " + (on.empty() ? "TRUE" : on);
    } else if (!on.empty()) {
      from += " JOIN " + item + " ON " + on;
    } else {
      from += " CROSS JOIN " + item;
    }
  }
  *out = from;
  return false;
}

// The top-level AND parts of a condition, each with the tables it uses;
// multiple equalities are split into pairs anchored on a column outside
// `avoid` (the semi-join tables) when there is one.
bool Generator::conjuncts(Item *cond, table_map avoid,
                          std::vector<Conj> *out) {
  // Conditions MySQL added to an IN subquery for its in-to-exists strategy:
  // we print the IN predicate itself (MySQL removes them the same way when
  // it changes strategy).
  if (cond->created_by_in2exists()) return false;
  cond = cond->real_item();
  if (cond->created_by_in2exists()) return false;
  if (m_block->skip_where.count(cond) != 0) return false;
  if (cond->type() == Item::COND_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::COND_AND_FUNC) {
    for (Item &arg : *down_cast<Item_cond *>(cond)->argument_list())
      if (conjuncts(&arg, avoid, out)) return true;
    return false;
  }
  if (cond->type() == Item::FUNC_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::MULTI_EQ_FUNC)
    return multi_eq(down_cast<Item_multi_eq *>(cond), avoid, out);
  std::string s;
  if (expr(cond, &s)) return true;
  Conj c{s, cond->used_tables() & ~PSEUDO_TABLE_BITS};
  if (cond->type() == Item::FUNC_ITEM &&
      down_cast<Item_func *>(cond)->functype() == Item_func::EQ_FUNC) {
    Item *l = down_cast<Item_func *>(cond)->arguments()[0]->real_item();
    Item *r = down_cast<Item_func *>(cond)->arguments()[1]->real_item();
    if (l->type() == Item::FIELD_ITEM && r->type() == Item::FIELD_ITEM) {
      c.eq_a = down_cast<Item_field *>(l);
      c.eq_b = down_cast<Item_field *>(r);
    }
  }
  out->push_back(c);
  return false;
}

// Two columns MySQL compares the same way DuckDB does.
bool Generator::comparable(Item *a, Item *b) {
  if (is_string(a) || is_string(b))
    return is_string(a) && is_string(b) && str_mode(a) == str_mode(b) &&
           str_mode(a) != StrMode::REFUSE &&
           (str_mode(a) != StrMode::WEIGHT ||
            a->collation.collation == b->collation.collation);
  if (a->is_temporal() || b->is_temporal()) {
    const bool da = is_date_or_datetime(a->data_type());
    const bool db = is_date_or_datetime(b->data_type());
    return (da && db) || a->data_type() == b->data_type();
  }
  return true;
}

// The optimizer's equality propagation: f1 = f2 = ... [= const], as pairs.
bool Generator::multi_eq(Item_multi_eq *eq, table_map avoid,
                         std::vector<Conj> *out) {
  std::vector<Item *> fields;
  for (Item_field &f : eq->get_fields()) fields.push_back(&f);
  if (fields.empty()) return refuse("empty_multiple_equality");
  size_t anchor = 0;
  for (size_t i = 0; i < fields.size(); i++)
    if ((fields[i]->used_tables() & avoid) == 0) {
      anchor = i;
      break;
    }
  Item *first = fields[anchor];
  for (Item *f : fields)
    if (special_column(f) == SpecialColumn::OPAQUE)
      return refuse("json_or_geometry_in_function:=");
  const bool str = is_string(first);
  if (str && str_mode(first) == StrMode::REFUSE)
    return refuse_collation(first);
  const enum_field_types t = first->data_type();
  const CHARSET_INFO *fcs = first->collation.collation;
  auto print = [&](Item *it, std::string *s) {
    return str ? string_key(it, fcs, false, it->max_length, s) : expr(it, s);
  };
  std::string f0;
  if (print(first, &f0)) return true;
  const table_map t0 = first->used_tables() & ~PSEUDO_TABLE_BITS;
  for (size_t i = 0; i < fields.size(); i++) {
    if (i == anchor) continue;
    if (!comparable(first, fields[i]))
      return refuse("multiple_equality_types");
    std::string a;
    if (print(fields[i], &a)) return true;
    out->push_back({"(" + f0 + " = " + a + ")",
                    t0 | (fields[i]->used_tables() & ~PSEUDO_TABLE_BITS),
                    true, eq});
  }
  if (Item *c = eq->const_arg(); c != nullptr) {
    std::string cs;
    if (t == MYSQL_TYPE_TIME) {
      if (c->is_null() ? literal(c, &cs) : time_literal(c, &cs)) return true;
    } else if (t == MYSQL_TYPE_TIMESTAMP || is_date_or_datetime(t)) {
      if (typed_literal(c, t, &cs)) return true;
    } else if (str != is_string(c)) {
      return refuse("multiple_equality_types");
    } else if (print(c, &cs)) {
      return true;
    }
    out->push_back({"(" + f0 + " = " + cs + ")", t0});
  }
  return false;
}

// One query block: FROM (joins), WHERE (with EXISTS / NOT EXISTS for the
// semi- and anti-join nests), GROUP BY, select list, HAVING, ORDER BY,
// LIMIT. `names`: the columns of a derived table (select values named, kept
// in their DuckDB type); otherwise the top block (values cast for reading
// back, kinds filled).
bool Generator::block_sql(Query_block *qb, const std::vector<std::string> *names,
                          std::vector<Kind> *kinds, std::string *sql,
                          const std::vector<Item *> *targets,
                          const std::string *from_sql,
                          std::set<std::string> *weights_out,
                          bool weights_required) {
  if (qb->active_options() & OPTION_FOUND_ROWS)
    return refuse("sql_calc_found_rows");
  if (qb->olap != UNSPECIFIED_OLAP_TYPE) return refuse("rollup");
  if (qb->has_windows()) return refuse("window");
  m_block->qb = qb;
  const bool drop_distinct = m_drop_distinct;  // this block only
  m_drop_distinct = false;
  const std::vector<std::pair<Item *, std::string>> lateral =
      std::move(m_lateral_keys);
  m_lateral_keys.clear();
  const table_map saved_main_visible = m_main_visible;

  std::string from;
  if (from_sql != nullptr)
    from = *from_sql;
  else if (from_list(qb->m_table_nest, false, false, &from))
    return true;
  // A block without tables (SELECT 1, as a set operation operand) has no
  // FROM.
  if (from.empty() && qb->leaf_table_count != 0) return refuse("no_table");
  m_main_visible = saved_main_visible;

  // WHERE, on rows before grouping. A semi-join's condition was merged into
  // WHERE: each AND part that uses a semi-join's tables goes into its EXISTS
  // (for a semi-join, a part on the outer tables alone is the same there or
  // in WHERE).
  table_map semi_tables = 0;
  for (const Block::Semi &s : m_block->semi)
    if (!s.anti) semi_tables |= s.tables;
  std::vector<std::string> where;
  std::vector<std::vector<std::string>> semi_conds(m_block->semi.size());
  if (qb->where_cond() != nullptr) {
    // Split in AND parts (in-to-exists conditions are left out).
    std::vector<Conj> parts;
    if (conjuncts(qb->where_cond(), semi_tables, &parts)) return true;
    // For each semi-join nest, the outer columns equal to each of its
    // columns (plain equalities: MySQL's best-equal-field substitution
    // anchors o.k = l2.k and l1.k = l2.k on l2, a table of the EXISTS).
    std::vector<std::map<const Field *, std::vector<Item_field *>>> links(
        m_block->semi.size());
    for (const Conj &c : parts) {
      int hit = -1;
      for (size_t i = 0; i < m_block->semi.size(); i++) {
        const Block::Semi &s = m_block->semi[i];
        if (s.anti || (c.tables & s.tables) == 0)
          continue;
        if (hit >= 0) return refuse("condition_across_semijoins");
        hit = static_cast<int>(i);
      }
      if (hit < 0) {
        where.push_back(c.sql);
        continue;
      }
      semi_conds[hit].push_back(c.sql);
      if (c.eq_a != nullptr && c.eq_a->field != nullptr &&
          c.eq_b->field != nullptr) {
        const table_map nest = m_block->semi[hit].tables;
        const table_map a = c.eq_a->used_tables(), b = c.eq_b->used_tables();
        if ((a & nest) != 0 && (b & semi_tables) == 0)
          links[hit][c.eq_a->field].push_back(c.eq_b);
        else if ((b & nest) != 0 && (a & semi_tables) == 0)
          links[hit][c.eq_b->field].push_back(c.eq_a);
      }
    }
    // Outer columns equal to the same column of a nest are equal to each
    // other whenever the EXISTS holds: we also say so in the outer WHERE,
    // so DuckDB joins them (without it, TPC-H Q21 at SF100 made DuckDB
    // build ORDERS x LINEITEM and fill memory). The result cannot change.
    for (const auto &nest_links : links)
      for (const auto &link : nest_links) {
        const std::vector<Item_field *> &outs = link.second;
        for (size_t k = 1; k < outs.size(); k++) {
          // Only where DuckDB's = is MySQL's: binary strings, or the same
          // exact type (no DOUBLE, no collation keys).
          Item *x = outs[0], *y = outs[k];
          const bool strings = is_string(x) || is_string(y);
          if (strings ? !(is_string(x) && is_string(y) &&
                          str_mode(x) == StrMode::RAW &&
                          str_mode(y) == StrMode::RAW)
                      : (x->result_type() != y->result_type() ||
                         x->result_type() == REAL_RESULT ||
                         !comparable(x, y)))
            continue;
          std::string a, b;
          if (expr(outs[0], &a) || expr(outs[k], &b)) return true;
          where.push_back("(" + a + " = " + b + ")");
        }
      }
  }
  for (size_t i = 0; i < m_block->semi.size(); i++) {
    const Block::Semi &s = m_block->semi[i];
    std::vector<std::string> conds = semi_conds[i];
    if (s.anti && s.cond != nullptr) {
      // An anti-join keeps its condition, as a LEFT JOIN's ON, split in AND
      // parts. MySQL's equality propagation may put a column of another
      // semi-join nest into it (TPC-H Q21: l1.k = l2.k = l3.k, l2 in the
      // EXISTS, l3 in this NOT EXISTS), and that table is not visible in
      // this subquery. We anchor each multiple equality on a visible column
      // (outer or ours); a pair that still ties in another nest's column
      // only repeats an equality that nest's own EXISTS enforces, and is
      // left out. Any other condition on another nest's table: refused.
      table_map others = 0;
      for (size_t j = 0; j < m_block->semi.size(); j++)
        if (j != i) others |= m_block->semi[j].tables;
      const table_map mine = s.tables;
      if (s.nest->nested_join == nullptr) {
        // A derived anti-join (derived_marker()): its ON condition,
        // where a column of another nest is printed as an equal outer
        // column (outer_equal()), as when it was a LEFT JOIN.
        std::string c;
        m_anti_others = others;
        m_anti_mine = mine;
        const bool failed = expr(s.cond, &c);
        m_anti_others = 0;
        m_anti_mine = 0;
        if (failed) return true;
        conds.push_back(c);
      }
      std::vector<Conj> parts;
      m_anti_others = others;
      m_anti_mine = mine;
      const bool failed = s.nest->nested_join != nullptr &&
                          conjuncts(s.cond, others, &parts);
      m_anti_others = 0;
      m_anti_mine = 0;
      if (failed) return true;
      for (const Conj &c : parts) {
        if ((c.tables & others) != 0) {
          // Left out only when the equality also holds an outer column:
          // then this nest's pair with it is printed here, and the other
          // nest's pair with it in that nest's EXISTS.
          bool outer_member = false;
          if (c.implied && c.eq != nullptr)
            for (Item_field &f : c.eq->get_fields())
              outer_member = outer_member ||
                             (f.used_tables() & (others | mine)) == 0;
          if (outer_member) continue;
          return refuse("condition_across_semijoins");
        }
        conds.push_back(c.sql);
      }
    }
    std::string body = "SELECT 1 FROM " + s.from;
    for (size_t j = 0; j < conds.size(); j++)
      body += (j == 0 ? " WHERE " : " AND ") + conds[j];
    where.push_back(std::string(s.anti ? "NOT EXISTS (" : "EXISTS (") + body +
                    ")");
  }
  // A LATERAL derived block: its GROUP BY columns equal to the outer
  // columns the join compares them with (lateral_keys()).
  for (const auto &k : lateral) {
    std::string inner;
    if (expr(k.first, &inner)) return true;
    where.push_back("(" + inner + " = " + k.second + ")");
  }

  // GROUP BY. PAD SPACE strings group on rtrim(); a plain column of the
  // select list must be grouped (DuckDB has no ONLY_FULL_GROUP_BY off mode).
  std::string group;
  for (ORDER *o = qb->group_list.first; o != nullptr; o = o->next) {
    Item *g = (*o->item)->real_item();
    if (special_column(g) == SpecialColumn::OPAQUE)
      return refuse("json_or_geometry_in_group_by");
    if (g->type() == Item::FIELD_ITEM)
      m_block->grouped.insert(down_cast<Item_field *>(g)->field);
    std::string e;
    if (is_string(g) && (str_mode(g) == StrMode::PAD ||
                         str_mode(g) == StrMode::WEIGHT)) {
      // Grouped on the equality key (rtrim / weight); the value shown is
      // decided by grouped_value().
      if (g->type() != Item::FIELD_ITEM && exact_text(g, false)) {
        if (expr(g, &e)) return true;
        group += (group.empty() ? "" : ", ") + e;
        continue;
      }
      if (g->type() != Item::FIELD_ITEM) {
        // An expression (Q22: SUBSTR(c_phone, 1, 2)) has no weight column:
        // only PAD SPACE, grouped on rtrim() (2026-11-22).
        // Also a binary PAD SPACE collation of latin1 / ascii (weights for
        // columns; string_key() keys an expression on rtrim()).
        const CHARSET_INFO *gcs = g->collation.collation;
        const bool binary_pad = (gcs->state & MY_CS_BINSORT) != 0 &&
                                gcs->pad_attribute != NO_PAD &&
                                (strcmp(gcs->csname, "latin1") == 0 ||
                                 strcmp(gcs->csname, "ascii") == 0);
        if (str_mode(g) != StrMode::PAD && !binary_pad)
          return refuse(std::string("group_by_expression (") +
                        gcs->m_coll_name + ")");
        if (string_key(g, gcs, false, g->max_length, &e)) return true;
        m_block->group_pad_exprs.push_back(g);
        group += (group.empty() ? "" : ", ") + e;
        continue;
      }
      m_block->group_pad.push_back(down_cast<Item_field *>(g)->field);
      if (string_key(g, g->collation.collation, false, g->max_length, &e))
        return true;
    } else {
      if (is_string(g) && str_mode(g) == StrMode::REFUSE)
        return refuse_collation(g);
      if (expr(g, &e)) return true;
    }
    group += (group.empty() ? "" : ", ") + e;
  }
  m_block->post_group = !group.empty();

  // Select list: the top block casts each value to the type we read back; a
  // derived table keeps DuckDB types and names its columns.
  const bool distinct = qb->is_distinct() && !drop_distinct;
  std::string select;
  size_t col = 0;
  for (Item *item : qb->visible_fields()) {
    if (special_column(item->real_item()) == SpecialColumn::OPAQUE &&
        (distinct || targets != nullptr))
      return refuse(distinct ? "json_or_geometry_in_distinct"
                             : "json_or_geometry_in_set_operation");
    if (distinct && is_string(item) && str_mode(item) != StrMode::RAW &&
        !exact_text(item, false))
      return str_mode(item) == StrMode::PAD
                 ? refuse("distinct_padded_string")
                 : refuse_collation(item);
    std::string e, cast;
    Kind kind;
    if (output_kind(item, &kind, &cast) || expr(item, &e)) return true;
    if (targets != nullptr) {
      // An operand of a set operation: cast to the column type MySQL
      // computed for the result. Mixing kinds (number and string, temporal
      // and other) is refused: MySQL's conversions differ (decided
      // 2026-11-13).
      if (col >= targets->size()) return refuse("set_operation_columns");
      Kind target;
      std::string target_cast;
      if (output_kind((*targets)[col], &target, &target_cast)) return true;
      // A NULL literal takes any type (is_null() of a column would read
      // the current row: only the item type is checked).
      if (item->real_item()->type() != Item::NULL_ITEM &&
          !same_class(kind, target))
        return refuse("set_operation_mixed_types");
      kind = target;
      cast = target_cast;
    }
    std::string value;
    if (names == nullptr) {
      value = kind == Kind::DECIMAL ? "CAST(CAST(" + e + " AS " + cast + ")"
                                    : "CAST(" + e + " AS " + cast + ")";
      kinds->push_back(kind);
    } else {
      if (col >= names->size()) return refuse("derived_column_count");
      if (kind == Kind::DECIMAL) cast = cast.substr(0, cast.find(')') + 1);
      value = "CAST(" + e + " AS " + cast + ") AS " + quote_ident((*names)[col]);
      // A string of a weight collation carries its weight, for grouping and
      // ordering on this column outside (derived table, set operation).
      Item *type_item = targets != nullptr ? (*targets)[col] : item;
      if (weights_out != nullptr && is_string(type_item) &&
          str_mode(type_item) == StrMode::WEIGHT) {
        std::string w;
        const std::string saved_reason = *m_reason;
        if (!weight_expr(item, type_item->collation.collation, &w)) {
          value += ", " + w + " AS " +
                   quote_ident(std::string(kWeightPrefix) + (*names)[col]);
          weights_out->insert((*names)[col]);
        } else if (weights_required) {
          return true;
        } else {
          *m_reason = saved_reason;  // best effort: no weight for it
        }
      }
    }
    col++;
    select += (select.empty() ? "" : ", ") + value;
  }

  std::string having;
  if (qb->having_cond() != nullptr) {
    std::vector<Conj> parts;
    if (conjuncts(qb->having_cond(), 0, &parts)) return true;
    for (const Conj &c : parts)
      having += (having.empty() ? "" : " AND ") + c.sql;
  }

  // MySQL sorts NULL first ascending and last descending; DuckDB sorts NULL
  // last by default.
  std::string order;
  for (ORDER *o = qb->order_list.first; o != nullptr; o = o->next) {
    std::string s;
    if (special_column((*o->item)->real_item()) == SpecialColumn::OPAQUE)
      return refuse("json_or_geometry_in_order_by");
    // MySQL sorts ENUM / SET on the index, not the string.
    const bool by_index =
        special_column((*o->item)->real_item()) == SpecialColumn::OTHER &&
        down_cast<Item_field *>((*o->item)->real_item())->field->real_type() !=
            MYSQL_TYPE_TIME2;
    if (by_index ? enum_index(*o->item, &s) : order_key(*o->item, &s))
      return true;
    const bool desc = o->direction == ORDER_DESC;
    order += (order.empty() ? "" : ", ") + s +
             (desc ? " DESC NULLS LAST" : " ASC NULLS FIRST");
  }

  *sql = std::string("SELECT ") + (distinct ? "DISTINCT " : "") + select +
         (from.empty() ? "" : " FROM " + from);
  for (size_t i = 0; i < where.size(); i++)
    *sql += (i == 0 ? " WHERE " : " AND ") + where[i];
  if (!group.empty()) *sql += " GROUP BY " + group;
  if (!having.empty()) *sql += " HAVING " + having;
  if (!order.empty()) *sql += " ORDER BY " + order;
  // A LIMIT 1 MySQL added to a subquery predicate (m_internal_limit, for its
  // EXISTS strategy) is not the query's: it would cut an IN list to one
  // value.
  if (qb->select_limit != nullptr && !qb->m_internal_limit) {
    if (!is_constant(qb->select_limit)) return refuse("limit_not_constant");
    *sql += " LIMIT " + std::to_string(qb->select_limit->val_uint());
  }
  if (qb->offset_limit != nullptr) {
    if (!is_constant(qb->offset_limit)) return refuse("offset_not_constant");
    *sql += " OFFSET " + std::to_string(qb->offset_limit->val_uint());
  }
  return false;
}

// IS [NOT] TRUE / FALSE and NOT, as MySQL's Bool_test (IS TRUE: NULL is
// false).
std::string Generator::truth(const std::string &e, Item::Bool_test test,
                             bool *ok) {
  *ok = true;
  switch (test) {
    case Item::BOOL_IDENTITY:
      return e;
    case Item::BOOL_NEGATED:
      return "(NOT " + e + ")";
    case Item::BOOL_IS_TRUE:
      return "coalesce(" + e + ", FALSE)";
    case Item::BOOL_IS_FALSE:
      return "coalesce(NOT " + e + ", FALSE)";
    case Item::BOOL_NOT_TRUE:
      return "(NOT coalesce(" + e + ", FALSE))";
    case Item::BOOL_NOT_FALSE:
      return "coalesce(" + e + ", TRUE)";
    default:
      *ok = false;
      return e;
  }
}

// The query of a subquery predicate or scalar subquery, inside the current
// block: it may refer to the enclosing blocks' tables (correlated).
bool Generator::subquery_sql(Query_expression *unit, std::string *sql) {
  if (unit == nullptr) return refuse("subquery_without_query");
  if (unit->is_recursive()) return refuse("recursive_cte");
  size_t columns = 0;
  if (unit->is_simple()) {
    for (Item *item : unit->first_query_block()->visible_fields()) {
      (void)item;
      columns++;
    }
  } else {
    for (Item *type : *unit->set_operation()->types_array())
      if (!type->hidden) columns++;
  }
  std::vector<std::string> names;
  for (size_t i = 0; i < columns; i++)
    names.push_back("c" + std::to_string(i + 1));
  Block inner;
  inner.parent = m_block;
  Block *outer = m_block;
  const enum_field_types temporal = m_temporal;
  m_block = &inner;
  m_temporal = MYSQL_TYPE_NULL;
  const bool failed =
      unit->is_simple()
          ? block_sql(unit->first_query_block(), &names, nullptr, sql)
          : setop_sql(unit->set_operation(), &names, nullptr, nullptr, sql);
  m_block = outer;
  m_temporal = temporal;
  return failed;
}

// Subquery items the server left as expressions: scalar subqueries,
// [NOT] EXISTS, [NOT] IN, with MySQL's truth test. DuckDB evaluates IN and
// NOT IN with the same three-valued logic (NULL when there is no match and
// the subquery or the operand has a NULL; FALSE for an empty subquery).
bool Generator::subquery(Item_subselect *sub, std::string *out) {
  std::string q;
  switch (sub->subquery_type()) {
    case Item_subselect::SCALAR_SUBQUERY: {
      auto *s = down_cast<Item_singlerow_subselect *>(sub);
      if (s->is_maxmin()) return refuse("quantified_subquery");
      if (s->cols() != 1) return refuse("row_subquery");
      if (subquery_sql(sub->query_expr(), &q)) return true;
      // More than one row: DuckDB raises an error, mapped to MySQL's.
      *out = "(" + q + ")";
      return false;
    }
    case Item_subselect::EXISTS_SUBQUERY: {
      if (subquery_sql(sub->query_expr(), &q)) return true;
      bool ok = true;
      *out = truth("EXISTS (" + q + ")",
                   down_cast<Item_exists_subselect *>(sub)->value_transform,
                   &ok);
      return ok ? false : refuse("truth_test");
    }
    case Item_subselect::IN_SUBQUERY: {
      auto *in = down_cast<Item_in_subselect *>(sub);
      Item *left = in->left_expr != nullptr ? in->left_expr->real_item()
                                            : nullptr;
      if (left == nullptr) return refuse("in_subquery_operand");
      if (left->type() == Item::CACHE_ITEM &&
          down_cast<Item_cache *>(left)->get_example() != nullptr)
        left = down_cast<Item_cache *>(left)->get_example()->real_item();
      if (left->type() == Item::ROW_ITEM) return refuse("row_in_subquery");
      Query_expression *unit = sub->query_expr();
      if (!unit->is_simple()) return refuse("in_subquery_set_operation");
      Item *inner = nullptr;
      for (Item *item : unit->first_query_block()->visible_fields())
        if (inner == nullptr) inner = item->real_item();
      if (inner == nullptr) return refuse("in_subquery_columns");
      // Compared as MySQL compares them: byte strings, same kinds.
      if (is_string(left) && is_string(inner)) {
        if (str_mode(left) == StrMode::REFUSE) return refuse_collation(left);
        if (str_mode(left) != StrMode::RAW || str_mode(inner) != StrMode::RAW)
          return refuse("in_subquery_collation");
      }
      if (!comparable(left, inner)) return refuse("in_subquery_types");
      std::string l;
      if (expr(left, &l) || subquery_sql(unit, &q)) return true;
      bool ok = true;
      *out = truth("(" + l + " IN (" + q + "))", in->value_transform, &ok);
      return ok ? false : refuse("truth_test");
    }
    default:
      // ALL / ANY: the server rewrites the usual cases into joins.
      return refuse("quantified_subquery");
  }
}

// A set operation (UNION, INTERSECT, EXCEPT, or a parenthesized query with
// its own ORDER BY / LIMIT): `SELECT ... FROM ((c0) OP (c1) ...) AS tN
// [ORDER BY] [LIMIT]`, printed by the post-processing block whose only
// table is the result. The operands' columns are cast to the result types
// and named c1, c2, ...; operators are left-associative in both:
// - UNION: DISTINCT up to the last DISTINCT operand, ALL after it;
// - INTERSECT: DISTINCT if any operator is (the parser folds it so);
// - EXCEPT: ALL before the first DISTINCT operand, DISTINCT from it on.
bool Generator::setop_sql(Query_term_set_op *t,
                          const std::vector<std::string> *names,
                          const std::vector<Item *> *targets,
                          std::vector<Kind> *kinds, std::string *sql,
                          std::set<std::string> *weights_out,
                          bool weights_required) {
  Query_block *post = t->query_block();
  if (post == nullptr || post->get_table_list() == nullptr ||
      post->get_table_list()->table == nullptr)
    return refuse("set_operation_without_result_table");
  TABLE *result = post->get_table_list()->table;
  std::vector<Item *> types;
  for (Item *type : *t->types_array())
    if (!type->hidden) types.push_back(type);
  std::vector<std::string> cols;
  for (size_t i = 0; i < types.size(); i++)
    cols.push_back("c" + std::to_string(i + 1));

  const Query_term_type op = t->term_type();
  const size_t n = t->child_count();
  // Comparing rows: strings must compare as bytes.
  const bool compares = op == QT_INTERSECT || op == QT_EXCEPT ||
                        (op == QT_UNION && t->last_distinct() > 0);
  if (compares)
    for (Item *type : types)
      if (is_string(type) && str_mode(type) != StrMode::RAW)
        return str_mode(type) == StrMode::PAD
                   ? refuse("set_operation_padded_string")
                   : refuse_collation(type);

  std::string chain;
  for (size_t i = 0; i < n; i++) {
    Query_term *c = t->child(i);
    std::string csql;
    Block inner;
    Block *outer = m_block;
    m_block = &inner;
    // Every operand carries the weights of the result's weight-collation
    // string columns (the same columns in each, for ORDER BY on them).
    std::set<std::string> child_weights;
    const bool failed =
        c->term_type() == QT_QUERY_BLOCK
            ? block_sql(down_cast<Query_block *>(c), &cols, nullptr, &csql,
                        &types, nullptr, &child_weights, true)
            : setop_sql(down_cast<Query_term_set_op *>(c), &cols, &types,
                        nullptr, &csql, &child_weights, true);
    m_block = outer;
    if (failed) return true;
    if (i > 0) {
      const int64_t idx = static_cast<int64_t>(i);
      switch (op) {
        case QT_UNION:
          chain += idx <= t->last_distinct() ? " UNION " : " UNION ALL ";
          break;
        case QT_INTERSECT:
          chain += t->last_distinct() > 0 ? " INTERSECT " : " INTERSECT ALL ";
          break;
        case QT_EXCEPT:
          chain += idx >= t->first_distinct() ? " EXCEPT " : " EXCEPT ALL ";
          break;
        default:
          return refuse("set_operation_type");
      }
    }
    chain += "(" + csql + ")";
  }

  TableInfo ti;
  ti.alias = "t" + std::to_string(m_next_alias++);
  ti.derived = true;
  ti.positional = true;
  for (size_t i = 0; i < types.size(); i++)
    if (is_string(types[i]) && str_mode(types[i]) == StrMode::WEIGHT)
      ti.weights.insert(cols[i]);
  m_info[result] = ti;
  Block block;
  block.tables.insert(result);
  Block *outer = m_block;
  m_block = &block;
  const std::string from = "(" + chain + ") AS " + ti.alias;
  const bool failed = block_sql(post, names, kinds, sql, targets, &from,
                                weights_out, weights_required);
  m_block = outer;
  return failed;
}

bool Generator::generate(LEX *lex, std::string *sql,
                         std::vector<Kind> *kinds) {
  if (lex->sql_command != SQLCOM_SELECT)
    return refuse("unsupported_statement");
  // PAD_CHAR_TO_FULL_LENGTH (deprecated): MySQL returns CHAR values padded
  // to their length; the copy holds them without trailing spaces.
  if (m_thd->variables.sql_mode & MODE_PAD_CHAR_TO_FULL_LENGTH)
    return refuse("sql_mode:PAD_CHAR_TO_FULL_LENGTH");
  Query_expression *unit = lex->unit;

  // History scope (DWL#5): LIVE hides archived rows; FULL shows them and
  // needs DUCKDB_HISTORY_READER at query time too (a session can inherit
  // FULL from the global value).
  m_full = history_scope_full(m_thd);
  if (m_full && !m_thd->security_context()
                     ->has_global_grant(STRING_WITH_LEN("DUCKDB_HISTORY_READER"))
                     .first)
    return refuse("history_scope_full_without_privilege");

  Block top;
  m_block = &top;
  const bool failed =
      unit->is_simple()
          ? block_sql(unit->first_query_block(), nullptr, kinds, sql)
          : setop_sql(unit->set_operation(), nullptr, nullptr, kinds, sql);
  m_block = nullptr;
  return failed;
}

}  // namespace offload
}  // namespace duckdb_se
