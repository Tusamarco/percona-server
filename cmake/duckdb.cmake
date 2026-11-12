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

# We build CRoaring, DuckDB and DuckLake from the submodules in extra/duckdb as
# ExternalProjects, inside the server build, with the server's compilers, C++
# standard, build type and sanitizer settings. The result is the interface
# target ext::duckdb that the plugin links.

INCLUDE(ExternalProject)
FIND_PACKAGE(Threads REQUIRED)

SET(DUCKDB_SOURCE_ROOT  ${CMAKE_SOURCE_DIR}/extra/duckdb)
SET(DUCKDB_BINARY_ROOT  ${CMAKE_BINARY_DIR}/extra/duckdb)
SET(DUCKDB_INSTALL_ROOT ${DUCKDB_BINARY_ROOT}/install)

# Version string reported by DuckDB. The submodule is a shallow clone, so git
# describe cannot find the tag. We keep this in sync with the submodule pin.
SET(DUCKDB_GIT_DESCRIBE "v2.0.0")

FOREACH(sub duckdb ducklake croaring)
  IF(NOT EXISTS ${DUCKDB_SOURCE_ROOT}/${sub}/CMakeLists.txt)
    MESSAGE(FATAL_ERROR
      "extra/duckdb/${sub} is missing. "
      "Run: git submodule update --init extra/duckdb/${sub}")
  ENDIF()
ENDFOREACH()

# Build type of the dependencies: the server one by default. DuckDB debug builds
# enable internal assertions and are slow, so we allow an override.
IF(NOT DUCKDB_BUILD_TYPE)
  IF(CMAKE_BUILD_TYPE)
    SET(_duckdb_default_build_type ${CMAKE_BUILD_TYPE})
  ELSE()
    SET(_duckdb_default_build_type RelWithDebInfo)
  ENDIF()
  SET(DUCKDB_BUILD_TYPE ${_duckdb_default_build_type} CACHE STRING
    "Build type for DuckDB, DuckLake and CRoaring")
ENDIF()

# We pass the server's sanitizer choice explicitly and keep DuckDB's own
# sanitizer options OFF (they default to ON upstream).
SET(_duckdb_san_flags "")
IF(WITH_ASAN)
  STRING(APPEND _duckdb_san_flags " -fsanitize=address -fno-omit-frame-pointer")
ENDIF()
IF(WITH_UBSAN)
  STRING(APPEND _duckdb_san_flags " -fsanitize=undefined")
ENDIF()
IF(WITH_TSAN)
  STRING(APPEND _duckdb_san_flags " -fsanitize=thread")
ENDIF()
IF(WITH_MSAN)
  MESSAGE(FATAL_ERROR "WITH_DUCKDB is not supported together with WITH_MSAN")
ENDIF()

SET(_duckdb_common_args
  -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
  -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
  -DCMAKE_BUILD_TYPE=${DUCKDB_BUILD_TYPE}
  -DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}
  -DCMAKE_CXX_STANDARD_REQUIRED=ON
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
  -DCMAKE_INSTALL_PREFIX=${DUCKDB_INSTALL_ROOT}
  -DCMAKE_INSTALL_LIBDIR=lib
  "-DCMAKE_C_FLAGS=${_duckdb_san_flags}"
  "-DCMAKE_CXX_FLAGS=${_duckdb_san_flags}"
)
IF(CMAKE_C_COMPILER_LAUNCHER)
  LIST(APPEND _duckdb_common_args
    -DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER})
ENDIF()
IF(CMAKE_CXX_COMPILER_LAUNCHER)
  LIST(APPEND _duckdb_common_args
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER})
ENDIF()

SET(_duckdb_libdir ${DUCKDB_INSTALL_ROOT}/lib)

ExternalProject_Add(croaring_ext
  SOURCE_DIR       ${DUCKDB_SOURCE_ROOT}/croaring
  BINARY_DIR       ${DUCKDB_BINARY_ROOT}/build/croaring
  INSTALL_DIR      ${DUCKDB_INSTALL_ROOT}
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND   ""
  CMAKE_ARGS
    ${_duckdb_common_args}
    -DBUILD_SHARED_LIBS=OFF
    -DENABLE_ROARING_TESTS=OFF
    -DROARING_USE_CPM=OFF
  BUILD_BYPRODUCTS ${_duckdb_libdir}/libroaring.a
)

