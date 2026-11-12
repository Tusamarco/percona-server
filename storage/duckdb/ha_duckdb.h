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

#ifndef STORAGE_DUCKDB_HA_DUCKDB_H_
#define STORAGE_DUCKDB_HA_DUCKDB_H_

#include "my_base.h"
#include "sql/handler.h"
#include "sql/partitioning/partition_handler.h"
#include "thr_lock.h"

class THD;
struct TABLE;
struct TABLE_SHARE;

namespace dd {
class Table;
}

namespace duckdb_se {

/**
  We do not implement partitioning, but we support the Partition_handler
  interface to access tables that are partitioned in the primary engine
  (same approach as the MOCK engine).
*/
class FakePartitionHandler : public Partition_handler {
 public:
  explicit FakePartitionHandler(const handler *engine) : m_engine(engine) {}

  void get_dynamic_partition_info(ha_statistics *stat_info,
                                  ha_checksum *check_sum,
                                  uint part_id) override {
    auto *primary_part_handler =
        m_engine->ha_get_primary_handler()->get_partition_handler();
    return primary_part_handler->get_dynamic_partition_info(stat_info,
                                                            check_sum, part_id);
  }
  void set_part_info(partition_info *, bool) override {}
  enum row_type get_partition_row_type(const dd::Table *, uint) override {
    return ROW_TYPE_NOT_USED;
  }

 private:
  const handler *m_engine;
};

/**
  Handler of the DuckDB secondary engine.
  In this step tables cannot be loaded yet (DWL#3) and no query is offloaded
  (DWL#6).
*/
class ha_duckdb : public handler {
 public:
  ha_duckdb(handlerton *hton, TABLE_SHARE *table_share);

  Partition_handler *get_partition_handler() override {
    return &m_part_handler;
  }

 private:
  int create(const char *, TABLE *, HA_CREATE_INFO *, dd::Table *) override {
    return HA_ERR_WRONG_COMMAND;
  }
  int open(const char *name, int mode, unsigned int test_if_locked,
           const dd::Table *table_def) override;
  int close() override { return 0; }
  int rnd_init(bool) override { return 0; }
  int rnd_next(unsigned char *) override { return HA_ERR_END_OF_FILE; }
  int rnd_pos(unsigned char *, unsigned char *) override {
    return HA_ERR_WRONG_COMMAND;
  }
  int info(unsigned int) override;
  ha_rows records_in_range(unsigned int index, key_range *min_key,
                           key_range *max_key) override;
  void position(const unsigned char *) override {}
  unsigned long index_flags(unsigned int, unsigned int, bool) const override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;
  Table_flags table_flags() const override;
  const char *table_type() const override { return "DUCKDB"; }

  int load_table(const TABLE &table, bool *skip_metadata_update) override;
  int unload_table(const char *db_name, const char *table_name,
                   bool error_if_not_loaded) override;

  THR_LOCK_DATA m_lock;
  FakePartitionHandler m_part_handler;
};

}  // namespace duckdb_se

#endif  // STORAGE_DUCKDB_HA_DUCKDB_H_
