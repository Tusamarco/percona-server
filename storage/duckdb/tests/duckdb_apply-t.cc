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

/*
  Unit tests of the DuckDB applier row decoder and collapse rules (DWL#4 step
  4b). Row images are built byte by byte, as the binary log stores them.
  Expected date values were computed independently (Python datetime).
*/

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "decimal.h"
#include "field_types.h"
#include "my_temporal.h"  // Time_val
#include "my_time.h"
#include "my_sys.h"
#include "mysql/strings/m_ctype.h"
#include "storage/duckdb/duckdb_changes.h"
#include "storage/duckdb/duckdb_collation.h"
#include "storage/duckdb/duckdb_ddl.h"
#include "storage/duckdb/duckdb_row_decoder.h"

namespace duckdb_se {
namespace {

constexpr unsigned int kUtf8mb3 = 33;   // utf8mb3_general_ci
constexpr unsigned int kUtf8mb4 = 255;  // utf8mb4_0900_ai_ci
constexpr unsigned int kLatin1 = 8;     // latin1_swedish_ci
constexpr unsigned int kBinary = 63;

ColumnInfo column(const char *name, unsigned int type, unsigned int metadata,
                  bool is_unsigned = false, unsigned int charset = 0) {
  ColumnInfo c;
  c.name = name;
  c.type = type;
  c.metadata = metadata;
  c.is_unsigned = is_unsigned;
  c.charset = charset;
  return c;
}

// A one-column table and an image with that column present and not NULL.
struct OneColumn {
  TableInfo table;
  std::vector<uint8_t> present{0x01};
  std::vector<unsigned char> image{0x00};  // null bitmap: not NULL

  explicit OneColumn(const ColumnInfo &c) {
    table.columns.push_back(c);
    table.primary_key.push_back(0);
  }
  void add(const void *bytes, size_t n) {
    const auto *b = static_cast<const unsigned char *>(bytes);
    image.insert(image.end(), b, b + n);
  }
  Value decode() {
    const unsigned char *pos = image.data();
    std::vector<Value> row;
    std::string err;
    EXPECT_FALSE(decode_row(table, present, &pos, image.data() + image.size(),
                            &row, &err))
        << err;
    EXPECT_EQ(pos, image.data() + image.size());
    return row.empty() ? Value() : row[0];
  }
};

TEST(DuckdbRowDecoder, Integers) {
  {
    OneColumn t(column("a", MYSQL_TYPE_TINY, 0));
    const unsigned char b = 0xFB;  // -5
    t.add(&b, 1);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::INT);
    EXPECT_EQ(v.i, -5);
  }
  {
    OneColumn t(column("a", MYSQL_TYPE_SHORT, 0, true));
    const unsigned char b[] = {0xFF, 0xFF};
    t.add(b, 2);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::UINT);
    EXPECT_EQ(v.u, 65535u);
  }
  {
    OneColumn t(column("a", MYSQL_TYPE_INT24, 0));
    const unsigned char b[] = {0xFF, 0xFF, 0xFF};  // -1
    t.add(b, 3);
    EXPECT_EQ(t.decode().i, -1);
  }
  {
    OneColumn t(column("a", MYSQL_TYPE_LONG, 0));
    const unsigned char b[] = {0x00, 0x00, 0x00, 0x80};  // INT_MIN
    t.add(b, 4);
    EXPECT_EQ(t.decode().i, -2147483648LL);
  }
  {
    OneColumn t(column("a", MYSQL_TYPE_LONGLONG, 0, true));
    const unsigned char b[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    t.add(b, 8);
    EXPECT_EQ(t.decode().u, 18446744073709551615ULL);
  }
}

TEST(DuckdbRowDecoder, FloatDouble) {
  {
    OneColumn t(column("f", MYSQL_TYPE_FLOAT, 4));
    const float f = 1.5f;
    t.add(&f, 4);
    EXPECT_DOUBLE_EQ(t.decode().d, 1.5);
  }
  {
    OneColumn t(column("d", MYSQL_TYPE_DOUBLE, 8));
    const double d = 2.25;
    t.add(&d, 8);
    EXPECT_DOUBLE_EQ(t.decode().d, 2.25);
  }
}

TEST(DuckdbRowDecoder, Decimal) {
  const int precision = 12, scale = 3;
  decimal_digit_t digits[9];
  decimal_t dec;
  dec.len = 9;
  dec.buf = digits;
  const char *text = "-123.456";
  const char *end = text + strlen(text);
  ASSERT_EQ(string2decimal(text, &dec, &end), 0);
  std::vector<unsigned char> bin(decimal_bin_size(precision, scale));
  ASSERT_EQ(decimal2bin(&dec, bin.data(), precision, scale), 0);

  OneColumn t(column("d", MYSQL_TYPE_NEWDECIMAL, (precision << 8) | scale));
  t.add(bin.data(), bin.size());
  const Value v = t.decode();
  EXPECT_EQ(v.kind, Value::Kind::TEXT);
  EXPECT_EQ(v.s, "-123.456");
}

TEST(DuckdbRowDecoder, YearBitTime) {
  {  // YEAR: 0 is 0000, otherwise 1900 + byte
    OneColumn t(column("y", MYSQL_TYPE_YEAR, 0));
    const unsigned char b = 0;
    t.add(&b, 1);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::UINT);
    EXPECT_EQ(v.u, 0u);
  }
  {
    OneColumn t(column("y", MYSQL_TYPE_YEAR, 0));
    const unsigned char b = 255;
    t.add(&b, 1);
    EXPECT_EQ(t.decode().u, 2155u);
  }
  {  // BIT(10): metadata bits % 8 = 2, bytes = 1; value big-endian, 2 bytes
    OneColumn t(column("b", MYSQL_TYPE_BIT, (1 << 8) | 2));
    const unsigned char b[] = {0x03, 0xFF};
    t.add(b, 2);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::UINT);
    EXPECT_EQ(v.u, 1023u);
  }
  {  // BIT(64)
    OneColumn t(column("b", MYSQL_TYPE_BIT, 8 << 8));
    const unsigned char b[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
    t.add(b, 8);
    EXPECT_EQ(t.decode().u, 18446744073709551614ULL);
  }
  {  // TIME(3) negative, more than 24 hours: signed microseconds
    Time_val tv;
    ASSERT_FALSE(Time_val::make_time(true, 837, 59, 59, 500000, &tv));
    unsigned char b[5];
    tv.store_time(b, 3);
    OneColumn t(column("t", MYSQL_TYPE_TIME2, 3));
    t.add(b, 5);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::INT);
    EXPECT_EQ(v.i, -(((837LL * 60 + 59) * 60 + 59) * 1000000 + 500000));
  }
  {  // TIME(0) zero
    Time_val tv;
    ASSERT_FALSE(Time_val::make_time(false, 0, 0, 0, 0, &tv));
    unsigned char b[3];
    tv.store_time(b, 0);
    OneColumn t(column("t", MYSQL_TYPE_TIME2, 0));
    t.add(b, 3);
    EXPECT_EQ(t.decode().i, 0);
  }
}

