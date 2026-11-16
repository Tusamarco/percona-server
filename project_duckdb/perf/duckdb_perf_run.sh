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
# DWL#6 step 6d-3: runs each workload query RUNS times (default 3) on InnoDB
# (use_secondary_engine=OFF) and with ON and the default cost threshold
# (DuckDB when the server and the engine take it, as in production), checks
# that the results are the same (not for FULL scope: archived rows exist only
# in DuckDB), and prints the median times.
#
# usage: MYSQL="mysql -S /path/mysql.sock -u dba -pSECRET perf" ./duckdb_perf_run.sh [queries file]
: "${MYSQL:?set MYSQL to the mysql command with connection options and database}"
RUNS=${RUNS:-3}
Q=${1:-$(dirname "$0")/duckdb_perf_queries.sql}
M="$MYSQL --force -N -B"
st() { $M -e "SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = '$1'"; }
median() { sort -n | awk '{a[NR]=$1} END {print a[int((NR+1)/2)]}'; }
run() {  # $1 = session settings, $2 = query; prints seconds, output in $OUT
  local t0 t1; t0=$(date +%s.%N)
  OUT=$($M -e "$1 $2" 2>&1); t1=$(date +%s.%N)
  echo "$t1 - $t0" | bc
}
printf "%-5s %-10s %10s %10s %8s  %s\n" query ran_in innodb_s duckdb_s speedup note
name=""
while IFS= read -r line; do
  case "$line" in
    "-- q"*) name=${line#-- }; continue ;;
    ''|--*) continue ;;
  esac
  id=${name%% *}
  ti=(); td=()
  for i in $(seq "$RUNS"); do ti+=("$(run "SET SESSION use_secondary_engine = OFF;" "$line")"); done
  a=$OUT
  off0=$(st duckdb_offload_queries)
  for i in $(seq "$RUNS"); do td+=("$(run "SET SESSION use_secondary_engine = ON;" "$line")"); done
  b=$OUT
  where=InnoDB; [ "$(st duckdb_offload_queries)" != "$off0" ] && where=DuckDB
  mi=$(printf "%s\n" "${ti[@]}" | median); md=$(printf "%s\n" "${td[@]}" | median)
  note=""
  if echo "$a" | grep -q "^ERROR"; then note="InnoDB ERROR: $(echo "$a" | head -1)";
  elif [ "$a" != "$b" ] && ! echo "$line" | grep -q "duckdb_history_scope = FULL"; then note="RESULTS DIFFER";
  elif [ $where == InnoDB ]; then note="not offloaded (cost below threshold, or: $(st duckdb_offload_last_refusal | cut -c1-50))"; fi
  printf "%-5s %-10s %10.3f %10.3f %7.1fx  %s\n" "$id" "$where" "$mi" "$md" "$(echo "$mi / $md" | bc -l)" "$note"
done < "$Q"
