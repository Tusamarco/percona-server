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

#ifndef STORAGE_DUCKDB_DUCKDB_LOG_H_
#define STORAGE_DUCKDB_DUCKDB_LOG_H_

// We write to the server error log through the log_builtins service, as other
// dynamic plugins do. LOG_COMPONENT_TAG must be defined before the include.
#define LOG_COMPONENT_TAG "duckdb"
#include <mysql/components/services/log_builtins.h>

#include "mysqld_error.h"

#define DUCKDB_LOG_ERROR(...) \
  LogPluginErrMsg(ERROR_LEVEL, ER_LOG_PRINTF_MSG, __VA_ARGS__)
#define DUCKDB_LOG_WARNING(...) \
  LogPluginErrMsg(WARNING_LEVEL, ER_LOG_PRINTF_MSG, __VA_ARGS__)
#define DUCKDB_LOG_INFO(...) \
  LogPluginErrMsg(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG, __VA_ARGS__)

#endif  // STORAGE_DUCKDB_DUCKDB_LOG_H_
