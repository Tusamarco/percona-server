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
  Decoder of binlog row images for the DuckDB applier (DWL#4 step 4b).

  It needs no THD and opens no table: it uses the column types and metadata of
  the Table_map event and the server's helpers for packed values (DECIMAL,
  DATETIME2, TIMESTAMP2) and character set conversion. It links with mysys and
  strings only, so the unit tests can use it without the server.
*/

#include "storage/duckdb/duckdb_row_decoder.h"

#include <bit>
#include <cstring>
#include <utility>

#include "decimal.h"
#include "field_types.h"  // MYSQL_TYPE_*
#include "my_byteorder.h"
#include "my_sys.h"  // get_charset, get_charset_by_csname
#include "storage/duckdb/duckdb_collation.h"
#include "my_time.h"
#include "mysql/strings/m_ctype.h"

namespace duckdb_se {

namespace {

static_assert(std::endian::native == std::endian::little,
              "the binlog stores FLOAT/DOUBLE little-endian; big-endian hosts "
              "are not supported by this POC");

constexpr unsigned int kBinaryCharset = 63;  // my_charset_bin
constexpr int kDecimalBuffLength = 9;        // as DECIMAL_BUFF_LENGTH
constexpr int kDecimalTextLength = 128;      // 65 digits, sign and point

bool fail(std::string *err, const std::string &msg) {
  *err = msg;
  return true;
}

bool need(const unsigned char *pos, const unsigned char *end, size_t n,
          std::string *err) {
  if (static_cast<size_t>(end - pos) >= n) return false;
  return fail(err, "row image truncated");
}

// Length prefix of `bytes` bytes, little-endian.
uint32_t read_length(const unsigned char *p, unsigned int bytes) {
  switch (bytes) {
    case 1:
      return p[0];
    case 2:
      return uint2korr(p);
    case 3:
      return uint3korr(p);
    default:
      return uint4korr(p);
  }
}

// CHAR metadata keeps the real type in the high byte and the maximum length in
// the low byte; lengths above 255 borrow two bits of the type byte.
void decode_string_metadata(unsigned int metadata, unsigned int *real_type,
                            unsigned int *max_length) {
  const unsigned int byte0 = metadata >> 8;
  const unsigned int byte1 = metadata & 0xFF;
  if ((byte0 & 0x30) != 0x30) {
    *max_length = byte1 | (((byte0 & 0x30) ^ 0x30) << 4);
    *real_type = byte0 | 0x30;
  } else {
    *max_length = byte1;
    *real_type = byte0;
  }
}

// Text bytes in the column charset become UTF-8; binary stays BLOB.
bool string_value(const ColumnInfo &c, const unsigned char *p, size_t len,
                  Value *v, std::string *err) {
  if (c.charset == kBinaryCharset) {
    v->kind = Value::Kind::BLOB;
    v->s.assign(reinterpret_cast<const char *>(p), len);
    return false;
  }
  const CHARSET_INFO *cs = get_charset(c.charset, MYF(0));
  if (cs == nullptr)
    return fail(err, "column " + c.name + ": unknown character set id " +
                         std::to_string(c.charset));
  v->kind = Value::Kind::TEXT;
  if (needs_weight(cs)) {
    v->w = collation_weight(cs, reinterpret_cast<const char *>(p), len);
    v->weighted = true;
  }
  if (strncmp(cs->csname, "utf8mb", 6) == 0 ||
      strcmp(cs->csname, "ascii") == 0) {
    v->s.assign(reinterpret_cast<const char *>(p), len);
    return false;
  }
  const CHARSET_INFO *utf8 =
      get_charset_by_csname("utf8mb4", MY_CS_PRIMARY, MYF(0));
  if (utf8 == nullptr) return fail(err, "utf8mb4 character set not found");
  std::string out(len * 4 + 1, '\0');
  uint errors = 0;
  const size_t n =
      my_convert(out.data(), out.size(), utf8,
                 reinterpret_cast<const char *>(p), len, cs, &errors);
  if (errors != 0)
    return fail(err,
                "column " + c.name + ": value cannot be converted to UTF-8");
  out.resize(n);
  v->s = std::move(out);
  return false;
}

bool decode_value(const ColumnInfo &c, const unsigned char **pos,
                  const unsigned char *end, Value *v, std::string *err) {
  const unsigned char *p = *pos;
  size_t size = 0;
  switch (c.type) {
    case MYSQL_TYPE_TINY:
      if (need(p, end, size = 1, err)) return true;
      if (c.is_unsigned) {
        v->kind = Value::Kind::UINT;
        v->u = p[0];
      } else {
        v->kind = Value::Kind::INT;
        v->i = static_cast<int8_t>(p[0]);
      }
      break;
    case MYSQL_TYPE_SHORT:
      if (need(p, end, size = 2, err)) return true;
      if (c.is_unsigned) {
        v->kind = Value::Kind::UINT;
        v->u = uint2korr(p);
      } else {
        v->kind = Value::Kind::INT;
        v->i = sint2korr(p);
      }
      break;
    case MYSQL_TYPE_INT24:
      if (need(p, end, size = 3, err)) return true;
      if (c.is_unsigned) {
        v->kind = Value::Kind::UINT;
        v->u = uint3korr(p);
      } else {
        v->kind = Value::Kind::INT;
        v->i = sint3korr(p);
      }
      break;
    case MYSQL_TYPE_LONG:
      if (need(p, end, size = 4, err)) return true;
      if (c.is_unsigned) {
        v->kind = Value::Kind::UINT;
        v->u = uint4korr(p);
      } else {
        v->kind = Value::Kind::INT;
        v->i = sint4korr(p);
      }
      break;
    case MYSQL_TYPE_LONGLONG:
      if (need(p, end, size = 8, err)) return true;
      if (c.is_unsigned) {
        v->kind = Value::Kind::UINT;
        v->u = uint8korr(p);
      } else {
        v->kind = Value::Kind::INT;
        v->i = sint8korr(p);
      }
      break;
    case MYSQL_TYPE_FLOAT: {
      if (need(p, end, size = 4, err)) return true;
      float f;
      memcpy(&f, p, 4);
      v->kind = Value::Kind::DOUBLE;
      v->d = f;
      break;
    }
    case MYSQL_TYPE_DOUBLE:
      if (need(p, end, size = 8, err)) return true;
      v->kind = Value::Kind::DOUBLE;
      memcpy(&v->d, p, 8);
      break;
    case MYSQL_TYPE_NEWDECIMAL: {
      const int precision = static_cast<int>(c.metadata >> 8);
      const int scale = static_cast<int>(c.metadata & 0xFF);
      size = static_cast<size_t>(decimal_bin_size(precision, scale));
      if (need(p, end, size, err)) return true;
      decimal_digit_t digits[kDecimalBuffLength];
      decimal_t dec;
      dec.len = kDecimalBuffLength;
      dec.buf = digits;
      char text[kDecimalTextLength];
      int text_len = sizeof(text);
      if (bin2decimal(p, &dec, precision, scale) != 0 ||
          decimal2string(&dec, text, &text_len) != 0)
        return fail(err, "column " + c.name + ": invalid DECIMAL value");
      v->kind = Value::Kind::TEXT;
      v->s.assign(text, text_len);
      break;
    }
    case MYSQL_TYPE_VARCHAR: {
      const unsigned int prefix = c.metadata > 255 ? 2 : 1;
      if (need(p, end, prefix, err)) return true;
      const uint32_t len = read_length(p, prefix);
      if (need(p + prefix, end, len, err)) return true;
      if (string_value(c, p + prefix, len, v, err)) return true;
      size = prefix + len;
      break;
    }
    case MYSQL_TYPE_STRING: {
      unsigned int real_type, max_length;
      decode_string_metadata(c.metadata, &real_type, &max_length);
      if (real_type != MYSQL_TYPE_STRING)
        return fail(err, "column " + c.name + ": ENUM/SET not supported");
      const unsigned int prefix = max_length > 255 ? 2 : 1;
      if (need(p, end, prefix, err)) return true;
      const uint32_t len = read_length(p, prefix);
      if (need(p + prefix, end, len, err)) return true;
      if (string_value(c, p + prefix, len, v, err)) return true;
      size = prefix + len;
      break;
    }
    case MYSQL_TYPE_BLOB: {
      const unsigned int prefix = c.metadata;  // 1 to 4 bytes
      if (prefix < 1 || prefix > 4)
        return fail(err, "column " + c.name + ": invalid BLOB metadata");
      if (need(p, end, prefix, err)) return true;
      const uint32_t len = read_length(p, prefix);
      if (need(p + prefix, end, len, err)) return true;
      if (string_value(c, p + prefix, len, v, err)) return true;
      size = prefix + len;
      break;
    }
    // DATE columns appear in the binlog as MYSQL_TYPE_DATE (Field::binlog_type
    // is type()); the value is the same 3-byte packed date as NEWDATE.
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE: {
      if (need(p, end, size = 3, err)) return true;
      const uint32_t packed = uint3korr(p);
      const unsigned day = packed & 31;
      const unsigned month = (packed >> 5) & 15;
      const int64_t year = packed >> 9;
      if (day == 0 || month == 0)
        return fail(err, "column " + c.name + ": zero date");
      v->kind = Value::Kind::DATE;
      v->i = days_from_civil(year, month, day);
      break;
    }
    case MYSQL_TYPE_DATETIME2: {
      const unsigned int fsp = c.metadata;
      if (need(p, end, size = 5 + (fsp + 1) / 2, err)) return true;
      MYSQL_TIME t;
      TIME_from_longlong_datetime_packed(&t,
                                         my_datetime_packed_from_binary(p, fsp));
      if (t.month == 0 || t.day == 0)
        return fail(err, "column " + c.name + ": zero date");
      v->kind = Value::Kind::TIMESTAMP;
      v->i = days_from_civil(t.year, t.month, t.day) * 86400000000LL +
             ((t.hour * 60LL + t.minute) * 60 + t.second) * 1000000LL +
             static_cast<int64_t>(t.second_part);
      break;
    }
    case MYSQL_TYPE_TIMESTAMP2: {
      const unsigned int fsp = c.metadata;
      if (need(p, end, size = 4 + (fsp + 1) / 2, err)) return true;
      my_timeval tv;
      my_timestamp_from_binary(&tv, p, fsp);
      if (tv.m_tv_sec == 0 && tv.m_tv_usec == 0)
        return fail(err, "column " + c.name + ": zero timestamp");
      v->kind = Value::Kind::TIMESTAMP;
      v->i = tv.m_tv_sec * 1000000LL + tv.m_tv_usec;
      break;
    }
    default:
      return fail(err, "column " + c.name + ": binlog type " +
                           std::to_string(c.type) + " not supported");
  }
  *pos = p + size;
  return false;
}

}  // namespace

bool type_has_signedness(unsigned int type) {
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
      return true;
    default:
      return false;
  }
}

