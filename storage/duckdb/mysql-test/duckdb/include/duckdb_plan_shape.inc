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
# The shape of the DuckDB statement for $query (EXPLAIN notes): whether the
# DuckDB plan has a cross product, whether the SQL has a DISTINCT derived
# table, and whether it has NOT EXISTS. On small test tables a bad plan
# still gives the right rows quickly; this catches it (TPC-H Q21 at SF100,
# 2026-11-24).
# Usage: --let $query= SELECT ...
#        --source ../include/duckdb_plan_shape.inc
--echo # plan shape: $query
--disable_query_log
--disable_result_log
--disable_warnings
SET SESSION use_secondary_engine = FORCED;
--eval EXPLAIN $query
--enable_result_log
--let $duckdb_plan_file= $MYSQLTEST_VARDIR/tmp/duckdb_plan_shape.txt
--remove_files_wildcard $MYSQLTEST_VARDIR/tmp duckdb_plan_shape.txt
--output $duckdb_plan_file
SHOW WARNINGS;
--enable_warnings
SET SESSION use_secondary_engine = DEFAULT;
--enable_query_log
--let PLAN_FILE= $duckdb_plan_file
--perl
open(my $fh, '<', $ENV{PLAN_FILE}) or die "no plan file";
my $text = do { local $/; <$fh> };
close $fh;
# The notes split long text in pieces: join them.
$text =~ s/\\n//g;
my $sql = join('', $text =~ /DuckDB SQL(?: \(\d+\/\d+\))?:(.*?)(?=\n\S+\t\d+\t|\z)/sg);
my $plan = join('', $text =~ /DuckDB plan(?: \(\d+\/\d+\))?:(.*?)(?=\n\S+\t\d+\t|\z)/sg);
print "cross product in DuckDB plan: ", ($plan =~ /CROSS_PRODUCT|Cross Product/ ? "yes" : "no"), "\n";
print "DISTINCT derived table: ", ($sql =~ /\(SELECT DISTINCT/ ? "yes" : "no"), "\n";
print "NOT EXISTS: ", ($sql =~ /NOT EXISTS/ ? "yes" : "no"), "\n";
EOF
--remove_file $duckdb_plan_file
