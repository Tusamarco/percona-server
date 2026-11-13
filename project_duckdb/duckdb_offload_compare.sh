#!/bin/bash
# Copyright (c) 2026-present, Marco Tusa. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
#
# DWL#6: runs each query of a file (one per line, lines starting with -- are
# skipped) with use_secondary_engine=OFF (InnoDB) and FORCED (DuckDB) and
# compares the results. Stops if the server is lost.
#
# With EXPECT=refused each query must instead be refused: FORCED fails with
# the reason (error 3877) and ON gives the InnoDB rows.
#
# usage: MYSQL="mysql -S /path/mysql.sock -u dba -pSECRET windmills_small" \
#        ./duckdb_offload_compare.sh duckdb_offload_6a_queries.sql
#        EXPECT=refused ./duckdb_offload_compare.sh duckdb_offload_6b_refusals.sql
: "${MYSQL:?set MYSQL to the mysql command with connection options and database}"
M="$MYSQL --force"
st() { $M -NBe "SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = '$1'"; }
n=0; pass=0
while IFS= read -r q; do
  case "$q" in ''|--*) continue ;; esac
  n=$((n+1))
  if [ "${EXPECT:-}" == "refused" ]; then
    a=$($M -NB -e "SET SESSION use_secondary_engine = OFF; $q" 2>&1)
    b=$($M -NB -e "SET SESSION use_secondary_engine = FORCED; $q" 2>&1)
    c=$($M -NB -e "SET SESSION use_secondary_engine = ON; $q" 2>&1)
    if echo "$a$b$c" | grep -q "ERROR 20"; then echo "Q$n SERVER LOST: $q"; exit 1; fi
    if echo "$a" | grep -q "^ERROR"; then echo "Q$n FAILED ON InnoDB (fix the test): $(echo "$a" | head -1)"; continue; fi
    why=$(echo "$b" | grep -o "ERROR 3877.*" | sed 's/.*cannot run this query: //;s/.*secondary engine[^:]*: //' | head -1)
    if [ -n "$why" ] && [ "$a" == "$c" ]; then pass=$((pass+1)); echo "Q$n refused ($why), ON = InnoDB";
    else echo "Q$n NOT REFUSED AS EXPECTED: $q"; echo "  FORCED: $(echo "$b" | head -2 | tr '\n' '|')"; fi
    continue
  fi
  off0=$(st duckdb_offload_queries)
  a=$($M -NB -e "SET SESSION use_secondary_engine = OFF; $q" 2>&1)
  b=$($M -NB -e "SET SESSION use_secondary_engine = FORCED; $q" 2>&1)
  if echo "$a$b" | grep -q "ERROR 20"; then echo "Q$n SERVER LOST: $q"; echo "$b" | head -2; exit 1; fi
  # An error on the InnoDB side means the test itself is wrong (missing
  # table, typo): never a pass.
  if echo "$a" | grep -q "^ERROR"; then echo "Q$n FAILED ON InnoDB (fix the test): $(echo "$a" | head -1)"; continue; fi
  offloaded=yes; [ "$(st duckdb_offload_queries)" == "$off0" ] && offloaded=no
  how="offloaded"; [ $offloaded == no ] && how="NOT offloaded ($(st duckdb_offload_last_refusal))"
  # A pass needs the same rows AND the FORCED run really executed in DuckDB.
  if [ "$a" == "$b" ] && [ $offloaded == yes ]; then pass=$((pass+1)); echo "Q$n same, $how";
  elif [ "$a" == "$b" ]; then echo "Q$n same rows but $how: not a pass";
  else echo "Q$n DIFFERENT, $how: $q"; echo "  InnoDB: $(echo "$a" | head -4 | tr '\n' '|')"; echo "  DuckDB: $(echo "$b" | head -4 | tr '\n' '|')"; fi
done < "$1"
if [ "${EXPECT:-}" == "refused" ]; then echo "$pass/$n refused as expected"; else echo "$pass/$n identical and offloaded"; fi