TEST(DuckdbRowDecoder, EnumSet) {
  // ENUM('small','medium','large'): metadata real type ENUM, pack length 1
  ColumnInfo e = column("e", MYSQL_TYPE_STRING, (MYSQL_TYPE_ENUM << 8) | 1,
                        false, kUtf8mb4);
  e.enum_set = MYSQL_TYPE_ENUM;
  e.elements = {"small", "medium", "large"};
  {
    OneColumn t(e);
    const unsigned char b = 2;
    t.add(&b, 1);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::TEXT);
    EXPECT_EQ(v.s, "medium");
    EXPECT_TRUE(v.indexed);
    EXPECT_EQ(v.u, 2u);
    EXPECT_TRUE(v.weighted);  // utf8mb4_0900_ai_ci
  }
  {  // index 0: the invalid value ''
    OneColumn t(e);
    const unsigned char b = 0;
    t.add(&b, 1);
    const Value v = t.decode();
    EXPECT_EQ(v.s, "");
    EXPECT_EQ(v.u, 0u);
  }
  {  // index past the elements: an error
    OneColumn t(e);
    const unsigned char b = 4;
    t.add(&b, 1);
    const unsigned char *pos = t.image.data();
    std::vector<Value> row;
    std::string err;
    EXPECT_TRUE(decode_row(t.table, t.present, &pos,
                           t.image.data() + t.image.size(), &row, &err));
  }
  // SET('red','green','blue'), pack length 1: bits in element order
  ColumnInfo s = column("s", MYSQL_TYPE_STRING, (MYSQL_TYPE_SET << 8) | 1,
                        false, kUtf8mb4);
  s.enum_set = MYSQL_TYPE_SET;
  s.elements = {"red", "green", "blue"};
  {
    OneColumn t(s);
    const unsigned char b = 5;  // red, blue
    t.add(&b, 1);
    const Value v = t.decode();
    EXPECT_EQ(v.s, "red,blue");
    EXPECT_EQ(v.u, 5u);
  }
  {
    OneColumn t(s);
    const unsigned char b = 0;  // empty set
    t.add(&b, 1);
    EXPECT_EQ(t.decode().s, "");
  }
}

