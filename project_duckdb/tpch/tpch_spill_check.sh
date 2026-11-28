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
# Memory and spill of the TPC-H queries in DuckDB (use_secondary_engine =
# FORCED), alone or concurrent.
#
# STREAMS=1 (default): each query alone; time, peak DuckDB memory, peak
#   spill while it ran. The DuckDB SQL and plan of each query (EXPLAIN
#   notes) are saved in OUT.
# STREAMS=N (2 and more): N parallel streams, like HammerDB virtual users,
#   each running the 22 queries once in its own order (shuffled with a fixed
#   seed per stream). Reported: per query the mean / min / max time over the
#   streams, the peak memory and spill of the run, and for the spill, which
#   queries were running while it grew (the queries that cause it).
#   SOLO=1 runs the 1-stream pass first and shows the slowdown against it.
#
# Memory and spill are sampled every 0.2 s. The plugin's status variables
# are refreshed about every 5 s, so on the database host set SPILL_DIR to
# the repository's tmp/ directory (duckdb_data_path/tmp): its size is then
# measured directly.
#
# ORDER_FILE: one line per stream with the query numbers in the order that
# stream runs them (HammerDB uses the TPC-H specification's fixed
# permutations: put them here to reproduce its mix). Without it each stream
# gets its own seeded shuffle.
#
# While a pass runs, a progress line every PROGRESS seconds (default 10):
# finished queries, DuckDB memory, spill, the queries running now.
#
# Q15 reads the view revenue0, which HammerDB creates around it: the script
# creates it when it is missing and drops it at the end if it created it.
#
# usage: MYSQL="mysql -h 127.0.0.1 -P 3306 -u dba -pSECRET tpch" \
#        OUT=/tmp/tpch_spill SPILL_DIR=/data/duckdb_data/tmp \
#        STREAMS=8 SOLO=1 ./tpch_spill_check.sh [tpch_queries.sql]
: "${MYSQL:?set MYSQL to the mysql command with connection options and database}"
OUT=${OUT:-/tmp/tpch_spill}
STREAMS=${STREAMS:-1}
QUERIES=${1:-$(dirname "$0")/tpch_queries.sql}
mkdir -p "$OUT"
grep -v '^--' "$QUERIES" | grep -v '^$' > "$OUT/queries.sql"
NQ=$(wc -l < "$OUT/queries.sql")

st() {
  $MYSQL -NBe "SELECT VARIABLE_VALUE FROM performance_schema.global_status WHERE VARIABLE_NAME = '$1'" 2>/dev/null
}
spill_now() {
  if [ -n "$SPILL_DIR" ]; then
    du -sb "$SPILL_DIR" 2>/dev/null | cut -f1
  else
    st duckdb_temp_spill_bytes
  fi
}
now() { date +%s.%N; }

# The view of Q15.
created_view=0
if ! $MYSQL -NBe "SELECT 1 FROM revenue0 LIMIT 1" >/dev/null 2>&1; then
  $MYSQL -e "CREATE VIEW revenue0 (supplier_no, total_revenue) AS SELECT l_suppkey, SUM(l_extendedprice * (1 - l_discount)) FROM LINEITEM WHERE l_shipdate >= DATE '1996-01-01' AND l_shipdate < DATE '1996-04-01' GROUP BY l_suppkey" 2>/dev/null &&
    created_view=1
fi
cleanup() {
  [ "$created_view" = 1 ] && $MYSQL -e "DROP VIEW IF EXISTS revenue0" 2>/dev/null
}
trap cleanup EXIT

# Samples memory, spill and the running queries every 0.2 s until the file
# $1.stop exists: "time memory spill q,q,..." per line.
sampler() {
  local log=$1
  : > "$log"
  while [ ! -e "$log.stop" ]; do
    local running
    running=$(cat "$OUT"/running.* 2>/dev/null | tr '\n' ',' | sed 's/,$//')
    echo "$(now) $(st duckdb_memory_used_bytes) $(spill_now) ${running:--}" >> "$log"
    sleep 0.2
  done
}

# One stream: runs the queries in the order of $2 (query numbers), logs
# "stream query start end status" lines to $OUT/times.<stream>.
stream() {
  local s=$1 order=$2
  : > "$OUT/times.$s"
  for n in $order; do
    local q
    q=$(sed -n "${n}p" "$OUT/queries.sql")
    echo "$n" > "$OUT/running.$s"
    local t0 t1 rc=ok
    t0=$(now)
    $MYSQL -e "SET use_secondary_engine = FORCED; $q" > "$OUT/s$s.q$n.result.txt" 2>&1 ||
      rc=error
    t1=$(now)
    rm -f "$OUT/running.$s"
    echo "$s $n $t0 $t1 $rc" >> "$OUT/times.$s"
  done
}

# Every $PROGRESS seconds (default 10) while a pass runs: elapsed time,
# finished queries, DuckDB memory, spill and the running queries.
progress() {
  local k=$1 total=$2 t0 t
  t0=$(date +%s)
  while [ ! -e "$OUT/samples.$k.stop" ]; do
    sleep "${PROGRESS:-10}"
    [ -e "$OUT/samples.$k.stop" ] && break
    t=$(date +%s)
    local done last
    done=$(cat "$OUT"/times.* 2>/dev/null | wc -l)
    last=$(tail -1 "$OUT/samples.$k" 2>/dev/null)
    echo "$last" | awk -v el=$((t - t0)) -v d="$done" -v tot="$total" '{
      n = split($4, q, ",");
      out = "";
      if ($4 != "-" && $4 != "") {
        for (i = 1; i <= n; i++) c[q[i]]++;
        for (k in c) out = out " Q" k (c[k] > 1 ? "x" c[k] : "");
      }
      printf "[%5ds] done %4d/%d | DuckDB mem %6.1f GB | spill %7.1f GB | running:%s\n",
             el, d, tot, $2 / 1073741824, $3 / 1073741824, out
    }'
  done
}

