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

# DuckDB secondary engine. Built only with -DWITH_DUCKDB=ON.
OPTION(WITH_DUCKDB "Build the DuckDB secondary engine" OFF)

IF(NOT WITH_DUCKDB)
  MESSAGE(STATUS "Not building the DuckDB secondary engine "
                 "(use -DWITH_DUCKDB=ON to build it)")
  RETURN()
ENDIF()

MESSAGE(STATUS "Building the DuckDB secondary engine")
INCLUDE(${CMAKE_SOURCE_DIR}/cmake/duckdb.cmake)

# The plugin: ha_duckdb.so, loaded with plugin-load-add. VISIBILITY_HIDDEN hides
# our own symbols; --exclude-libs hides the symbols of the static DuckDB
# libraries, so they cannot clash with libraries the server also contains.
MYSQL_ADD_PLUGIN(duckdb
  ha_duckdb.cc
  duckdb_applier.cc
  duckdb_binlog_reader.cc
  duckdb_changes.cc
  duckdb_collation.cc
  duckdb_ddl.cc
  duckdb_executor.cc
  duckdb_guard.cc
  duckdb_lake.cc
  duckdb_load.cc
  duckdb_offload.cc
  duckdb_registry.cc
  duckdb_row_decoder.cc
  duckdb_session.cc
  duckdb_types.cc
  STORAGE_ENGINE
  MODULE_ONLY
  VISIBILITY_HIDDEN
  LINK_LIBRARIES ext::duckdb extra::rapidjson ext::zlib extra::unordered_dense
)
# The plugin uses server-internal headers (binlog reader, replication, GTID
# state), which declare their server-only parts under MYSQL_SERVER, as the
# MOCK engine does.
TARGET_COMPILE_DEFINITIONS(duckdb PRIVATE MYSQL_SERVER)
IF(LINUX)
  # We hide only the symbols of the DuckDB libraries. The server's service
  # library (libmysqlservices.a) must stay visible: mysqld binds the plugin
  # service pointers with dlsym() when it loads the plugin.
  SET(_duckdb_hidden_libs)
  FOREACH(lib ${DUCKDB_STATIC_LIBS} roaring)
    LIST(APPEND _duckdb_hidden_libs lib${lib}.a)
  ENDFOREACH()
  STRING(REPLACE ";" ":" _duckdb_hidden_libs "${_duckdb_hidden_libs}")
  TARGET_LINK_OPTIONS(duckdb PRIVATE
    "LINKER:--exclude-libs,${_duckdb_hidden_libs}")
ENDIF()

# Unit tests of the row decoder and of the collapse rules (DWL#4 step 4b).
# They link with mysys and strings only (gunit_small), not with the server.
IF(WITH_UNIT_TESTS)
  MYSQL_ADD_EXECUTABLE(duckdb_apply-t
    tests/duckdb_apply-t.cc
    duckdb_row_decoder.cc
    duckdb_collation.cc
    duckdb_changes.cc
    duckdb_ddl.cc
    ADD_TEST duckdb_apply
    LINK_LIBRARIES gunit_small
  )
  TARGET_INCLUDE_DIRECTORIES(duckdb_apply-t SYSTEM PRIVATE ${GMOCK_INCLUDE_DIRS})
ENDIF()

# We keep the build smoke test: it checks the DuckDB libraries alone.
MYSQL_ADD_EXECUTABLE(duckdb_build_smoke
  tests/duckdb_build_smoke.cc
  ADD_TEST duckdb_build_smoke
  LINK_LIBRARIES ext::duckdb
)