TEST(DuckdbRowDecoder, Strings) {
  {  // VARCHAR with a 1-byte length prefix
    OneColumn t(column("v", MYSQL_TYPE_VARCHAR, 150, false, kUtf8mb3));
    const unsigned char b[] = {3, 'a', 'b', 'c'};
    t.add(b, sizeof(b));
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::TEXT);
    EXPECT_EQ(v.s, "abc");
  }
  {  // VARCHAR with a 2-byte length prefix
    OneColumn t(column("v", MYSQL_TYPE_VARCHAR, 300, false, kUtf8mb4));
    const unsigned char b[] = {3, 0, 'x', 'y', 'z'};
    t.add(b, sizeof(b));
    EXPECT_EQ(t.decode().s, "xyz");
  }
  {  // CHAR(36) utf8mb3: real type 254, 108 bytes
    OneColumn t(
        column("c", MYSQL_TYPE_STRING, (0xFE << 8) | 108, false, kUtf8mb3));
    const unsigned char b[] = {4, 'u', 'u', 'i', 'd'};
    t.add(b, sizeof(b));
    EXPECT_EQ(t.decode().s, "uuid");
  }
  {  // CHAR with 600 bytes: the length borrows two bits of the type byte
    OneColumn t(
        column("c", MYSQL_TYPE_STRING, (0xDE << 8) | 88, false, kUtf8mb4));
    const unsigned char b[] = {2, 0, 'o', 'k'};
    t.add(b, sizeof(b));
    EXPECT_EQ(t.decode().s, "ok");
  }
  {  // TEXT with a 2-byte length prefix
    OneColumn t(column("t", MYSQL_TYPE_BLOB, 2, false, kUtf8mb4));
    const unsigned char b[] = {4, 0, 't', 'e', 'x', 't'};
    t.add(b, sizeof(b));
    EXPECT_EQ(t.decode().s, "text");
  }
  {  // VARBINARY keeps its bytes
    OneColumn t(column("b", MYSQL_TYPE_VARCHAR, 10, false, kBinary));
    const unsigned char b[] = {2, 0x00, 0xFF};
    t.add(b, sizeof(b));
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::BLOB);
    EXPECT_EQ(v.s, std::string("\x00\xFF", 2));
  }
  {  // latin1 is converted to UTF-8
    OneColumn t(column("l", MYSQL_TYPE_VARCHAR, 20, false, kLatin1));
    const unsigned char b[] = {2, 0xE0, 0xE9};  // "àé"
    t.add(b, sizeof(b));
    EXPECT_EQ(t.decode().s, "\xC3\xA0\xC3\xA9");
  }
}

TEST(DuckdbRowDecoder, Temporal) {
  // DATE 2024-02-29. The binlog writes DATE columns as MYSQL_TYPE_DATE; we
  // also accept MYSQL_TYPE_NEWDATE, the same 3-byte value.
  for (unsigned int type : {static_cast<unsigned int>(MYSQL_TYPE_DATE),
                            static_cast<unsigned int>(MYSQL_TYPE_NEWDATE)}) {
    OneColumn t(column("d", type, 0));
    const uint32_t packed = 29 | (2 << 5) | (2024 << 9);  // 1036381
    const unsigned char b[] = {static_cast<unsigned char>(packed & 0xFF),
                               static_cast<unsigned char>((packed >> 8) & 0xFF),
                               static_cast<unsigned char>(packed >> 16)};
    t.add(b, 3);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::DATE) << "type " << type;
    EXPECT_EQ(v.i, 19782) << "type " << type;
  }
  {  // DATETIME(6) 2024-02-29 12:34:56.123456
    MYSQL_TIME mt{};
    mt.year = 2024;
    mt.month = 2;
    mt.day = 29;
    mt.hour = 12;
    mt.minute = 34;
    mt.second = 56;
    mt.second_part = 123456;
    mt.time_type = MYSQL_TIMESTAMP_DATETIME;
    unsigned char b[8];
    my_datetime_packed_to_binary(TIME_to_longlong_datetime_packed(mt), b, 6);
    OneColumn t(column("dt", MYSQL_TYPE_DATETIME2, 6));
    t.add(b, 5 + 3);
    const Value v = t.decode();
    EXPECT_EQ(v.kind, Value::Kind::TIMESTAMP);
    EXPECT_EQ(v.i, 1709210096123456LL);
  }
  {  // TIMESTAMP(3)
    my_timeval tv{1709210096, 789000};
    unsigned char b[8];
    my_timestamp_to_binary(&tv, b, 3);
    OneColumn t(column("ts", MYSQL_TYPE_TIMESTAMP2, 3));
    t.add(b, 4 + 2);
    EXPECT_EQ(t.decode().i, 1709210096789000LL);
  }
}