# Link order matters for static libraries: extensions first, then the DuckDB
# core (which contains the bundled third-party libraries), then CRoaring.
SET(DUCKDB_STATIC_LIBS
  ducklake_extension
  parquet_extension
  json_extension
  icu_extension
  core_functions_extension
  duckdb_static
)
SET(_duckdb_byproducts)
FOREACH(lib ${DUCKDB_STATIC_LIBS})
  LIST(APPEND _duckdb_byproducts ${_duckdb_libdir}/lib${lib}.a)
ENDFOREACH()

ExternalProject_Add(duckdb_ext
  SOURCE_DIR       ${DUCKDB_SOURCE_ROOT}/duckdb
  BINARY_DIR       ${DUCKDB_BINARY_ROOT}/build/duckdb
  INSTALL_DIR      ${DUCKDB_INSTALL_ROOT}
  DOWNLOAD_COMMAND ""
  UPDATE_COMMAND   ""
  DEPENDS          croaring_ext
  CMAKE_ARGS
    ${_duckdb_common_args}
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DENABLE_UNITTEST_CPP_TESTS=OFF
    -DBUILD_BENCHMARKS=OFF
    -DENABLE_SANITIZER=OFF
    -DENABLE_UBSAN=OFF
    -DENABLE_THREAD_SANITIZER=OFF
    -DENABLE_JEMALLOC=OFF
    -DENABLE_EXTENSION_LOAD=OFF
    -DENABLE_BUILTIN_HTTPLIB=OFF
    -DOVERRIDE_GIT_DESCRIBE=${DUCKDB_GIT_DESCRIBE}
    -DDUCKDB_EXTENSION_CONFIGS=${DUCKDB_SOURCE_ROOT}/extension_config.cmake
    -DDUCKLAKE_SOURCE_DIR=${DUCKDB_SOURCE_ROOT}/ducklake
    -DPS_DUCKDB_CXX_STANDARD=${CMAKE_CXX_STANDARD}
    -DCMAKE_PREFIX_PATH=${DUCKDB_INSTALL_ROOT}
  BUILD_BYPRODUCTS ${_duckdb_byproducts}
)

# We do not rebuild the dependencies on every build. They are reconfigured and
# rebuilt (incrementally) when a submodule pin moves or our extension config
# changes; a change in the forwarded CMake arguments does the same through the
# ExternalProject stamps. The submodule HEAD files exist only in a git checkout;
# in a source tarball the pins cannot change, so there is nothing to track.
# To force a rebuild after editing DuckDB sources by hand, delete
# storage/duckdb/duckdb_ext-prefix/src/duckdb_ext-stamp/duckdb_ext-build in the
# build directory.
FUNCTION(duckdb_track_submodule ext sub)
  SET(head ${CMAKE_SOURCE_DIR}/.git/modules/extra/duckdb/${sub}/HEAD)
  IF(EXISTS ${head})
    ExternalProject_Add_StepDependencies(${ext} configure ${head})
  ENDIF()
ENDFUNCTION()
duckdb_track_submodule(croaring_ext croaring)
duckdb_track_submodule(duckdb_ext duckdb)
duckdb_track_submodule(duckdb_ext ducklake)
ExternalProject_Add_StepDependencies(duckdb_ext configure
  ${DUCKDB_SOURCE_ROOT}/extension_config.cmake)

# Imported static libraries. The include directory must exist at configure
# time, before the first build installs the headers.
FILE(MAKE_DIRECTORY ${DUCKDB_INSTALL_ROOT}/include)

SET(_duckdb_link_list)
FOREACH(lib ${DUCKDB_STATIC_LIBS} roaring)
  ADD_LIBRARY(ext_duckdb_${lib} STATIC IMPORTED GLOBAL)
  SET_TARGET_PROPERTIES(ext_duckdb_${lib} PROPERTIES
    IMPORTED_LOCATION ${_duckdb_libdir}/lib${lib}.a)
  LIST(APPEND _duckdb_link_list ext_duckdb_${lib})
ENDFOREACH()

ADD_LIBRARY(ext_duckdb INTERFACE)
TARGET_LINK_LIBRARIES(ext_duckdb INTERFACE
  ${_duckdb_link_list} ${CMAKE_DL_LIBS} Threads::Threads)
TARGET_INCLUDE_DIRECTORIES(ext_duckdb SYSTEM INTERFACE
  ${DUCKDB_INSTALL_ROOT}/include)
ADD_DEPENDENCIES(ext_duckdb duckdb_ext)
ADD_LIBRARY(ext::duckdb ALIAS ext_duckdb)

# Merging DuckDB's compile commands into the server's compile_commands.json for
# clangd and IDE navigation is a later step.