# A pass with $1 streams; results in $OUT/pass$1.*.
run_pass() {
  local k=$1
  rm -f "$OUT"/running.* "$OUT"/times.* "$OUT/samples.$k" "$OUT/samples.$k.stop"
  sampler "$OUT/samples.$k" &
  local sp=$!
  progress "$k" $((k * NQ)) &
  local pp=$!
  local pids=()
  for s in $(seq 1 "$k"); do
    local order
    if [ "$k" = 1 ]; then
      order=$(seq 1 "$NQ" | tr '\n' ' ')
    elif [ -n "$ORDER_FILE" ]; then
      # Line s of ORDER_FILE (lines reused from the first when there are
      # fewer lines than streams): the stream's query order.
      local lines
      lines=$(grep -c . "$ORDER_FILE")
      order=$(grep . "$ORDER_FILE" | sed -n "$(( (s - 1) % lines + 1 ))p")
    else
      # A shuffle seeded per stream, so the streams do not reach the
      # same query at the same time.
      order=$(python3 -c "import random; q=list(range(1, $NQ + 1)); random.Random($s * 7919 + 17).shuffle(q); print(' '.join(map(str, q)))")
    fi
    stream "$s" "$order" &
    pids+=($!)
  done
  wait "${pids[@]}"
  touch "$OUT/samples.$k.stop"
  wait $sp $pp
  cat "$OUT"/times.* > "$OUT/times_pass$k.txt"
}

if [ "$STREAMS" = 1 ] || [ "${SOLO:-0}" = 1 ]; then
  # The DuckDB SQL and plan of each query, once.
  n=0
  while IFS= read -r q; do
    n=$((n + 1))
    $MYSQL -e "SET use_secondary_engine = FORCED; EXPLAIN $q; SHOW WARNINGS" \
      > "$OUT/q$n.explain.txt" 2>&1
  done < "$OUT/queries.sql"
  echo "== 1 stream (each query alone)"
  run_pass 1
fi
if [ "$STREAMS" -gt 1 ]; then
  echo "== $STREAMS streams"
  run_pass "$STREAMS"
fi

python3 - "$OUT" "$STREAMS" "${SOLO:-0}" "$NQ" <<'EOF'
import sys, collections
out, streams, solo, nq = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1", int(sys.argv[4])
GB = 1024 ** 3

def times(k):
    t = collections.defaultdict(list)
    errors = collections.Counter()
    try:
        for line in open(f"{out}/times_pass{k}.txt"):
            s, q, a, b, rc = line.split()
            if rc == "ok":
                t[int(q)].append(float(b) - float(a))
            else:
                errors[int(q)] += 1
    except FileNotFoundError:
        pass
    return t, errors

def samples(k):
    rows = []
    try:
        for line in open(f"{out}/samples.{k}"):
            p = line.split()
            if len(p) < 4:
                continue
            try:
                rows.append((float(p[0]), int(p[1] or 0), int(p[2] or 0),
                             [] if p[3] == "-" else [int(x) for x in p[3].split(",") if x]))
            except ValueError:
                pass
    except FileNotFoundError:
        pass
    return rows

def peak(rows):
    return (max((r[1] for r in rows), default=0) / GB,
            max((r[2] for r in rows), default=0) / GB)

solo_t, _ = times(1)
if streams > 1:
    k = streams
    t, errors = times(k)
    rows = samples(k)
    pm, ps = peak(rows)
    print(f"\nPer query over {k} streams (seconds)" +
          (": mean / min / max, solo time, slowdown" if solo_t else ": mean / min / max"))
    for q in range(1, nq + 1):
        v = t.get(q, [])
        line = f"Q{q:<3}"
        if v:
            line += f" {sum(v)/len(v):8.2f} {min(v):8.2f} {max(v):8.2f}"
            if solo_t.get(q):
                s1 = solo_t[q][0]
                line += f"   solo {s1:6.2f}   x{(sum(v)/len(v))/s1:5.1f}"
        if errors[q]:
            line += f"   {errors[q]} error(s), see {out}/s*.q{q}.result.txt"
        print(line)
    print(f"\nPeak DuckDB memory {pm:.1f} GB, peak spill {ps:.1f} GB")
    # Spill growth attributed to the queries running while it grew.
    grow = collections.Counter()
    total = 0
    for a, b in zip(rows, rows[1:]):
        d = b[2] - a[2]
        if d > 0 and b[3]:
            total += d
            for q in set(b[3]):
                grow[q] += d
    if total:
        print("\nQueries running while the spill grew (GB of growth they were running"
              " for; overlaps counted for each):")
        for q, d in grow.most_common(10):
            print(f"  Q{q:<3} {d/GB:8.1f} GB  ({100*d/total:5.1f}% of {total/GB:.1f} GB)")
    else:
        print("\nThe spill did not grow during the run.")
if solo_t:
    rows = samples(1)
    pm, ps = peak(rows)
    print(f"\nSolo pass: total {sum(v[0] for v in solo_t.values()):.1f} s, "
          f"peak memory {pm:.1f} GB, peak spill {ps:.1f} GB")
print(f"Files in {out}: q<N>.explain.txt (plans), times_pass*.txt, samples.*")
EOF