TEST(DuckdbRowDecoder, NullsAndPartialImages) {
  TableInfo table;
  table.columns.push_back(column("a", MYSQL_TYPE_LONG, 0));
  table.columns.push_back(column("b", MYSQL_TYPE_LONG, 0));
  table.primary_key.push_back(0);
  std::string err;
  std::vector<Value> row;
  {  // both present, a is NULL
    const std::vector<uint8_t> present{0x03};
    const unsigned char image[] = {0x01, 7, 0, 0, 0};
    const unsigned char *pos = image;
    ASSERT_FALSE(
        decode_row(table, present, &pos, image + sizeof(image), &row, &err));
    EXPECT_EQ(row[0].kind, Value::Kind::NUL);
    EXPECT_EQ(row[1].i, 7);
  }
  {  // only b present
    const std::vector<uint8_t> present{0x02};
    const unsigned char image[] = {0x00, 9, 0, 0, 0};
    const unsigned char *pos = image;
    ASSERT_FALSE(
        decode_row(table, present, &pos, image + sizeof(image), &row, &err));
    EXPECT_EQ(row[0].kind, Value::Kind::NUL);
    EXPECT_EQ(row[1].i, 9);
  }
}

TEST(DuckdbRowDecoder, TruncatedImageIsAnError) {
  OneColumn t(column("a", MYSQL_TYPE_LONG, 0));
  const unsigned char b[] = {1, 2};  // 2 of 4 bytes
  t.add(b, 2);
  const unsigned char *pos = t.image.data();
  std::vector<Value> row;
  std::string err;
  EXPECT_TRUE(decode_row(t.table, t.present, &pos,
                         t.image.data() + t.image.size(), &row, &err));
  EXPECT_NE(err.find("truncated"), std::string::npos);
}

TEST(DuckdbRowDecoder, DaysFromCivil) {
  EXPECT_EQ(days_from_civil(1970, 1, 1), 0);
  EXPECT_EQ(days_from_civil(1969, 12, 31), -1);
  EXPECT_EQ(days_from_civil(2000, 3, 1), 11017);
  EXPECT_EQ(days_from_civil(2024, 2, 29), 19782);
  EXPECT_EQ(days_from_civil(1900, 1, 1), -25567);
}

// Collapse rules (DWL#4 and DWL#5). Rows are (id, value); the key is id.
Row row(int64_t id, int64_t value) {
  Value a, b;
  a.kind = b.kind = Value::Kind::INT;
  a.i = id;
  b.i = value;
  return {a, b};
}

const KeyChange &only(const TableChanges &t) {
  EXPECT_EQ(t.changes().size(), 1u);
  return t.changes().begin()->second;
}

TEST(DuckdbChanges, Insert) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.insert(row(1, 10), &err));
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::KEEP);
  EXPECT_TRUE(c.has_live_row);
  EXPECT_EQ(c.live_row, row(1, 10));
}

TEST(DuckdbChanges, InsertThenUpdate) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.insert(row(1, 10), &err));
  ASSERT_FALSE(t.update(row(1, 10), row(1, 11), &err));
  const KeyChange &c = only(t);
  EXPECT_TRUE(c.has_live_row);
  EXPECT_EQ(c.live_row, row(1, 11));
  EXPECT_EQ(t.input_rows(), 2u);
}

TEST(DuckdbChanges, InsertThenDelete) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.insert(row(1, 10), &err));
  t.remove(row(1, 10), false, 0);
  const KeyChange &c = only(t);
  EXPECT_FALSE(c.has_live_row);
  EXPECT_TRUE(c.archived_rows.empty());
}

TEST(DuckdbChanges, Updates) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.update(row(1, 10), row(1, 11), &err));
  ASSERT_FALSE(t.update(row(1, 11), row(1, 12), &err));
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::DELETE);
  EXPECT_EQ(c.live_row, row(1, 12));
}

TEST(DuckdbChanges, UpdateThenDelete) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.update(row(1, 10), row(1, 11), &err));
  t.remove(row(1, 11), false, 0);
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::DELETE);
  EXPECT_FALSE(c.has_live_row);
}

