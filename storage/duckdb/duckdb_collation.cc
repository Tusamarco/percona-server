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

#include "storage/duckdb/duckdb_collation.h"

#include <cstring>
#include <string>

#include "my_sys.h"  // get_charset_by_csname
#include "mysql/strings/m_ctype.h"

namespace duckdb_se {

bool needs_weight(const CHARSET_INFO *cs) {
  if (cs == nullptr || cs == &my_charset_bin) return false;
  const bool unicode = cs->csname != nullptr &&
                       (strcmp(cs->csname, "utf8mb4") == 0 ||
                        strcmp(cs->csname, "utf8mb3") == 0);
  return !((cs->state & MY_CS_BINSORT) != 0 && unicode);
}

std::string collation_weight(const CHARSET_INFO *cs, const char *s,
                             size_t len) {
  if (cs->pad_attribute != NO_PAD) len = cs->cset->lengthsp(cs, s, len);
  const size_t chars = cs->cset->numchars(cs, s, s + len);
  size_t cap = cs->coll->strnxfrmlen(cs, cs->mbmaxlen * (chars + 1));
  if (cap % 2 != 0) cap++;
  if (cap < 2) cap = 2;
  std::string out(cap, '\0');
  // num_codepoints = the value's own length: no padding.
  const size_t n = cs->coll->strnxfrm(
      cs, reinterpret_cast<uint8_t *>(out.data()), cap,
      static_cast<unsigned>(chars), reinterpret_cast<const uint8_t *>(s), len,
      0);
  out.resize(n);
  return out;
}

std::string space_weight(const CHARSET_INFO *cs) {
  const char space = ' ';
  size_t cap = cs->coll->strnxfrmlen(cs, cs->mbmaxlen * 2);
  if (cap % 2 != 0) cap++;
  std::string out(cap < 2 ? 2 : cap, '\0');
  const size_t n = cs->coll->strnxfrm(
      cs, reinterpret_cast<uint8_t *>(out.data()), out.size(), 1,
      reinterpret_cast<const uint8_t *>(&space), 1, 0);
  out.resize(n);
  return out;
}

bool collation_weight_utf8(const CHARSET_INFO *cs, const std::string &utf8,
                           std::string *weight) {
  if (strncmp(cs->csname, "utf8mb", 6) == 0 ||
      strcmp(cs->csname, "ascii") == 0) {
    *weight = collation_weight(cs, utf8.data(), utf8.size());
    return false;
  }
  const CHARSET_INFO *from =
      get_charset_by_csname("utf8mb4", MY_CS_PRIMARY, MYF(0));
  if (from == nullptr) return true;
  std::string native(utf8.size() * cs->mbmaxlen + 1, '\0');
  uint errors = 0;
  const size_t n = my_convert(native.data(), native.size(), cs, utf8.data(),
                              utf8.size(), from, &errors);
  if (errors != 0) return true;
  *weight = collation_weight(cs, native.data(), n);
  return false;
}

}  // namespace duckdb_se
