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

# DuckDB extension configuration for the DuckDB secondary engine.
# DuckDB includes this file while it configures (DUCKDB_EXTENSION_CONFIGS).

# We build only the extensions the plugin needs and link them statically.
duckdb_extension_load(core_functions)
duckdb_extension_load(parquet)
duckdb_extension_load(json)
duckdb_extension_load(icu)
duckdb_extension_load(ducklake SOURCE_DIR ${DUCKLAKE_SOURCE_DIR})
duckdb_extension_statically_link(core_functions parquet json icu ducklake)

# We compile every DuckDB component with the C++ standard of the server build.
# Some components force their own: DuckLake sets CMAKE_CXX_STANDARD 17 and the
# bundled re2 adds -std=c++17. At the end of configuration we set the standard on
# every target and drop hard-coded -std options. No upstream file is modified.
IF(NOT PS_DUCKDB_CXX_STANDARD)
  MESSAGE(FATAL_ERROR "PS_DUCKDB_CXX_STANDARD must be set by cmake/duckdb.cmake")
ENDIF()

FUNCTION(ps_duckdb_force_std dir)
  GET_PROPERTY(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  FOREACH(t IN LISTS targets)
    GET_TARGET_PROPERTY(type ${t} TYPE)
    IF(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
      CONTINUE()
    ENDIF()
    SET_TARGET_PROPERTIES(${t} PROPERTIES
      CXX_STANDARD ${PS_DUCKDB_CXX_STANDARD}
      CXX_STANDARD_REQUIRED ON)
    GET_TARGET_PROPERTY(opts ${t} COMPILE_OPTIONS)
    IF(opts)
      LIST(FILTER opts EXCLUDE REGEX "^-std=(c|gnu)\\+\\+[0-9]+$")
      SET_TARGET_PROPERTIES(${t} PROPERTIES COMPILE_OPTIONS "${opts}")
    ENDIF()
  ENDFOREACH()
  GET_PROPERTY(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  FOREACH(sd IN LISTS subdirs)
    ps_duckdb_force_std("${sd}")
  ENDFOREACH()
ENDFUNCTION()

FUNCTION(ps_duckdb_finalize)
  ps_duckdb_force_std("${CMAKE_SOURCE_DIR}")
  # DuckDB copies compile_commands.json to <build>/../../.cache/clangd on every
  # build. We merge compile commands in the server build instead.
  IF(TARGET clangd_cache)
    SET_TARGET_PROPERTIES(clangd_cache PROPERTIES EXCLUDE_FROM_ALL ON)
  ENDIF()
ENDFUNCTION()

CMAKE_LANGUAGE(DEFER DIRECTORY ${CMAKE_SOURCE_DIR} CALL ps_duckdb_finalize)