TEST(DuckdbChanges, DeleteThenInsert) {
  TableChanges t({0}, false);
  std::string err;
  t.remove(row(1, 10), false, 0);
  ASSERT_FALSE(t.insert(row(1, 20), &err));
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::DELETE);
  EXPECT_EQ(c.live_row, row(1, 20));
}

TEST(DuckdbChanges, KeyChangeByUpdate) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.update(row(1, 10), row(2, 10), &err));
  EXPECT_EQ(t.changes().size(), 2u);
  EXPECT_EQ(t.changes().at("(1)").original, KeyChange::Original::DELETE);
  EXPECT_TRUE(t.changes().at("(2)").has_live_row);
}

TEST(DuckdbChanges, PurgeOnHistoryTableArchives) {
  TableChanges t({0}, true);
  t.remove(row(1, 10), true, 1000);
  EXPECT_EQ(only(t).original, KeyChange::Original::ARCHIVE);
  EXPECT_EQ(only(t).original_archived_at, 1000u);
  EXPECT_EQ(only(t).key_row, row(1, 10));
}

TEST(DuckdbChanges, PurgeOnClassicTableDeletes) {
  TableChanges t({0}, false);
  t.remove(row(1, 10), true, 1000);
  EXPECT_EQ(only(t).original, KeyChange::Original::DELETE);
}

TEST(DuckdbChanges, InsertThenPurgeOnHistoryTable) {
  TableChanges t({0}, true);
  std::string err;
  ASSERT_FALSE(t.insert(row(1, 10), &err));
  t.remove(row(1, 10), true, 1000);
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::KEEP);
  ASSERT_EQ(c.archived_rows.size(), 1u);
  EXPECT_EQ(c.archived_rows[0].row, row(1, 10));
  EXPECT_EQ(c.archived_rows[0].archived_at, 1000u);
  EXPECT_FALSE(c.has_live_row);
}

TEST(DuckdbChanges, UpdateThenPurgeOnHistoryTable) {
  TableChanges t({0}, true);
  std::string err;
  ASSERT_FALSE(t.update(row(1, 10), row(1, 11), &err));
  t.remove(row(1, 11), true, 1000);
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::DELETE);
  ASSERT_EQ(c.archived_rows.size(), 1u);
  EXPECT_EQ(c.archived_rows[0].row, row(1, 11));
}

TEST(DuckdbChanges, PurgeThenInsertSameKey) {
  TableChanges t({0}, true);
  std::string err;
  t.remove(row(1, 10), true, 1000);
  ASSERT_FALSE(t.insert(row(1, 20), &err));
  const KeyChange &c = only(t);
  EXPECT_EQ(c.original, KeyChange::Original::ARCHIVE);
  EXPECT_EQ(c.live_row, row(1, 20));
}

TEST(DuckdbChanges, DuplicateInsertIsAnError) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.insert(row(1, 10), &err));
  EXPECT_TRUE(t.insert(row(1, 11), &err));
}

TEST(DuckdbChanges, NetChanges) {
  TableChanges t({0}, false);
  std::string err;
  ASSERT_FALSE(t.insert(row(1, 10), &err));
  ASSERT_FALSE(t.insert(row(2, 10), &err));
  t.remove(row(2, 10), false, 0);  // key 2: inserted then deleted: no effect
  EXPECT_EQ(t.input_rows(), 3u);
  EXPECT_EQ(t.net_changes(), 1u);
}

// Merging transactions into a batch (DWL#4 step 4c-1): the result is what
// applying both transactions in order does.
TEST(DuckdbChangesMerge, InsertThenUpdate) {
  TableChanges batch({0}, false), t2({0}, false);
  std::string err;
  ASSERT_FALSE(batch.insert(row(1, 10), &err));
  ASSERT_FALSE(t2.update(row(1, 10), row(1, 11), &err));
  ASSERT_FALSE(batch.merge(t2, &err)) << err;
  const KeyChange &c = only(batch);
  EXPECT_EQ(c.original, KeyChange::Original::KEEP);
  EXPECT_TRUE(c.has_live_row);
  EXPECT_EQ(c.live_row, row(1, 11));
  EXPECT_EQ(batch.input_rows(), 2u);
}

TEST(DuckdbChangesMerge, PurgeThenInsertOnHistoryTable) {
  TableChanges batch({0}, true), t2({0}, true);
  std::string err;
  batch.remove(row(1, 10), true, 1000);
  ASSERT_FALSE(t2.insert(row(1, 20), &err));
  ASSERT_FALSE(batch.merge(t2, &err)) << err;
  const KeyChange &c = only(batch);
  EXPECT_EQ(c.original, KeyChange::Original::ARCHIVE);
  EXPECT_EQ(c.original_archived_at, 1000u);
  EXPECT_EQ(c.live_row, row(1, 20));
}

