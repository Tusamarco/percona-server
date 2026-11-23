#!/bin/tclsh
# HammerDB TPROC-H run on MySQL. We run it with:
#   ./hammerdbcli auto mysql_tproch_run.tcl
# Results go to /tmp/hammerdb.log ("RESULTS FOR N THREAD(S)" + per-query timings),
# the format hammerdb_tpch_stats.py parses.

# use_secondary_engine for the HammerDB sessions when the duckdb plugin is
# active: ON (a query DuckDB refuses runs on InnoDB) or FORCED (every query
# must run in DuckDB; a refused one fails with the reason). It is a
# session-only variable, so the driver sets it on its own connection.
set duckdb_secondary_engine FORCED

puts "SETTING CONFIGURATION"
dbset db mysql
dbset bm TPROC-H

# Connection: TCP to the server (a socket is only used with "localhost").
diset connection mysql_host 10.30.2.10
diset connection mysql_port 3306
diset connection mysql_ssl true
diset connection mysql_ssl_two_way false
diset connection mysql_ssl_linux_capath ""
diset connection mysql_ssl_ca ""
diset connection mysql_ssl_cert ""
diset connection mysql_ssl_key ""

# Schema / driver settings.
# mysql_scale_fact MUST match the loaded data: the driver uses it to build the
# query parameters (e.g. Q11's FRACTION = 0.0001 / SF).
diset tpch mysql_scale_fact 100
diset tpch mysql_tpch_user dba
diset tpch mysql_tpch_pass dba
diset tpch mysql_tpch_dbase tpch
diset tpch mysql_total_querysets 1
diset tpch mysql_raise_query_error true
diset tpch mysql_verbose false
diset tpch mysql_refresh_on false

# DuckDB secondary engine: MySQL offloads only outside transactions, but
# HammerDB runs the queries with autocommit off unless it detects HeatWave.
# We patch the loaded driver script so autocommit is on (and
# use_secondary_engine as chosen above) when the duckdb plugin is active.
# The script lives in ::_ED(package), the buffer that loadscript /
# customscript fill and vucreate uses. HammerDB may clear or regenerate it
# between runs (vudestroy, vuset), so we check it before every vucreate:
# loaded and patched, or loaded again and patched.
# No diset may come after this point: changing a driver setting clears the
# loaded script.
if { [ lsearch -exact {ON FORCED} $duckdb_secondary_engine ] < 0 } {
  puts "ERROR: duckdb_secondary_engine must be ON or FORCED"
  exit
}
set hw_line {if { [string equal -nocase $engine "Heatwave"] } { set autocommit 1 }}
set duckdb_block {if { [string equal -nocase $engine "Heatwave"] } { set autocommit 1 }
        if { ![catch { set duckdb_active [ mysql::sel $mysql_handler "SELECT COUNT(*) FROM information_schema.PLUGINS WHERE PLUGIN_NAME = 'duckdb' AND PLUGIN_STATUS = 'ACTIVE'" -list ] }] && [lindex $duckdb_active 0] > 0 } {
            set autocommit 1
            mysql::exec $mysql_handler "SET SESSION use_secondary_engine = DUCKDB_SE_MODE"
        }}
set duckdb_block [ string map [ list DUCKDB_SE_MODE $duckdb_secondary_engine ] $duckdb_block ]

proc ensure_patched {} {
  global hw_line duckdb_block duckdb_secondary_engine
  if { ![ info exists ::_ED(package) ] || [ string length $::_ED(package) ] == 0 } {
    loadscript
  }
  if { ![ info exists ::_ED(package) ] } {
    puts "ERROR: no loaded driver script in ::_ED(package): cannot patch autocommit"
    exit
  }
  # Already patched (the buffer survived since the last run).
  if { [ string first "duckdb_active" $::_ED(package) ] >= 0 } { return }
  set patched [ string map [ list $hw_line $duckdb_block ] $::_ED(package) ]
  if { $patched eq $::_ED(package) } {
    puts "ERROR: HeatWave autocommit line not found in the driver script: not patched"
    exit
  }
  set ::_ED(package) $patched
  puts "Driver script patched: autocommit on and use_secondary_engine = $duckdb_secondary_engine when the duckdb plugin is active"
}

# We load the TPROC-H driver script with the settings above, and patch it.
loadscript
ensure_patched

# We send the virtual users' output to /tmp/hammerdb.log, with timestamps.
# unique 0 keeps one file name, so every run appends to the same log.
vuset logtotemp 1
vuset unique 0
vuset timestamps 1
vuset showoutput 1

# puts in this script goes to the terminal, not to hammerdb.log, so we append the
# run markers to the log ourselves: the stats parser splits the runs on them.
if { [ info exists ::env(TMP) ] } { set logfile "$::env(TMP)/hammerdb.log" } else { set logfile "/tmp/hammerdb.log" }
proc logmark { line } {
  global logfile
  set fh [ open $logfile a ]
  puts $fh $line
  close $fh
  puts $line
}

# We run the same query set at increasing concurrency; each VU runs all 22
# queries once, in its own order. In the CLI, vurun blocks until every VU has
# finished, so no custom wait loop is needed, however long the queries take.
foreach vu {1 2 4 8} {
  logmark " RESULTS FOR $vu THREAD(S)"
  vuset vu $vu
  ensure_patched
  vucreate
  # vucreate may regenerate the script: check again before vurun sends it
  # to the virtual users.
  ensure_patched
  set jobid [ vurun ]
  puts "JOB $jobid COMPLETE FOR $vu THREAD(S)"
  vudestroy
  # We pause between runs so the server settles (purge, flushing) before the next one.
  after 30000
}

puts "TEST COMPLETE"
