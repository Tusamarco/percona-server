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

// Query offload, strings: collations (StrMode), PAD SPACE and weight keys, ENUM
// and SET index, grouped string values.

#include "storage/duckdb/duckdb_offload_internal.h"

#include <cstring>
#include <string>

#include "sql/sql_class.h"
#include "storage/duckdb/duckdb_collation.h"
#include "storage/duckdb/duckdb_types.h"  // quote_ident

namespace duckdb_se {
namespace offload {

bool is_string(const Item *item) {
  return item->result_type() == STRING_RESULT && !item->is_temporal();
}

namespace {

bool is_unicode_charset(const CHARSET_INFO *cs) {
  return cs->csname != nullptr && (strcmp(cs->csname, "utf8mb4") == 0 ||
                                   strcmp(cs->csname, "utf8mb3") == 0);
}

}  // namespace

StrMode str_mode_cs(const CHARSET_INFO *cs) {
  if (cs == &my_charset_bin) return StrMode::RAW;
  if ((cs->state & MY_CS_BINSORT) != 0 && is_unicode_charset(cs))
    return cs->pad_attribute == NO_PAD ? StrMode::RAW : StrMode::PAD;
  return needs_weight(cs) ? StrMode::WEIGHT : StrMode::REFUSE;
}

namespace {

// A BLOB literal of any bytes.
std::string blob_literal(const std::string &bytes) {
  static const char hex[] = "0123456789ABCDEF";
  std::string out = "'";
  for (unsigned char b : bytes) {
    out += "\\x";
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out + "'::BLOB";
}

}  // namespace

StrMode str_mode(const Item *item) {
  return str_mode_cs(item->collation.collation);
}

// A string expression whose values compare under its collation as their
// bytes: DATE_FORMAT with a format we translate (equality: one format gives
// one spelling of each value; ordering: fixed-width numeric formats only).
// GROUP BY, DISTINCT and ORDER BY use it directly instead of a weight.
bool exact_text(const Item *item, bool ordering) {
  const Item *r = const_cast<Item *>(item)->real_item();
  if (r->type() != Item::FUNC_ITEM) return false;
  const auto *fn = down_cast<const Item_func *>(r);
  if (strcmp(fn->func_name(), "date_format") != 0) return false;
  std::string fmt;
  bool fixed = false;
  if (!date_format_format(fn, &fmt) ||
      !date_format_sql(fmt, "x", nullptr, &fixed))
    return false;
  return !ordering || fixed;
}

namespace {

// Comparing "as if padded with spaces": rtrim, then pad with at least as many
// spaces as the longest operand can have. This orders exactly like the PAD
// SPACE collation, including strings that continue with characters below a
// space ('a' > 'a\t').
std::string pad_sort_key(const std::string &e, size_t max_bytes) {
  return "(rtrim(" + e + ", ' ') || repeat(' ', " +
         std::to_string(max_bytes + 1) + "))";
}

}  // namespace

// A CHAR column (not BINARY): stored without trailing spaces (the binary
// log strips them, the load too), so under PAD SPACE its equality key is the
// column itself; rtrim() would hide it from DuckDB's filter pushdown and
// statistics (TPC-H: PART 4M estimated rows instead of 20K, 2026-11-26).
bool is_char_column(const Item *item) {
  const Item *r = const_cast<Item *>(item)->real_item();
  if (r->type() != Item::FIELD_ITEM) return false;
  const auto *fi = down_cast<const Item_field *>(r);
  const Field *f = fi->field;
  // Base tables only: a derived table's or view's CHAR column is an
  // expression in DuckDB, which may keep trailing spaces.
  return f != nullptr && f->real_type() == MYSQL_TYPE_STRING &&
         f->charset() != &my_charset_bin && fi->m_table_ref != nullptr &&
         !fi->m_table_ref->is_view_or_derived() &&
         f->table != nullptr && f->table->s->tmp_table == NO_TMP_TABLE;
}

std::string pad_equal_key(const std::string &e) {
  return "rtrim(" + e + ", ' ')";
}

// The hidden index column of an ENUM / SET column of this block's base
// tables (ENUM index, SET bitmask): what MySQL sorts on and uses as a
// number. In the grouped part of a block the column is grouped on its value,
// which has one index (ENUM / SET elements are distinct under the
// collation): min() picks it. Columns of derived tables and set operations
// carry no index: refused.
bool Generator::enum_index(Item *item, std::string *out) {
  Item *e = item->real_item();
  if (special_column(e) != SpecialColumn::OTHER ||
      down_cast<Item_field *>(e)->field->real_type() == MYSQL_TYPE_TIME2)
    return refuse("enum_index");
  const Field *f = down_cast<Item_field *>(e)->field;
  const auto info = m_info.find(f->table);
  if (info == m_info.end() || info->second.derived ||
      info->second.positional || info->second.scalar ||
      m_block->tables.count(f->table) == 0)
    return refuse("enum_order_outside_base_table");
  const std::string col = info->second.alias + "." +
                          quote_ident(std::string(kIndexPrefix) + f->field_name);
  *out = m_block->post_group && m_block->agg_depth == 0 ? "min(" + col + ")"
                                                        : col;
  return false;
}

// An ORDER BY expression or MIN/MAX argument: PAD SPACE strings use the
// padded sort key.
bool Generator::order_key(Item *item, std::string *out) {
  Item *e = item->real_item();
  // DATE_FORMAT with a fixed-width numeric format: its bytes order as its
  // collation does (exact_text()).
  if (is_string(e) && exact_text(e, true)) return expr(e, out);
  if (is_string(e))
    return string_key(e, e->collation.collation, true, e->max_length, out);
  return expr(e, out);
}

// The weight of a string under collation cs: the weight column of a column
// (the column's own collation must be cs), or the weight of a constant,
// computed now with MySQL's collation code. Other expressions are refused.
bool Generator::weight_expr(Item *item, const CHARSET_INFO *cs,
                            std::string *out) {
  item = item->real_item();
  if (item->type() == Item::CACHE_ITEM &&
      down_cast<Item_cache *>(item)->get_example() != nullptr)
    item = down_cast<Item_cache *>(item)->get_example()->real_item();
  if (is_constant(item)) {
    if (item->is_null()) {
      *out = "NULL";
      return false;
    }
    String buf;
    const String *v = item->val_str(&buf);
    if (v == nullptr) {
      *out = "NULL";
      return false;
    }
    String conv;
    uint errors = 0;
    if (conv.copy(v->ptr(), v->length(), v->charset(), cs, &errors) ||
        errors != 0)
      return refuse("weighted_constant_charset");
    *out = blob_literal(collation_weight(cs, conv.ptr(), conv.length()));
    return false;
  }
  if (item->type() != Item::FIELD_ITEM)
    return refuse(std::string("collation:expression (") + cs->m_coll_name +
                  ")");
  auto *f = down_cast<Item_field *>(item);
  if (f->field == nullptr) return refuse("weight_without_field");
  if (f->field->charset() != cs)
    return refuse(std::string("collation:") + f->field_name + " compared as " +
                  cs->m_coll_name);
  const Block *owner = m_block;
  while (owner != nullptr && owner->tables.count(f->field->table) == 0)
    owner = owner->parent;
  if (owner == nullptr) return refuse("outer_or_other_table_column");
  const TableInfo &ti = m_info[f->field->table];
  const std::string name =
      ti.positional ? "c" + std::to_string(f->field->field_index() -
                                           f->field->table->hidden_field_count +
                                           1)
                    : std::string(f->field->field_name);
  if (ti.weights.count(name) == 0)
    return refuse(std::string("collation:") + f->field_name + " (" +
                  cs->m_coll_name + ", copy without weights: reload it)");
  *out = ti.alias + "." + quote_ident(std::string(kWeightPrefix) + name);
  return false;
}

// The key a string compares by under collation cs: its bytes (binary
// collations), rtrim / padded key (PAD SPACE binary), or MySQL's weight;
// for ordering a PAD SPACE weight gets the space weight appended up to the
// longest value (max_bytes), so 'a' sorts as 'a   ' (after 'a\t').
bool Generator::string_key(Item *item, const CHARSET_INFO *cs, bool ordering,
                           size_t max_bytes, std::string *out) {
  switch (str_mode_cs(cs)) {
    case StrMode::RAW:
      return expr(item, out);
    case StrMode::PAD: {
      std::string e;
      if (expr(item, &e)) return true;
      *out = ordering                ? pad_sort_key(e, max_bytes)
             : is_char_column(item) ? e
                                    : pad_equal_key(e);
      return false;
    }
    case StrMode::WEIGHT: {
      // An expression (no weight column) under a binary PAD SPACE collation
      // of latin1 or ascii: equality is the bytes without trailing spaces,
      // and the order of the bytes is the order of the code points, which
      // is the order of the UTF-8 text in the lake (2026-11-22, TPC-H Q22).
      if (item->real_item()->type() != Item::FIELD_ITEM &&
          (cs->state & MY_CS_BINSORT) != 0 && cs->pad_attribute != NO_PAD &&
          (strcmp(cs->csname, "latin1") == 0 ||
           strcmp(cs->csname, "ascii") == 0)) {
        std::string e;
        if (expr(item, &e)) return true;
        *out = ordering ? pad_sort_key(e, max_bytes) : pad_equal_key(e);
        return false;
      }
      if (weight_expr(item, cs, out)) return true;
      if (ordering && cs->pad_attribute != NO_PAD) {
        const size_t chars = max_bytes / std::max<uint>(cs->mbmaxlen, 1) + 1;
        if (chars > 4096) return refuse("padded_order_too_long");
        std::string pad;
        const std::string space = space_weight(cs);
        for (size_t i = 0; i < chars; i++) pad += space;
        *out = "(" + *out + " || " + blob_literal(pad) + ")";
      }
      return false;
    }
    default:
      return refuse(std::string("collation:") + cs->m_coll_name);
  }
}

// A grouped string column (PAD or weight collation) outside an aggregate:
// the group holds values equal under the collation that may differ ('a',
// 'A '). MySQL shows the first row it met. On one table that is the row
// with the smallest primary key; when GROUP BY contains the column's
// table's primary key every row of the group has the same value; otherwise
// (a join) the first row depends on the join order: refused (decided
// 2026-11-14: exactly as InnoDB). Exception (2026-11-17): a CHAR column
// under a binary collation: MySQL reads CHAR without trailing spaces (the
// copy stores it so, from the load and from the binary log, where
// Field_string::pack strips them), so values equal under the PAD SPACE
// binary comparison are byte-identical: every row of a group shows the
// same value, whichever InnoDB meets first.
bool Generator::grouped_value(const Field *field, const TableInfo &ti,
                              const std::string &col, std::string *out) {
  if (field != nullptr && field->real_type() == MYSQL_TYPE_STRING &&
      (field->charset()->state & MY_CS_BINSORT) != 0) {
    *out = "any_value(" + col + ")";
    return false;
  }
  bool pk_grouped = !ti.pk_fields.empty();
  for (const Field *k : ti.pk_fields)
    pk_grouped = pk_grouped && m_block->grouped.count(k) > 0;
  if (pk_grouped) {
    *out = "any_value(" + col + ")";
    return false;
  }
  if (m_block->tables.size() == 1 && !ti.pk.empty()) {
    *out = "arg_min(" + col + ", " + ti.pk + ")";
    return false;
  }
  // A join: InnoDB shows the value of the first row of the group in its
  // join order, which we cannot reproduce. When the group holds a single
  // stored value (no 'done' next to 'done ' or 'Done'), there is no choice
  // and any row's value is InnoDB's. We check that per group, in the query
  // itself (same snapshot as the result): otherwise the marker error
  // stops it. Such statements run first, during optimization; the marker
  // becomes a refusal and InnoDB answers (2026-11-22, next step 2).
  m_grouped_check = true;
  *out = "CASE WHEN count(DISTINCT " + col + ") > 1 THEN error('" +
         kGroupedAmbiguous + "') ELSE any_value(" + col + ") END";
  return false;
}

}  // namespace offload
}  // namespace duckdb_se