TEST(DuckdbChangesMerge, InsertThenPurgeOnHistoryTable) {
  TableChanges batch({0}, true), t2({0}, true);
  std::string err;
  ASSERT_FALSE(batch.insert(row(1, 10), &err));
  t2.remove(row(1, 10), true, 2000);
  ASSERT_FALSE(batch.merge(t2, &err)) << err;
  const KeyChange &c = only(batch);
  EXPECT_EQ(c.original, KeyChange::Original::KEEP);
  EXPECT_FALSE(c.has_live_row);
  ASSERT_EQ(c.archived_rows.size(), 1u);
  EXPECT_EQ(c.archived_rows[0].row, row(1, 10));
  EXPECT_EQ(c.archived_rows[0].archived_at, 2000u);
}

TEST(DuckdbChangesMerge, PurgeOfOriginalInLaterTransaction) {
  TableChanges batch({0}, true), t2({0}, true);
  std::string err;
  ASSERT_FALSE(batch.insert(row(2, 20), &err));  // another key
  t2.remove(row(1, 10), true, 3000);
  ASSERT_FALSE(batch.merge(t2, &err)) << err;
  const KeyChange &c = batch.changes().at("(1)");
  EXPECT_EQ(c.original, KeyChange::Original::ARCHIVE);
  EXPECT_EQ(c.original_archived_at, 3000u);
  EXPECT_EQ(c.key_row, row(1, 10));
}

TEST(DuckdbChangesMerge, DeleteThenInsert) {
  TableChanges batch({0}, false), t2({0}, false);
  std::string err;
  batch.remove(row(1, 10), false, 0);
  ASSERT_FALSE(t2.insert(row(1, 30), &err));
  ASSERT_FALSE(batch.merge(t2, &err)) << err;
  const KeyChange &c = only(batch);
  EXPECT_EQ(c.original, KeyChange::Original::DELETE);
  EXPECT_EQ(c.live_row, row(1, 30));
}

TEST(DuckdbChangesMerge, UpdateThenUpdateKeepsOneDelete) {
  TableChanges batch({0}, false), t2({0}, false);
  std::string err;
  ASSERT_FALSE(batch.update(row(1, 10), row(1, 11), &err));
  ASSERT_FALSE(t2.update(row(1, 11), row(1, 12), &err));
  ASSERT_FALSE(batch.merge(t2, &err)) << err;
  const KeyChange &c = only(batch);
  EXPECT_EQ(c.original, KeyChange::Original::DELETE);
  EXPECT_EQ(c.live_row, row(1, 12));
}

TEST(DuckdbChangesMerge, DuplicateInsertIsAnError) {
  TableChanges batch({0}, false), t2({0}, false);
  std::string err;
  ASSERT_FALSE(batch.insert(row(1, 10), &err));
  ASSERT_FALSE(t2.insert(row(1, 11), &err));
  EXPECT_TRUE(batch.merge(t2, &err));
  EXPECT_NE(err.find("duplicate"), std::string::npos);
}

// ---- DDL statements from the binary log (DWL#4 step 4d) -------------------

std::string refreshed(const DdlStatement &d) {
  std::string out;
  for (const TableName &t : d.refresh)
    out += (out.empty() ? "" : ",") + t.db + "." + t.table;
  return out;
}

std::string renamed(const DdlStatement &d) {
  std::string out;
  for (const auto &[from, to] : d.renames)
    out += (out.empty() ? "" : ",") + from.db + "." + from.table + ">" +
           to.db + "." + to.table;
  return out;
}

TEST(DuckdbDdl, AlterChangesRefresh) {
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE mill1 MODIFY v BIGINT", "w",
                                false)),
            "w.mill1");
  EXPECT_EQ(refreshed(parse_ddl("alter table `w`.`mill1` add column c int "
                                "default 7, algorithm=inplace",
                                "other", false)),
            "w.mill1");
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE t DROP COLUMN c", "w", false)),
            "w.t");
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE t CONVERT TO CHARACTER SET "
                                "utf8mb4",
                                "w", false)),
            "w.t");
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE t DROP PRIMARY KEY, ADD PRIMARY "
                                "KEY (a, b)",
                                "w", false)),
            "w.t");
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE t TRUNCATE PARTITION p1", "w",
                                false)),
            "w.t");
  // Unknown clause: refresh (a needless refresh is safe).
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE t SOMETHING_NEW 1", "w", false)),
            "w.t");
}

