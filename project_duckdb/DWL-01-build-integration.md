# DWL#1: Build integration of DuckDB, DuckLake and CRoaring

Affects: Percona Server 9.7 — Status: Design, under review

## Description

### GENERAL IDEA

We build DuckDB, its extensions, the DuckLake extension and the CRoaring library
from their upstream sources as part of the normal Percona Server build: one
`cmake` and one `make` or `ninja` run, with the same compiler and the same C++
standard (C++20) as the server. The result is a set of static libraries and
headers that the plugin (DWL#2) links into `ha_duckdb.so`.

### UPSTREAM SOURCES

Three git submodules, taken from the original upstream repositories (no Percona
fork):

| Path | Repository | Pin |
|---|---|---|
| `extra/duckdb/duckdb` | https://github.com/duckdb/duckdb.git | tag v2.0.0 (d54cbbe3df17) |
| `extra/duckdb/ducklake` | https://github.com/duckdb/ducklake.git | commit 55cc50cf080f (its `.github/duckdb-version` is d54cbbe) |
| `extra/duckdb/croaring` | https://github.com/RoaringBitmap/CRoaring.git | tag v5.5.5 |

CRoaring is required by DuckLake for deletion vectors; upstream gets it through
vcpkg, which we do not use.

We do not patch upstream code unless a bug is a hard stopper; then we prefer an
upstream fix and a pin move, and only as a last resort a patch file applied at
build time.

### WHAT WE BUILD AND WHAT WE SKIP

From DuckDB: `src/`, `third_party/`, and the extensions `core_functions`,
`parquet`, `json`, `icu`. Not built: the shell, unit tests, benchmarks, jemalloc,
the HTTP client, runtime extension loading. From DuckLake: `src/` and its
CMakeLists; its own `duckdb` and `extension-ci-tools` submodules are not used.

### VERIFICATION ALREADY DONE

On 2026-11-05, with GCC 15.3 on Fedora 43, outside the tree (/opt/duckdb_tmp):
all 480 C++ files compiled with `-std=c++20` and no errors, and a statically
linked C++20 smoke test attached a DuckLake, appended 100,000 rows through the
Appender and read them back. Commands and results are in the design document,
Appendix B.

## Requirements

### Functional Requirements

1. DuckDB, DuckLake and CRoaring are built in the same CMake configure and build
   run as the server, as dependencies of the plugin target.
2. All C++ sources of the three components are compiled with the C++ standard of
   the server build (C++20), including components that force another standard in
   their own CMake files (DuckLake, DuckDB's bundled re2).
3. The components are built with the same C and C++ compilers, build type and
   sanitizer settings as the server. The DuckDB build type can be overridden with
   `DUCKDB_BUILD_TYPE`.
4. The build produces position-independent static libraries and headers only;
   nothing is installed into the server installation tree.
5. The extensions core_functions, parquet, json, icu and ducklake are statically
   linked and available without runtime loading.
6. Runtime extension loading and the built-in HTTP client are not compiled in.
7. A change in a submodule source is picked up by the next incremental build.
8. The build never accesses the network.
9. The feature can be disabled at configure time with `-DWITH_DUCKDB=OFF`
   (proposal: default ON on this branch; to be decided).
10. Source release tarballs include the three submodules.
11. The SBOM lists the three components with their pins and licenses.

### Non-Functional Requirements

1. The server's warning settings (`-Werror` in maintainer mode) do not apply to
   upstream sources; upstream warnings never break the server build.
2. An incremental build with no change in the submodules adds only a few seconds.
3. The DuckDB build directory needs about 6 GB of disk; a full build takes about
   65 minutes with 6 jobs on the reference machine. ccache is supported.
4. Debug information of DuckDB sources is usable from gdb and IDEs together with
   server sources.

## Dependent Tasks

None. All other DWLs depend on this one.

## High Level Architecture

### Contents

1. Submodules
2. Extension configuration
3. ExternalProject wiring
4. Imported targets
5. Developer tooling

### Submodules

The three repositories are added under `extra/duckdb/`, pinned as listed in the
Description. The DuckDB submodule is marked `shallow = true`. Developers clone
without `--recursive`, so DuckLake's own submodules are not fetched.

### Extension configuration

`extra/duckdb/extension_config.cmake` (ours) tells DuckDB which extensions to
build and link statically, and loads DuckLake from our submodule with
`duckdb_extension_load(ducklake SOURCE_DIR ...)`. At the end of configuration it
sets `CXX_STANDARD` on every target of the DuckDB project and removes hard-coded
`-std=c++NN` options, and it excludes DuckDB's `clangd_cache` helper from the
default build. No upstream file is modified.

### ExternalProject wiring

`cmake/duckdb.cmake` defines two ExternalProjects:

```
croaring_ext  ──►  duckdb_ext  ──►  ext::duckdb (imported)  ──►  duckdb plugin
```

Both are configured at build time with options forwarded from the server build,
and installed into `${CMAKE_BINARY_DIR}/extra/duckdb/install`.

### Imported targets

Because the installed DuckDB CMake package exists only after the build, we
declare imported static targets ourselves, with `BUILD_BYPRODUCTS` for Ninja.

### Developer tooling

A post-build step merges DuckDB's `compile_commands.json` into the top-level one,
so clangd and IDE navigation cover DuckDB sources.

## Low Level Design

### Files

| File | Content |
|---|---|
| `.gitmodules` | Three new entries; DuckDB with `shallow = true` |
| `extra/duckdb/extension_config.cmake` | Extension list, C++ standard enforcement, clangd_cache exclusion (text in design doc Appendix B) |
| `extra/duckdb/CMakeLists.txt` | Includes `cmake/duckdb.cmake` when `WITH_DUCKDB` is ON |
| `cmake/duckdb.cmake` | ExternalProjects, imported targets, compile_commands merge |
| `storage/duckdb/CMakeLists.txt` | Plugin target linking `ext::duckdb` (DWL#2) |

### croaring_ext options

`CMAKE_C_COMPILER`, `CMAKE_CXX_COMPILER`, `CMAKE_BUILD_TYPE` (from
`DUCKDB_BUILD_TYPE`), `CMAKE_CXX_STANDARD=20`, `CMAKE_POSITION_INDEPENDENT_CODE=ON`,
`BUILD_SHARED_LIBS=OFF`, `ENABLE_ROARING_TESTS=OFF`, `ROARING_USE_CPM=OFF`,
`CMAKE_INSTALL_PREFIX=<install>/croaring`, compiler launcher and sanitizer flags
from the server build.

### duckdb_ext options

| Option | Value | Reason |
|---|---|---|
| `CMAKE_CXX_STANDARD` | 20 (from the server build) | Same standard everywhere |
| `CMAKE_BUILD_TYPE` | `DUCKDB_BUILD_TYPE` | Default: server build type; DuckDB debug assertions are slow |
| `CMAKE_POSITION_INDEPENDENT_CODE` | ON | Linked into a shared plugin |
| `CMAKE_EXPORT_COMPILE_COMMANDS` | ON | IDE support |
| `BUILD_SHELL`, `BUILD_UNITTESTS`, `ENABLE_UNITTEST_CPP_TESTS`, `BUILD_BENCHMARKS` | OFF | Not needed |
| `ENABLE_SANITIZER`, `ENABLE_UBSAN` | follow `WITH_ASAN`, `WITH_UBSAN` | Upstream default is ON |
| `ENABLE_JEMALLOC` | OFF | No second allocator inside mysqld |
| `ENABLE_EXTENSION_LOAD`, `ENABLE_BUILTIN_HTTPLIB` | OFF | No runtime loading, no HTTP client |
| `OVERRIDE_GIT_DESCRIBE` | v2.0.0 | Correct version string with a shallow clone (to verify) |
| `DUCKDB_EXTENSION_CONFIGS` | our config file | CMake variable name in DuckDB 2.0 (`EXTENSION_CONFIGS` is ignored by CMake) |
| `DUCKLAKE_SOURCE_DIR` | `extra/duckdb/ducklake` | Used by our config |
| `CMAKE_PREFIX_PATH` | CRoaring install directory | DuckLake's `find_package(roaring)` |

`BUILD_ALWAYS ON` so DuckDB's own incremental build runs on every build.

### Link set

Verified in the smoke test, in this order: `libducklake_extension.a`,
`libparquet_extension.a`, `libjson_extension.a`, `libicu_extension.a`,
`libcore_functions_extension.a`, `libduckdb_static.a` (contains the bundled
third-party libraries), `libroaring.a`, `-ldl`, `-lpthread`. Library directory
is `lib64/` on Fedora; `cmake/duckdb.cmake` must handle `lib/` and `lib64/`.
Headers: `duckdb.h`, `duckdb_static_extension.h`.

### Open points

- Clang build not yet verified (Phase 2).
- Two ICU copies (server ICU 77 and DuckDB's icu extension) in one process: to
  test with hidden visibility (DWL#2).
- `WITH_DUCKDB` default and packaging: to be decided.

## Test Plan

1. Configure and build the server from a clean build directory with GCC; the
   plugin target builds DuckDB, DuckLake and CRoaring first.
2. Check `compile_commands.json`: every C++ file of the three components has
   `-std=c++20` and no other `-std` option.
3. Touch a DuckDB source file; an incremental build recompiles it and relinks the
   plugin.
4. Build with `WITH_ASAN=ON`; DuckDB objects carry `-fsanitize=address`.
5. Build with `-DWITH_DUCKDB=OFF`; no DuckDB target is configured.
6. Build without network access.
7. Run the smoke test program against the installed libraries (design doc
   Appendix B).