bool type_is_character(unsigned int type, unsigned int metadata) {
  switch (type) {
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VECTOR:
      return true;
    case MYSQL_TYPE_STRING: {
      unsigned int real_type, max_length;
      decode_string_metadata(metadata, &real_type, &max_length);
      return real_type == MYSQL_TYPE_STRING;
    }
    default:
      return false;
  }
}

int64_t days_from_civil(int64_t y, unsigned m, unsigned d) {
  // Howard Hinnant's algorithm.
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool read_column_metadata(unsigned int type, const unsigned char **pos,
                          const unsigned char *end, unsigned int *metadata,
                          std::string *err) {
  const unsigned char *p = *pos;
  size_t size = 0;
  switch (type) {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_NEWDATE:
      *metadata = 0;
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIMESTAMP2:
      if (need(p, end, size = 1, err)) return true;
      *metadata = p[0];
      break;
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_STRING:
      // precision/scale, or real type/length: a big-endian pair.
      if (need(p, end, size = 2, err)) return true;
      *metadata = (static_cast<unsigned int>(p[0]) << 8) | p[1];
      break;
    case MYSQL_TYPE_VARCHAR:
      if (need(p, end, size = 2, err)) return true;
      *metadata = uint2korr(p);
      break;
    default:
      return fail(err,
                  "binlog type " + std::to_string(type) + " not supported");
  }
  *pos = p + size;
  return false;
}

bool decode_row(const TableInfo &table, const std::vector<uint8_t> &present,
                const unsigned char **pos, const unsigned char *end,
                std::vector<Value> *row, std::string *err) {
  const size_t n = table.columns.size();
  auto is_present = [&](size_t i) {
    return i / 8 < present.size() && ((present[i / 8] >> (i % 8)) & 1) != 0;
  };
  size_t present_count = 0;
  for (size_t i = 0; i < n; i++) present_count += is_present(i) ? 1 : 0;

  const unsigned char *p = *pos;
  const size_t null_bytes = (present_count + 7) / 8;
  if (need(p, end, null_bytes, err)) return true;
  const unsigned char *null_bits = p;
  p += null_bytes;

  row->assign(n, Value());
  size_t k = 0;  // index among present columns
  for (size_t i = 0; i < n; i++) {
    if (!is_present(i)) continue;
    const bool is_null = ((null_bits[k / 8] >> (k % 8)) & 1) != 0;
    k++;
    if (is_null) continue;
    if (decode_value(table.columns[i], &p, end, &(*row)[i], err)) return true;
  }
  *pos = p;
  return false;
}

bool Value::operator==(const Value &o) const {
  return kind == o.kind && i == o.i && u == o.u && d == o.d && s == o.s;
}

std::string Value::to_string() const {
  switch (kind) {
    case Kind::NUL:
      return "NULL";
    case Kind::INT:
      return std::to_string(i);
    case Kind::UINT:
      return std::to_string(u);
    case Kind::DOUBLE:
      return std::to_string(d);
    case Kind::TEXT:
      return "'" + s + "'";
    case Kind::BLOB:
      return "x(" + std::to_string(s.size()) + " bytes)";
    case Kind::DATE:
      return "date:" + std::to_string(i);
    case Kind::TIMESTAMP:
      return "ts:" + std::to_string(i);
  }
  return "?";
}

}  // namespace duckdb_se