TEST(DuckdbDdl, AlterWithoutChangeToTheCopy) {
  for (const char *q :
       {"ALTER TABLE t ADD INDEX i (a)", "ALTER TABLE t ADD UNIQUE KEY u (a)",
        "ALTER TABLE t DROP INDEX i, ADD KEY j (b)",
        "ALTER TABLE t RENAME INDEX i TO j", "ALTER TABLE t ALTER INDEX i "
        "INVISIBLE",
        "ALTER TABLE t COMMENT = 'x', AUTO_INCREMENT = 10",
        "ALTER TABLE t ALTER COLUMN c SET DEFAULT 5",
        "ALTER TABLE t DEFAULT CHARSET = utf8mb4", "ALTER TABLE t ENGINE_X",
        "ALTER TABLE t SECONDARY_LOAD", "ALTER TABLE t SECONDARY_UNLOAD",
        "ALTER TABLE t SECONDARY_ENGINE = NULL",
        "ALTER TABLE t ADD CONSTRAINT c1 CHECK (a > 0)",
        "ALTER TABLE t ADD CONSTRAINT u1 UNIQUE (a)"}) {
    const DdlStatement d = parse_ddl(q, "w", false);
    if (std::string(q) == "ALTER TABLE t ENGINE_X") {
      EXPECT_FALSE(d.empty()) << q;  // unknown: refresh
      continue;
    }
    EXPECT_TRUE(d.empty()) << q;
  }
}

TEST(DuckdbDdl, ColumnRenames) {
  DdlStatement d = parse_ddl(
      "ALTER TABLE t RENAME COLUMN note TO remark, CHANGE COLUMN qty quantity "
      "BIGINT, CHANGE v v INT",
      "w", false);
  EXPECT_EQ(refreshed(d), "w.t");
  ASSERT_EQ(d.column_renames.size(), 2u);
  EXPECT_EQ(d.column_renames[0].first, "note");
  EXPECT_EQ(d.column_renames[0].second, "remark");
  EXPECT_EQ(d.column_renames[1].first, "qty");
  EXPECT_EQ(d.column_renames[1].second, "quantity");
}

TEST(DuckdbDdl, TableRenames) {
  EXPECT_EQ(renamed(parse_ddl("RENAME TABLE a TO b, x.c TO y.d", "w", false)),
            "w.a>w.b,x.c>y.d");
  // ALTER ... RENAME with a change: rename, then refresh the new name.
  DdlStatement d =
      parse_ddl("ALTER TABLE a RENAME TO b, ADD COLUMN z INT", "w", false);
  EXPECT_EQ(renamed(d), "w.a>w.b");
  EXPECT_EQ(refreshed(d), "w.b");
  // ALTER ... RENAME alone: rename only.
  d = parse_ddl("ALTER TABLE a RENAME AS `x`.`b`", "w", false);
  EXPECT_EQ(renamed(d), "w.a>x.b");
  EXPECT_EQ(refreshed(d), "");
}

TEST(DuckdbDdl, TruncateAndIndexStatements) {
  EXPECT_EQ(refreshed(parse_ddl("TRUNCATE TABLE t", "w", false)), "w.t");
  EXPECT_EQ(refreshed(parse_ddl("truncate x.t", "w", false)), "x.t");
  EXPECT_TRUE(parse_ddl("CREATE INDEX i ON t (a)", "w", false).empty());
  EXPECT_TRUE(parse_ddl("DROP INDEX i ON t", "w", false).empty());
  EXPECT_EQ(refreshed(parse_ddl("DROP INDEX `PRIMARY` ON t", "w", false)),
            "w.t");
}

TEST(DuckdbDdl, IgnoredStatements) {
  for (const char *q :
       {"BEGIN", "COMMIT", "INSERT INTO t VALUES (1)",
        "CREATE TABLE t (a INT PRIMARY KEY)",
        "DROP TABLE `t` /* generated by server */", "OPTIMIZE TABLE t",
        "ALTER DATABASE w CHARACTER SET utf8mb4", "ALTER VIEW v AS SELECT 1",
        "ALTER TABLE t", ""})
    EXPECT_TRUE(parse_ddl(q, "w", false).empty()) << q;
  // No default database and no qualifier: the table cannot be named.
  EXPECT_TRUE(parse_ddl("TRUNCATE TABLE t", "", false).empty());
}

