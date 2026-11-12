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

#include "storage/duckdb/duckdb_ddl.h"

#include <cctype>
#include <cstddef>

namespace duckdb_se {

namespace {

struct Token {
  enum class Kind { WORD, QUOTED, STRING, PUNCT };
  Kind kind;
  std::string text;  // QUOTED: unescaped name; PUNCT: one character
};

bool is_word_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$' ||
         static_cast<unsigned char>(c) >= 0x80;
}

std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

std::string lower(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Splits a statement into tokens. Comments are skipped, except versioned
// comments (/*!NNNNN ... */), whose content is code. Optimizer hints (/*+ */)
// are comments.
std::vector<Token> tokenize(std::string_view q) {
  std::vector<Token> out;
  size_t i = 0;
  const size_t n = q.size();
  while (i < n) {
    const char c = q[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
    } else if (c == '/' && i + 1 < n && q[i + 1] == '*') {
      if (i + 2 < n && q[i + 2] == '!') {
        // Versioned comment: skip the marker and its version, keep the code.
        i += 3;
        while (i < n && std::isdigit(static_cast<unsigned char>(q[i]))) i++;
      } else {
        const size_t end = q.find("*/", i + 2);
        i = end == std::string_view::npos ? n : end + 2;
      }
    } else if (c == '*' && i + 1 < n && q[i + 1] == '/') {
      i += 2;  // end of a versioned comment
    } else if (c == '#' ||
               (c == '-' && i + 2 < n && q[i + 1] == '-' &&
                std::isspace(static_cast<unsigned char>(q[i + 2])))) {
      const size_t end = q.find('\n', i);
      i = end == std::string_view::npos ? n : end + 1;
    } else if (c == '`') {
      std::string name;
      i++;
      while (i < n) {
        if (q[i] == '`') {
          if (i + 1 < n && q[i + 1] == '`') {
            name += '`';
            i += 2;
            continue;
          }
          i++;
          break;
        }
        name += q[i++];
      }
      out.push_back({Token::Kind::QUOTED, name});
    } else if (c == '\'' || c == '"') {
      i++;
      while (i < n && q[i] != c) {
        if (q[i] == '\\' && i + 1 < n) i++;
        i++;
      }
      i++;
      out.push_back({Token::Kind::STRING, ""});
    } else if (is_word_char(c)) {
      const size_t start = i;
      while (i < n && is_word_char(q[i])) i++;
      out.push_back({Token::Kind::WORD, std::string(q.substr(start, i - start))});
    } else {
      out.push_back({Token::Kind::PUNCT, std::string(1, c)});
      i++;
    }
  }
  return out;
}

class Parser {
 public:
  Parser(std::vector<Token> tokens, const std::string &default_db,
         bool lower_case)
      : m_t(std::move(tokens)), m_db(default_db), m_lower(lower_case) {}

  DdlStatement parse() {
    DdlStatement ddl;
    if (kw("ALTER")) {
      m_i++;
      if (kw("TABLE")) {
        m_i++;
        alter_table(&ddl);
      }
    } else if (kw("TRUNCATE")) {
      m_i++;
      if (kw("TABLE")) m_i++;
      TableName t;
      if (name(&t)) ddl.refresh.push_back(t);
    } else if (kw("RENAME")) {
      m_i++;
      if (kw("TABLE") || kw("TABLES")) {
        m_i++;
        rename_tables(&ddl);
      }
    } else if (kw("DROP")) {
      // DROP INDEX `PRIMARY` ON t changes the primary key; other indexes do
      // not change the copy. DROP TABLE is handled by the server.
      m_i++;
      if (kw("INDEX")) {
        m_i++;
        const bool primary = is_primary(m_i);
        m_i++;
        if (kw("ON")) {
          m_i++;
          TableName t;
          if (name(&t) && primary) ddl.refresh.push_back(t);
        }
      }
    }
    // CREATE INDEX and everything else: nothing to do.
    return ddl;
  }

 private:
  bool at_end() const { return m_i >= m_t.size(); }
  bool kw(const char *word, size_t at) const {
    return at < m_t.size() && m_t[at].kind == Token::Kind::WORD &&
           upper(m_t[at].text) == word;
  }
  bool kw(const char *word) const { return kw(word, m_i); }
  bool punct(char c, size_t at) const {
    return at < m_t.size() && m_t[at].kind == Token::Kind::PUNCT &&
           m_t[at].text[0] == c;
  }
  bool ident(size_t at, std::string *out) const {
    if (at >= m_t.size()) return false;
    const Token &t = m_t[at];
    if (t.kind != Token::Kind::WORD && t.kind != Token::Kind::QUOTED)
      return false;
    *out = t.text;
    return true;
  }

  // The index name PRIMARY, quoted or not.
  bool is_primary(size_t at) const {
    std::string id;
    return ident(at, &id) && upper(id) == "PRIMARY";
  }

  // [db.]table at m_i.
  bool name(TableName *t) {
    std::string first;
    if (!ident(m_i, &first)) return false;
    m_i++;
    if (punct('.', m_i)) {
      std::string second;
      if (!ident(m_i + 1, &second)) return false;
      m_i += 2;
      t->db = first;
      t->table = second;
    } else {
      t->db = m_db;
      t->table = first;
    }
    if (t->db.empty()) return false;
    if (m_lower) {
      t->db = lower(t->db);
      t->table = lower(t->table);
    }
    return true;
  }

  void rename_tables(DdlStatement *ddl) {
    while (!at_end()) {
      TableName from, to;
      if (!name(&from) || !kw("TO")) return;
      m_i++;
      if (!name(&to)) return;
      ddl->renames.emplace_back(from, to);
      if (!punct(',', m_i)) return;
      m_i++;
    }
  }

  // Clauses of ALTER TABLE t, separated by commas outside parentheses.
  void alter_table(DdlStatement *ddl) {
    TableName table;
    if (!name(&table)) return;
    bool change = false;
    bool secondary = false;
    TableName renamed_to;
    bool renamed = false;
    std::vector<std::pair<std::string, std::string>> column_renames;

    while (!at_end() && !punct(';', m_i)) {
      // One clause: [m_i, end).
      size_t end = m_i;
      int depth = 0;
      while (end < m_t.size()) {
        if (punct('(', end)) depth++;
        if (punct(')', end)) depth--;
        if (depth == 0 && (punct(',', end) || punct(';', end))) break;
        end++;
      }
      classify(end, &change, &secondary, &renamed, &renamed_to,
               &column_renames);
      m_i = end;
      if (punct(',', m_i)) m_i++;
    }
    // SECONDARY_LOAD / SECONDARY_UNLOAD stand alone and change nothing in
    // InnoDB; the server sends them to the plugin.
    if (secondary) return;
    const TableName target = renamed ? renamed_to : table;
    if (renamed) ddl->renames.emplace_back(table, renamed_to);
    if (change) {
      ddl->refresh.push_back(target);
      ddl->column_renames = std::move(column_renames);
    }
  }

  void classify(size_t end, bool *change, bool *secondary, bool *renamed,
                TableName *renamed_to,
                std::vector<std::pair<std::string, std::string>> *renames) {
    const size_t i = m_i;
    if (i >= end) return;
    auto word = [&](size_t k) {
      return k < end && m_t[k].kind == Token::Kind::WORD ? upper(m_t[k].text)
                                                          : std::string();
    };
    const std::string w0 = word(i);
    const std::string w1 = word(i + 1);
    auto index_word = [](const std::string &w) {
      return w == "INDEX" || w == "KEY" || w == "FULLTEXT" || w == "SPATIAL" ||
             w == "UNIQUE";
    };

    if (w0 == "SECONDARY_LOAD" || w0 == "SECONDARY_UNLOAD") {
      *secondary = true;
    } else if (w0 == "ALGORITHM" || w0 == "LOCK" || w0 == "FORCE" ||
               w0 == "WITH" || w0 == "WITHOUT" || w0 == "SECONDARY_ENGINE" ||
               w0 == "COMMENT" || w0 == "AUTO_INCREMENT" || w0 == "ORDER" ||
               w0 == "CHARACTER" || w0 == "CHARSET" || w0 == "COLLATE" ||
               w0 == "STATS_PERSISTENT" || w0 == "STATS_AUTO_RECALC" ||
               w0 == "STATS_SAMPLE_PAGES" || w0 == "KEY_BLOCK_SIZE" ||
               w0 == "ROW_FORMAT" || w0 == "PACK_KEYS" ||
               w0 == "CHECKSUM" || w0 == "DELAY_KEY_WRITE" ||
               w0 == "ENABLE" || w0 == "DISABLE" || w0 == "ANALYZE" ||
               w0 == "CHECK" || w0 == "OPTIMIZE" || w0 == "REPAIR" ||
               w0 == "REBUILD" || w0 == "COALESCE" ||
               w0 == "REORGANIZE" || w0 == "REMOVE" || w0 == "PARTITION") {
      // No change to the rows or the columns of the copy. DEFAULT CHARSET
      // changes only the default for new columns (CONVERT TO is a change).
    } else if (w0 == "DEFAULT" &&
               (w1 == "CHARACTER" || w1 == "CHARSET" || w1 == "COLLATE")) {
    } else if (w0 == "ADD") {
      if (index_word(w1) || w1 == "PARTITION") return;
      if (w1 == "CONSTRAINT") {
        // ADD CONSTRAINT [name] UNIQUE / CHECK: no change to the copy.
        size_t k = i + 2;
        if (k < end && !index_word(word(k)) && word(k) != "PRIMARY" &&
            word(k) != "CHECK" && word(k) != "FOREIGN")
          k++;
        if (index_word(word(k)) || word(k) == "CHECK") return;
      }
      if (w1 == "CHECK") return;
      *change = true;
    } else if (w0 == "DROP") {
      if (w1 == "INDEX" || w1 == "KEY") {
        // DROP INDEX `PRIMARY` drops the primary key.
        if (is_primary(i + 2)) *change = true;
        return;
      }
      if (w1 == "FOREIGN" || w1 == "CHECK") return;
      *change = true;  // COLUMN, PRIMARY KEY, CONSTRAINT, PARTITION
    } else if (w0 == "ALTER") {
      // ALTER INDEX ... VISIBLE, ALTER [COLUMN] c SET/DROP DEFAULT,
      // SET VISIBLE / INVISIBLE, ALTER CHECK / CONSTRAINT: rows unchanged.
    } else if (w0 == "RENAME") {
      if (index_word(w1)) return;
      if (w1 == "COLUMN") {
        std::string from, to;
        if (ident(i + 2, &from) && kw("TO", i + 3) && ident(i + 4, &to))
          renames->emplace_back(from, to);
        *change = true;
        return;
      }
      // RENAME [TO | AS] new_name
      size_t k = i + 1;
      if (w1 == "TO" || w1 == "AS") k++;
      const size_t saved = m_i;
      m_i = k;
      TableName to;
      if (name(&to)) {
        *renamed = true;
        *renamed_to = to;
      }
      m_i = saved;
    } else if (w0 == "CHANGE") {
      size_t k = i + 1;
      if (w1 == "COLUMN") k++;
      std::string from, to;
      if (ident(k, &from) && ident(k + 1, &to) && lower(from) != lower(to))
        renames->emplace_back(from, to);
      *change = true;
    } else {
      // MODIFY, CONVERT TO, ENGINE, SECONDARY_ENGINE_ATTRIBUTE, TRUNCATE /
      // EXCHANGE PARTITION, DISCARD / IMPORT TABLESPACE, unknown clauses.
      *change = true;
    }
  }

  std::vector<Token> m_t;
  size_t m_i{0};
  std::string m_db;
  bool m_lower;
};

}  // namespace

ColumnRenames compose_renames(const ColumnRenames &first,
                              const ColumnRenames &then) {
  ColumnRenames out = first;
  for (const auto &[from, to] : then) {
    bool chained = false;
    for (auto &pair : out)
      if (pair.second == from) {
        pair.second = to;
        chained = true;
      }
    if (!chained) out.emplace_back(from, to);
  }
  ColumnRenames result;
  for (auto &pair : out)
    if (pair.first != pair.second) result.push_back(std::move(pair));
  return result;
}

// Column names cannot contain these control characters.
constexpr char kPairSep = '\x1e';
constexpr char kNameSep = '\x1f';

std::string encode_renames(const ColumnRenames &renames) {
  std::string out;
  for (const auto &[from, to] : renames) {
    if (!out.empty()) out += kPairSep;
    out += from + kNameSep + to;
  }
  return out;
}

ColumnRenames decode_renames(std::string_view text) {
  ColumnRenames out;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(kPairSep, start);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view pair = text.substr(start, end - start);
    const size_t sep = pair.find(kNameSep);
    if (sep != std::string_view::npos)
      out.emplace_back(std::string(pair.substr(0, sep)),
                       std::string(pair.substr(sep + 1)));
    start = end + 1;
  }
  return out;
}

DdlStatement parse_ddl(std::string_view query, const std::string &default_db,
                       bool lower_case_names) {
  return Parser(tokenize(query), default_db, lower_case_names).parse();
}

}  // namespace duckdb_se