TEST(DuckdbDdl, CommentsQuotesAndCase) {
  EXPECT_EQ(refreshed(parse_ddl("/* hi */ ALTER /*!80000 TABLE */ `my``t` "
                                "-- c\n MODIFY a INT # end",
                                "w", false)),
            "w.my`t");
  // A comma inside parentheses or a string does not split clauses.
  EXPECT_TRUE(parse_ddl("ALTER TABLE t ADD INDEX i (a, b), COMMENT 'a,b'", "w",
                        false)
                  .empty());
  EXPECT_EQ(refreshed(parse_ddl("ALTER TABLE W.MILL1 MODIFY a INT", "x",
                                true)),
            "w.mill1");
}

TEST(DuckdbDdl, ComposeRenames) {
  // a->b, then b->c and x->y: a->c, x->y
  ColumnRenames r = compose_renames({{"a", "b"}}, {{"b", "c"}, {"x", "y"}});
  ASSERT_EQ(r.size(), 2u);
  EXPECT_EQ(r[0], (std::pair<std::string, std::string>("a", "c")));
  EXPECT_EQ(r[1], (std::pair<std::string, std::string>("x", "y")));
  // a->b then b->a: back to the original name, nothing left
  EXPECT_TRUE(compose_renames({{"a", "b"}}, {{"b", "a"}}).empty());
  EXPECT_EQ(compose_renames({}, {{"p", "q"}}).size(), 1u);
}

TEST(DuckdbDdl, EncodeDecodeRenames) {
  const ColumnRenames r = {{"note", "remark"}, {"we ird", "na`me"}};
  EXPECT_EQ(decode_renames(encode_renames(r)), r);
  EXPECT_TRUE(decode_renames("").empty());
  EXPECT_TRUE(encode_renames({}).empty());
}


// DWL#6 step A1: weight strings follow the MySQL collation.
TEST(DuckdbCollation, Weights) {
  const CHARSET_INFO *ai = get_charset_by_name("utf8mb4_0900_ai_ci", MYF(0));
  const CHARSET_INFO *gen = get_charset_by_name("utf8mb4_general_ci", MYF(0));
  const CHARSET_INFO *bin = get_charset_by_name("utf8mb4_bin", MYF(0));
  const CHARSET_INFO *l1 = get_charset_by_name("latin1_swedish_ci", MYF(0));
  const CHARSET_INFO *l1bin = get_charset_by_name("latin1_bin", MYF(0));
  ASSERT_NE(ai, nullptr);
  ASSERT_NE(gen, nullptr);
  ASSERT_NE(bin, nullptr);
  ASSERT_NE(l1, nullptr);
  ASSERT_NE(l1bin, nullptr);
  auto w = [](const CHARSET_INFO *cs, const std::string &s) {
    return collation_weight(cs, s.data(), s.size());
  };
  // Which collations need weights.
  EXPECT_TRUE(needs_weight(ai));
  EXPECT_TRUE(needs_weight(gen));
  EXPECT_TRUE(needs_weight(l1));
  EXPECT_TRUE(needs_weight(l1bin));  // latin1 bytes do not sort as UTF-8
  EXPECT_FALSE(needs_weight(bin));
  EXPECT_FALSE(needs_weight(&my_charset_bin));
  // Accent and case insensitive, NO PAD.
  EXPECT_EQ(w(ai, "a"), w(ai, "A"));
  EXPECT_EQ(w(ai, "a"), w(ai, "\xC3\xA1"));  // a with acute accent
  EXPECT_LT(w(ai, "a"), w(ai, "B"));
  EXPECT_LT(w(ai, "Z"), w(ai, "zz"));
  EXPECT_NE(w(ai, "a"), w(ai, "a "));
  EXPECT_EQ(w(ai, ""), std::string());
  // PAD SPACE: trailing spaces do not count.
  EXPECT_EQ(w(gen, "a"), w(gen, "a  "));
  EXPECT_EQ(w(gen, "abc"), w(gen, "ABC"));
  EXPECT_LT(w(gen, "abc"), w(gen, "abd"));
  // A UTF-8 value read back from DuckDB weighs as the original bytes.
  std::string from_utf8;
  ASSERT_FALSE(collation_weight_utf8(l1, "\xC3\xA9t\xC3\xA9", &from_utf8));
  EXPECT_EQ(from_utf8, w(l1, "\xE9t\xE9"));  // latin1 bytes
  EXPECT_EQ(w(l1, "ETE"), w(l1, "ete"));
}

}  // namespace
}  // namespace duckdb_se
