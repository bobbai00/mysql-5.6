/*
   Copyright (c) 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation  // gcc: Class implementation
#endif

#define MYSQL_SERVER 1

/* The C++ file's header */
#include "./ha_rocksdb.h"

#ifdef TARGET_OS_LINUX
#include <errno.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#endif

/* C++ standard header files */
#include <inttypes.h>
#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string>
#include <vector>

/* MySQL includes */
#include <mysql/psi/mysql_table.h>
#include <mysql/thread_pool_priv.h>
#include <mysys_err.h>
#include "./debug_sync.h"
#include "./my_bit.h"
#include "./my_stacktrace.h"
#include "./my_sys.h"
#include "./sql_audit.h"
#include "./sql_table.h"

/* RocksDB includes */
#include "monitoring/histogram.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/env.h"
#include "rocksdb/memory_allocator.h"
#include "rocksdb/persistent_cache.h"
#include "rocksdb/rate_limiter.h"
#include "rocksdb/slice_transform.h"
#include "rocksdb/thread_status.h"
#include "rocksdb/trace_reader_writer.h"
#include "rocksdb/utilities/checkpoint.h"
#include "rocksdb/utilities/convenience.h"
#include "rocksdb/utilities/memory_util.h"
#include "rocksdb/utilities/sim_cache.h"
#include "rocksdb/utilities/write_batch_with_index.h"
#include "util/stop_watch.h"

/* MyRocks includes */
#include "./event_listener.h"
#include "./ha_rocksdb_proto.h"
#include "./logger.h"
#include "./nosql_access.h"
#include "./rdb_cf_manager.h"
#include "./rdb_cf_options.h"
#include "./rdb_converter.h"
#include "./rdb_datadic.h"
#include "./rdb_i_s.h"
#include "./rdb_index_merge.h"
#include "./rdb_mutex_wrapper.h"
#include "./rdb_psi.h"
#include "./rdb_threads.h"

// Internal MySQL APIs not exposed in any header.
extern "C" {
/**
  Mark transaction to rollback and mark error as fatal to a sub-statement.
  @param  thd   Thread handle
  @param  all   TRUE <=> rollback main transaction.
*/
void thd_mark_transaction_to_rollback(MYSQL_THD thd, bool all);

/**
 *   Get the user thread's binary logging format
 *   @param thd  user thread
 *   @return Value to be used as index into the binlog_format_names array
 */
int thd_binlog_format(const MYSQL_THD thd);

/**
 *   Check if binary logging is filtered for thread's current db.
 *   @param  thd   Thread handle
 *   @retval 1 the query is not filtered, 0 otherwise.
 */
bool thd_binlog_filter_ok(const MYSQL_THD thd);
}

extern my_bool opt_core_file;

namespace myrocks_rpc {

static st_global_stats global_stats;
static st_export_stats export_stats;
static st_memory_stats memory_stats;
static st_io_stall_stats io_stall_stats;

const std::string DEFAULT_CF_NAME("default");
const std::string DEFAULT_SYSTEM_CF_NAME("__system__");
const std::string PER_INDEX_CF_NAME("$per_index_cf");
const std::string DEFAULT_SK_CF_NAME("default_sk");
const std::string TRUNCATE_TABLE_PREFIX("#truncate_tmp#");

static std::vector<std::string> rdb_tables_to_recalc;

static Rdb_exec_time st_rdb_exec_time;

class Rdb_explicit_snapshot : public explicit_snapshot {
 public:
  static std::shared_ptr<Rdb_explicit_snapshot> create(
      snapshot_info_st *ss_info, rocksdb::DB *db,
      const rocksdb::Snapshot *snapshot) {
    std::lock_guard<std::mutex> lock(explicit_snapshot_mutex);

    rocksdb_rpc_log(140, "Rdb_explicit_snapshot: rocksdb_NewManagedSnapshot");
    // ALTER
    // auto s = std::unique_ptr<rocksdb::ManagedSnapshot>(
    //     new rocksdb::ManagedSnapshot(db, snapshot));
    rocksdb::ManagedSnapshot *s = rocksdb_NewManagedSnapshot(db, snapshot);
    if (!s) {
      return nullptr;
    }
    ss_info->snapshot_id = ++explicit_snapshot_counter;

    rocksdb_rpc_log(
        150, "Rdb_explicit_snapshot: make_shared<Rdb_explicit_snapshot>");
    // ALTER
    // auto ret = std::make_shared<Rdb_explicit_snapshot>(*ss_info,
    // std::move(s));
    auto ret = std::make_shared<Rdb_explicit_snapshot>(*ss_info, s);

    if (!ret) {
      return nullptr;
    }
    explicit_snapshots[ss_info->snapshot_id] = ret;
    return ret;
  }

  static std::string dump_snapshots() {
    std::string str;
    std::lock_guard<std::mutex> lock(explicit_snapshot_mutex);
    for (const auto &elem : explicit_snapshots) {
      const auto &ss = elem.second.lock();
      DBUG_ASSERT(ss != nullptr);
      const auto &info = ss->ss_info;
      str += "\nSnapshot ID: " + std::to_string(info.snapshot_id) +
             "\nBinlog File: " + info.binlog_file +
             "\nBinlog Pos: " + std::to_string(info.binlog_pos) +
             "\nGtid Executed: " + info.gtid_executed + "\n";
    }

    return str;
  }

  static std::shared_ptr<Rdb_explicit_snapshot> get(
      const ulonglong snapshot_id) {
    std::lock_guard<std::mutex> lock(explicit_snapshot_mutex);
    auto elem = explicit_snapshots.find(snapshot_id);
    if (elem == explicit_snapshots.end()) {
      return nullptr;
    }
    return elem->second.lock();
  }

  rocksdb::ManagedSnapshot *get_snapshot() { return snapshot; }

  Rdb_explicit_snapshot(snapshot_info_st ss_info,
                        rocksdb::ManagedSnapshot *snap)
      : explicit_snapshot(ss_info), snapshot(snap) {}

  virtual ~Rdb_explicit_snapshot() {
    std::lock_guard<std::mutex> lock(explicit_snapshot_mutex);
    explicit_snapshots.erase(ss_info.snapshot_id);
  }

 private:
  // ALTER
  // std::unique_ptr<rocksdb::ManagedSnapshot> snapshot;
  rocksdb::ManagedSnapshot *snapshot;

  static std::mutex explicit_snapshot_mutex;
  static ulonglong explicit_snapshot_counter;
  static std::unordered_map<ulonglong, std::weak_ptr<Rdb_explicit_snapshot>>
      explicit_snapshots;
};

std::mutex Rdb_explicit_snapshot::explicit_snapshot_mutex;
ulonglong Rdb_explicit_snapshot::explicit_snapshot_counter = 0;
std::unordered_map<ulonglong, std::weak_ptr<Rdb_explicit_snapshot>>
    Rdb_explicit_snapshot::explicit_snapshots;

/**
  Updates row counters based on the table type and operation type.
*/
void ha_rocksdb::update_row_stats(const operation_type &type, ulonglong count) {
  DBUG_ASSERT(type < ROWS_MAX);
  // Find if we are modifying system databases.
  if (table->s && m_tbl_def->m_is_mysql_system_table) {
    global_stats.system_rows[type].add(count);
  } else {
    global_stats.rows[type].add(count);
  }
}

void ha_rocksdb::update_row_read(ulonglong count) {
  stats.rows_read += count;
  update_row_stats(ROWS_READ, count);
}

void ha_rocksdb::inc_covered_sk_lookup() {
  global_stats.covered_secondary_key_lookups.inc();
}

void dbug_dump_database(rocksdb::DB *db);
static handler *rocksdb_create_handler(my_core::handlerton *hton,
                                       my_core::TABLE_SHARE *table_arg,
                                       my_core::MEM_ROOT *mem_root);

static rocksdb::CompactRangeOptions getCompactRangeOptions(
    int concurrency = 0,
    rocksdb::BottommostLevelCompaction bottommost_level_compaction =
        rocksdb::BottommostLevelCompaction::kForceOptimized) {
  rocksdb::CompactRangeOptions compact_range_options;
  compact_range_options.bottommost_level_compaction =
      bottommost_level_compaction;
  compact_range_options.exclusive_manual_compaction = false;
  if (concurrency > 0) {
    compact_range_options.max_subcompactions = concurrency;
  }
  return compact_range_options;
}

///////////////////////////////////////////////////////////
// Parameters and settings
///////////////////////////////////////////////////////////
static char *rocksdb_default_cf_options = nullptr;
static char *rocksdb_override_cf_options = nullptr;
static char *rocksdb_update_cf_options = nullptr;
static my_bool rocksdb_use_default_sk_cf = false;

///////////////////////////////////////////////////////////
// Globals
///////////////////////////////////////////////////////////
handlerton *rocksdb_hton;

rocksdb::TransactionDB *rdb = nullptr;
rocksdb::HistogramImpl *commit_latency_stats = nullptr;

// ALTER
// static std::shared_ptr<rocksdb::Statistics> rocksdb_stats;
static rocksdb::Statistics *rocksdb_stats;

static std::unique_ptr<rocksdb::Env> flashcache_aware_env;
static std::shared_ptr<Rdb_tbl_prop_coll_factory> properties_collector_factory;

Rdb_dict_manager dict_manager;
Rdb_cf_manager cf_manager;
Rdb_ddl_manager ddl_manager;
Rdb_binlog_manager binlog_manager;
Rdb_io_watchdog *io_watchdog = nullptr;

/**
  MyRocks background thread control
  N.B. This is besides RocksDB's own background threads
       (@see rocksdb::CancelAllBackgroundWork())
*/

static Rdb_background_thread rdb_bg_thread;

static Rdb_index_stats_thread rdb_is_thread;

static Rdb_manual_compaction_thread rdb_mc_thread;

static Rdb_drop_index_thread rdb_drop_idx_thread;
// List of table names (using regex) that are exceptions to the strict
// collation check requirement.
Regex_list_handler *rdb_collation_exceptions;

static const char *rdb_get_error_message(int nr);

static void rocksdb_flush_all_memtables() {
  const Rdb_cf_manager &cf_manager = rdb_get_cf_manager();

  // RocksDB will fail the flush if the CF is deleted,
  // but here we don't handle return status
  for (const auto &cf_handle : cf_manager.get_all_cf()) {
    // ALTER
    // rdb->Flush(rocksdb::FlushOptions(), cf_handle.get());
    rocksdb_TransactionDB__Flush(rdb, rocksdb::FlushOptions(), cf_handle);
  }
}

static void rocksdb_delete_column_family_stub(
    THD *const /* thd */, struct st_mysql_sys_var *const /* var */,
    void *const /* var_ptr */, const void *const /* save */) {}

static int rocksdb_delete_column_family(
    THD *const /* thd */, struct st_mysql_sys_var *const /* var */,
    void *const /* var_ptr */, struct st_mysql_value *const value) {
  DBUG_ASSERT(value != nullptr);

  char buff[STRING_BUFFER_USUAL_SIZE];
  int len = sizeof(buff);

  const char *const cf = value->val_str(value, buff, &len);
  if (cf == nullptr) return HA_EXIT_SUCCESS;

  std::string cf_name = std::string(cf);
  // Forbid to remove these built-in CFs
  if (cf_name == DEFAULT_SYSTEM_CF_NAME || cf_name == DEFAULT_CF_NAME ||
      cf_name.empty() ||
      (cf_name == DEFAULT_SK_CF_NAME && rocksdb_use_default_sk_cf)) {
    my_error(ER_CANT_DROP_CF, MYF(0), cf);
    return HA_EXIT_FAILURE;
  }

  auto &cf_manager = rdb_get_cf_manager();
  int ret = 0;

  {
    std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
    ret = cf_manager.drop_cf(&ddl_manager, &dict_manager, cf_name);
  }

  if (ret == HA_EXIT_SUCCESS) {
    rdb_drop_idx_thread.signal();
  } else {
    my_error(ER_CANT_DROP_CF, MYF(0), cf);
  }

  return ret;
}

///////////////////////////////////////////////////////////
// Hash map: table name => open table handler
///////////////////////////////////////////////////////////

namespace  // anonymous namespace = not visible outside this source file
{

const ulong TABLE_HASH_SIZE = 32;

class Rdb_open_tables_map {
 private:
  /* Hash table used to track the handlers of open tables */
  std::unordered_map<std::string, Rdb_table_handler *> m_table_map;

  /* The mutex used to protect the hash table */
  mutable mysql_mutex_t m_mutex;

 public:
  void init() {
    m_table_map.clear();
    mysql_mutex_init(rdb_psi_open_tbls_mutex_key, &m_mutex, MY_MUTEX_INIT_FAST);
  }

  void free() {
    m_table_map.clear();
    mysql_mutex_destroy(&m_mutex);
  }

  size_t count() { return m_table_map.size(); }

  Rdb_table_handler *get_table_handler(const char *const table_name);
  void release_table_handler(Rdb_table_handler *const table_handler);

  std::vector<std::string> get_table_names(void) const;
};

}  // anonymous namespace

static Rdb_open_tables_map rdb_open_tables;

static std::string rdb_normalize_dir(std::string dir) {
  while (dir.size() > 0 && dir.back() == '/') {
    dir.resize(dir.size() - 1);
  }
  return dir;
}

static int rocksdb_create_checkpoint(
    THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const save MY_ATTRIBUTE((__unused__)),
    struct st_mysql_value *const value) {
  char buf[FN_REFLEN];
  int len = sizeof(buf);
  const char *const checkpoint_dir_raw = value->val_str(value, buf, &len);
  if (checkpoint_dir_raw) {
    if (rdb != nullptr) {
      std::string checkpoint_dir = rdb_normalize_dir(checkpoint_dir_raw);
      // NO_LINT_DEBUG
      sql_print_information("RocksDB: creating checkpoint in directory : %s\n",
                            checkpoint_dir.c_str());
      rocksdb::Checkpoint *checkpoint;

      // ALTER
      // auto status = rocksdb::Checkpoint::Create(rdb, &checkpoint);
      auto status = rocksdb_Checkpoint_Create(rdb, checkpoint);

      // We can only return HA_EXIT_FAILURE/HA_EXIT_SUCCESS here which is why
      // the return code is ignored, but by calling into rdb_error_to_mysql,
      // it will call my_error for us, which will propogate up to the client.
      int rc __attribute__((__unused__));
      if (status.ok()) {
        // ALTER
        // status = checkpoint->CreateCheckpoint(checkpoint_dir.c_str());
        status = rocksdb_Checkpoint__CreateCheckpoint(checkpoint,
                                                      checkpoint_dir.c_str());

        // ALTER
        // delete checkpoint;
        rocksdb_Checkpoint__delete(checkpoint);

        if (status.ok()) {
          // NO_LINT_DEBUG
          sql_print_information(
              "RocksDB: created checkpoint in directory : %s\n",
              checkpoint_dir.c_str());
          return HA_EXIT_SUCCESS;
        } else {
          rc = ha_rocksdb::rdb_error_to_mysql(status);
        }
      } else {
        rc = ha_rocksdb::rdb_error_to_mysql(status);
      }
    }
  }
  return HA_EXIT_FAILURE;
}

/* This method is needed to indicate that the
   ROCKSDB_CREATE_CHECKPOINT command is not read-only */
static void rocksdb_create_checkpoint_stub(THD *const thd,
                                           struct st_mysql_sys_var *const var,
                                           void *const var_ptr,
                                           const void *const save) {}

static void rocksdb_select_bypass_rejected_query_history_size_update(
    THD *const thd, struct st_mysql_sys_var *const /* unused */,
    void *const var_ptr, const void *const save);

static void rocksdb_force_flush_memtable_now_stub(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save) {}

static int rocksdb_force_flush_memtable_now(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    struct st_mysql_value *const value) {
  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Manual memtable flush.");
  rocksdb_flush_all_memtables();
  return HA_EXIT_SUCCESS;
}

static void rocksdb_force_flush_memtable_and_lzero_now_stub(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save) {}

static int rocksdb_force_flush_memtable_and_lzero_now(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    struct st_mysql_value *const value) {
  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Manual memtable and L0 flush.");
  rocksdb_flush_all_memtables();

  const Rdb_cf_manager &cf_manager = rdb_get_cf_manager();
  rocksdb::CompactionOptions c_options = rocksdb::CompactionOptions();
  rocksdb::ColumnFamilyMetaData metadata;
  // ALTER
  // rocksdb::ColumnFamilyDescriptor cf_descr;
  rocksdb::ColumnFamilyDescriptor *cf_descr;

  int i, max_attempts = 3, num_errors = 0;

  for (const auto &cf_handle : cf_manager.get_all_cf()) {
    for (i = 0; i < max_attempts; i++) {
      // ALTER
      // rdb->GetColumnFamilyMetaData(cf_handle.get(), &metadata);
      rocksdb_TransactionDB__GetColumnFamilyMetaData(rdb, cf_handle, metadata);

      // ALTER
      // cf_handle->GetDescriptor(&cf_descr);
      rocksdb_ColumnFamilyHandle__GetDescriptorPtr(cf_handle, cf_descr);

      rocksdb::ColumnFamilyOptions *opt =
          rocksdb_ColumnFamilyDescriptor__Options(cf_descr);

      // ALTER
      // c_options.output_file_size_limit =
      // cf_descr.options.target_file_size_base;
      c_options.output_file_size_limit =
          rocksdb_ColumnFamilyOptions__GetUInt64Prop(opt,
                                                     "target_file_size_base");

      DBUG_ASSERT(metadata.levels[0].level == 0);
      std::vector<std::string> file_names;
      for (auto &file : metadata.levels[0].files) {
        file_names.emplace_back(file.db_path + file.name);
      }

      if (file_names.empty()) {
        break;
      }

      rocksdb::Status s;
      // ALTER
      // s = rdb->CompactFiles(c_options, cf_handle.get(), file_names, 1);
      s = rocksdb_TransactionDB__CompactFiles(rdb, c_options, cf_handle,
                                              file_names, 1);

      if (!s.ok()) {
        // ALTER
        // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
        //     cf_manager.get_cf(cf_handle->GetID());
        rocksdb::ColumnFamilyHandle *cfh =
            cf_manager.get_cf(cf_handle->GetID());

        // If the CF handle has been removed from cf_manager, it is not an
        // error. We are done with this CF and proceed to the next CF.
        if (!cfh) {
          // NO_LINT_DEBUG
          // ALTER
          sql_print_information(
              "cf %s has been dropped during CompactFiles.",
              rocksdb_ColumnFamilyHandle__GetName(cfh).c_str());
          break;
        }

        // Due to a race, it's possible for CompactFiles to collide
        // with auto compaction, causing an error to return
        // regarding file not found. In that case, retry.
        if (s.IsInvalidArgument()) {
          continue;
        }

        if (!s.ok() && !s.IsAborted()) {
          rdb_handle_io_error(s, RDB_IO_ERROR_GENERAL);
          return HA_EXIT_FAILURE;
        }
        break;
      }
    }
    if (i == max_attempts) {
      num_errors++;
    }
  }

  return num_errors == 0 ? HA_EXIT_SUCCESS : HA_EXIT_FAILURE;
}

static void rocksdb_cancel_manual_compactions_stub(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save) {}

static int rocksdb_cancel_manual_compactions(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    struct st_mysql_value *const value) {
  rdb_mc_thread.cancel_all_pending_manual_compaction_requests();
  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Stopping all Manual Compactions.");
  // ALTER
  // rdb->GetBaseDB()->DisableManualCompaction();
  rocksdb_DB__DisableManualCompaction(rocksdb_TransactionDB__GetBaseDB(rdb));

  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Enabling Manual Compactions.");

  // ALTER
  // rdb->GetBaseDB()->EnableManualCompaction();
  rocksdb_DB__EnableManualCompaction(rocksdb_TransactionDB__GetBaseDB(rdb));

  return HA_EXIT_SUCCESS;
}

static void rocksdb_drop_index_wakeup_thread(
    my_core::THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save);

static my_bool rocksdb_pause_background_work = 0;
static mysql_mutex_t rdb_sysvars_mutex;
static mysql_mutex_t rdb_block_cache_resize_mutex;
static mysql_mutex_t rdb_bottom_pri_background_compactions_resize_mutex;

static void rocksdb_set_pause_background_work(
    my_core::THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);
  const bool pause_requested = *static_cast<const bool *>(save);
  if (rocksdb_pause_background_work != pause_requested) {
    if (pause_requested) {
      // ALTER
      // rdb->PauseBackgroundWork();
      rocksdb_TransactionDB__PauseBackgroundWork(rdb);
    } else {
      // ALTER
      // rdb->ContinueBackgroundWork();
      rocksdb_TransactionDB__ContinueBackgroundWork(rdb);
    }
    rocksdb_pause_background_work = pause_requested;
  }
  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

static void rocksdb_set_compaction_options(THD *thd,
                                           struct st_mysql_sys_var *var,
                                           void *var_ptr, const void *save);

static void rocksdb_set_table_stats_sampling_pct(THD *thd,
                                                 struct st_mysql_sys_var *var,
                                                 void *var_ptr,
                                                 const void *save);

static void rocksdb_update_table_stats_use_table_scan(
    THD *const /* thd */, struct st_mysql_sys_var *const /* var */,
    void *const var_ptr, const void *const save);

static int rocksdb_index_stats_thread_renice(
    THD *const /* thd */, struct st_mysql_sys_var *const /* var */,
    void *const save, struct st_mysql_value *const value);

static void rocksdb_set_rate_limiter_bytes_per_sec(THD *thd,
                                                   struct st_mysql_sys_var *var,
                                                   void *var_ptr,
                                                   const void *save);

static void rocksdb_set_sst_mgr_rate_bytes_per_sec(THD *thd,
                                                   struct st_mysql_sys_var *var,
                                                   void *var_ptr,
                                                   const void *save);

static void rocksdb_set_delayed_write_rate(THD *thd,
                                           struct st_mysql_sys_var *var,
                                           void *var_ptr, const void *save);

static void rocksdb_set_max_latest_deadlocks(THD *thd,
                                             struct st_mysql_sys_var *var,
                                             void *var_ptr, const void *save);

static void rdb_set_collation_exception_list(const char *exception_list);
static void rocksdb_set_collation_exception_list(THD *thd,
                                                 struct st_mysql_sys_var *var,
                                                 void *var_ptr,
                                                 const void *save);

static int rocksdb_validate_update_cf_options(THD *thd,
                                              struct st_mysql_sys_var *var,
                                              void *save,
                                              st_mysql_value *value);

static void rocksdb_set_update_cf_options(THD *thd,
                                          struct st_mysql_sys_var *var,
                                          void *var_ptr, const void *save);

static int rocksdb_check_bulk_load(
    THD *const thd, struct st_mysql_sys_var *var MY_ATTRIBUTE((__unused__)),
    void *save, struct st_mysql_value *value);

static int rocksdb_check_bulk_load_allow_unsorted(
    THD *const thd, struct st_mysql_sys_var *var MY_ATTRIBUTE((__unused__)),
    void *save, struct st_mysql_value *value);

static void rocksdb_set_max_background_jobs(THD *thd,
                                            struct st_mysql_sys_var *const var,
                                            void *const var_ptr,
                                            const void *const save);
static void rocksdb_set_max_background_compactions(
    THD *thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save);

static void rocksdb_set_bytes_per_sync(THD *thd,
                                       struct st_mysql_sys_var *const var,
                                       void *const var_ptr,
                                       const void *const save);
static void rocksdb_set_wal_bytes_per_sync(THD *thd,
                                           struct st_mysql_sys_var *const var,
                                           void *const var_ptr,
                                           const void *const save);
static int rocksdb_validate_set_block_cache_size(
    THD *thd, struct st_mysql_sys_var *const var, void *var_ptr,
    struct st_mysql_value *value);
static int rocksdb_tracing(THD *const thd MY_ATTRIBUTE((__unused__)),
                           struct st_mysql_sys_var *const var, void *const save,
                           struct st_mysql_value *const value,
                           bool trace_block_cache_access = true);
static int rocksdb_validate_max_bottom_pri_background_compactions(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *var_ptr, struct st_mysql_value *value);

static int check_rocksdb_skip_locks_if_skip_unique_check(
    THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var, void *const save,
    struct st_mysql_value *const value);
//////////////////////////////////////////////////////////////////////////////
// Options definitions
//////////////////////////////////////////////////////////////////////////////
static long long rocksdb_block_cache_size;
static long long rocksdb_sim_cache_size;
static my_bool rocksdb_use_clock_cache;
static double rocksdb_cache_high_pri_pool_ratio;
static my_bool rocksdb_cache_dump;
/* Use unsigned long long instead of uint64_t because of MySQL compatibility */
static unsigned long long  // NOLINT(runtime/int)
    rocksdb_rate_limiter_bytes_per_sec;
static unsigned long long  // NOLINT(runtime/int)
    rocksdb_sst_mgr_rate_bytes_per_sec;
static unsigned long long rocksdb_delayed_write_rate;
static uint32_t rocksdb_max_latest_deadlocks;
static unsigned long  // NOLINT(runtime/int)
    rocksdb_persistent_cache_size_mb;
static uint64_t rocksdb_info_log_level;
static char *rocksdb_wal_dir;
static char *rocksdb_persistent_cache_path;
static uint64_t rocksdb_index_type;
static uint32_t rocksdb_flush_log_at_trx_commit;
static uint32_t rocksdb_debug_optimizer_n_rows;
static my_bool rocksdb_force_compute_memtable_stats;
static uint32_t rocksdb_force_compute_memtable_stats_cachetime;
static my_bool rocksdb_debug_optimizer_no_zero_cardinality;
static uint32_t rocksdb_wal_recovery_mode;
static my_bool rocksdb_track_and_verify_wals_in_manifest;
static uint32_t rocksdb_stats_level;
static uint32_t rocksdb_access_hint_on_compaction_start;
static char *rocksdb_compact_cf_name;
static char *rocksdb_delete_cf_name;
static char *rocksdb_checkpoint_name;
static char *rocksdb_block_cache_trace_options_str;
static char *rocksdb_trace_options_str;
static my_bool rocksdb_signal_drop_index_thread;
static my_bool rocksdb_strict_collation_check = 1;
static my_bool rocksdb_ignore_unknown_options = 1;
static my_bool rocksdb_enable_2pc = 0;
static char *rocksdb_strict_collation_exceptions;
static my_bool rocksdb_collect_sst_properties = 0;
static my_bool rocksdb_force_flush_memtable_now_var = 0;
static my_bool rocksdb_force_flush_memtable_and_lzero_now_var = 0;
static my_bool rocksdb_cancel_manual_compactions_var = 0;
static my_bool rocksdb_enable_ttl = 0;
static my_bool rocksdb_enable_ttl_read_filtering = 1;
static int rocksdb_debug_ttl_rec_ts = 0;
static int rocksdb_debug_ttl_snapshot_ts = 0;
static int rocksdb_debug_ttl_read_filter_ts = 0;
static my_bool rocksdb_debug_ttl_ignore_pk = 0;
static my_bool rocksdb_reset_stats = 0;
static uint32_t rocksdb_io_write_timeout_secs = 0;
static uint32_t rocksdb_seconds_between_stat_computes = 3600;
static long long rocksdb_compaction_sequential_deletes = 0l;
static long long rocksdb_compaction_sequential_deletes_window = 0l;
static long long rocksdb_compaction_sequential_deletes_file_size = 0l;
static uint32_t rocksdb_validate_tables = 1;
static char *rocksdb_datadir;
static uint32_t rocksdb_max_bottom_pri_background_compactions = 0;
static uint32_t rocksdb_table_stats_sampling_pct;
static uint32_t rocksdb_table_stats_recalc_threshold_pct = 10;
static unsigned long long rocksdb_table_stats_recalc_threshold_count = 100ul;
static my_bool rocksdb_table_stats_use_table_scan = 0;
static int32_t rocksdb_table_stats_background_thread_nice_value =
    THREAD_PRIO_MAX;
static unsigned long long rocksdb_table_stats_max_num_rows_scanned = 0ul;
static my_bool rocksdb_enable_bulk_load_api = 1;
static my_bool rocksdb_enable_remove_orphaned_dropped_cfs = 1;
static my_bool rocksdb_print_snapshot_conflict_queries = 0;
static my_bool rocksdb_large_prefix = 0;
static my_bool rocksdb_allow_to_start_after_corruption = 0;
static uint64_t rocksdb_write_policy =
    rocksdb::TxnDBWritePolicy::WRITE_COMMITTED;
char *rocksdb_read_free_rpl_tables;
ulong rocksdb_max_row_locks;
std::mutex rocksdb_read_free_rpl_tables_mutex;
#if defined(HAVE_PSI_INTERFACE)
Regex_list_handler rdb_read_free_regex_handler(key_rwlock_read_free_rpl_tables);
#else
Regex_list_handler rdb_read_free_regex_handler;
#endif
enum read_free_rpl_type { OFF = 0, PK_ONLY, PK_SK };
static uint64_t rocksdb_read_free_rpl = read_free_rpl_type::OFF;
static my_bool rocksdb_error_on_suboptimal_collation = 1;
static uint32_t rocksdb_stats_recalc_rate = 0;
static uint32_t rocksdb_debug_manual_compaction_delay = 0;
static uint32_t rocksdb_max_manual_compactions = 0;
static my_bool rocksdb_rollback_on_timeout = FALSE;
static my_bool rocksdb_enable_insert_with_update_caching = TRUE;
static uint64_t rocksdb_select_bypass_policy =
    select_bypass_policy_type::default_value;
static my_bool rocksdb_select_bypass_fail_unsupported = TRUE;
static my_bool rocksdb_select_bypass_log_rejected = TRUE;
static my_bool rocksdb_select_bypass_log_failed = FALSE;
static my_bool rocksdb_select_bypass_allow_filters = TRUE;
static uint32_t rocksdb_select_bypass_rejected_query_history_size = 0;
static uint32_t rocksdb_select_bypass_debug_row_delay = 0;
static unsigned long long  // NOLINT(runtime/int)
    rocksdb_select_bypass_multiget_min = 0;
static my_bool rocksdb_skip_locks_if_skip_unique_check = FALSE;
static my_bool rocksdb_alter_column_default_inplace = FALSE;
std::atomic<uint64_t> rocksdb_row_lock_deadlocks(0);
std::atomic<uint64_t> rocksdb_row_lock_wait_timeouts(0);
std::atomic<uint64_t> rocksdb_snapshot_conflict_errors(0);
std::atomic<uint64_t> rocksdb_wal_group_syncs(0);
std::atomic<uint64_t> rocksdb_manual_compactions_processed(0);
std::atomic<uint64_t> rocksdb_manual_compactions_cancelled(0);
std::atomic<uint64_t> rocksdb_manual_compactions_running(0);
std::atomic<uint64_t> rocksdb_manual_compactions_pending(0);
#ifndef DBUG_OFF
std::atomic<uint64_t> rocksdb_num_get_for_update_calls(0);
#endif
std::atomic<uint64_t> rocksdb_select_bypass_executed(0);
std::atomic<uint64_t> rocksdb_select_bypass_rejected(0);
std::atomic<uint64_t> rocksdb_select_bypass_failed(0);

static int rocksdb_trace_block_cache_access(
    THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var, void *const save,
    struct st_mysql_value *const value) {
  return rocksdb_tracing(thd, var, save, value,
                         /* trace_block_cache_accecss = */ true);
}

static int rocksdb_trace_queries(THD *const thd MY_ATTRIBUTE((__unused__)),
                                 struct st_mysql_sys_var *const var,
                                 void *const save,
                                 struct st_mysql_value *const value) {
  return rocksdb_tracing(thd, var, save, value,
                         /* trace_block_cache_accecss = */ false);
}

static int rocksdb_tracing(THD *const thd MY_ATTRIBUTE((__unused__)),
                           struct st_mysql_sys_var *const var, void *const save,
                           struct st_mysql_value *const value,
                           bool trace_block_cache_access) {
  std::string trace_folder =
      trace_block_cache_access ? "/block_cache_traces" : "/queries_traces";
  int len = 0;
  const char *const trace_opt_str_raw = value->val_str(value, nullptr, &len);
  rocksdb::Status s;
  if (trace_opt_str_raw == nullptr || rdb == nullptr) {
    return HA_EXIT_FAILURE;
  }
  int rc __attribute__((__unused__));
  std::string trace_opt_str(trace_opt_str_raw);
  if (trace_opt_str.empty()) {
    // End tracing block cache accesses or queries.
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: Stop tracing block cache accesses or queries.\n");
    // ALTER
    // s = trace_block_cache_access ? rdb->EndBlockCacheTrace() :
    // rdb->EndTrace();
    s = trace_block_cache_access
            ? rocksdb_TransactionDB__EndBlockCacheTrace(rdb)
            : rocksdb_TransactionDB__EndTrace(rdb);

    if (!s.ok()) {
      rc = ha_rocksdb::rdb_error_to_mysql(s);
      return HA_EXIT_FAILURE;
    }
    *static_cast<const char **>(save) = trace_opt_str_raw;
    return HA_EXIT_SUCCESS;
  }

  // Start tracing block cache accesses or queries.
  std::stringstream ss(trace_opt_str);
  std::vector<std::string> trace_opts_strs;
  while (ss.good()) {
    std::string substr;
    getline(ss, substr, ':');
    trace_opts_strs.push_back(substr);
  }
  rocksdb::TraceOptions trace_opt;
  try {
    if (trace_opts_strs.size() != 3) {
      throw std::invalid_argument("Incorrect number of arguments.");
    }
    trace_opt.sampling_frequency = std::stoull(trace_opts_strs[0]);
    trace_opt.max_trace_file_size = std::stoull(trace_opts_strs[1]);
  } catch (const std::exception &e) {
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: Failed to parse trace option string: %s. The correct "
        "format is sampling_frequency:max_trace_file_size:trace_file_name. "
        "sampling_frequency and max_trace_file_size are positive integers. "
        "The block accesses or quries are saved to the "
        "rocksdb_datadir%s/trace_file_name.\n",
        trace_opt_str.c_str(), trace_folder.c_str());
    return HA_EXIT_FAILURE;
  }
  const std::string &trace_file_name = trace_opts_strs[2];
  if (trace_file_name.find("/") != std::string::npos) {
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: Start tracing failed (trace option string: %s). The file "
        "name contains directory separator.\n",
        trace_opt_str.c_str());
    return HA_EXIT_FAILURE;
  }
  const std::string trace_dir = std::string(rocksdb_datadir) + trace_folder;

  // ALTER
  // s = rdb->GetEnv()->CreateDirIfMissing(trace_dir);
  s = rocksdb_Env__CreateDirIfMissing(rocksdb_TransactionDB__GetEnv(rdb),
                                      trace_dir);

  if (!s.ok()) {
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: Start tracing failed (trace option string: %s). Failed to "
        "create the trace directory %s: %s\n",
        trace_opt_str.c_str(), trace_dir.c_str(), s.ToString().c_str());
    return HA_EXIT_FAILURE;
  }
  const std::string trace_file_path = trace_dir + "/" + trace_file_name;

  // ALTER
  // s = rdb->GetEnv()->FileExists(trace_file_path);
  s = rocksdb_Env__FileExists(rocksdb_TransactionDB__GetEnv(rdb),
                              trace_file_path);

  if (s.ok() || !s.IsNotFound()) {
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: Start tracing failed (trace option string: %s). The trace "
        "file either already exists or we encountered an error "
        "when calling rdb->GetEnv()->FileExists. The returned status string "
        "is: %s\n",
        trace_opt_str.c_str(), s.ToString().c_str());
    return HA_EXIT_FAILURE;
  }

  // ALTER
  // std::unique_ptr<rocksdb::TraceWriter> trace_writer;
  uint64_t trace_writer_idx;

  // ALTER
  // const rocksdb::EnvOptions env_option(rdb->GetDBOptions());
  rocksdb::EnvOptions env_option;
  rocksdb_EnvOptions__EnvOptions(rocksdb_TransactionDB__GetDBOptions(rdb),
                                 env_option);

  // ALTER
  // s = rocksdb::NewFileTraceWriter(rdb->GetEnv(), env_option, trace_file_path,
  //                                 &trace_writer);
  s = rocksdb_NewFileTraceWriter(rocksdb_TransactionDB__GetEnv(rdb), env_option,
                                 trace_file_path, trace_writer_idx);

  if (!s.ok()) {
    rc = ha_rocksdb::rdb_error_to_mysql(s);
    return HA_EXIT_FAILURE;
  }
  if (trace_block_cache_access) {
    // ALTER
    // s = rdb->StartBlockCacheTrace(trace_opt, std::move(trace_writer));
    s = rocksdb_TransactionDB__StartBlockCacheTrace(rdb, trace_opt,
                                                    trace_writer_idx);
  } else {
    // ALTER
    // s = rdb->StartTrace(trace_opt, std::move(trace_writer));
    s = rocksdb_TransactionDB__StartTrace(rdb, trace_opt, trace_writer_idx);
  }
  if (!s.ok()) {
    rc = ha_rocksdb::rdb_error_to_mysql(s);
    return HA_EXIT_FAILURE;
  }
  // NO_LINT_DEBUG
  sql_print_information(
      "RocksDB: Start tracing block cache accesses or queries. Sampling "
      "frequency: %lu, "
      "Maximum trace file size: %lu, Trace file path %s.\n",
      trace_opt.sampling_frequency, trace_opt.max_trace_file_size,
      trace_file_path.c_str());
  // Save the trace option.
  *static_cast<const char **>(save) = trace_opt_str_raw;
  return HA_EXIT_SUCCESS;
}

/* This method is needed to indicate that the
  ROCKSDB_TRACE_BLOCK_CACHE_ACCESS command is not read-only */
static void rocksdb_trace_stub(THD *const thd,
                               struct st_mysql_sys_var *const var,
                               void *const var_ptr, const void *const save) {
  const auto trace_opt_str_raw = *static_cast<const char *const *>(save);
  DBUG_ASSERT(trace_opt_str_raw != nullptr);
  *static_cast<const char **>(var_ptr) = trace_opt_str_raw;
}

static rocksdb::DBOptions *rdb_init_rocksdb_db_options(void) {
  // ALTER
  auto o = myrocks_RdbInitRocksdbDBOptions();
  return o;
}

/* DBOptions contains Statistics and needs to be destructed last */
// ALTER
// static std::unique_ptr<rocksdb::BlockBasedTableOptions> rocksdb_tbl_options =
//     std::unique_ptr<rocksdb::BlockBasedTableOptions>(
//         new rocksdb::BlockBasedTableOptions());
static rocksdb::BlockBasedTableOptions *rocksdb_tbl_options =
    rocksdb_BlockBasedTableOptions();

// ALTER
// static std::unique_ptr<rocksdb::DBOptions> rocksdb_db_options =
//     rdb_init_rocksdb_db_options();
rpc_logger l_1(1023, "rdb_init_rocksdb_db_options");
static rocksdb::DBOptions *rocksdb_db_options = rdb_init_rocksdb_db_options();

// ALTER
// static std::shared_ptr<rocksdb::RateLimiter> rocksdb_rate_limiter;
static rocksdb::RateLimiter *rocksdb_rate_limiter;

/* This enum needs to be kept up to date with rocksdb::TxnDBWritePolicy */
static const char *write_policy_names[] = {"write_committed", "write_prepared",
                                           "write_unprepared", NullS};

static TYPELIB write_policy_typelib = {array_elements(write_policy_names) - 1,
                                       "write_policy_typelib",
                                       write_policy_names, nullptr};

/* This array needs to be kept up to date with myrocks::read_free_rpl_type */
static const char *read_free_rpl_names[] = {"OFF", "PK_ONLY", "PK_SK", NullS};

static TYPELIB read_free_rpl_typelib = {array_elements(read_free_rpl_names) - 1,
                                        "read_free_rpl_typelib",
                                        read_free_rpl_names, nullptr};

/* This enum needs to be kept up to date with myrocks::select_bypass_policy_type
 */
static const char *select_bypass_policy_names[] = {"always_off", "always_on",
                                                   "opt_in", "opt_out", NullS};

static TYPELIB select_bypass_policy_typelib = {
    array_elements(select_bypass_policy_names) - 1,
    "select_bypass_policy_typelib", select_bypass_policy_names, nullptr};

/* This enum needs to be kept up to date with rocksdb::InfoLogLevel */
static const char *info_log_level_names[] = {"debug_level", "info_level",
                                             "warn_level",  "error_level",
                                             "fatal_level", NullS};

static TYPELIB info_log_level_typelib = {
    array_elements(info_log_level_names) - 1, "info_log_level_typelib",
    info_log_level_names, nullptr};

/* This enum needs to be kept up to date with rocksdb::BottommostLevelCompaction
 */
static const char *bottommost_level_compaction_names[] = {
    "kSkip", "kIfHaveCompactionFilter", "kForce", "kForceOptimized", NullS};

static TYPELIB bottommost_level_compaction_typelib = {
    array_elements(bottommost_level_compaction_names) - 1,
    "bottommost_level_compaction_typelib", bottommost_level_compaction_names,
    nullptr};

rpc_logger l_2(1075, "init static variables");

static void rocksdb_set_rocksdb_info_log_level(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save) {
  DBUG_ASSERT(save != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_info_log_level = *static_cast<const uint64_t *>(save);
  // ALTER
  // rocksdb_db_options->info_log->SetInfoLogLevel(
  //     static_cast<const rocksdb::InfoLogLevel>(rocksdb_info_log_level));
  rocksdb_DBOptions__SetInfoLogLevel(
      rocksdb_db_options,
      static_cast<const rocksdb::InfoLogLevel>(rocksdb_info_log_level));
  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

static void rocksdb_set_rocksdb_stats_level(THD *const thd,
                                            struct st_mysql_sys_var *const var,
                                            void *const var_ptr,
                                            const void *const save) {
  DBUG_ASSERT(save != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  // ALTER
  // rocksdb_db_options->statistics->set_stats_level(
  //     static_cast<const rocksdb::StatsLevel>(
  //         *static_cast<const uint64_t *>(save)));

  rocksdb_rpc_log(
      1103,
      "rocksdb_set_rocksdb_stats_level: rocksdb_DBOptions__SetStatsLevel");
  rocksdb_DBOptions__SetStatsLevel(rocksdb_db_options,
                                   static_cast<const rocksdb::StatsLevel>(
                                       *static_cast<const uint64_t *>(save)));
  // Actual stats level is defined at rocksdb dbopt::statistics::stats_level_
  // so adjusting rocksdb_stats_level here to make sure it points to
  // the correct stats level.

  // ALTER
  // rocksdb_stats_level = rocksdb_db_options->statistics->get_stats_level();
  rocksdb_rpc_log(
      1115,
      "rocksdb_set_rocksdb_stats_level: rocksdb_DBOptions__GetStatsLevel");
  rocksdb_stats_level = rocksdb_DBOptions__GetStatsLevel(rocksdb_db_options);
  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

static void rocksdb_set_reset_stats(
    my_core::THD *const /* unused */,
    my_core::st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr, const void *const save) {
  DBUG_ASSERT(save != nullptr);
  DBUG_ASSERT(rdb != nullptr);
  DBUG_ASSERT(rocksdb_stats != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  *static_cast<bool *>(var_ptr) = *static_cast<const bool *>(save);

  if (rocksdb_reset_stats) {
    rocksdb_rpc_log(
        1136, "rocksdb_set_reset_stats: rocksdb_TransactionDB__ResetStats");
    rocksdb::Status s = rocksdb_TransactionDB__ResetStats(rdb);

    // RocksDB will always return success. Let's document this assumption here
    // as well so that we'll get immediately notified when contract changes.
    DBUG_ASSERT(s == rocksdb::Status::OK());

    // ALTER
    // s = rocksdb_stats->Reset();
    rocksdb_rpc_log(1147, "rocksdb_set_reset_stats: rocksdb_Statistics__Reset");
    s = rocksdb_Statistics__Reset(rocksdb_stats);

    DBUG_ASSERT(s == rocksdb::Status::OK());
  }

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

static void rocksdb_set_io_write_timeout(
    my_core::THD *const thd MY_ATTRIBUTE((__unused__)),
    my_core::st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  DBUG_ASSERT(save != nullptr);
  DBUG_ASSERT(rdb != nullptr);
  DBUG_ASSERT(io_watchdog != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const uint32_t new_val = *static_cast<const uint32_t *>(save);

  rocksdb_io_write_timeout_secs = new_val;
  io_watchdog->reset_timeout(rocksdb_io_write_timeout_secs);

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

enum rocksdb_flush_log_at_trx_commit_type : unsigned int {
  FLUSH_LOG_NEVER = 0,
  FLUSH_LOG_SYNC,
  FLUSH_LOG_BACKGROUND,
  FLUSH_LOG_MAX /* must be last */
};

static int rocksdb_validate_flush_log_at_trx_commit(
    THD *const thd,
    struct st_mysql_sys_var *const var, /* in: pointer to system variable */
    void *var_ptr, /* out: immediate result for update function */
    struct st_mysql_value *const value /* in: incoming value */) {
  long long new_value;

  /* value is NULL */
  if (value->val_int(value, &new_value)) {
    return HA_EXIT_FAILURE;
  }
  rocksdb_rpc_log(1193,
                  "rocksdb_validate_flush_log_at_trx_commit: "
                  "rocksdb_DBOptions__GetStatsLevel");
  // ALTER
  // if (rocksdb_db_options->allow_mmap_writes && new_value != FLUSH_LOG_NEVER)
  // {
  //   return HA_EXIT_FAILURE;
  // }
  if (rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                        "allow_mmap_writes") &&
      new_value != FLUSH_LOG_NEVER) {
    return HA_EXIT_FAILURE;
  }
  *static_cast<uint32_t *>(var_ptr) = static_cast<uint32_t>(new_value);
  return HA_EXIT_SUCCESS;
}
static void rocksdb_compact_column_family_stub(
    THD *const thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save) {}

static int rocksdb_compact_column_family(THD *const thd,
                                         struct st_mysql_sys_var *const var,
                                         void *const var_ptr,
                                         struct st_mysql_value *const value);

static const char *index_type_names[] = {"kBinarySearch", "kHashSearch", NullS};

static TYPELIB index_type_typelib = {array_elements(index_type_names) - 1,
                                     "index_type_typelib", index_type_names,
                                     nullptr};
rpc_logger l_3(1223, "init constant");
const ulong RDB_MAX_LOCK_WAIT_SECONDS = 1024 * 1024 * 1024;
const ulong RDB_DEFAULT_MAX_ROW_LOCKS = 1024 * 1024;
const ulong RDB_MAX_ROW_LOCKS = 1024 * 1024 * 1024;
const ulong RDB_DEFAULT_BULK_LOAD_SIZE = 1000;
const ulong RDB_MAX_BULK_LOAD_SIZE = 1024 * 1024 * 1024;
const size_t RDB_DEFAULT_MERGE_BUF_SIZE = 64 * 1024 * 1024;
const size_t RDB_MIN_MERGE_BUF_SIZE = 100;
const size_t RDB_DEFAULT_MERGE_COMBINE_READ_SIZE = 1024 * 1024 * 1024;
const size_t RDB_MIN_MERGE_COMBINE_READ_SIZE = 100;
const size_t RDB_DEFAULT_MERGE_TMP_FILE_REMOVAL_DELAY = 0;
const size_t RDB_MIN_MERGE_TMP_FILE_REMOVAL_DELAY = 0;
const int64 RDB_DEFAULT_BLOCK_CACHE_SIZE = 512 * 1024 * 1024;
const int64 RDB_MIN_BLOCK_CACHE_SIZE = 1024;
const int RDB_MAX_CHECKSUMS_PCT = 100;
const ulong RDB_DEADLOCK_DETECT_DEPTH = 50;
const ulong ROCKSDB_MAX_MRR_BATCH_SIZE = 1000;
const uint ROCKSDB_MAX_BOTTOM_PRI_BACKGROUND_COMPACTIONS = 64;
rpc_logger l_4(1240, "init constant finish");

// TODO: 0 means don't wait at all, and we don't support it yet?
static MYSQL_THDVAR_ULONG(lock_wait_timeout, PLUGIN_VAR_RQCMDARG,
                          "Number of seconds to wait for lock", nullptr,
                          nullptr, /*default*/ 1, /*min*/ 1,
                          /*max*/ RDB_MAX_LOCK_WAIT_SECONDS, 0);

static MYSQL_THDVAR_BOOL(deadlock_detect, PLUGIN_VAR_RQCMDARG,
                         "Enables deadlock detection", nullptr, nullptr, FALSE);

static MYSQL_THDVAR_ULONG(deadlock_detect_depth, PLUGIN_VAR_RQCMDARG,
                          "Number of transactions deadlock detection will "
                          "traverse through before assuming deadlock",
                          nullptr, nullptr,
                          /*default*/ RDB_DEADLOCK_DETECT_DEPTH,
                          /*min*/ 2,
                          /*max*/ ULONG_MAX, 0);

static MYSQL_THDVAR_BOOL(
    commit_time_batch_for_recovery, PLUGIN_VAR_RQCMDARG,
    "TransactionOptions::commit_time_batch_for_recovery for RocksDB", nullptr,
    nullptr, TRUE);

static MYSQL_THDVAR_BOOL(
    trace_sst_api, PLUGIN_VAR_RQCMDARG,
    "Generate trace output in the log for each call to the SstFileWriter",
    nullptr, nullptr, FALSE);

static MYSQL_THDVAR_BOOL(
    bulk_load, PLUGIN_VAR_RQCMDARG,
    "Use bulk-load mode for inserts. This disables "
    "unique_checks and enables rocksdb_commit_in_the_middle.",
    rocksdb_check_bulk_load, nullptr, FALSE);

static MYSQL_THDVAR_BOOL(bulk_load_allow_sk, PLUGIN_VAR_RQCMDARG,
                         "Allow bulk loading of sk keys during bulk-load. "
                         "Can be changed only when bulk load is disabled.",
                         /* Intentionally reuse unsorted's check function */
                         rocksdb_check_bulk_load_allow_unsorted, nullptr,
                         FALSE);

static MYSQL_THDVAR_BOOL(bulk_load_allow_unsorted, PLUGIN_VAR_RQCMDARG,
                         "Allow unsorted input during bulk-load. "
                         "Can be changed only when bulk load is disabled.",
                         rocksdb_check_bulk_load_allow_unsorted, nullptr,
                         FALSE);

static MYSQL_SYSVAR_BOOL(enable_bulk_load_api, rocksdb_enable_bulk_load_api,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enables using SstFileWriter for bulk loading",
                         nullptr, nullptr, rocksdb_enable_bulk_load_api);
rpc_logger l_5(1292, "init SYSVAR");

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     enable_pipelined_write,
//     *reinterpret_cast<my_bool
//     *>(&rocksdb_db_options->enable_pipelined_write), PLUGIN_VAR_RQCMDARG |
//     PLUGIN_VAR_READONLY, "DBOptions::enable_pipelined_write for RocksDB",
//     nullptr, nullptr, rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//                                       "enable_pipelined_write"));

static MYSQL_SYSVAR_BOOL(enable_remove_orphaned_dropped_cfs,
                         rocksdb_enable_remove_orphaned_dropped_cfs,
                         PLUGIN_VAR_RQCMDARG,
                         "Enables removing dropped cfs from metadata if it "
                         "doesn't exist in cf manager",
                         nullptr, nullptr,
                         rocksdb_enable_remove_orphaned_dropped_cfs);

static MYSQL_THDVAR_STR(tmpdir, PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Directory for temporary files during DDL operations.",
                        nullptr, nullptr, "");

#define DEFAULT_SKIP_UNIQUE_CHECK_TABLES ".*"
static MYSQL_THDVAR_STR(
    skip_unique_check_tables, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Skip unique constraint checking for the specified tables", nullptr,
    nullptr, DEFAULT_SKIP_UNIQUE_CHECK_TABLES);

static MYSQL_THDVAR_BOOL(
    commit_in_the_middle, PLUGIN_VAR_RQCMDARG,
    "Commit rows implicitly every rocksdb_bulk_load_size, on bulk load/insert, "
    "update and delete",
    nullptr, nullptr, FALSE);

static MYSQL_THDVAR_BOOL(
    blind_delete_primary_key, PLUGIN_VAR_RQCMDARG,
    "Deleting rows by primary key lookup, without reading rows (Blind Deletes)."
    " Blind delete is disabled if the table has secondary key",
    nullptr, nullptr, FALSE);

static MYSQL_THDVAR_BOOL(
    enable_iterate_bounds, PLUGIN_VAR_OPCMDARG,
    "Enable rocksdb iterator upper/lower bounds in read options.", nullptr,
    nullptr, TRUE);

static const char *DEFAULT_READ_FREE_RPL_TABLES = ".*";

rpc_logger l_6(1341, "init SYSVAR");

static int rocksdb_validate_read_free_rpl_tables(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *var MY_ATTRIBUTE((__unused__)), void *save,
    struct st_mysql_value *value) {
  rocksdb_rpc_log(1348, "rocksdb_validate_read_free_rpl_tables: start");
  char buff[STRING_BUFFER_USUAL_SIZE];
  int length = sizeof(buff);
  const char *wlist_buf = value->val_str(value, buff, &length);
  const auto wlist = wlist_buf ? wlist_buf : DEFAULT_READ_FREE_RPL_TABLES;

#if defined(HAVE_PSI_INTERFACE)
  Regex_list_handler regex_handler(key_rwlock_read_free_rpl_tables);
#else
  Regex_list_handler regex_handler;
#endif

  if (!regex_handler.set_patterns(wlist)) {
    warn_about_bad_patterns(&regex_handler, "rocksdb_read_free_rpl_tables");
    rocksdb_rpc_log(1363, "rocksdb_validate_read_free_rpl_tables: failure");
    return HA_EXIT_FAILURE;
  }

  *static_cast<const char **>(save) = my_strdup(wlist, MYF(MY_WME));
  rocksdb_rpc_log(1367, "rocksdb_validate_read_free_rpl_tables: success");
  return HA_EXIT_SUCCESS;
}

static void rocksdb_update_read_free_rpl_tables(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *var MY_ATTRIBUTE((__unused__)), void *var_ptr,
    const void *save) {
  const auto wlist = *static_cast<const char *const *>(save);
  DBUG_ASSERT(wlist != nullptr);

  // This is bound to succeed since we've already checked for bad patterns in
  // rocksdb_validate_read_free_rpl_tables
  rdb_read_free_regex_handler.set_patterns(wlist);

  // update all table defs
  struct Rdb_read_free_rpl_updater : public Rdb_tables_scanner {
    int add_table(Rdb_tbl_def *tdef) override {
      tdef->check_and_set_read_free_rpl_table();
      return HA_EXIT_SUCCESS;
    }
  } updater;
  ddl_manager.scan_for_tables(&updater);

  if (wlist == DEFAULT_READ_FREE_RPL_TABLES) {
    // If running SET var = DEFAULT, then rocksdb_validate_read_free_rpl_tables
    // isn't called, and memory is never allocated for the value. Allocate it
    // here.
    *static_cast<const char **>(var_ptr) = my_strdup(wlist, MYF(MY_WME));
  } else {
    // Otherwise, we just reuse the value allocated from
    // rocksdb_validate_read_free_rpl_tables.
    *static_cast<const char **>(var_ptr) = wlist;
  }
}

static void rocksdb_set_max_bottom_pri_background_compactions_internal(
    uint val) {
  // Set lower priority for compactions
  if (val > 0) {
    // This creates background threads in rocksdb with BOTTOM priority pool.
    // Compactions for bottommost level use threads in the BOTTOM pool, and
    // the threads in the BOTTOM pool run with lower OS priority (19 in Linux).

    // ALTER
    // rdb->GetEnv()->SetBackgroundThreads(val, rocksdb::Env::Priority::BOTTOM);
    // rdb->GetEnv()->LowerThreadPoolCPUPriority(rocksdb::Env::Priority::BOTTOM);
    rocksdb_Env__SetBackgroundThreads(rocksdb_TransactionDB__GetEnv(rdb), val,
                                      rocksdb::Env::Priority::BOTTOM);
    rocksdb_Env__LowerThreadPoolCPUPriority(rocksdb_TransactionDB__GetEnv(rdb),
                                            rocksdb::Env::Priority::BOTTOM);

    sql_print_information(
        "Set %d compaction thread(s) with "
        "lower scheduling priority.",
        val);
  }
}

static MYSQL_SYSVAR_STR(
    read_free_rpl_tables, rocksdb_read_free_rpl_tables,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC | PLUGIN_VAR_ALLOCATED,
    "List of tables that will use read-free replication on the slave "
    "(i.e. not lookup a row during replication)",
    rocksdb_validate_read_free_rpl_tables, rocksdb_update_read_free_rpl_tables,
    DEFAULT_READ_FREE_RPL_TABLES);

rpc_logger l_7(1433, "init read_free_rpl_tables");

static MYSQL_SYSVAR_ENUM(
    read_free_rpl, rocksdb_read_free_rpl,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Use read-free replication on the slave (i.e. no row lookup during "
    "replication). Default is OFF, PK_SK will enable it on all tables with "
    "primary key. PK_ONLY will enable it on tables where the only key is the "
    "primary key (i.e. no secondary keys).",
    nullptr, nullptr, read_free_rpl_type::OFF, &read_free_rpl_typelib);

static MYSQL_THDVAR_BOOL(skip_bloom_filter_on_read, PLUGIN_VAR_RQCMDARG,
                         "Skip using bloom filter for reads", nullptr, nullptr,
                         FALSE);

static MYSQL_SYSVAR_ULONG(max_row_locks, rocksdb_max_row_locks,
                          PLUGIN_VAR_RQCMDARG,
                          "Maximum number of locks a transaction can have",
                          nullptr, nullptr,
                          /*default*/ RDB_DEFAULT_MAX_ROW_LOCKS,
                          /*min*/ 1,
                          /*max*/ RDB_MAX_ROW_LOCKS, 0);

static MYSQL_THDVAR_ULONGLONG(
    write_batch_max_bytes, PLUGIN_VAR_RQCMDARG,
    "Maximum size of write batch in bytes. 0 means no limit.", nullptr, nullptr,
    /* default */ 0, /* min */ 0, /* max */ SIZE_T_MAX, 1);

static MYSQL_THDVAR_ULONGLONG(
    write_batch_flush_threshold, PLUGIN_VAR_RQCMDARG,
    "Maximum size of write batch in bytes before flushing. Only valid if "
    "rocksdb_write_policy is WRITE_UNPREPARED. 0 means no limit.",
    nullptr, nullptr, /* default */ 0, /* min */ 0, /* max */ SIZE_T_MAX, 1);

static MYSQL_THDVAR_BOOL(
    lock_scanned_rows, PLUGIN_VAR_RQCMDARG,
    "Take and hold locks on rows that are scanned but not updated", nullptr,
    nullptr, FALSE);

static MYSQL_THDVAR_ULONG(bulk_load_size, PLUGIN_VAR_RQCMDARG,
                          "Max #records in a batch for bulk-load mode", nullptr,
                          nullptr,
                          /*default*/ RDB_DEFAULT_BULK_LOAD_SIZE,
                          /*min*/ 1,
                          /*max*/ RDB_MAX_BULK_LOAD_SIZE, 0);

static MYSQL_THDVAR_ULONGLONG(
    merge_buf_size, PLUGIN_VAR_RQCMDARG,
    "Size to allocate for merge sort buffers written out to disk "
    "during inplace index creation.",
    nullptr, nullptr,
    /* default (64MB) */ RDB_DEFAULT_MERGE_BUF_SIZE,
    /* min (100B) */ RDB_MIN_MERGE_BUF_SIZE,
    /* max */ SIZE_T_MAX, 1);

rpc_logger l_70(1489, "init merge_buf_size");

static MYSQL_THDVAR_ULONGLONG(
    merge_combine_read_size, PLUGIN_VAR_RQCMDARG,
    "Size that we have to work with during combine (reading from disk) phase "
    "of "
    "external sort during fast index creation.",
    nullptr, nullptr,
    /* default (1GB) */ RDB_DEFAULT_MERGE_COMBINE_READ_SIZE,
    /* min (100B) */ RDB_MIN_MERGE_COMBINE_READ_SIZE,
    /* max */ SIZE_T_MAX, 1);

static MYSQL_THDVAR_ULONGLONG(
    merge_tmp_file_removal_delay_ms, PLUGIN_VAR_RQCMDARG,
    "Fast index creation creates a large tmp file on disk during index "
    "creation.  Removing this large file all at once when index creation is "
    "complete can cause trim stalls on Flash.  This variable specifies a "
    "duration to sleep (in milliseconds) between calling chsize() to truncate "
    "the file in chunks.  The chunk size is  the same as merge_buf_size.",
    nullptr, nullptr,
    /* default (0ms) */ RDB_DEFAULT_MERGE_TMP_FILE_REMOVAL_DELAY,
    /* min (0ms) */ RDB_MIN_MERGE_TMP_FILE_REMOVAL_DELAY,
    /* max */ SIZE_T_MAX, 1);

static MYSQL_THDVAR_INT(
    manual_compaction_threads, PLUGIN_VAR_RQCMDARG,
    "How many rocksdb threads to run for manual compactions", nullptr, nullptr,
    /* default rocksdb.dboption max_subcompactions */ 0,
    /* min */ 0, /* max */ 128, 0);

static MYSQL_THDVAR_ENUM(
    manual_compaction_bottommost_level, PLUGIN_VAR_RQCMDARG,
    "Option for bottommost level compaction during manual "
    "compaction",
    nullptr, nullptr,
    /* default */
    (ulong)rocksdb::BottommostLevelCompaction::kForceOptimized,
    &bottommost_level_compaction_typelib);
rpc_logger l_8(1528, "init manual_compaction_bottommost_level");

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     create_if_missing,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->create_if_missing),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::create_if_missing for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "create_if_missing"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     two_write_queues,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->two_write_queues),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::two_write_queues for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "two_write_queues"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     manual_wal_flush,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->manual_wal_flush),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::manual_wal_flush for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "manual_wal_flush"));

static MYSQL_SYSVAR_ENUM(write_policy, rocksdb_write_policy,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "DBOptions::write_policy for RocksDB", nullptr,
                         nullptr, rocksdb::TxnDBWritePolicy::WRITE_COMMITTED,
                         &write_policy_typelib);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     create_missing_column_families,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_db_options->create_missing_column_families),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::create_missing_column_families for RocksDB", nullptr,
//     nullptr, rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//                                       "create_missing_column_families"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     error_if_exists,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->error_if_exists),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::error_if_exists for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "error_if_exists"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     paranoid_checks,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->paranoid_checks),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::paranoid_checks for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "paranoid_checks"));

static MYSQL_SYSVAR_ULONGLONG(
    rate_limiter_bytes_per_sec, rocksdb_rate_limiter_bytes_per_sec,
    PLUGIN_VAR_RQCMDARG, "DBOptions::rate_limiter bytes_per_sec for RocksDB",
    nullptr, rocksdb_set_rate_limiter_bytes_per_sec, /* default */ 0L,
    /* min */ 0L, /* max */ MAX_RATE_LIMITER_BYTES_PER_SEC, 0);

static MYSQL_SYSVAR_ULONGLONG(
    sst_mgr_rate_bytes_per_sec, rocksdb_sst_mgr_rate_bytes_per_sec,
    PLUGIN_VAR_RQCMDARG,
    "DBOptions::sst_file_manager rate_bytes_per_sec for RocksDB", nullptr,
    rocksdb_set_sst_mgr_rate_bytes_per_sec,
    /* default */ DEFAULT_SST_MGR_RATE_BYTES_PER_SEC,
    /* min */ 0L, /* max */ UINT64_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_ULONGLONG(
//     delayed_write_rate, rocksdb_delayed_write_rate, PLUGIN_VAR_RQCMDARG,
//     "DBOptions::delayed_write_rate", nullptr, rocksdb_set_delayed_write_rate,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "delayed_write_rate"),
//     0, UINT64_MAX, 0);

static MYSQL_SYSVAR_UINT(max_latest_deadlocks, rocksdb_max_latest_deadlocks,
                         PLUGIN_VAR_RQCMDARG,
                         "Maximum number of recent "
                         "deadlocks to store",
                         nullptr, rocksdb_set_max_latest_deadlocks,
                         rocksdb::kInitialMaxDeadlocks, 0, UINT32_MAX, 0);

static MYSQL_SYSVAR_ENUM(
    info_log_level, rocksdb_info_log_level, PLUGIN_VAR_RQCMDARG,
    "Filter level for info logs to be written mysqld error log. "
    "Valid values include 'debug_level', 'info_level', 'warn_level'"
    "'error_level' and 'fatal_level'.",
    nullptr, rocksdb_set_rocksdb_info_log_level,
    rocksdb::InfoLogLevel::ERROR_LEVEL, &info_log_level_typelib);

static MYSQL_THDVAR_INT(
    perf_context_level, PLUGIN_VAR_RQCMDARG,
    "Perf Context Level for rocksdb internal timer stat collection", nullptr,
    nullptr,
    /* default */ rocksdb::PerfLevel::kUninitialized,
    /* min */ rocksdb::PerfLevel::kUninitialized,
    /* max */ rocksdb::PerfLevel::kOutOfBounds - 1, 0);

static MYSQL_SYSVAR_UINT(
    wal_recovery_mode, rocksdb_wal_recovery_mode, PLUGIN_VAR_RQCMDARG,
    "DBOptions::wal_recovery_mode for RocksDB. Default is kPointInTimeRecovery",
    nullptr, nullptr,
    /* default */ (uint)rocksdb::WALRecoveryMode::kPointInTimeRecovery,
    /* min */ (uint)rocksdb::WALRecoveryMode::kTolerateCorruptedTailRecords,
    /* max */ (uint)rocksdb::WALRecoveryMode::kSkipAnyCorruptedRecords, 0);

static MYSQL_SYSVAR_BOOL(
    track_and_verify_wals_in_manifest,
    *reinterpret_cast<my_bool *>(&rocksdb_track_and_verify_wals_in_manifest),
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "DBOptions::track_and_verify_wals_in_manifest for RocksDB", nullptr,
    nullptr, true);

static MYSQL_SYSVAR_UINT(
    stats_level, rocksdb_stats_level, PLUGIN_VAR_RQCMDARG,
    "Statistics Level for RocksDB. Default is 1 (kExceptHistogramOrTimers)",
    nullptr, rocksdb_set_rocksdb_stats_level,
    /* default */ (uint)rocksdb::StatsLevel::kExceptHistogramOrTimers,
    /* min */ (uint)rocksdb::StatsLevel::kExceptTickers,
    /* max */ (uint)rocksdb::StatsLevel::kAll, 0);
rpc_logger l_9(1659, "init stats_level");

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     compaction_readahead_size,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//                                        "compaction_readahead_size"),
//     PLUGIN_VAR_RQCMDARG, "DBOptions::compaction_readahead_size for RocksDB",
//     nullptr, nullptr,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//                                        "compaction_readahead_size"),
//     /* min */ 0L, /* max */ ULONG_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     new_table_reader_for_compaction_inputs,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_db_options->new_table_reader_for_compaction_inputs),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::new_table_reader_for_compaction_inputs for RocksDB", nullptr,
//     nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//                                       new_table_reader_for_compaction_inputs));

static MYSQL_SYSVAR_UINT(
    access_hint_on_compaction_start, rocksdb_access_hint_on_compaction_start,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "DBOptions::access_hint_on_compaction_start for RocksDB", nullptr, nullptr,
    /* default */ (uint)rocksdb::Options::AccessHint::NORMAL,
    /* min */ (uint)rocksdb::Options::AccessHint::NONE,
    /* max */ (uint)rocksdb::Options::AccessHint::WILLNEED, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     allow_concurrent_memtable_write,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_db_options->allow_concurrent_memtable_write),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::allow_concurrent_memtable_write for RocksDB", nullptr,
//     nullptr, false);

// // TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     enable_write_thread_adaptive_yield,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_db_options->enable_write_thread_adaptive_yield),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::enable_write_thread_adaptive_yield for RocksDB", nullptr,
//     nullptr, false);

// // ALTER
// static MYSQL_SYSVAR_INT(
//     max_open_files,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options, "max_open_files"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::max_open_files for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options, "max_open_files"),
//     /* min */ -2, /* max */ INT_MAX, 0);

// // ALTER
// static MYSQL_SYSVAR_ULONG(
//     max_total_wal_size,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "max_total_wal_size"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::max_total_wal_size for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "max_total_wal_size"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// // TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     use_fsync, *reinterpret_cast<my_bool *>(&rocksdb_db_options->use_fsync),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::use_fsync for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options, "use_fsync"));

// static MYSQL_SYSVAR_STR(wal_dir, rocksdb_wal_dir,
//                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//                         "DBOptions::wal_dir for RocksDB", nullptr, nullptr,
//                         rocksdb_db_options->wal_dir.c_str());

static MYSQL_SYSVAR_STR(
    persistent_cache_path, rocksdb_persistent_cache_path,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Path for BlockBasedTableOptions::persistent_cache for RocksDB", nullptr,
    nullptr, "");

static MYSQL_SYSVAR_ULONG(
    persistent_cache_size_mb, rocksdb_persistent_cache_size_mb,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Size of cache in MB for BlockBasedTableOptions::persistent_cache "
    "for RocksDB",
    nullptr, nullptr, rocksdb_persistent_cache_size_mb,
    /* min */ 0L, /* max */ ULONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     delete_obsolete_files_period_micros,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "delete_obsolete_files_period_micros"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::delete_obsolete_files_period_micros for RocksDB", nullptr,
//     nullptr,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "delete_obsolete_files_period_micros"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_INT(
//     max_background_jobs,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//     "max_background_jobs"), PLUGIN_VAR_RQCMDARG,
//     "DBOptions::max_background_jobs for RocksDB", nullptr,
//     rocksdb_set_max_background_jobs,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//     "max_background_jobs"),
//     /* min */ -1, /* max */ MAX_BACKGROUND_JOBS, 0);

// ALTER
// static MYSQL_SYSVAR_INT(
//     max_background_flushes,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//                                      "max_background_flushes"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::max_background_flushes for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//                                      "max_background_flushes"),
//     /* min */ -1, /* max */ 64, 0);

// ALTER
// static MYSQL_SYSVAR_INT(
//     max_background_compactions,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//                                      "max_background_compactions"),
//     PLUGIN_VAR_RQCMDARG, "DBOptions::max_background_compactions for RocksDB",
//     nullptr, rocksdb_set_max_background_compactions,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//                                      "max_background_compactions"),
//     /* min */ -1, /* max */ 64, 0);

static MYSQL_SYSVAR_UINT(
    max_bottom_pri_background_compactions,
    rocksdb_max_bottom_pri_background_compactions, PLUGIN_VAR_RQCMDARG,
    "Creating specified number of threads, setting lower "
    "CPU priority, and letting Lmax compactions use them. "
    "Maximum total compaction concurrency continues to be capped to "
    "rocksdb_max_background_compactions or "
    "rocksdb_max_background_jobs. In addition to that, Lmax "
    "compaction concurrency is capped to "
    "rocksdb_max_bottom_pri_background_compactions. Default value is 0, "
    "which means all compactions are under concurrency of "
    "rocksdb_max_background_compactions|jobs. If you set very low "
    "rocksdb_max_bottom_pri_background_compactions (e.g. 1 or 2), compactions "
    "may not be able to keep up. Since Lmax normally has "
    "90 percent of data, it is recommended to set closer number to "
    "rocksdb_max_background_compactions|jobs. This option is helpful to "
    "give more CPU resources to other threads (e.g. query processing).",
    rocksdb_validate_max_bottom_pri_background_compactions, nullptr, 0,
    /* min */ 0, /* max */ ROCKSDB_MAX_BOTTOM_PRI_BACKGROUND_COMPACTIONS, 0);

// ALTER
// static MYSQL_SYSVAR_UINT(
//     max_subcompactions,
//     rocksdb_DBOptions__GetUInt32Options(rocksdb_db_options,
//                                         "max_subcompactions"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::max_subcompactions for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetUInt32Options(rocksdb_db_options,
//                                         "max_subcompactions"),
//     /* min */ 1, /* max */ MAX_SUBCOMPACTIONS, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     max_log_file_size,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//     "max_log_file_size"), PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::max_log_file_size for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//     "max_log_file_size"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     log_file_time_to_roll,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//                                        "log_file_time_to_roll"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::log_file_time_to_roll for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//                                        "log_file_time_to_roll"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     keep_log_file_num,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//     "keep_log_file_num"), PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::keep_log_file_num for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//     "keep_log_file_num"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     max_manifest_file_size,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "max_manifest_file_size"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::max_manifest_file_size for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "max_manifest_file_size"),
//     /* min */ 0L, /* max */ ULONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_INT(
//     table_cache_numshardbits,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//                                      "table_cache_numshardbits"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::table_cache_numshardbits for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
//                                      "table_cache_numshardbits"),
//     // LRUCache limits this to 19 bits, anything greater
//     // fails to create a cache and returns a nullptr
//     /* min */ 0, /* max */ 19, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     wal_ttl_seconds,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//     "WAL_ttl_seconds"), PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::WAL_ttl_seconds for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//     "WAL_ttl_seconds"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     wal_size_limit_mb,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "WAL_size_limit_MB"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::WAL_size_limit_MB for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "WAL_size_limit_MB"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     manifest_preallocation_size,
//     rocksdb_ColumnFamilyOptions__GetSizeTProp(rocksdb_db_options,
//                                               "manifest_preallocation_size"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::manifest_preallocation_size for RocksDB", nullptr, nullptr,
//     rocksdb_ColumnFamilyOptions__GetSizeTProp(rocksdb_db_options,
//                                               "manifest_preallocation_size"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     use_direct_reads,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->use_direct_reads),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::use_direct_reads for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "use_direct_reads"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     use_direct_io_for_flush_and_compaction,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_db_options->use_direct_io_for_flush_and_compaction),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::use_direct_io_for_flush_and_compaction for RocksDB", nullptr,
//     nullptr,
//     rocksdb_DBOptions__GetBoolOptions(
//         rocksdb_db_options, "use_direct_io_for_flush_and_compaction"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     allow_mmap_reads,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->allow_mmap_reads),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::allow_mmap_reads for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "allow_mmap_reads"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     allow_mmap_writes,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->allow_mmap_writes),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::allow_mmap_writes for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//     "allow_mmap_writes"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     is_fd_close_on_exec,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->is_fd_close_on_exec),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::is_fd_close_on_exec for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//                                       "is_fd_close_on_exec"));

// ALTER
// static MYSQL_SYSVAR_UINT(
//     stats_dump_period_sec,
//     rocksdb_DBOptions__GetUInt32Options(rocksdb_db_options,
//                                         "stats_dump_period_sec"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::stats_dump_period_sec for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetUInt32Options(rocksdb_db_options,
//                                         "stats_dump_period_sec"),
//     /* min */ 0, /* max */ INT_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     advise_random_on_open,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->advise_random_on_open),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::advise_random_on_open for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//                                       "advise_random_on_open"));

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     db_write_buffer_size,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//                                        "db_write_buffer_size"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::db_write_buffer_size for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetSizeTOptions(rocksdb_db_options,
//                                        "db_write_buffer_size"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     use_adaptive_mutex,
//     *reinterpret_cast<my_bool *>(&rocksdb_db_options->use_adaptive_mutex),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "DBOptions::use_adaptive_mutex for RocksDB", nullptr, nullptr,
//     rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
//                                       "use_adaptive_mutex"));

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     bytes_per_sync,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//     "bytes_per_sync"), PLUGIN_VAR_RQCMDARG, "DBOptions::bytes_per_sync for
//     RocksDB", nullptr, rocksdb_set_bytes_per_sync,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//     "bytes_per_sync"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_ULONG(
//     wal_bytes_per_sync,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "wal_bytes_per_sync"),
//     PLUGIN_VAR_RQCMDARG, "DBOptions::wal_bytes_per_sync for RocksDB",
//     nullptr, rocksdb_set_wal_bytes_per_sync,
//     rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
//                                         "wal_bytes_per_sync"),
//     /* min */ 0L, /* max */ LONG_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     enable_thread_tracking,
//     *reinterpret_cast<my_bool
//     *>(&rocksdb_db_options->enable_thread_tracking), PLUGIN_VAR_RQCMDARG |
//     PLUGIN_VAR_READONLY, "DBOptions::enable_thread_tracking for RocksDB",
//     nullptr, nullptr, true);

static MYSQL_SYSVAR_LONGLONG(block_cache_size, rocksdb_block_cache_size,
                             PLUGIN_VAR_RQCMDARG,
                             "block_cache size for RocksDB",
                             rocksdb_validate_set_block_cache_size, nullptr,
                             /* default */ RDB_DEFAULT_BLOCK_CACHE_SIZE,
                             /* min */ RDB_MIN_BLOCK_CACHE_SIZE,
                             /* max */ LLONG_MAX,
                             /* Block size */ RDB_MIN_BLOCK_CACHE_SIZE);

static MYSQL_SYSVAR_LONGLONG(sim_cache_size, rocksdb_sim_cache_size,
                             PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                             "Simulated cache size for RocksDB", nullptr,
                             nullptr,
                             /* default */ 0,
                             /* min */ 0,
                             /* max */ LLONG_MAX,
                             /* Block size */ 0);

static MYSQL_SYSVAR_BOOL(
    use_clock_cache, rocksdb_use_clock_cache,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Use ClockCache instead of default LRUCache for RocksDB", nullptr, nullptr,
    false);

static MYSQL_SYSVAR_BOOL(cache_dump, rocksdb_cache_dump,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Include RocksDB block cache content in core dump.",
                         nullptr, nullptr, true);

static MYSQL_SYSVAR_DOUBLE(cache_high_pri_pool_ratio,
                           rocksdb_cache_high_pri_pool_ratio,
                           PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                           "Specify the size of block cache high-pri pool",
                           nullptr, nullptr, /* default */ 0.0, /* min */ 0.0,
                           /* max */ 1.0, 0);

rpc_logger l_10(2071, "init cache_high_pri_pool_ratio");

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     cache_index_and_filter_blocks,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_tbl_options->cache_index_and_filter_blocks),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "BlockBasedTableOptions::cache_index_and_filter_blocks for RocksDB",
//     nullptr, nullptr, true);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     cache_index_and_filter_with_high_priority,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_tbl_options->cache_index_and_filter_blocks_with_high_priority),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "cache_index_and_filter_blocks_with_high_priority for RocksDB", nullptr,
//     nullptr, true);

// When pin_l0_filter_and_index_blocks_in_cache is true, RocksDB will  use the
// LRU cache, but will always keep the filter & idndex block's handle checked
// out (=won't call ShardedLRUCache::Release), plus the parsed out objects
// the LRU cache will never push flush them out, hence they're pinned.
//
// This fixes the mutex contention between :ShardedLRUCache::Lookup and
// ShardedLRUCache::Release which reduced the QPS ratio (QPS using secondary
// index / QPS using PK).

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     pin_l0_filter_and_index_blocks_in_cache,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_tbl_options->pin_l0_filter_and_index_blocks_in_cache),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "pin_l0_filter_and_index_blocks_in_cache for RocksDB", nullptr, nullptr,
//     true);

// ALTER
// static MYSQL_SYSVAR_ENUM(
//     index_type, rocksdb_index_type, PLUGIN_VAR_RQCMDARG |
//     PLUGIN_VAR_READONLY, "BlockBasedTableOptions::index_type for RocksDB",
//     nullptr, nullptr,
//     (uint64_t)rocksdb_BlockBasedTableOptions__GetIndexType(rocksdb_tbl_options),
//     &index_type_typelib);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     hash_index_allow_collision,
//     *reinterpret_cast<my_bool *>(
//         &rocksdb_tbl_options->hash_index_allow_collision),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "BlockBasedTableOptions::hash_index_allow_collision for RocksDB",
//     nullptr, nullptr, rocksdb_BlockBasedTableOptions__GetBoolOptions(
//         rocksdb_tbl_options, "hash_index_allow_collision"));

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     no_block_cache,
//     *reinterpret_cast<my_bool *>(&rocksdb_tbl_options->no_block_cache),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "BlockBasedTableOptions::no_block_cache for RocksDB", nullptr, nullptr,
//     rocksdb_BlockBasedTableOptions__GetBoolOptions(rocksdb_tbl_options,
//                                                    "no_block_cache"));

// ALTER
// static MYSQL_SYSVAR_ULONG(block_size, rocksdb_tbl_options->block_size,
//                           PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//                           "BlockBasedTableOptions::block_size for RocksDB",
//                           nullptr, nullptr, rocksdb_tbl_options->block_size,
//                           /* min */ 1L, /* max */ LONG_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_INT(
//     block_size_deviation,
//     rocksdb_BlockBasedTableOptions__GetSizeTOptions(rocksdb_tbl_options,
//                                                     "block_size_deviation"),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "BlockBasedTableOptions::block_size_deviation for RocksDB", nullptr,
//     nullptr,
//     rocksdb_BlockBasedTableOptions__GetSizeTOptions(rocksdb_tbl_options,
//                                                     "block_size_deviation"),
//     /* min */ 0, /* max */ INT_MAX, 0);

// ALTER
// static MYSQL_SYSVAR_INT(
//     block_restart_interval, rocksdb_tbl_options->block_restart_interval,
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "BlockBasedTableOptions::block_restart_interval for RocksDB", nullptr,
//     nullptr, rocksdb_tbl_options->block_restart_interval,
//     /* min */ 1, /* max */ INT_MAX, 0);

// TODO: ALTER
// static MYSQL_SYSVAR_BOOL(
//     whole_key_filtering,
//     *reinterpret_cast<my_bool *>(&rocksdb_tbl_options->whole_key_filtering),
//     PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
//     "BlockBasedTableOptions::whole_key_filtering for RocksDB", nullptr,
//     nullptr,
//     rocksdb_BlockBasedTableOptions__GetBoolOptions(rocksdb_tbl_options,
//                                                    "whole_key_filtering"));

static MYSQL_SYSVAR_STR(default_cf_options, rocksdb_default_cf_options,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "default cf options for RocksDB", nullptr, nullptr, "");

static MYSQL_SYSVAR_STR(override_cf_options, rocksdb_override_cf_options,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "option overrides per cf for RocksDB", nullptr, nullptr,
                        "");

static MYSQL_SYSVAR_STR(update_cf_options, rocksdb_update_cf_options,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC |
                            PLUGIN_VAR_ALLOCATED,
                        "Option updates per column family for RocksDB",
                        rocksdb_validate_update_cf_options,
                        rocksdb_set_update_cf_options, nullptr);

static MYSQL_SYSVAR_BOOL(use_default_sk_cf, rocksdb_use_default_sk_cf,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use default_sk for secondary keys", nullptr, nullptr,
                         false);

static MYSQL_SYSVAR_UINT(flush_log_at_trx_commit,
                         rocksdb_flush_log_at_trx_commit, PLUGIN_VAR_RQCMDARG,
                         "Sync on transaction commit. Similar to "
                         "innodb_flush_log_at_trx_commit. 1: sync on commit, "
                         "0,2: not sync on commit",
                         rocksdb_validate_flush_log_at_trx_commit, nullptr,
                         /* default */ FLUSH_LOG_SYNC,
                         /* min */ FLUSH_LOG_NEVER,
                         /* max */ FLUSH_LOG_BACKGROUND, 0);

static MYSQL_THDVAR_BOOL(write_disable_wal, PLUGIN_VAR_RQCMDARG,
                         "WriteOptions::disableWAL for RocksDB", nullptr,
                         nullptr, rocksdb::WriteOptions().disableWAL);

static MYSQL_THDVAR_BOOL(
    write_ignore_missing_column_families, PLUGIN_VAR_RQCMDARG,
    "WriteOptions::ignore_missing_column_families for RocksDB", nullptr,
    nullptr, rocksdb::WriteOptions().ignore_missing_column_families);

static MYSQL_THDVAR_BOOL(skip_fill_cache, PLUGIN_VAR_RQCMDARG,
                         "Skip filling block cache on read requests", nullptr,
                         nullptr, FALSE);

static MYSQL_THDVAR_BOOL(
    unsafe_for_binlog, PLUGIN_VAR_RQCMDARG,
    "Allowing statement based binary logging which may break consistency",
    nullptr, nullptr, FALSE);

static MYSQL_THDVAR_UINT(records_in_range, PLUGIN_VAR_RQCMDARG,
                         "Used to override the result of records_in_range(). "
                         "Set to a positive number to override",
                         nullptr, nullptr, 0,
                         /* min */ 0, /* max */ INT_MAX, 0);

static MYSQL_THDVAR_UINT(force_index_records_in_range, PLUGIN_VAR_RQCMDARG,
                         "Used to override the result of records_in_range() "
                         "when FORCE INDEX is used.",
                         nullptr, nullptr, 0,
                         /* min */ 0, /* max */ INT_MAX, 0);

static MYSQL_SYSVAR_UINT(
    debug_optimizer_n_rows, rocksdb_debug_optimizer_n_rows,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR,
    "Test only to override rocksdb estimates of table size in a memtable",
    nullptr, nullptr, 0, /* min */ 0, /* max */ INT_MAX, 0);

static MYSQL_SYSVAR_BOOL(force_compute_memtable_stats,
                         rocksdb_force_compute_memtable_stats,
                         PLUGIN_VAR_RQCMDARG,
                         "Force to always compute memtable stats", nullptr,
                         nullptr, TRUE);

static MYSQL_SYSVAR_UINT(force_compute_memtable_stats_cachetime,
                         rocksdb_force_compute_memtable_stats_cachetime,
                         PLUGIN_VAR_RQCMDARG,
                         "Time in usecs to cache memtable estimates", nullptr,
                         nullptr, /* default */ 60 * 1000 * 1000,
                         /* min */ 0, /* max */ INT_MAX, 0);

static MYSQL_SYSVAR_BOOL(
    debug_optimizer_no_zero_cardinality,
    rocksdb_debug_optimizer_no_zero_cardinality, PLUGIN_VAR_RQCMDARG,
    "In case if cardinality is zero, overrides it with some value", nullptr,
    nullptr, TRUE);

static MYSQL_SYSVAR_STR(compact_cf, rocksdb_compact_cf_name,
                        PLUGIN_VAR_RQCMDARG, "Compact column family",
                        rocksdb_compact_column_family,
                        rocksdb_compact_column_family_stub, "");

static MYSQL_SYSVAR_STR(delete_cf, rocksdb_delete_cf_name, PLUGIN_VAR_RQCMDARG,
                        "Delete column family", rocksdb_delete_column_family,
                        rocksdb_delete_column_family_stub, "");

static MYSQL_SYSVAR_STR(create_checkpoint, rocksdb_checkpoint_name,
                        PLUGIN_VAR_RQCMDARG, "Checkpoint directory",
                        rocksdb_create_checkpoint,
                        rocksdb_create_checkpoint_stub, "");

static MYSQL_SYSVAR_BOOL(signal_drop_index_thread,
                         rocksdb_signal_drop_index_thread, PLUGIN_VAR_RQCMDARG,
                         "Wake up drop index thread", nullptr,
                         rocksdb_drop_index_wakeup_thread, FALSE);

static MYSQL_SYSVAR_BOOL(pause_background_work, rocksdb_pause_background_work,
                         PLUGIN_VAR_RQCMDARG,
                         "Disable all rocksdb background operations", nullptr,
                         rocksdb_set_pause_background_work, FALSE);

static MYSQL_SYSVAR_BOOL(
    enable_ttl, rocksdb_enable_ttl, PLUGIN_VAR_RQCMDARG,
    "Enable expired TTL records to be dropped during compaction.", nullptr,
    nullptr, TRUE);

static MYSQL_SYSVAR_BOOL(
    enable_ttl_read_filtering, rocksdb_enable_ttl_read_filtering,
    PLUGIN_VAR_RQCMDARG,
    "For tables with TTL, expired records are skipped/filtered out during "
    "processing and in query results. Disabling this will allow these records "
    "to be seen, but as a result rows may disappear in the middle of "
    "transactions as they are dropped during compaction. Use with caution.",
    nullptr, nullptr, TRUE);

rpc_logger l_11(2297, "init enable_ttl_read_filtering");

static MYSQL_SYSVAR_INT(
    debug_ttl_rec_ts, rocksdb_debug_ttl_rec_ts, PLUGIN_VAR_RQCMDARG,
    "For debugging purposes only.  Overrides the TTL of records to "
    "now() + debug_ttl_rec_ts.  The value can be +/- to simulate "
    "a record inserted in the past vs a record inserted in the 'future'. "
    "A value of 0 denotes that the variable is not set. This variable is a "
    "no-op in non-debug builds.",
    nullptr, nullptr, 0, /* min */ -3600, /* max */ 3600, 0);

static MYSQL_SYSVAR_INT(
    debug_ttl_snapshot_ts, rocksdb_debug_ttl_snapshot_ts, PLUGIN_VAR_RQCMDARG,
    "For debugging purposes only.  Sets the snapshot during compaction to "
    "now() + debug_set_ttl_snapshot_ts.  The value can be +/- to simulate "
    "a snapshot in the past vs a snapshot created in the 'future'. "
    "A value of 0 denotes that the variable is not set. This variable is a "
    "no-op in non-debug builds.",
    nullptr, nullptr, 0, /* min */ -3600, /* max */ 3600, 0);

static MYSQL_SYSVAR_INT(
    debug_ttl_read_filter_ts, rocksdb_debug_ttl_read_filter_ts,
    PLUGIN_VAR_RQCMDARG,
    "For debugging purposes only.  Overrides the TTL read filtering time to "
    "time + debug_ttl_read_filter_ts. A value of 0 denotes that the variable "
    "is not set. This variable is a no-op in non-debug builds.",
    nullptr, nullptr, 0, /* min */ -3600, /* max */ 3600, 0);

static MYSQL_SYSVAR_BOOL(
    debug_ttl_ignore_pk, rocksdb_debug_ttl_ignore_pk, PLUGIN_VAR_RQCMDARG,
    "For debugging purposes only. If true, compaction filtering will not occur "
    "on PK TTL data. This variable is a no-op in non-debug builds.",
    nullptr, nullptr, FALSE);

static MYSQL_SYSVAR_UINT(
    max_manual_compactions, rocksdb_max_manual_compactions, PLUGIN_VAR_RQCMDARG,
    "Maximum number of pending + ongoing number of manual compactions.",
    nullptr, nullptr, /* default */ 10, /* min */ 0, /* max */ UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(
    rollback_on_timeout, rocksdb_rollback_on_timeout, PLUGIN_VAR_OPCMDARG,
    "Whether to roll back the complete transaction or a single statement on "
    "lock wait timeout (a single statement by default)",
    NULL, NULL, FALSE);

static MYSQL_SYSVAR_UINT(
    debug_manual_compaction_delay, rocksdb_debug_manual_compaction_delay,
    PLUGIN_VAR_RQCMDARG,
    "For debugging purposes only. Sleeping specified seconds "
    "for simulating long running compactions.",
    nullptr, nullptr, 0, /* min */ 0, /* max */ UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(
    reset_stats, rocksdb_reset_stats, PLUGIN_VAR_RQCMDARG,
    "Reset the RocksDB internal statistics without restarting the DB.", nullptr,
    rocksdb_set_reset_stats, FALSE);

static MYSQL_SYSVAR_UINT(io_write_timeout, rocksdb_io_write_timeout_secs,
                         PLUGIN_VAR_RQCMDARG,
                         "Timeout for experimental I/O watchdog.", nullptr,
                         rocksdb_set_io_write_timeout, /* default */ 0,
                         /* min */ 0L,
                         /* max */ UINT_MAX, 0);

static MYSQL_SYSVAR_BOOL(enable_2pc, rocksdb_enable_2pc, PLUGIN_VAR_RQCMDARG,
                         "Enable two phase commit for MyRocks", nullptr,
                         nullptr, TRUE);

static MYSQL_SYSVAR_BOOL(ignore_unknown_options, rocksdb_ignore_unknown_options,
                         PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                         "Enable ignoring unknown options passed to RocksDB",
                         nullptr, nullptr, TRUE);

static MYSQL_SYSVAR_BOOL(strict_collation_check, rocksdb_strict_collation_check,
                         PLUGIN_VAR_RQCMDARG,
                         "Enforce case sensitive collation for MyRocks indexes",
                         nullptr, nullptr, TRUE);

static MYSQL_SYSVAR_STR(strict_collation_exceptions,
                        rocksdb_strict_collation_exceptions,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "List of tables (using regex) that are excluded "
                        "from the case sensitive collation enforcement",
                        nullptr, rocksdb_set_collation_exception_list, "");

static MYSQL_SYSVAR_BOOL(collect_sst_properties, rocksdb_collect_sst_properties,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enables collecting SST file properties on each flush",
                         nullptr, nullptr, rocksdb_collect_sst_properties);

static MYSQL_SYSVAR_BOOL(
    force_flush_memtable_now, rocksdb_force_flush_memtable_now_var,
    PLUGIN_VAR_RQCMDARG,
    "Forces memstore flush which may block all write requests so be careful",
    rocksdb_force_flush_memtable_now, rocksdb_force_flush_memtable_now_stub,
    FALSE);

static MYSQL_SYSVAR_BOOL(
    force_flush_memtable_and_lzero_now,
    rocksdb_force_flush_memtable_and_lzero_now_var, PLUGIN_VAR_RQCMDARG,
    "Acts similar to force_flush_memtable_now, but also compacts all L0 files.",
    rocksdb_force_flush_memtable_and_lzero_now,
    rocksdb_force_flush_memtable_and_lzero_now_stub, FALSE);

static MYSQL_SYSVAR_BOOL(cancel_manual_compactions,
                         rocksdb_cancel_manual_compactions_var,
                         PLUGIN_VAR_RQCMDARG,
                         "Cancelling all ongoing manual compactions.",
                         rocksdb_cancel_manual_compactions,
                         rocksdb_cancel_manual_compactions_stub, FALSE);

static MYSQL_SYSVAR_UINT(
    seconds_between_stat_computes, rocksdb_seconds_between_stat_computes,
    PLUGIN_VAR_RQCMDARG,
    "Sets a number of seconds to wait between optimizer stats recomputation. "
    "Only changed indexes will be refreshed.",
    nullptr, nullptr, rocksdb_seconds_between_stat_computes,
    /* min */ 0L, /* max */ UINT_MAX, 0);

rpc_logger l_12(2417, "init seconds_between_stat_computes");

static MYSQL_SYSVAR_LONGLONG(compaction_sequential_deletes,
                             rocksdb_compaction_sequential_deletes,
                             PLUGIN_VAR_RQCMDARG,
                             "RocksDB will trigger compaction for the file if "
                             "it has more than this number sequential deletes "
                             "per window",
                             nullptr, rocksdb_set_compaction_options,
                             DEFAULT_COMPACTION_SEQUENTIAL_DELETES,
                             /* min */ 0L,
                             /* max */ MAX_COMPACTION_SEQUENTIAL_DELETES, 0);

static MYSQL_SYSVAR_LONGLONG(
    compaction_sequential_deletes_window,
    rocksdb_compaction_sequential_deletes_window, PLUGIN_VAR_RQCMDARG,
    "Size of the window for counting rocksdb_compaction_sequential_deletes",
    nullptr, rocksdb_set_compaction_options,
    DEFAULT_COMPACTION_SEQUENTIAL_DELETES_WINDOW,
    /* min */ 0L, /* max */ MAX_COMPACTION_SEQUENTIAL_DELETES_WINDOW, 0);

static MYSQL_SYSVAR_LONGLONG(
    compaction_sequential_deletes_file_size,
    rocksdb_compaction_sequential_deletes_file_size, PLUGIN_VAR_RQCMDARG,
    "Minimum file size required for compaction_sequential_deletes", nullptr,
    rocksdb_set_compaction_options, 0L,
    /* min */ -1L, /* max */ LLONG_MAX, 0);

static MYSQL_SYSVAR_BOOL(
    compaction_sequential_deletes_count_sd,
    rocksdb_compaction_sequential_deletes_count_sd, PLUGIN_VAR_RQCMDARG,
    "Counting SingleDelete as rocksdb_compaction_sequential_deletes", nullptr,
    nullptr, rocksdb_compaction_sequential_deletes_count_sd);

static MYSQL_SYSVAR_BOOL(
    print_snapshot_conflict_queries, rocksdb_print_snapshot_conflict_queries,
    PLUGIN_VAR_RQCMDARG,
    "Logging queries that got snapshot conflict errors into *.err log", nullptr,
    nullptr, rocksdb_print_snapshot_conflict_queries);

static MYSQL_THDVAR_INT(checksums_pct, PLUGIN_VAR_RQCMDARG,
                        "How many percentages of rows to be checksummed",
                        nullptr, nullptr, RDB_MAX_CHECKSUMS_PCT,
                        /* min */ 0, /* max */ RDB_MAX_CHECKSUMS_PCT, 0);

static MYSQL_THDVAR_BOOL(store_row_debug_checksums, PLUGIN_VAR_RQCMDARG,
                         "Include checksums when writing index/table records",
                         nullptr, nullptr, false /* default value */);

static MYSQL_THDVAR_BOOL(verify_row_debug_checksums, PLUGIN_VAR_RQCMDARG,
                         "Verify checksums when reading index/table records",
                         nullptr, nullptr, false /* default value */);

static MYSQL_THDVAR_BOOL(master_skip_tx_api, PLUGIN_VAR_RQCMDARG,
                         "Skipping holding any lock on row access. "
                         "Not effective on slave.",
                         nullptr, nullptr, false);

static MYSQL_SYSVAR_UINT(
    validate_tables, rocksdb_validate_tables,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Verify all .frm files match all RocksDB tables (0 means no verification, "
    "1 means verify and fail on error, and 2 means verify but continue",
    nullptr, nullptr, 1 /* default value */, 0 /* min value */,
    2 /* max value */, 0);

static MYSQL_SYSVAR_STR(datadir, rocksdb_datadir,
                        PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                        "RocksDB data directory", nullptr, nullptr,
                        "./.rocksdb_test10");

rpc_logger l_13(2489, "init datadir");

static MYSQL_SYSVAR_UINT(
    table_stats_sampling_pct, rocksdb_table_stats_sampling_pct,
    PLUGIN_VAR_RQCMDARG,
    "Percentage of entries to sample when collecting statistics about table "
    "properties. Specify either 0 to sample everything or percentage "
    "[" STRINGIFY_ARG(RDB_TBL_STATS_SAMPLE_PCT_MIN) ".." STRINGIFY_ARG(
        RDB_TBL_STATS_SAMPLE_PCT_MAX) "]. "
                                      "By default " STRINGIFY_ARG(
                                          RDB_DEFAULT_TBL_STATS_SAMPLE_PCT) "% "
                                                                            "of"
                                                                            " e"
                                                                            "nt"
                                                                            "ri"
                                                                            "es"
                                                                            " a"
                                                                            "re"
                                                                            " "
                                                                            "sa"
                                                                            "mp"
                                                                            "le"
                                                                            "d"
                                                                            ".",
    nullptr, rocksdb_set_table_stats_sampling_pct, /* default */
    RDB_DEFAULT_TBL_STATS_SAMPLE_PCT, /* everything */ 0,
    /* max */ RDB_TBL_STATS_SAMPLE_PCT_MAX, 0);

static MYSQL_SYSVAR_UINT(table_stats_recalc_threshold_pct,
                         rocksdb_table_stats_recalc_threshold_pct,
                         PLUGIN_VAR_RQCMDARG,
                         "Percentage of number of modified rows over total "
                         "number of rows to trigger stats recalculation",
                         nullptr, nullptr, /* default */
                         rocksdb_table_stats_recalc_threshold_pct,
                         /* everything */ 0,
                         /* max */ RDB_TBL_STATS_RECALC_THRESHOLD_PCT_MAX, 0);

static MYSQL_SYSVAR_ULONGLONG(
    table_stats_recalc_threshold_count,
    rocksdb_table_stats_recalc_threshold_count, PLUGIN_VAR_RQCMDARG,
    "Number of modified rows to trigger stats recalculation", nullptr,
    nullptr, /* default */
    rocksdb_table_stats_recalc_threshold_count,
    /* everything */ 0,
    /* max */ UINT64_MAX, 0);

static MYSQL_SYSVAR_INT(
    table_stats_background_thread_nice_value,
    rocksdb_table_stats_background_thread_nice_value, PLUGIN_VAR_RQCMDARG,
    "nice value for index stats", rocksdb_index_stats_thread_renice, nullptr,
    /* default */ rocksdb_table_stats_background_thread_nice_value,
    /* min */ THREAD_PRIO_MIN, /* max */ THREAD_PRIO_MAX, 0);

static MYSQL_SYSVAR_ULONGLONG(
    table_stats_max_num_rows_scanned, rocksdb_table_stats_max_num_rows_scanned,
    PLUGIN_VAR_RQCMDARG,
    "The maximum number of rows to scan in table scan based "
    "cardinality calculation",
    nullptr, nullptr, /* default */
    0, /* everything */ 0,
    /* max */ UINT64_MAX, 0);

static MYSQL_SYSVAR_UINT(
    stats_recalc_rate, rocksdb_stats_recalc_rate, PLUGIN_VAR_RQCMDARG,
    "The number of indexes per second to recalculate statistics for. 0 to "
    "disable background recalculation.",
    nullptr, nullptr, 0 /* default value */, 0 /* min value */,
    UINT_MAX /* max value */, 0);

static MYSQL_SYSVAR_BOOL(table_stats_use_table_scan,
                         rocksdb_table_stats_use_table_scan,
                         PLUGIN_VAR_RQCMDARG,
                         "Enable table scan based index calculation.", nullptr,
                         rocksdb_update_table_stats_use_table_scan,
                         rocksdb_table_stats_use_table_scan);

static MYSQL_SYSVAR_BOOL(
    large_prefix, rocksdb_large_prefix, PLUGIN_VAR_RQCMDARG,
    "Support large index prefix length of 3072 bytes. If off, the maximum "
    "index prefix length is 767.",
    nullptr, nullptr, FALSE);

static MYSQL_SYSVAR_BOOL(
    allow_to_start_after_corruption, rocksdb_allow_to_start_after_corruption,
    PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
    "Allow server still to start successfully even if RocksDB corruption is "
    "detected.",
    nullptr, nullptr, FALSE);

static MYSQL_SYSVAR_BOOL(error_on_suboptimal_collation,
                         rocksdb_error_on_suboptimal_collation,
                         PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
                         "Raise an error instead of warning if a sub-optimal "
                         "collation is used",
                         nullptr, nullptr, TRUE);

static MYSQL_SYSVAR_BOOL(
    enable_insert_with_update_caching,
    rocksdb_enable_insert_with_update_caching, PLUGIN_VAR_OPCMDARG,
    "Whether to enable optimization where we cache the read from a failed "
    "insertion attempt in INSERT ON DUPLICATE KEY UPDATE",
    nullptr, nullptr, TRUE);

static MYSQL_SYSVAR_STR(
    trace_block_cache_access, rocksdb_block_cache_trace_options_str,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Block cache trace option string. The format is "
    "sampling_frequency:max_trace_file_size:trace_file_name. "
    "sampling_frequency and max_trace_file_size are positive integers. The "
    "block accesses are saved to the "
    "rocksdb_datadir/block_cache_traces/trace_file_name.",
    rocksdb_trace_block_cache_access, rocksdb_trace_stub, "");

static MYSQL_SYSVAR_STR(
    trace_queries, rocksdb_trace_options_str,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
    "Trace option string. The format is "
    "sampling_frequency:max_trace_file_size:trace_file_name. "
    "sampling_frequency and max_trace_file_size are positive integers. The "
    "queries are saved to the "
    "rocksdb_datadir/queries_traces/trace_file_name.",
    rocksdb_trace_queries, rocksdb_trace_stub, "");

static MYSQL_SYSVAR_ENUM(
    select_bypass_policy, rocksdb_select_bypass_policy, PLUGIN_VAR_RQCMDARG,
    "Change bypass SELECT related policy and allow directly talk to RocksDB. "
    "Valid values include 'always_off', 'always_on', 'opt_in', 'opt_out'. ",
    nullptr, nullptr, select_bypass_policy_type::default_value,
    &select_bypass_policy_typelib);

static MYSQL_SYSVAR_BOOL(
    select_bypass_fail_unsupported, rocksdb_select_bypass_fail_unsupported,
    PLUGIN_VAR_RQCMDARG,
    "Select bypass would fail for unsupported SELECT commands", nullptr,
    nullptr, TRUE);

static MYSQL_SYSVAR_BOOL(select_bypass_log_rejected,
                         rocksdb_select_bypass_log_rejected,
                         PLUGIN_VAR_RQCMDARG,
                         "Log rejected SELECT bypass queries", nullptr, nullptr,
                         TRUE);

static MYSQL_SYSVAR_BOOL(select_bypass_log_failed,
                         rocksdb_select_bypass_log_failed, PLUGIN_VAR_RQCMDARG,
                         "Log failed SELECT bypass queries", nullptr, nullptr,
                         FALSE);

static MYSQL_SYSVAR_BOOL(select_bypass_allow_filters,
                         rocksdb_select_bypass_allow_filters,
                         PLUGIN_VAR_RQCMDARG,
                         "Allow non-optimal filters in SELECT bypass queries",
                         nullptr, nullptr, TRUE);

rpc_logger l_14(2644, "init select_bypass_allow_filters");

static MYSQL_SYSVAR_UINT(
    select_bypass_rejected_query_history_size,
    rocksdb_select_bypass_rejected_query_history_size, PLUGIN_VAR_RQCMDARG,
    "History size of rejected bypass queries in "
    "information_schema.bypass_rejected_query_history. "
    "Set to 0 to turn off",
    nullptr, rocksdb_select_bypass_rejected_query_history_size_update, 0,
    /* min */ 0, /* max */ INT_MAX, 0);

static MYSQL_SYSVAR_UINT(
    select_bypass_debug_row_delay, rocksdb_select_bypass_debug_row_delay,
    PLUGIN_VAR_RQCMDARG,
    "Test only to inject delays in bypass select to simulate long queries "
    "for each row sent",
    nullptr, nullptr, 0, /* min */ 0, /* max */ INT_MAX, 0);

static MYSQL_SYSVAR_ULONGLONG(
    select_bypass_multiget_min, rocksdb_select_bypass_multiget_min,
    PLUGIN_VAR_RQCMDARG,
    "Minimum number of items to use RocksDB MultiGet API. Default is "
    "SIZE_T_MAX meaning it is turned off. Set to 0 to enable always using "
    "MultiGet",
    nullptr, nullptr, SIZE_T_MAX, /* min */ 0, /* max */ SIZE_T_MAX, 0);

static MYSQL_THDVAR_LONG(mrr_batch_size, PLUGIN_VAR_RQCMDARG,
                         "maximum number of keys to fetch during each MRR",
                         nullptr, nullptr, /* default */ 100, /* min */ 0,
                         /* max */ ROCKSDB_MAX_MRR_BATCH_SIZE, 0);

static MYSQL_SYSVAR_BOOL(skip_locks_if_skip_unique_check,
                         rocksdb_skip_locks_if_skip_unique_check,
                         PLUGIN_VAR_RQCMDARG,
                         "Skip row locking when unique checks are disabled.",
                         check_rocksdb_skip_locks_if_skip_unique_check, nullptr,
                         FALSE);

static MYSQL_SYSVAR_BOOL(
    alter_column_default_inplace, rocksdb_alter_column_default_inplace,
    PLUGIN_VAR_RQCMDARG,
    "Allow inplace alter for alter column default operation", nullptr, nullptr,
    TRUE);

static const int ROCKSDB_ASSUMED_KEY_VALUE_DISK_SIZE = 100;

rpc_logger l_15(2691, "init alter_column_default_inplace");

static struct st_mysql_sys_var *rocksdb_system_variables[] = {
    MYSQL_SYSVAR(lock_wait_timeout), MYSQL_SYSVAR(deadlock_detect),
    MYSQL_SYSVAR(deadlock_detect_depth),
    MYSQL_SYSVAR(commit_time_batch_for_recovery), MYSQL_SYSVAR(max_row_locks),
    MYSQL_SYSVAR(write_batch_max_bytes),
    MYSQL_SYSVAR(write_batch_flush_threshold), MYSQL_SYSVAR(lock_scanned_rows),
    MYSQL_SYSVAR(bulk_load), MYSQL_SYSVAR(bulk_load_allow_sk),
    MYSQL_SYSVAR(bulk_load_allow_unsorted),
    MYSQL_SYSVAR(skip_unique_check_tables), MYSQL_SYSVAR(trace_sst_api),
    MYSQL_SYSVAR(commit_in_the_middle), MYSQL_SYSVAR(blind_delete_primary_key),
    MYSQL_SYSVAR(enable_iterate_bounds), MYSQL_SYSVAR(read_free_rpl_tables),
    MYSQL_SYSVAR(read_free_rpl), MYSQL_SYSVAR(bulk_load_size),
    MYSQL_SYSVAR(merge_buf_size), MYSQL_SYSVAR(enable_bulk_load_api),
    // MYSQL_SYSVAR(enable_pipelined_write),
    MYSQL_SYSVAR(enable_remove_orphaned_dropped_cfs), MYSQL_SYSVAR(tmpdir),
    MYSQL_SYSVAR(merge_combine_read_size),
    MYSQL_SYSVAR(merge_tmp_file_removal_delay_ms),
    MYSQL_SYSVAR(skip_bloom_filter_on_read),

    // MYSQL_SYSVAR(create_if_missing),
    // MYSQL_SYSVAR(two_write_queues),
    // MYSQL_SYSVAR(manual_wal_flush),
    MYSQL_SYSVAR(write_policy),
    // MYSQL_SYSVAR(create_missing_column_families),
    // MYSQL_SYSVAR(error_if_exists),
    // MYSQL_SYSVAR(paranoid_checks),
    MYSQL_SYSVAR(rate_limiter_bytes_per_sec),
    MYSQL_SYSVAR(sst_mgr_rate_bytes_per_sec),
    // MYSQL_SYSVAR(delayed_write_rate),
    MYSQL_SYSVAR(max_latest_deadlocks), MYSQL_SYSVAR(info_log_level),
    // MYSQL_SYSVAR(max_open_files),
    // MYSQL_SYSVAR(max_total_wal_size),
    // MYSQL_SYSVAR(use_fsync),
    // MYSQL_SYSVAR(wal_dir),
    MYSQL_SYSVAR(persistent_cache_path), MYSQL_SYSVAR(persistent_cache_size_mb),
    // MYSQL_SYSVAR(delete_obsolete_files_period_micros),
    // MYSQL_SYSVAR(max_background_jobs),
    // MYSQL_SYSVAR(max_background_flushes),
    // MYSQL_SYSVAR(max_background_compactions),
    MYSQL_SYSVAR(max_bottom_pri_background_compactions),
    // MYSQL_SYSVAR(max_log_file_size),
    // MYSQL_SYSVAR(max_subcompactions),
    // MYSQL_SYSVAR(log_file_time_to_roll),
    // MYSQL_SYSVAR(keep_log_file_num),
    // MYSQL_SYSVAR(max_manifest_file_size),
    // MYSQL_SYSVAR(table_cache_numshardbits),
    // MYSQL_SYSVAR(wal_ttl_seconds),
    // MYSQL_SYSVAR(wal_size_limit_mb),
    // MYSQL_SYSVAR(manifest_preallocation_size),
    // MYSQL_SYSVAR(use_direct_reads),
    // MYSQL_SYSVAR(use_direct_io_for_flush_and_compaction),
    // MYSQL_SYSVAR(allow_mmap_reads),
    // MYSQL_SYSVAR(allow_mmap_writes),
    // MYSQL_SYSVAR(is_fd_close_on_exec),
    // MYSQL_SYSVAR(stats_dump_period_sec),
    // MYSQL_SYSVAR(advise_random_on_open),
    // MYSQL_SYSVAR(db_write_buffer_size),
    // MYSQL_SYSVAR(use_adaptive_mutex),
    // MYSQL_SYSVAR(bytes_per_sync),
    // MYSQL_SYSVAR(wal_bytes_per_sync),
    // MYSQL_SYSVAR(enable_thread_tracking),
    MYSQL_SYSVAR(perf_context_level), MYSQL_SYSVAR(wal_recovery_mode),
    MYSQL_SYSVAR(track_and_verify_wals_in_manifest), MYSQL_SYSVAR(stats_level),
    MYSQL_SYSVAR(access_hint_on_compaction_start),
    // MYSQL_SYSVAR(new_table_reader_for_compaction_inputs),
    // MYSQL_SYSVAR(compaction_readahead_size),
    // MYSQL_SYSVAR(allow_concurrent_memtable_write),
    // MYSQL_SYSVAR(enable_write_thread_adaptive_yield),

    MYSQL_SYSVAR(block_cache_size), MYSQL_SYSVAR(sim_cache_size),
    MYSQL_SYSVAR(use_clock_cache), MYSQL_SYSVAR(cache_high_pri_pool_ratio),
    MYSQL_SYSVAR(cache_dump),
    // MYSQL_SYSVAR(cache_index_and_filter_blocks),
    // MYSQL_SYSVAR(cache_index_and_filter_with_high_priority),
    // MYSQL_SYSVAR(pin_l0_filter_and_index_blocks_in_cache),
    // MYSQL_SYSVAR(index_type),
    // MYSQL_SYSVAR(hash_index_allow_collision),
    // MYSQL_SYSVAR(no_block_cache),
    // MYSQL_SYSVAR(block_size),
    // MYSQL_SYSVAR(block_size_deviation),
    // MYSQL_SYSVAR(block_restart_interval),
    // MYSQL_SYSVAR(whole_key_filtering),

    MYSQL_SYSVAR(default_cf_options), MYSQL_SYSVAR(override_cf_options),
    MYSQL_SYSVAR(update_cf_options), MYSQL_SYSVAR(use_default_sk_cf),

    MYSQL_SYSVAR(flush_log_at_trx_commit), MYSQL_SYSVAR(write_disable_wal),
    MYSQL_SYSVAR(write_ignore_missing_column_families),

    MYSQL_SYSVAR(skip_fill_cache), MYSQL_SYSVAR(unsafe_for_binlog),

    MYSQL_SYSVAR(records_in_range), MYSQL_SYSVAR(force_index_records_in_range),
    MYSQL_SYSVAR(debug_optimizer_n_rows),
    MYSQL_SYSVAR(force_compute_memtable_stats),
    MYSQL_SYSVAR(force_compute_memtable_stats_cachetime),
    MYSQL_SYSVAR(debug_optimizer_no_zero_cardinality),

    MYSQL_SYSVAR(compact_cf), MYSQL_SYSVAR(delete_cf),
    MYSQL_SYSVAR(signal_drop_index_thread), MYSQL_SYSVAR(pause_background_work),
    MYSQL_SYSVAR(enable_2pc), MYSQL_SYSVAR(ignore_unknown_options),
    MYSQL_SYSVAR(strict_collation_check),
    MYSQL_SYSVAR(strict_collation_exceptions),
    MYSQL_SYSVAR(collect_sst_properties),
    MYSQL_SYSVAR(force_flush_memtable_now),
    MYSQL_SYSVAR(force_flush_memtable_and_lzero_now),
    MYSQL_SYSVAR(cancel_manual_compactions), MYSQL_SYSVAR(enable_ttl),
    MYSQL_SYSVAR(enable_ttl_read_filtering), MYSQL_SYSVAR(debug_ttl_rec_ts),
    MYSQL_SYSVAR(debug_ttl_snapshot_ts), MYSQL_SYSVAR(debug_ttl_read_filter_ts),
    MYSQL_SYSVAR(debug_ttl_ignore_pk), MYSQL_SYSVAR(reset_stats),
    MYSQL_SYSVAR(io_write_timeout), MYSQL_SYSVAR(seconds_between_stat_computes),

    MYSQL_SYSVAR(compaction_sequential_deletes),
    MYSQL_SYSVAR(compaction_sequential_deletes_window),
    MYSQL_SYSVAR(compaction_sequential_deletes_file_size),
    MYSQL_SYSVAR(compaction_sequential_deletes_count_sd),
    MYSQL_SYSVAR(print_snapshot_conflict_queries),

    MYSQL_SYSVAR(datadir), MYSQL_SYSVAR(create_checkpoint),

    MYSQL_SYSVAR(checksums_pct), MYSQL_SYSVAR(store_row_debug_checksums),
    MYSQL_SYSVAR(verify_row_debug_checksums), MYSQL_SYSVAR(master_skip_tx_api),

    MYSQL_SYSVAR(validate_tables), MYSQL_SYSVAR(table_stats_sampling_pct),
    MYSQL_SYSVAR(table_stats_recalc_threshold_pct),
    MYSQL_SYSVAR(table_stats_recalc_threshold_count),
    MYSQL_SYSVAR(table_stats_max_num_rows_scanned),
    MYSQL_SYSVAR(table_stats_use_table_scan),
    MYSQL_SYSVAR(table_stats_background_thread_nice_value),

    MYSQL_SYSVAR(large_prefix), MYSQL_SYSVAR(allow_to_start_after_corruption),
    MYSQL_SYSVAR(error_on_suboptimal_collation),
    MYSQL_SYSVAR(stats_recalc_rate),
    MYSQL_SYSVAR(debug_manual_compaction_delay),
    MYSQL_SYSVAR(max_manual_compactions),
    MYSQL_SYSVAR(manual_compaction_threads),
    MYSQL_SYSVAR(manual_compaction_bottommost_level),
    MYSQL_SYSVAR(rollback_on_timeout),

    MYSQL_SYSVAR(enable_insert_with_update_caching),
    MYSQL_SYSVAR(trace_block_cache_access), MYSQL_SYSVAR(trace_queries),
    MYSQL_SYSVAR(select_bypass_policy),
    MYSQL_SYSVAR(select_bypass_fail_unsupported),
    MYSQL_SYSVAR(select_bypass_log_failed),
    MYSQL_SYSVAR(select_bypass_rejected_query_history_size),
    MYSQL_SYSVAR(select_bypass_log_rejected),
    MYSQL_SYSVAR(select_bypass_allow_filters),
    MYSQL_SYSVAR(select_bypass_debug_row_delay),
    MYSQL_SYSVAR(select_bypass_multiget_min), MYSQL_SYSVAR(mrr_batch_size),
    MYSQL_SYSVAR(skip_locks_if_skip_unique_check),
    MYSQL_SYSVAR(alter_column_default_inplace), nullptr};

static rocksdb::WriteOptions rdb_get_rocksdb_write_options(
    my_core::THD *const thd) {
  rocksdb_rpc_log(2838, "rdb_get_rocksdb_write_options: start");
  rocksdb::WriteOptions opt;

  opt.sync = (rocksdb_flush_log_at_trx_commit == FLUSH_LOG_SYNC);
  opt.disableWAL = THDVAR(thd, write_disable_wal);
  opt.ignore_missing_column_families =
      THDVAR(thd, write_ignore_missing_column_families);

  rocksdb_rpc_log(2846, "rdb_get_rocksdb_write_options: end");

  return opt;
}

static int rocksdb_compact_column_family(THD *const thd,
                                         struct st_mysql_sys_var *const var,
                                         void *const var_ptr,
                                         struct st_mysql_value *const value) {
  rocksdb_rpc_log(2855, "rocksdb_compact_column_family: start");

  char buff[STRING_BUFFER_USUAL_SIZE];
  int len = sizeof(buff);

  DBUG_ASSERT(value != nullptr);

  if (const char *const cf = value->val_str(value, buff, &len)) {
    DBUG_EXECUTE_IF("rocksdb_compact_column_family", {
      const char act[] =
          "now signal ready_to_mark_cf_dropped_in_compact_column_family "
          "wait_for mark_cf_dropped_done_in_compact_column_family";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
    });

    std::string cf_name = std::string(cf);
    // use rocksdb_compact_cf="" or "default" to compact default CF
    if (cf_name.empty()) cf_name = DEFAULT_CF_NAME;

    auto cfh = cf_manager.get_cf(cf_name);
    if (cfh != nullptr && rdb != nullptr) {
      rocksdb::BottommostLevelCompaction bottommost_level_compaction =
          (rocksdb::BottommostLevelCompaction)THDVAR(
              thd, manual_compaction_bottommost_level);

      int mc_id = rdb_mc_thread.request_manual_compaction(
          cfh, nullptr, nullptr, THDVAR(thd, manual_compaction_threads),
          bottommost_level_compaction);
      if (mc_id == -1) {
        my_error(ER_INTERNAL_ERROR, MYF(0),
                 "Can't schedule more manual compactions. "
                 "Increase rocksdb_max_manual_compactions or stop issuing "
                 "more manual compactions.");
        rocksdb_rpc_log(2888, "rocksdb_compact_column_family: end");
        return HA_EXIT_FAILURE;
      } else if (mc_id < 0) {
        rocksdb_rpc_log(2891, "rocksdb_compact_column_family: end");
        return HA_EXIT_FAILURE;
      }
      // NO_LINT_DEBUG
      sql_print_information("RocksDB: Manual compaction of column family: %s\n",
                            cf);
      // Checking thd state every short cycle (100ms). This is for allowing to
      // exiting this function without waiting for CompactRange to finish.
      Rdb_manual_compaction_thread::Manual_compaction_request::mc_state
          mc_status;
      do {
        my_sleep(100000);
        mc_status = rdb_mc_thread.manual_compaction_state(mc_id);
      } while (!thd->killed &&
               (mc_status == Rdb_manual_compaction_thread::
                                 Manual_compaction_request::PENDING ||
                mc_status == Rdb_manual_compaction_thread::
                                 Manual_compaction_request::RUNNING));

      bool mc_timeout = false;
      if (thd->killed) {
        // Cancelling pending or running manual compaction with 60s timeout
        mc_timeout = rdb_mc_thread.cancel_manual_compaction_request(mc_id, 600);
      }

      mc_status = rdb_mc_thread.manual_compaction_state(mc_id);
      if (mc_status !=
          Rdb_manual_compaction_thread::Manual_compaction_request::SUCCESS) {
        std::string msg = "Manual Compaction Failed. Reason: ";
        if (thd->killed) {
          msg += "Cancelled by client.";
        } else if (mc_status == Rdb_manual_compaction_thread::
                                    Manual_compaction_request::CANCEL) {
          msg += "Cancelled by server.";
        } else {
          msg += "General failures.";
        }
        if (mc_timeout) {
          msg += " (timeout)";
        }
        my_error(ER_INTERNAL_ERROR, MYF(0), msg.c_str());
        rdb_mc_thread.set_client_done(mc_id);
        rocksdb_rpc_log(2931, "rocksdb_compact_column_family: end");

        return HA_EXIT_FAILURE;
      }
      // manual compaction succeeded.
      rdb_mc_thread.set_client_done(mc_id);
    }
  }
  rocksdb_rpc_log(2939, "rocksdb_compact_column_family: end");

  return HA_EXIT_SUCCESS;
}

/*
 * Serializes an xid to a string so that it can
 * be used as a rocksdb transaction name
 */
static std::string rdb_xid_to_string(const XID &src) {
  rocksdb_rpc_log(2951, "rdb_xid_to_string: start");
  DBUG_ASSERT(src.gtrid_length >= 0 && src.gtrid_length <= MAXGTRIDSIZE);
  DBUG_ASSERT(src.bqual_length >= 0 && src.bqual_length <= MAXBQUALSIZE);

  std::string buf;
  buf.reserve(RDB_XIDHDR_LEN + src.gtrid_length + src.bqual_length);

  /*
   * expand formatID to fill 8 bytes if it doesn't already
   * then reinterpret bit pattern as unsigned and store in network order
   */
  uchar fidbuf[RDB_FORMATID_SZ];
  int64 signed_fid8 = src.formatID;
  const uint64 raw_fid8 = *reinterpret_cast<uint64 *>(&signed_fid8);
  rdb_netbuf_store_uint64(fidbuf, raw_fid8);
  buf.append(reinterpret_cast<const char *>(fidbuf), RDB_FORMATID_SZ);

  buf.push_back(src.gtrid_length);
  buf.push_back(src.bqual_length);
  buf.append(src.data, (src.gtrid_length) + (src.bqual_length));
  rocksdb_rpc_log(2971, "rdb_xid_to_string: end");
  return buf;
}

///////////////////////////////////////////////////////////////////////////////////////////

/*
  Drop index thread's control
*/

static void rocksdb_drop_index_wakeup_thread(
    my_core::THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  rocksdb_rpc_log(2985, "rocksdb_drop_index_wakeup_thread: start");
  if (*static_cast<const bool *>(save)) {
    rdb_drop_idx_thread.signal();
  }
  rocksdb_rpc_log(2989, "rocksdb_drop_index_wakeup_thread: end");
}

static inline uint32_t rocksdb_perf_context_level(THD *const thd) {
  DBUG_ASSERT(thd != nullptr);
  rocksdb_rpc_log(2994, "rocksdb_perf_context_level: start");
  const int session_perf_context_level = THDVAR(thd, perf_context_level);
  if (session_perf_context_level > rocksdb::PerfLevel::kUninitialized) {
    rocksdb_rpc_log(2997, "rocksdb_perf_context_level: end");
    return session_perf_context_level;
  }

  /*
    Fallback to global thdvar, if session specific one was not set to a valid
    value.
  */

  const int global_perf_context_level = THDVAR(nullptr, perf_context_level);
  if (global_perf_context_level > rocksdb::PerfLevel::kUninitialized) {
    rocksdb_rpc_log(3008, "rocksdb_perf_context_level: end");
    return global_perf_context_level;
  }
  rocksdb_rpc_log(3011, "rocksdb_perf_context_level: end");
  return rocksdb::PerfLevel::kDisable;
}

/*
  Very short (functor-like) interface to be passed to
  Rdb_transaction::walk_tx_list()
*/

interface Rdb_tx_list_walker {
  virtual ~Rdb_tx_list_walker() {}
  virtual void process_tran(const Rdb_transaction *const) = 0;
};

/*
  This is a helper class that is passed to RocksDB to get notifications when
  a snapshot gets created.
*/

class Rdb_snapshot_notifier : public rocksdb::TransactionNotifier {
  Rdb_transaction *m_owning_tx;

  void SnapshotCreated(const rocksdb::Snapshot *snapshot) override;

 public:
  Rdb_snapshot_notifier(const Rdb_snapshot_notifier &) = delete;
  Rdb_snapshot_notifier &operator=(const Rdb_snapshot_notifier &) = delete;

  explicit Rdb_snapshot_notifier(Rdb_transaction *const owning_tx)
      : m_owning_tx(owning_tx) {}

  // If the owning Rdb_transaction gets destructed we need to not reference
  // it anymore.
  void detach() { m_owning_tx = nullptr; }
};

/* This is the base class for transactions when interacting with rocksdb.
 */
class Rdb_transaction {
 protected:
  ulonglong m_write_count = 0;
  ulonglong m_insert_count = 0;
  ulonglong m_update_count = 0;
  ulonglong m_delete_count = 0;
  // per row data
  ulonglong m_row_lock_count = 0;
  std::unordered_map<GL_INDEX_ID, ulonglong> m_auto_incr_map;

  bool m_is_delayed_snapshot = false;
  bool m_is_two_phase = false;

  std::unordered_set<Rdb_tbl_def *> modified_tables;

 private:
  /*
    Number of write operations this transaction had when we took the last
    savepoint (the idea is not to take another savepoint if we haven't made
    any changes)
  */
  ulonglong m_writes_at_last_savepoint;

 protected:
  THD *m_thd = nullptr;

  static std::multiset<Rdb_transaction *> s_tx_list;
  static mysql_mutex_t s_tx_list_mutex;

  Rdb_io_perf *m_tbl_io_perf;

  bool m_tx_read_only = false;

  int m_timeout_sec; /* Cached value of @@rocksdb_lock_wait_timeout */

  /* Maximum number of locks the transaction can have */
  ulonglong m_max_row_locks;

  bool m_is_tx_failed = false;
  bool m_rollback_only = false;

  std::shared_ptr<Rdb_snapshot_notifier> m_notifier;

  // This should be used only when updating binlog information.
  virtual rocksdb::WriteBatchBase *get_write_batch() = 0;
  virtual bool commit_no_binlog() = 0;

  /*
    @detail
      This function takes in the WriteBatch of the transaction to add
      all the AUTO_INCREMENT merges. It does so by iterating through
      m_auto_incr_map and then constructing key/value pairs to call merge upon.

    @param wb
   */
  rocksdb::Status merge_auto_incr_map(rocksdb::WriteBatchBase *const wb) {
    DBUG_EXECUTE_IF("myrocks_autoinc_upgrade", return rocksdb::Status::OK(););
    rocksdb_rpc_log(3106, "merge_auto_incr_map: start");

    // Iterate through the merge map merging all keys into data dictionary.
    rocksdb::Status s;
    for (auto &it : m_auto_incr_map) {
      s = dict_manager.put_auto_incr_val(wb, it.first, it.second);
      if (!s.ok()) {
        rocksdb_rpc_log(3113, "merge_auto_incr_map: end");
        return s;
      }
    }
    m_auto_incr_map.clear();
    rocksdb_rpc_log(3118, "merge_auto_incr_map: end");
    return s;
  }

 protected:
  /*
    The following two are helper functions to be overloaded by child classes.
    They should provide RocksDB's savepoint semantics.
  */
  virtual void do_set_savepoint() = 0;
  virtual rocksdb::Status do_pop_savepoint() = 0;
  virtual void do_rollback_to_savepoint() = 0;

 public:
  rocksdb::ReadOptions *m_read_opts;
  const char *m_mysql_log_file_name;
  my_off_t m_mysql_log_offset;
  const char *m_mysql_gtid;
  const char *m_mysql_max_gtid;
  String m_detailed_error;
  int64_t m_snapshot_timestamp = 0;
  bool m_ddl_transaction;
  std::shared_ptr<Rdb_explicit_snapshot> m_explicit_snapshot;

  /*
    Tracks the number of tables in use through external_lock.
    This should not be reset during start_tx().
  */
  int64_t m_n_mysql_tables_in_use = 0;

  /*
    for distinction between rdb_transaction_impl and rdb_writebatch_impl
    when using walk tx list
  */
  virtual bool is_writebatch_trx() const = 0;

  static void init_mutex() {
    rocksdb_rpc_log(3155, "init_mutex: start");
    mysql_mutex_init(key_mutex_tx_list, &s_tx_list_mutex, MY_MUTEX_INIT_FAST);
    rocksdb_rpc_log(3157, "init_mutex: end");
  }

  static void term_mutex() {
    DBUG_ASSERT(s_tx_list.size() == 0);
    rocksdb_rpc_log(3162, "term_mutex: start");
    mysql_mutex_destroy(&s_tx_list_mutex);
    rocksdb_rpc_log(3164, "term_mutex: start");
  }

  static void walk_tx_list(Rdb_tx_list_walker *walker) {
    rocksdb_rpc_log(3168, "walk_tx_list: start");
    DBUG_ASSERT(walker != nullptr);

    RDB_MUTEX_LOCK_CHECK(s_tx_list_mutex);

    for (auto it : s_tx_list) {
      walker->process_tran(it);
    }

    RDB_MUTEX_UNLOCK_CHECK(s_tx_list_mutex);
    rocksdb_rpc_log(3178, "walk_tx_list: end");
  }

  int set_status_error(THD *const thd, const rocksdb::Status &s,
                       const Rdb_key_def &kd, Rdb_tbl_def *const tbl_def,
                       Rdb_table_handler *const table_handler) {
    rocksdb_rpc_log(3184, "set_status_error: start");
    DBUG_ASSERT(!s.ok());
    DBUG_ASSERT(tbl_def != nullptr);

    if (s.IsTimedOut()) {
      /*
        SQL layer has weird expectations. If we return an error when
        doing a read in DELETE IGNORE, it will ignore the error ("because it's
        an IGNORE command!) but then will fail an assert, because "error code
        was returned, but no error happened".  Do what InnoDB's
        convert_error_code_to_mysql() does: force a statement
        rollback before returning HA_ERR_LOCK_WAIT_TIMEOUT:
        */
      my_core::thd_mark_transaction_to_rollback(
          thd, static_cast<bool>(rocksdb_rollback_on_timeout));
      m_detailed_error.copy(timeout_message(
          "index", tbl_def->full_tablename().c_str(), kd.get_name().c_str()));
      table_handler->m_lock_wait_timeout_counter.inc();
      rocksdb_row_lock_wait_timeouts++;
      rocksdb_rpc_log(3203, "set_status_error: end");
      return HA_ERR_LOCK_WAIT_TIMEOUT;
    }

    if (s.IsDeadlock()) {
      my_core::thd_mark_transaction_to_rollback(thd,
                                                true /* whole transaction */);
      m_detailed_error = String();
      table_handler->m_deadlock_counter.inc();
      rocksdb_row_lock_deadlocks++;
      rocksdb_rpc_log(3213, "set_status_error: end");
      return HA_ERR_LOCK_DEADLOCK;
    } else if (s.IsBusy()) {
      rocksdb_snapshot_conflict_errors++;
      if (rocksdb_print_snapshot_conflict_queries) {
        char user_host_buff[MAX_USER_HOST_SIZE + 1];
        make_user_name(thd, user_host_buff);
        // NO_LINT_DEBUG
        sql_print_warning(
            "Got snapshot conflict errors: User: %s "
            "Query: %s",
            user_host_buff, thd->query());
      }
      m_detailed_error = String(" (snapshot conflict)", system_charset_info);
      table_handler->m_deadlock_counter.inc();
      rocksdb_rpc_log(3228, "set_status_error: end");
      return HA_ERR_ROCKSDB_STATUS_BUSY;
    }

    if (s.IsIOError() || s.IsCorruption()) {
      rdb_handle_io_error(s, RDB_IO_ERROR_GENERAL);
    }
    rocksdb_rpc_log(3235, "set_status_error: end");
    return ha_rocksdb::rdb_error_to_mysql(s);
  }

  THD *get_thd() const {
    rocksdb_rpc_log(3240, "get_thd");
    return m_thd;
  }

  /* Used for tracking io_perf counters */
  void io_perf_start(Rdb_io_perf *const io_perf) {
    /*
      Since perf_context is tracked per thread, it is difficult and expensive
      to maintain perf_context on a per table basis. Therefore, roll all
      perf_context data into the first table used in a query. This works well
      for single table queries and is probably good enough for queries that hit
      multiple tables.

      perf_context stats gathering is started when the table lock is acquired
      or when ha_rocksdb::start_stmt is called in case of LOCK TABLES. They
      are recorded when the table lock is released, or when commit/rollback
      is called on the transaction, whichever comes first. Table lock release
      and commit/rollback can happen in different orders. In the case where
      the lock is released before commit/rollback is called, an extra step to
      gather stats during commit/rollback is needed.
    */
    rocksdb_rpc_log(3261, "io_perf_start: start");
    if (m_tbl_io_perf == nullptr &&
        io_perf->start(rocksdb_perf_context_level(m_thd))) {
      m_tbl_io_perf = io_perf;
    }
    rocksdb_rpc_log(3266, "io_perf_start: end");
  }

  void io_perf_end_and_record(void) {
    rocksdb_rpc_log(3270, "io_perf_end_and_record: start");
    if (m_tbl_io_perf != nullptr) {
      m_tbl_io_perf->end_and_record(rocksdb_perf_context_level(m_thd));
      m_tbl_io_perf = nullptr;
    }
    rocksdb_rpc_log(3275, "io_perf_end_and_record: end");
  }

  void io_perf_end_and_record(Rdb_io_perf *const io_perf) {
    rocksdb_rpc_log(3279, "io_perf_end_and_record: start");
    if (m_tbl_io_perf == io_perf) {
      io_perf_end_and_record();
    }
    rocksdb_rpc_log(3283, "io_perf_end_and_record: end");
  }

  void update_bytes_written(ulonglong bytes_written) {
    rocksdb_rpc_log(3287, "update_bytes_written: start");
    if (m_tbl_io_perf != nullptr) {
      m_tbl_io_perf->update_bytes_written(rocksdb_perf_context_level(m_thd),
                                          bytes_written);
    }
    rocksdb_rpc_log(3292, "update_bytes_written: end");
  }

  void set_params(int timeout_sec_arg, int max_row_locks_arg) {
    rocksdb_rpc_log(3296, "set_params: start");
    m_timeout_sec = timeout_sec_arg;
    m_max_row_locks = max_row_locks_arg;
    set_lock_timeout(timeout_sec_arg);
    rocksdb_rpc_log(3300, "set_params: start");
  }

  virtual void set_lock_timeout(int timeout_sec_arg) = 0;

  ulonglong get_write_count() const { return m_write_count; }

  ulonglong get_insert_count() const { return m_insert_count; }

  ulonglong get_update_count() const { return m_update_count; }

  ulonglong get_delete_count() const { return m_delete_count; }

  ulonglong get_row_lock_count() const { return m_row_lock_count; }

  void incr_insert_count() { ++m_insert_count; }

  void incr_update_count() { ++m_update_count; }

  void incr_delete_count() { ++m_delete_count; }

  void incr_row_lock_count() { ++m_row_lock_count; }

  ulonglong get_max_row_lock_count() const { return m_max_row_locks; }

  int get_timeout_sec() const { return m_timeout_sec; }

  virtual void set_sync(bool sync) = 0;

  virtual void release_lock(const Rdb_key_def &key_descr,
                            const std::string &rowkey) = 0;

  virtual bool prepare() = 0;

  bool commit_or_rollback() {
    rocksdb_rpc_log(3335, "commit_or_rollback: start");
    bool res;
    if (m_is_tx_failed) {
      rollback();
      res = false;
    } else {
      res = commit();
    }
    rocksdb_rpc_log(3343, "commit_or_rollback: start");
    return res;
  }

  bool commit() {
    rocksdb_rpc_log(3348, "commit: start");
    if (get_write_count() == 0) {
      rollback();
      rocksdb_rpc_log(3351, "commit: end");
      return false;
    } else if (m_rollback_only) {
      /*
        Transactions marked as rollback_only are expected to be rolled back at
        prepare(). But there are some exceptions like below that prepare() is
        never called and commit() is called instead.
         1. Binlog is disabled
         2. No modification exists in binlog cache for the transaction (#195)
        In both cases, rolling back transaction is safe. Nothing is written to
        binlog.
       */
      my_error(ER_ROLLBACK_ONLY, MYF(0));
      rollback();
      rocksdb_rpc_log(3365, "commit: end");
      return true;
    } else {
      my_core::thd_binlog_pos(m_thd, &m_mysql_log_file_name,
                              &m_mysql_log_offset, &m_mysql_gtid,
                              &m_mysql_max_gtid);
      binlog_manager.update(m_mysql_log_file_name, m_mysql_log_offset,
                            m_mysql_max_gtid, get_write_batch());
      rocksdb_rpc_log(3373, "commit: end");
      return commit_no_binlog();
    }
  }

  virtual void rollback() = 0;

  // ALTER
  void snapshot_created(const rocksdb::Snapshot *snapshot) {
    rocksdb_rpc_log(3382, "snapshot_created: start");
    DBUG_ASSERT(snapshot != nullptr);

    rocksdb_rpc_log(3385, "snapshot_created: rocksdb_ReadOptions__SetSnapshot");
    // ALTER
    // m_read_opts.snapshot = snapshot;
    rocksdb_ReadOptions__SetSnapshot(m_read_opts, snapshot);

    rocksdb_rpc_log(3393, "snapshot_created: rocksdb_Env__GetCurrentTime");
    // ALTER
    // rdb->GetEnv()->GetCurrentTime(&m_snapshot_timestamp);
    rocksdb_Env__GetCurrentTime(rocksdb_TransactionDB__GetEnv(rdb),
                                &m_snapshot_timestamp);
    m_is_delayed_snapshot = false;
    rocksdb_rpc_log(3396, "snapshot_created: start");
  }

  virtual void acquire_snapshot(bool acquire_now) = 0;
  virtual void release_snapshot() = 0;

  bool has_snapshot() const {
    // ALTER
    // return m_read_opts.snapshot != nullptr;
    rocksdb_rpc_log(3406, "has_snapshot: rocksdb_ReadOptions__GetSnapshot");
    return rocksdb_ReadOptions__GetSnapshot(m_read_opts) != nullptr;
  }

 private:
  // The Rdb_sst_info structures we are currently loading.  In a partitioned
  // table this can have more than one entry
  std::vector<std::shared_ptr<Rdb_sst_info>> m_curr_bulk_load;
  std::string m_curr_bulk_load_tablename;

  /* External merge sorts for bulk load: key ID -> merge sort instance */
  std::unordered_map<GL_INDEX_ID, Rdb_index_merge> m_key_merge;

 public:
  int get_key_merge(GL_INDEX_ID kd_gl_id, rocksdb::ColumnFamilyHandle *cf,
                    Rdb_index_merge **key_merge) {
    rocksdb_rpc_log(3421, "get_key_merge: start");
    int res;
    auto it = m_key_merge.find(kd_gl_id);
    if (it == m_key_merge.end()) {
      m_key_merge.emplace(
          std::piecewise_construct, std::make_tuple(kd_gl_id),
          std::make_tuple(
              get_rocksdb_tmpdir(), THDVAR(get_thd(), merge_buf_size),
              THDVAR(get_thd(), merge_combine_read_size),
              THDVAR(get_thd(), merge_tmp_file_removal_delay_ms), cf));
      it = m_key_merge.find(kd_gl_id);
      if ((res = it->second.init()) != 0) {
        rocksdb_rpc_log(3433, "get_key_merge: end");
        return res;
      }
    }
    *key_merge = &it->second;
    rocksdb_rpc_log(3438, "get_key_merge: end");
    return HA_EXIT_SUCCESS;
  }

  /* Finish bulk loading for all table handlers belongs to one connection */
  int finish_bulk_load(bool *is_critical_error = nullptr,
                       int print_client_error = true) {
    rocksdb_rpc_log(3445, "finish_bulk_load: start");
    Ensure_cleanup cleanup([&]() {
      // Always clear everything regardless of success/failure
      m_curr_bulk_load.clear();
      m_curr_bulk_load_tablename.clear();
      m_key_merge.clear();
    });

    int rc = 0;
    if (is_critical_error) {
      *is_critical_error = true;
    }

    // PREPARE phase: finish all on-going bulk loading Rdb_sst_info and
    // collect all Rdb_sst_commit_info containing (SST files, cf)
    int rc2 = 0;
    std::vector<Rdb_sst_info::Rdb_sst_commit_info> sst_commit_list;
    sst_commit_list.reserve(m_curr_bulk_load.size());

    for (auto &sst_info : m_curr_bulk_load) {
      Rdb_sst_info::Rdb_sst_commit_info commit_info;

      // Commit the list of SST files and move it to the end of
      // sst_commit_list, effectively transfer the ownership over
      rc2 = sst_info->finish(&commit_info, print_client_error);
      if (rc2 && rc == 0) {
        // Don't return yet - make sure we finish all the SST infos
        rc = rc2;
      }

      // Make sure we have work to do - we might be losing the race
      if (rc2 == 0 && commit_info.has_work()) {
        sst_commit_list.emplace_back(std::move(commit_info));
        DBUG_ASSERT(!commit_info.has_work());
      }
    }

    if (rc) {
      rocksdb_rpc_log(3483, "finish_bulk_load: end");
      return rc;
    }

    // MERGING Phase: Flush the index_merge sort buffers into SST files in
    // Rdb_sst_info and collect all Rdb_sst_commit_info containing
    // (SST files, cf)
    if (!m_key_merge.empty()) {
      Ensure_cleanup malloc_cleanup([]() {
        /*
          Explicitly tell jemalloc to clean up any unused dirty pages at this
          point.
          See https://reviews.facebook.net/D63723 for more details.
        */
        purge_all_jemalloc_arenas();
      });

      rocksdb::Slice merge_key;
      rocksdb::Slice merge_val;
      for (auto it = m_key_merge.begin(); it != m_key_merge.end(); it++) {
        GL_INDEX_ID index_id = it->first;
        std::shared_ptr<const Rdb_key_def> keydef =
            ddl_manager.safe_find(index_id);
        std::string table_name = ddl_manager.safe_get_table_name(index_id);

        // Unable to find key definition or table name since the
        // table could have been dropped.
        // TODO(herman): there is a race here between dropping the table
        // and detecting a drop here. If the table is dropped while bulk
        // loading is finishing, these keys being added here may
        // be missed by the compaction filter and not be marked for
        // removal. It is unclear how to lock the sql table from the storage
        // engine to prevent modifications to it while bulk load is occurring.
        if (keydef == nullptr) {
          if (is_critical_error) {
            // We used to set the error but simply ignores it. This follows
            // current behavior and we should revisit this later
            *is_critical_error = false;
          }
          rocksdb_rpc_log(3522, "finish_bulk_load: end");
          return HA_ERR_KEY_NOT_FOUND;
        } else if (table_name.empty()) {
          if (is_critical_error) {
            // We used to set the error but simply ignores it. This follows
            // current behavior and we should revisit this later
            *is_critical_error = false;
          }
          rocksdb_rpc_log(3530, "finish_bulk_load: end");
          return HA_ERR_NO_SUCH_TABLE;
        }
        const std::string &index_name = keydef->get_name();
        Rdb_index_merge &rdb_merge = it->second;

        // Rdb_sst_info expects a denormalized table name in the form of
        // "./database/table"
        std::replace(table_name.begin(), table_name.end(), '.', '/');
        table_name = "./" + table_name;
        // ALTER
        // auto sst_info = std::make_shared<Rdb_sst_info>(
        //     rdb, table_name, index_name, rdb_merge.get_cf(),
        //     *rocksdb_db_options, THDVAR(get_thd(), trace_sst_api));
        auto sst_info = std::make_shared<Rdb_sst_info>(
            rdb, table_name, index_name, rdb_merge.get_cf(), rocksdb_db_options,
            THDVAR(get_thd(), trace_sst_api));

        while ((rc2 = rdb_merge.next(&merge_key, &merge_val)) == 0) {
          if ((rc2 = sst_info->put(merge_key, merge_val)) != 0) {
            rc = rc2;

            // Don't return yet - make sure we finish the sst_info
            break;
          }
        }

        // -1 => no more items
        if (rc2 != -1 && rc != 0) {
          rc = rc2;
        }

        Rdb_sst_info::Rdb_sst_commit_info commit_info;
        rc2 = sst_info->finish(&commit_info, print_client_error);
        if (rc2 != 0 && rc == 0) {
          // Only set the error from sst_info->finish if finish failed and we
          // didn't fail before. In other words, we don't have finish's
          // success mask earlier failures
          rc = rc2;
        }

        if (rc) {
          rocksdb_rpc_log(3572, "finish_bulk_load: end");
          return rc;
        }

        if (commit_info.has_work()) {
          sst_commit_list.emplace_back(std::move(commit_info));
          DBUG_ASSERT(!commit_info.has_work());
        }
      }
    }

    // Early return in case we lost the race completely and end up with no
    // work at all
    if (sst_commit_list.size() == 0) {
      rocksdb_rpc_log(3586, "finish_bulk_load: end");
      return rc;
    }

    // INGEST phase: Group all Rdb_sst_commit_info by cf (as they might
    // have the same cf across different indexes) and call out to RocksDB
    // to ingest all SST files in one atomic operation
    rocksdb::IngestExternalFileOptions options;
    options.move_files = true;
    options.snapshot_consistency = false;
    options.allow_global_seqno = false;
    options.allow_blocking_flush = false;

    std::map<rocksdb::ColumnFamilyHandle *, rocksdb::IngestExternalFileArg>
        arg_map;

    // Group by column_family
    for (auto &commit_info : sst_commit_list) {
      if (arg_map.find(commit_info.get_cf()) == arg_map.end()) {
        rocksdb::IngestExternalFileArg arg;
        arg.column_family = commit_info.get_cf(),
        arg.external_files = commit_info.get_committed_files(),
        arg.options = options;

        arg_map.emplace(commit_info.get_cf(), arg);
      } else {
        auto &files = arg_map[commit_info.get_cf()].external_files;
        files.insert(files.end(), commit_info.get_committed_files().begin(),
                     commit_info.get_committed_files().end());
      }
    }

    std::vector<rocksdb::IngestExternalFileArg> args;
    size_t file_count = 0;
    for (auto &cf_files_pair : arg_map) {
      args.push_back(cf_files_pair.second);
      file_count += cf_files_pair.second.external_files.size();
    }

    // ALTER
    // const rocksdb::Status s = rdb->IngestExternalFiles(args);
    rocksdb_rpc_log(
        3627, "finish_bulk_load: rocksdb_TransactionDB__IngestExternalFiles");
    const rocksdb::Status s =
        rocksdb_TransactionDB__IngestExternalFiles(rdb, args);

    if (THDVAR(m_thd, trace_sst_api)) {
      // NO_LINT_DEBUG
      sql_print_information(
          "SST Tracing: IngestExternalFile '%zu' files returned %s", file_count,
          s.ok() ? "ok" : "not ok");
    }

    if (!s.ok()) {
      if (print_client_error) {
        Rdb_sst_info::report_error_msg(s, nullptr);
      }
      rocksdb_rpc_log(3642, "finish_bulk_load: end");
      return HA_ERR_ROCKSDB_BULK_LOAD;
    }

    // COMMIT phase: mark everything as completed. This avoids SST file
    // deletion kicking in. Otherwise SST files would get deleted if this
    // entire operation is aborted
    for (auto &commit_info : sst_commit_list) {
      commit_info.commit();
    }
    rocksdb_rpc_log(3652, "finish_bulk_load: end");
    return rc;
  }

  int start_bulk_load(ha_rocksdb *const bulk_load,
                      std::shared_ptr<Rdb_sst_info> sst_info) {
    /*
     If we already have an open bulk load of a table and the name doesn't
     match the current one, close out the currently running one.  This allows
     multiple bulk loads to occur on a partitioned table, but then closes
     them all out when we switch to another table.
    */
    DBUG_ASSERT(bulk_load != nullptr);
    rocksdb_rpc_log(3665, "start_bulk_load: start");

    if (!m_curr_bulk_load.empty() &&
        bulk_load->get_table_basename() != m_curr_bulk_load_tablename) {
      const auto res = finish_bulk_load();
      if (res != HA_EXIT_SUCCESS) {
        rocksdb_rpc_log(3671, "start_bulk_load: end");
        return res;
      }
    }

    /*
     This used to track ha_rocksdb handler objects, but those can be
     freed by the table cache while this was referencing them. Instead
     of tracking ha_rocksdb handler objects, this now tracks the
     Rdb_sst_info allocated, and both the ha_rocksdb handler and the
     Rdb_transaction both have shared pointers to them.

     On transaction complete, it will commit each Rdb_sst_info structure found.
     If the ha_rocksdb object is freed, etc., it will also commit
     the Rdb_sst_info. The Rdb_sst_info commit path needs to be idempotent.
    */
    m_curr_bulk_load.push_back(sst_info);
    m_curr_bulk_load_tablename = bulk_load->get_table_basename();
    rocksdb_rpc_log(3689, "start_bulk_load: end");
    return HA_EXIT_SUCCESS;
  }

  int num_ongoing_bulk_load() const { return m_curr_bulk_load.size(); }

  const char *get_rocksdb_tmpdir() const {
    rocksdb_rpc_log(3696, "get_rocksdb_tmpdir: start");
    const char *tmp_dir = THDVAR(get_thd(), tmpdir);

    /*
      We want to treat an empty string as nullptr, in these cases DDL operations
      will use the default --tmpdir passed to mysql instead.
    */
    if (tmp_dir != nullptr && *tmp_dir == '\0') {
      tmp_dir = nullptr;
    }
    rocksdb_rpc_log(3706, "get_rocksdb_tmpdir: end");
    return (tmp_dir);
  }

  /*
    Flush the data accumulated so far. This assumes we're doing a bulk insert.

    @detail
      This should work like transaction commit, except that we don't
      synchronize with the binlog (there is no API that would allow to have
      binlog flush the changes accumulated so far and return its current
      position)

    @todo
      Add test coverage for what happens when somebody attempts to do bulk
      inserts while inside a multi-statement transaction.
  */
  bool flush_batch() {
    rocksdb_rpc_log(3724, "flush_batch: start");
    if (get_write_count() == 0) return false;

    /* Commit the current transaction */
    if (commit_no_binlog()) return true;

    /* Start another one */
    start_tx();
    rocksdb_rpc_log(3732, "flush_batch: end");
    return false;
  }

  void set_auto_incr(const GL_INDEX_ID &gl_index_id, ulonglong curr_id) {
    rocksdb_rpc_log(3737, "set_auto_incr: start");
    m_auto_incr_map[gl_index_id] =
        std::max(m_auto_incr_map[gl_index_id], curr_id);
    rocksdb_rpc_log(3740, "set_auto_incr: end");
  }

#ifndef DBUG_OFF
  ulonglong get_auto_incr(const GL_INDEX_ID &gl_index_id) {
    rocksdb_rpc_log(3745, "get_auto_incr: start");
    if (m_auto_incr_map.count(gl_index_id) > 0) {
      rocksdb_rpc_log(3747, "get_auto_incr: end");
      return m_auto_incr_map[gl_index_id];
    }
    rocksdb_rpc_log(3750, "get_auto_incr: end");
    return 0;
  }
#endif

  virtual rocksdb::Status put(rocksdb::ColumnFamilyHandle *const column_family,
                              const rocksdb::Slice &key,
                              const rocksdb::Slice &value,
                              const bool assume_tracked) = 0;
  virtual rocksdb::Status delete_key(
      rocksdb::ColumnFamilyHandle *const column_family,
      const rocksdb::Slice &key, const bool assume_tracked) = 0;
  virtual rocksdb::Status single_delete(
      rocksdb::ColumnFamilyHandle *const column_family,
      const rocksdb::Slice &key, const bool assume_tracked) = 0;

  virtual bool has_modifications() const = 0;

  virtual rocksdb::WriteBatchBase *get_indexed_write_batch() = 0;
  /*
    Return a WriteBatch that one can write to. The writes will skip any
    transaction locking. The writes will NOT be visible to the transaction.
  */
  rocksdb::WriteBatchBase *get_blind_write_batch() {
    // ALTER
    rocksdb_rpc_log(
        3775, "get_blind_write_batch: rocksdb_WriteBatchBase__GetWriteBatch");
    return rocksdb_WriteBatchBase__GetWriteBatch(get_indexed_write_batch());
  }

  virtual rocksdb::Status get(rocksdb::ColumnFamilyHandle *const column_family,
                              const rocksdb::Slice &key,
                              rocksdb::PinnableSlice *&value) const = 0;

  virtual rocksdb::Status get_for_update(const Rdb_key_def &key_descr,
                                         const rocksdb::Slice &key,
                                         rocksdb::PinnableSlice *&value,
                                         bool exclusive,
                                         const bool do_validate) = 0;

  // ALTER
  virtual rocksdb::Iterator *get_iterator(
      rocksdb::ReadOptions *options,
      rocksdb::ColumnFamilyHandle *column_family) = 0;

  virtual void multi_get(rocksdb::ColumnFamilyHandle *const column_family,
                         const size_t num_keys, const rocksdb::Slice *keys,
                         rocksdb::PinnableSlice **values,
                         rocksdb::Status *statuses,
                         const bool sorted_input) const = 0;

  rocksdb::Iterator *get_iterator(
      rocksdb::ColumnFamilyHandle *const column_family, bool skip_bloom_filter,
      bool fill_cache, const rocksdb::Slice &eq_cond_lower_bound,
      const rocksdb::Slice &eq_cond_upper_bound, bool read_current = false,
      bool create_snapshot = true) {
    rocksdb_rpc_log(3805, "get_iterator: start");
    // Make sure we are not doing both read_current (which implies we don't
    // want a snapshot) and create_snapshot which makes sure we create
    // a snapshot
    DBUG_ASSERT(column_family != nullptr);
    DBUG_ASSERT(!read_current || !create_snapshot);

    if (create_snapshot) acquire_snapshot(true);

    // rocksdb::ReadOptions options = m_read_opts;

    const bool enable_iterate_bounds = THDVAR(get_thd(), enable_iterate_bounds);

    // // ALTER
    // if (skip_bloom_filter) {
    //   // const bool enable_iterate_bounds =
    //   //     THDVAR(get_thd(), enable_iterate_bounds);

    //   options.total_order_seek = true;

    //   // ALTER
    //   options.iterate_lower_bound =
    //       enable_iterate_bounds ? &eq_cond_lower_bound : nullptr;
    //   options.iterate_upper_bound =
    //       enable_iterate_bounds ? &eq_cond_upper_bound : nullptr;
    // } else {
    //   // With this option, Iterator::Valid() returns false if key
    //   // is outside of the prefix bloom filter range set at Seek().
    //   // Must not be set to true if not using bloom filter.
    //   options.prefix_same_as_start = true;
    // }

    // // ALTER
    // options.fill_cache = fill_cache;

    // if (read_current) {
    //   options.snapshot = nullptr;
    // }
    rocksdb_rpc_log(3843, "get_iterator: myrocks_GetIterator");
    rocksdb::ReadOptions *options = myrocks_GetIterator(
        m_read_opts, column_family, skip_bloom_filter, fill_cache,
        eq_cond_lower_bound, eq_cond_upper_bound, read_current, create_snapshot,
        enable_iterate_bounds);
    rocksdb_rpc_log(3848, "get_iterator: end");
    return get_iterator(options, column_family);
  }

  virtual bool is_tx_started() const = 0;
  virtual void start_tx() = 0;
  virtual void start_stmt() = 0;
  virtual void set_name() = 0;

 protected:
  // Non-virtual functions with actions to be done on transaction start and
  // commit.
  void on_commit() {
    rocksdb_rpc_log(3861, "on_commit: start");
    time_t tm;
    tm = time(nullptr);
    for (auto &it : modified_tables) {
      it->m_update_time = tm;
    }
    modified_tables.clear();
    rocksdb_rpc_log(3868, "on_commit: end");
  }
  void on_rollback() { modified_tables.clear(); }

 public:
  void log_table_write_op(Rdb_tbl_def *tbl) { modified_tables.insert(tbl); }

  void set_initial_savepoint() {
    /*
      Set the initial savepoint. If the first statement in the transaction
      fails, we need something to roll back to, without rolling back the
      entire transaction.
    */
    rocksdb_rpc_log(3881, "set_initial_savepoint: start");
    do_set_savepoint();
    m_writes_at_last_savepoint = m_write_count;
    rocksdb_rpc_log(3884, "set_initial_savepoint: end");
  }

  /*
    Called when a "top-level" statement inside a transaction completes
    successfully and its changes become part of the transaction's changes.
  */
  int make_stmt_savepoint_permanent() {
    // Take another RocksDB savepoint only if we had changes since the last
    // one. This is very important for long transactions doing lots of
    // SELECTs.
    rocksdb_rpc_log(3895, "make_stmt_savepoint_permanent: start");
    if (m_writes_at_last_savepoint != m_write_count) {
      rocksdb::Status status = rocksdb::Status::NotFound();
      while ((status = do_pop_savepoint()) == rocksdb::Status::OK()) {
      }

      if (status != rocksdb::Status::NotFound()) {
        rocksdb_rpc_log(3902, "make_stmt_savepoint_permanent: end");
        return HA_EXIT_FAILURE;
      }

      do_set_savepoint();
      m_writes_at_last_savepoint = m_write_count;
    }
    rocksdb_rpc_log(3909, "make_stmt_savepoint_permanent: end");
    return HA_EXIT_SUCCESS;
  }

  /*
    Rollback to the savepoint we've set before the last statement
  */
  void rollback_to_stmt_savepoint() {
    rocksdb_rpc_log(3917, "rollback_to_stmt_savepoint: start");
    if (m_writes_at_last_savepoint != m_write_count) {
      do_rollback_to_savepoint();
      /*
        RollbackToSavePoint "removes the most recent SetSavePoint()", so
        we need to set it again so that next statement can roll back to this
        stage.
        It's ok to do it here at statement end (instead of doing it at next
        statement start) because setting a savepoint is cheap.
      */
      do_set_savepoint();
      m_write_count = m_writes_at_last_savepoint;
    }
    rocksdb_rpc_log(3930, "rollback_to_stmt_savepoint: end");
  }

  virtual void rollback_stmt() = 0;

  void set_tx_failed(bool failed_arg) { m_is_tx_failed = failed_arg; }

  bool can_prepare() const {
    rocksdb_rpc_log(3938, "can_prepare: start");
    if (m_rollback_only) {
      my_error(ER_ROLLBACK_ONLY, MYF(0));
      rocksdb_rpc_log(3941, "can_prepare: end");
      return false;
    }
    rocksdb_rpc_log(3944, "can_prepare: end");
    return true;
  }

  int rollback_to_savepoint(void *const savepoint) {
    rocksdb_rpc_log(3949, "rollback_to_savepoint: start");
    if (has_modifications()) {
      my_error(ER_ROLLBACK_TO_SAVEPOINT, MYF(0));
      m_rollback_only = true;
      rocksdb_rpc_log(3953, "rollback_to_savepoint: end");
      return HA_EXIT_FAILURE;
    }
    rocksdb_rpc_log(3956, "rollback_to_savepoint: end");
    return HA_EXIT_SUCCESS;
  }

  /*
    This is used by transactions started with "START TRANSACTION WITH "
    "CONSISTENT [ROCKSDB] SNAPSHOT". When tx_read_only is turned on,
    snapshot has to be created via DB::GetSnapshot(), not via Transaction
    API.
  */
  bool is_tx_read_only() const { return m_tx_read_only; }

  bool is_two_phase() const { return m_is_two_phase; }

  void set_tx_read_only(bool val) { m_tx_read_only = val; }

  /*
    Add or remove from the global list of active transactions
    needed by information_schema queries.
  */
  void add_to_global_trx_list() {
    rocksdb_rpc_log(3977, "add_to_global_trx_list: start");
    RDB_MUTEX_LOCK_CHECK(s_tx_list_mutex);
    s_tx_list.insert(this);
    RDB_MUTEX_UNLOCK_CHECK(s_tx_list_mutex);
    rocksdb_rpc_log(3981, "add_to_global_trx_list: end");
  }

  void remove_from_global_trx_list(void) {
    rocksdb_rpc_log(3985, "remove_from_global_trx_list: start");
    DBUG_EXECUTE_IF("rocksdb_trx_list_crash", {
      THD *thd = new THD();
      thd->thread_stack = reinterpret_cast<char *>(&(thd));
      thd->store_globals();

      const char act[] =
          "now signal destructor_started wait_for trx_list_query";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

      thd->restore_globals();
      delete thd;
    });
    RDB_MUTEX_LOCK_CHECK(s_tx_list_mutex);
    s_tx_list.erase(this);
    RDB_MUTEX_UNLOCK_CHECK(s_tx_list_mutex);
    rocksdb_rpc_log(4001, "remove_from_global_trx_list: end");
  }

  explicit Rdb_transaction(THD *const thd)
      : m_thd(thd), m_tbl_io_perf(nullptr) {}

  virtual ~Rdb_transaction() {
#ifndef DEBUG_OFF
    RDB_MUTEX_LOCK_CHECK(s_tx_list_mutex);
    DBUG_ASSERT(s_tx_list.find(this) == s_tx_list.end());
    RDB_MUTEX_UNLOCK_CHECK(s_tx_list_mutex);
#endif
  }
};

#ifndef DBUG_OFF
// simulate that RocksDB has reported corrupted data
static void dbug_change_status_to_corrupted(rocksdb::Status *status) {
  *status = rocksdb::Status::Corruption();
}
static void dbug_change_status_to_io_error(rocksdb::Status *status) {
  *status = rocksdb::Status::IOError();
}
static void dbug_change_status_to_incomplete(rocksdb::Status *status) {
  *status = rocksdb::Status::Incomplete();
}
#endif

/*
  This is a rocksdb transaction. Its members represent the current transaction,
  which consists of:
  - the snapshot
  - the changes we've made but are not seeing yet.

  The changes are made to individual tables, which store them here and then
  this object commits them on commit.
*/
class Rdb_transaction_impl : public Rdb_transaction {
  rocksdb::Transaction *m_rocksdb_tx = nullptr;
  rocksdb::Transaction *m_rocksdb_reuse_tx = nullptr;

 public:
  void set_lock_timeout(int timeout_sec_arg) override {
    rocksdb_rpc_log(4044, "set_lock_timeout: start");
    if (m_rocksdb_tx) {
      // ALTER
      // m_rocksdb_tx->SetLockTimeout(rdb_convert_sec_to_ms(m_timeout_sec));
      rocksdb_rpc_log(4048,
                      "set_lock_timeout: rocksdb_Transaction__SetLockTimeout");
      rocksdb_Transaction__SetLockTimeout(m_rocksdb_tx,
                                          rdb_convert_sec_to_ms(m_timeout_sec));
    }
    rocksdb_rpc_log(4051, "set_lock_timeout: end");
  }

  void set_sync(bool sync) override {
    rocksdb_rpc_log(4056, "set_sync: start");
    // ALTER
    // m_rocksdb_tx->GetWriteOptions()->sync = sync;
    rocksdb::WriteOptions *opt =
        rocksdb_Transaction__GetWriteOptions(m_rocksdb_tx);
    rocksdb_WriteOptions__SetSync(opt, sync);
    rocksdb_rpc_log(4062, "set_sync: end");
  }

  void release_lock(const Rdb_key_def &key_descr,
                    const std::string &rowkey) override {
    rocksdb_rpc_log(4067, "release_lock: start");
    if (!THDVAR(m_thd, lock_scanned_rows)) {
      // ALTER
      // m_rocksdb_tx->UndoGetForUpdate(key_descr.get_cf(),
      //                                rocksdb::Slice(rowkey));

      rocksdb_rpc_log(4073,
                      "release_lock: rocksdb_Transaction__UndoGetForUpdate");
      rocksdb_Transaction__UndoGetForUpdate(m_rocksdb_tx, key_descr.get_cf(),
                                            rocksdb::Slice(rowkey));

      // row_lock_count track row(pk)
      DBUG_ASSERT(!key_descr.is_primary_key() ||
                  (key_descr.is_primary_key() && m_row_lock_count > 0));
      // m_row_lock_count tracks per row data instead of per key data
      if (key_descr.is_primary_key() && m_row_lock_count > 0) {
        m_row_lock_count--;
      }
    }
    rocksdb_rpc_log(4083, "release_lock: end");
  }

  virtual bool is_writebatch_trx() const override { return false; }

 private:
  void release_tx(void) {
    // We are done with the current active transaction object.  Preserve it
    // for later reuse.
    rocksdb_rpc_log(4098, "release_tx: start");
    DBUG_ASSERT(m_rocksdb_reuse_tx == nullptr);
    m_rocksdb_reuse_tx = m_rocksdb_tx;
    m_rocksdb_tx = nullptr;
    rocksdb_rpc_log(4102, "release_tx: end");
  }

  bool prepare() override {
    rocksdb_rpc_log(4106, "prepare: start");
    rocksdb::Status s;

    rocksdb_rpc_log(4112,
                    "prepare: rocksdb_WriteBatchWithIndex__GetWriteBatch");
    // ALTER
    // s = merge_auto_incr_map(m_rocksdb_tx->GetWriteBatch()->GetWriteBatch());
    s = merge_auto_incr_map(rocksdb_WriteBatchWithIndex__GetWriteBatch(
        rocksdb_Transaction__GetWriteBatch(m_rocksdb_tx)));

#ifndef DBUG_OFF
    DBUG_EXECUTE_IF("myrocks_prepare_io_error",
                    dbug_change_status_to_io_error(&s););
    DBUG_EXECUTE_IF("myrocks_prepare_incomplete",
                    dbug_change_status_to_incomplete(&s););
#endif
    if (!s.ok()) {
      std::string msg =
          "RocksDB error on COMMIT (Prepare/merge): " + s.ToString();
      my_error(ER_INTERNAL_ERROR, MYF(0), msg.c_str());
      rocksdb_rpc_log(4124, "prepare: end");
      return false;
    }

    // ALTER
    // s = m_rocksdb_tx->Prepare();
    s = rocksdb_Transaction__Prepare(m_rocksdb_tx);

    if (!s.ok()) {
      std::string msg = "RocksDB error on COMMIT (Prepare): " + s.ToString();
      my_error(ER_INTERNAL_ERROR, MYF(0), msg.c_str());
      rocksdb_rpc_log(4134, "prepare: end");
      return false;
    }
    rocksdb_rpc_log(4136, "prepare: end");
    return true;
  }

  bool commit_no_binlog() override {
    rocksdb_rpc_log(4145, "commit_no_binlog: start");
    bool res = false;
    rocksdb::Status s;

    s = merge_auto_incr_map(rocksdb_WriteBatchWithIndex__GetWriteBatch(
        rocksdb_Transaction__GetWriteBatch(m_rocksdb_tx)));
#ifndef DBUG_OFF
    DBUG_EXECUTE_IF("myrocks_commit_merge_io_error",
                    dbug_change_status_to_io_error(&s););
    DBUG_EXECUTE_IF("myrocks_commit_merge_incomplete",
                    dbug_change_status_to_incomplete(&s););
#endif
    if (!s.ok()) {
      rdb_handle_io_error(s, RDB_IO_ERROR_TX_COMMIT);
      res = true;
      goto error;
    }

    release_snapshot();
    rocksdb_rpc_log(4164, "commit_no_binlog: rocksdb_Transaction__Commit");
    // ALTER
    // s = m_rocksdb_tx->Commit();
    s = rocksdb_Transaction__Commit(m_rocksdb_tx);

#ifndef DBUG_OFF
    DBUG_EXECUTE_IF("myrocks_commit_io_error",
                    dbug_change_status_to_io_error(&s););
    DBUG_EXECUTE_IF("myrocks_commit_incomplete",
                    dbug_change_status_to_incomplete(&s););
#endif
    if (!s.ok()) {
      rdb_handle_io_error(s, RDB_IO_ERROR_TX_COMMIT);
      res = true;
      goto error;
    }

    on_commit();
  error:
    on_rollback();
    /* Save the transaction object to be reused */
    release_tx();

    m_write_count = 0;
    m_insert_count = 0;
    m_update_count = 0;
    m_delete_count = 0;
    m_row_lock_count = 0;
    set_tx_read_only(false);
    m_rollback_only = false;
    return res;
  }

 public:
  void rollback() override {
    rocksdb_rpc_log(4199, "rollback: start");
    on_rollback();
    m_write_count = 0;
    m_insert_count = 0;
    m_update_count = 0;
    m_delete_count = 0;
    m_row_lock_count = 0;
    m_auto_incr_map.clear();
    m_ddl_transaction = false;
    if (m_rocksdb_tx) {
      release_snapshot();
      /* This will also release all of the locks: */

      // ALTER
      // m_rocksdb_tx->Rollback();
      rocksdb_rpc_log(4214, "rollback: rocksdb_Transaction__Rollback");
      rocksdb_Transaction__Rollback(m_rocksdb_tx);

      /* Save the transaction object to be reused */
      release_tx();

      set_tx_read_only(false);
      m_rollback_only = false;
    }
    rocksdb_rpc_log(4223, "rollback: end");
  }

  void acquire_snapshot(bool acquire_now) override {
    rocksdb_rpc_log(4227, "acquire_snapshot: start");
    // ALTER
    // if (m_read_opts.snapshot == nullptr) {
    rocksdb_rpc_log(4231, "acquire_snapshot: rocksdb_ReadOptions__GetSnapshot");
    if (rocksdb_ReadOptions__GetSnapshot(m_read_opts) == nullptr) {
      const auto thd_ss = std::static_pointer_cast<Rdb_explicit_snapshot>(
          m_thd->get_explicit_snapshot());
      if (thd_ss) {
        m_explicit_snapshot = thd_ss;
      }
      if (m_explicit_snapshot) {
        // ALTER
        // auto snapshot = m_explicit_snapshot->get_snapshot()->snapshot();
        rocksdb_rpc_log(4240,
                        "acquire_snapshot: rocksdb_ManagedSnapshot__snapshot");
        auto snapshot = rocksdb_ManagedSnapshot__snapshot(
            m_explicit_snapshot->get_snapshot());

        snapshot_created(snapshot);
      } else if (is_tx_read_only()) {
        // ALTER
        // snapshot_created(rdb->GetSnapshot());
        rocksdb_rpc_log(4249,
                        "acquire_snapshot: rocksdb_TransactionDB__GetSnapshot");
        snapshot_created(rocksdb_TransactionDB__GetSnapshot(rdb));
      } else if (acquire_now) {
        rocksdb_rpc_log(4252,
                        "acquire_snapshot: rocksdb_Transaction__SetSnapshot");
        // ALTER
        // m_rocksdb_tx->SetSnapshot();
        rocksdb_Transaction__SetSnapshot(m_rocksdb_tx);

        // ALTER
        // snapshot_created(m_rocksdb_tx->GetSnapshot());
        snapshot_created(rocksdb_Transaction__GetSnapshot(m_rocksdb_tx));
      } else if (!m_is_delayed_snapshot) {
        // TODO: ALTER
        // m_rocksdb_tx->SetSnapshotOnNextOperation(m_notifier);
        // m_is_delayed_snapshot = true;
      }
    }
    rocksdb_rpc_log(4266, "acquire_snapshot: end");
  }

  void release_snapshot() override {
    rocksdb_rpc_log(4270, "release_snapshot: start");
    bool need_clear = m_is_delayed_snapshot;

    // ALTER
    // if (m_read_opts.snapshot != nullptr) {
    if (rocksdb_ReadOptions__GetSnapshot(m_read_opts) != nullptr) {
      m_snapshot_timestamp = 0;
      if (m_explicit_snapshot) {
        m_explicit_snapshot.reset();
        need_clear = false;
      } else if (is_tx_read_only()) {
        // ALTER
        // rdb->ReleaseSnapshot(m_read_opts.snapshot);
        rocksdb_TransactionDB__ReleaseSnapshot(
            rdb, rocksdb_ReadOptions__GetSnapshot(m_read_opts));
        need_clear = false;
      } else {
        need_clear = true;
      }
      // ALTER
      // m_read_opts.snapshot = nullptr;
      rocksdb_ReadOptions__SetSnapshot(m_read_opts, nullptr);
    }

    if (need_clear && m_rocksdb_tx != nullptr) {
      // ALTER
      // m_rocksdb_tx->ClearSnapshot();
      rocksdb_Transaction__ClearSnapshot(m_rocksdb_tx);
    }
    rocksdb_rpc_log(4299, "release_snapshot: end");
  }

  bool has_snapshot() {
    rocksdb_rpc_log(4303, "has_snapshot: start");
    // ALTER
    // return m_read_opts.snapshot != nullptr;
    rocksdb_rpc_log(4307, "has_snapshot: rocksdb_ReadOptions__GetSnapshot");
    return rocksdb_ReadOptions__GetSnapshot(m_read_opts) != nullptr;
  }

  rocksdb::Status put(rocksdb::ColumnFamilyHandle *const column_family,
                      const rocksdb::Slice &key, const rocksdb::Slice &value,
                      const bool assume_tracked) override {
    rocksdb_rpc_log(4315, "put: start");
    ++m_write_count;

    // ALTER
    // return m_rocksdb_tx->Put(column_family, key, value, assume_tracked);
    rocksdb_rpc_log(4320, "put: rocksdb_Transaction__Put");
    return rocksdb_Transaction__Put(m_rocksdb_tx, column_family, key, value,
                                    assume_tracked);
  }

  rocksdb::Status delete_key(rocksdb::ColumnFamilyHandle *const column_family,
                             const rocksdb::Slice &key,
                             const bool assume_tracked) override {
    rocksdb_rpc_log(4328, "delete_key: start");
    ++m_write_count;

    // ALTER
    // return m_rocksdb_tx->Delete(column_family, key, assume_tracked);
    rocksdb_rpc_log(4333, "delete_key: rocksdb_Transaction__Delete");
    return rocksdb_Transaction__Delete(m_rocksdb_tx, column_family, key,
                                       assume_tracked);
  }

  rocksdb::Status single_delete(
      rocksdb::ColumnFamilyHandle *const column_family,
      const rocksdb::Slice &key, const bool assume_tracked) override {
    rocksdb_rpc_log(4341, "single_delete: begin");
    ++m_write_count;

    rocksdb_rpc_log(4347, "single_delete: rocksdb_Transaction__SingleDelete");
    // ALTER
    // return m_rocksdb_tx->SingleDelete(column_family, key, assume_tracked);
    return rocksdb_Transaction__SingleDelete(m_rocksdb_tx, column_family, key,
                                             assume_tracked);
  }

  bool has_modifications() const override {
    // ALTER
    // return m_rocksdb_tx->GetWriteBatch() &&
    //        m_rocksdb_tx->GetWriteBatch()->GetWriteBatch() &&
    //        m_rocksdb_tx->GetWriteBatch()->GetWriteBatch()->Count() > 0;
    rocksdb_rpc_log(4356,
                    "has_modifications: rocksdb_Transaction__GetWriteBatch");
    rocksdb::WriteBatchWithIndex *wbidx =
        rocksdb_Transaction__GetWriteBatch(m_rocksdb_tx);
    rocksdb::WriteBatch *wb = nullptr;
    if (wbidx) {
      wb = rocksdb_WriteBatchWithIndex__GetWriteBatch(wbidx);
    }
    rocksdb_rpc_log(4363, "has_modifications: end");
    return wbidx && wb && rocksdb_WriteBatch__Count(wb) > 0;
  }

  rocksdb::WriteBatchBase *get_write_batch() override {
    rocksdb_rpc_log(4368, "get_write_batch: start");
    if (is_two_phase()) {
      //  ALTER
      // return m_rocksdb_tx->GetCommitTimeWriteBatch();
      return rocksdb_Transaction__GetCommitTimeWriteBatch(m_rocksdb_tx);
    }
    // ALTER
    // return m_rocksdb_tx->GetWriteBatch()->GetWriteBatch();
    return rocksdb_WriteBatchWithIndex__GetWriteBatch(
        rocksdb_Transaction__GetWriteBatch(m_rocksdb_tx));
  }

  /*
    Return a WriteBatch that one can write to. The writes will skip any
    transaction locking. The writes WILL be visible to the transaction.
  */
  rocksdb::WriteBatchBase *get_indexed_write_batch() override {
    rocksdb_rpc_log(4386, "get_indexed_write_batch: start");
    ++m_write_count;
    // ALTER
    // return m_rocksdb_tx->GetWriteBatch();
    return rocksdb_Transaction__GetWriteBatch(m_rocksdb_tx);
  }

  // ALTER
  rocksdb::Status get(rocksdb::ColumnFamilyHandle *const column_family,
                      const rocksdb::Slice &key,
                      rocksdb::PinnableSlice *&value) const override {
    rocksdb_rpc_log(4386, "get: start");
    // clean PinnableSlice right begfore Get() for multiple gets per statement
    // the resources after the last Get in a statement are cleared in
    // handler::reset call

    // ALTER
    // value->Reset();
    rocksdb_PinnableSlice__Reset(value);

    global_stats.queries[QUERIES_POINT].inc();

    // ALTER
    // return m_rocksdb_tx->Get(m_read_opts, column_family, key, value);
    rocksdb_rpc_log(4410, "get: rocksdb_Transaction__Get");
    return rocksdb_Transaction__Get(m_rocksdb_tx, m_read_opts, column_family,
                                    key, value);
  }

  void multi_get(rocksdb::ColumnFamilyHandle *const column_family,
                 const size_t num_keys, const rocksdb::Slice *keys,
                 rocksdb::PinnableSlice **values, rocksdb::Status *statuses,
                 const bool sorted_input) const override {
    rocksdb_rpc_log(4424, "get: begin");
    // ALTER
    // m_rocksdb_tx->MultiGet(m_read_opts, column_family, num_keys, keys,
    // values,
    //                        statuses, sorted_input);
    rocksdb_Transaction__MultiGet(m_rocksdb_tx, m_read_opts, column_family,
                                  num_keys, keys, values, statuses,
                                  sorted_input);
  }

  rocksdb::Status get_for_update(const Rdb_key_def &key_descr,
                                 const rocksdb::Slice &key,
                                 rocksdb::PinnableSlice *&value, bool exclusive,
                                 const bool do_validate) override {
    rocksdb_rpc_log(4434, "get_for_update: begin");
    rocksdb::ColumnFamilyHandle *const column_family = key_descr.get_cf();
    /* check row lock limit in a trx */
    if (get_row_lock_count() >= get_max_row_lock_count()) {
      return rocksdb::Status::Aborted(rocksdb::Status::kLockLimit);
    }

    if (value != nullptr) {
      // ALTER
      // value->Reset();
      rocksdb_rpc_log(4445, "get_for_update: rocksdb_PinnableSlice__Reset");
      rocksdb_PinnableSlice__Reset(value);
    }
    rocksdb::Status s;
    // If snapshot is null, pass it to GetForUpdate and snapshot is
    // initialized there. Snapshot validation is skipped in that case.

    // if (m_read_opts.snapshot == nullptr || do_validate) {
    if (rocksdb_ReadOptions__GetSnapshot(m_read_opts) || do_validate) {
      // ALTER
      // s = m_rocksdb_tx->GetForUpdate(
      //     m_read_opts, column_family, key, value, exclusive,
      //     m_read_opts.snapshot ? do_validate : false);
      rocksdb_rpc_log(4457,
                      "get_for_update: rocksdb_Transaction__GetForUpdate");
      s = rocksdb_Transaction__GetForUpdate(
          m_rocksdb_tx, m_read_opts, column_family, key, value, exclusive,
          rocksdb_ReadOptions__GetSnapshot(m_read_opts) ? do_validate : false);
    } else {
      // If snapshot is set, and if skipping validation,
      // call GetForUpdate without validation and set back old snapshot

      // ALTER
      // auto saved_snapshot = m_read_opts.snapshot;
      // m_read_opts.snapshot = nullptr;
      auto saved_snapshot = rocksdb_ReadOptions__GetSnapshot(m_read_opts);
      rocksdb::Snapshot *null_snap = nullptr;
      rocksdb_rpc_log(4470, "get_for_update: rocksdb_ReadOptions__SetSnapshot");
      rocksdb_ReadOptions__SetSnapshot(m_read_opts, null_snap);

      // ALTER
      // s = m_rocksdb_tx->GetForUpdate(m_read_opts, column_family, key, value,
      //                                exclusive, false);
      rocksdb_rpc_log(4477,
                      "get_for_update: rocksdb_Transaction__GetForUpdate");
      s = rocksdb_Transaction__GetForUpdate(m_rocksdb_tx, m_read_opts,
                                            column_family, key, value,
                                            exclusive, false);

      // ALTER
      // m_read_opts.snapshot = saved_snapshot;
      rocksdb_ReadOptions__SetSnapshot(m_read_opts, saved_snapshot);
    }
    // row_lock_count is to track per row instead of per key
    if (key_descr.is_primary_key()) incr_row_lock_count();
    rocksdb_rpc_log(4477, "get_for_update: end");
    return s;
  }

  rocksdb::Iterator *get_iterator(
      rocksdb::ReadOptions *options,
      rocksdb::ColumnFamilyHandle *const column_family) override {
    global_stats.queries[QUERIES_RANGE].inc();
    rocksdb_rpc_log(4495, "get_iterator: rocksdb_Transaction__GetIterator");
    // ALTER
    // return m_rocksdb_tx->GetIterator(options, column_family);
    return rocksdb_Transaction__GetIterator(m_rocksdb_tx, options,
                                            column_family);
  }

  const rocksdb::Transaction *get_rdb_trx() const { return m_rocksdb_tx; }

  bool is_tx_started() const override { return (m_rocksdb_tx != nullptr); }

  void start_tx() override {
    rocksdb_rpc_log(4507, "start_tx: begin");
    rocksdb::TransactionOptions tx_opts;
    rocksdb::WriteOptions write_opts;
    tx_opts.set_snapshot = false;
    tx_opts.lock_timeout = rdb_convert_sec_to_ms(m_timeout_sec);
    tx_opts.deadlock_detect = THDVAR(m_thd, deadlock_detect);
    tx_opts.deadlock_detect_depth = THDVAR(m_thd, deadlock_detect_depth);
    // If this variable is set, this will write commit time write batch
    // information on recovery or memtable flush.
    tx_opts.use_only_the_last_commit_time_batch_for_recovery =
        THDVAR(m_thd, commit_time_batch_for_recovery);
    tx_opts.max_write_batch_size = THDVAR(m_thd, write_batch_max_bytes);
    tx_opts.write_batch_flush_threshold =
        THDVAR(m_thd, write_batch_flush_threshold);

    write_opts.sync = (rocksdb_flush_log_at_trx_commit == FLUSH_LOG_SYNC);
    write_opts.disableWAL = THDVAR(m_thd, write_disable_wal);
    write_opts.ignore_missing_column_families =
        THDVAR(m_thd, write_ignore_missing_column_families);
    m_is_two_phase = rocksdb_enable_2pc;
    rocksdb_rpc_log(4527, "start_tx: init txopt");

    /*
      If m_rocksdb_reuse_tx is null this will create a new transaction object.
      Otherwise it will reuse the existing one.
    */
    // ALTER
    // m_rocksdb_tx =
    //     rdb->BeginTransaction(write_opts, tx_opts, m_rocksdb_reuse_tx);
    rocksdb_rpc_log(4537, "start_tx: rocksdb_TransactionDB__BeginTransaction");
    m_rocksdb_tx = rocksdb_TransactionDB__BeginTransaction(
        rdb, write_opts, tx_opts, m_rocksdb_reuse_tx);
    m_rocksdb_reuse_tx = nullptr;

    // ALTER
    // m_read_opts = rocksdb::ReadOptions();
    m_read_opts = rocksdb_ReadOptions__NewReadOptions();
    set_initial_savepoint();

    m_ddl_transaction = false;
    rocksdb_rpc_log(4547, "start_tx: end");
  }

  void set_name() override {
    rocksdb_rpc_log(4551, "set_name: start");
    XID xid;
    thd_get_xid(m_thd, reinterpret_cast<MYSQL_XID *>(&xid));

    // ALTER
    // auto name = m_rocksdb_tx->GetName();
    rocksdb_rpc_log(4557, "set_name: rocksdb_Transaction__GetName");
    auto name = rocksdb_Transaction__GetName(m_rocksdb_tx);

    if (!name.empty()) {
      DBUG_ASSERT(name == rdb_xid_to_string(xid));
      return;
    }

    rocksdb_rpc_log(4565, "set_name: rocksdb_Transaction__SetName");
    // ALTER
    // rocksdb::Status s = m_rocksdb_tx->SetName(rdb_xid_to_string(xid));
    rocksdb::Status s =
        rocksdb_Transaction__SetName(m_rocksdb_tx, rdb_xid_to_string(xid));

    DBUG_ASSERT(s.ok());
    if (!s.ok()) {
      rdb_handle_io_error(s, RDB_IO_ERROR_TX_COMMIT);
    }
    rocksdb_rpc_log(4575, "set_name: end");
  }

  /* Implementations of do_*savepoint based on rocksdB::Transaction savepoints
   */
  void do_set_savepoint() override {
    // ALTER
    // m_rocksdb_tx->SetSavePoint();
    rocksdb_rpc_log(4583,
                    "do_set_savepoint: rocksdb_Transaction__SetSavePoint");
    rocksdb_Transaction__SetSavePoint(m_rocksdb_tx);
  }
  rocksdb::Status do_pop_savepoint() override {
    // ALTER
    // return m_rocksdb_tx->PopSavePoint();
    rocksdb_rpc_log(4589,
                    "do_set_savepoint: rocksdb_Transaction__PopSavePoint");
    return rocksdb_Transaction__PopSavePoint(m_rocksdb_tx);
  }

  void do_rollback_to_savepoint() override {
    // ALTER
    // m_rocksdb_tx->RollbackToSavePoint();
    rocksdb_rpc_log(
        4597, "do_set_savepoint: rocksdb_Transaction__RollbackToSavePoint");
    rocksdb_Transaction__RollbackToSavePoint(m_rocksdb_tx);
  }

  /*
    Start a statement inside a multi-statement transaction.

    @todo: are we sure this is called once (and not several times) per
    statement start?

    For hooking to start of statement that is its own transaction, see
    ha_rocksdb::external_lock().
  */
  void start_stmt() override {
    // Set the snapshot to delayed acquisition (SetSnapshotOnNextOperation)
    rocksdb_rpc_log(4611, "start_stmt: begin");
    acquire_snapshot(false);
  }

  /*
    This must be called when last statement is rolled back, but the transaction
    continues
  */
  void rollback_stmt() override {
    rocksdb_rpc_log(4620, "rollback_stmt: begin");
    /* TODO: here we must release the locks taken since the start_stmt() call */
    if (m_rocksdb_tx) {
      // ALTER
      // const rocksdb::Snapshot *const org_snapshot =
      // m_rocksdb_tx->GetSnapshot();
      const rocksdb::Snapshot *const org_snapshot =
          rocksdb_Transaction__GetSnapshot(m_rocksdb_tx);
      rollback_to_stmt_savepoint();

      // ALTER
      // const rocksdb::Snapshot *const cur_snapshot =
      // m_rocksdb_tx->GetSnapshot();
      const rocksdb::Snapshot *const cur_snapshot =
          rocksdb_Transaction__GetSnapshot(m_rocksdb_tx);

      if (org_snapshot != cur_snapshot) {
        if (org_snapshot != nullptr) m_snapshot_timestamp = 0;

        // ALTER
        // m_read_opts.snapshot = cur_snapshot;
        // m_read_opts.snapshot = cur_snapshot;
        rocksdb_ReadOptions__SetSnapshot(m_read_opts, cur_snapshot);

        if (cur_snapshot != nullptr) {
          // ALTER
          // rdb->GetEnv()->GetCurrentTime(&m_snapshot_timestamp);
          rocksdb_rpc_log(4647, "rollback_stmt: rocksdb_Env__GetCurrentTime");
          rocksdb_Env__GetCurrentTime(rocksdb_TransactionDB__GetEnv(rdb),
                                      &m_snapshot_timestamp);

        } else {
          m_is_delayed_snapshot = true;
        }
      }
    }
    rocksdb_rpc_log(4656, "rollback_stmt: end");
  }

  explicit Rdb_transaction_impl(THD *const thd)
      : Rdb_transaction(thd), m_rocksdb_tx(nullptr) {
    // Create a notifier that can be called when a snapshot gets generated.
    m_notifier = std::make_shared<Rdb_snapshot_notifier>(this);
  }

  virtual ~Rdb_transaction_impl() override {
    rocksdb_rpc_log(4666, "~Rdb_transaction_impl: start");
    // Remove from the global list before all other processing is started.
    // Otherwise, information_schema.rocksdb_trx can crash on this object.
    Rdb_transaction::remove_from_global_trx_list();

    rollback();

    // Theoretically the notifier could outlive the Rdb_transaction_impl
    // (because of the shared_ptr), so let it know it can't reference
    // the transaction anymore.
    m_notifier->detach();

    // Free any transaction memory that is still hanging around.

    // ALTER
    // delete m_rocksdb_reuse_tx;
    // DBUG_ASSERT(m_rocksdb_tx == nullptr);
    rocksdb_Transaction__delete(m_rocksdb_reuse_tx);
    m_rocksdb_reuse_tx = nullptr;
    rocksdb_rpc_log(4685, "~Rdb_transaction_impl: end");
  }
};

/* This is a rocksdb write batch. This class doesn't hold or wait on any
   transaction locks (skips rocksdb transaction API) thus giving better
   performance.

   Currently this is only used for replication threads which are guaranteed
   to be non-conflicting. Any further usage of this class should completely
   be thought thoroughly.
*/
class Rdb_writebatch_impl : public Rdb_transaction {
  rocksdb::WriteBatchWithIndex *m_batch;
  rocksdb::WriteOptions write_opts;
  // Called after commit/rollback.
  void reset() {
    rocksdb_rpc_log(4702, "reset: start");
    // ALTER
    // m_batch->Clear();
    rocksdb_WriteBatchWithIndex__Clear(m_batch);

    // ALTER
    // m_read_opts = rocksdb::ReadOptions();
    m_read_opts = rocksdb_ReadOptions__NewReadOptions();

    m_ddl_transaction = false;
    rocksdb_rpc_log(4712, "reset: end");
  }

 private:
  bool prepare() override { return true; }

  bool commit_no_binlog() override {
    rocksdb_rpc_log(4719, "commit_no_binlog: start");
    bool res = false;
    rocksdb::Status s;
    rocksdb::TransactionDBWriteOptimizations optimize;
    optimize.skip_concurrency_control = true;

    // ALTER
    // s = merge_auto_incr_map(m_batch->GetWriteBatch());
    rocksdb_rpc_log(
        4728, "commit_no_binlog: rocksdb_WriteBatchWithIndex__GetWriteBatch");
    s = merge_auto_incr_map(
        rocksdb_WriteBatchWithIndex__GetWriteBatch(m_batch));
    if (!s.ok()) {
      rdb_handle_io_error(s, RDB_IO_ERROR_TX_COMMIT);
      res = true;
      goto error;
    }

    release_snapshot();

    // ALTER
    // s = rdb->Write(write_opts, optimize, m_batch->GetWriteBatch());
    rocksdb_rpc_log(4740, "commit_no_binlog: rocksdb_TransactionDB__Write");
    s = rocksdb_TransactionDB__Write(
        rdb, write_opts, optimize,
        rocksdb_WriteBatchWithIndex__GetWriteBatch(m_batch));
    if (!s.ok()) {
      rdb_handle_io_error(s, RDB_IO_ERROR_TX_COMMIT);
      res = true;
      goto error;
    }
    on_commit();
  error:
    on_rollback();
    reset();

    m_write_count = 0;
    m_insert_count = 0;
    m_update_count = 0;
    m_delete_count = 0;
    set_tx_read_only(false);
    m_rollback_only = false;

    rocksdb_rpc_log(4762, "commit_no_binlog: end");
    return res;
  }

  /* Implementations of do_*savepoint based on rocksdB::WriteBatch savepoints */
  void do_set_savepoint() override {
    rocksdb_rpc_log(
        4770, "commit_no_binlog: rocksdb_WriteBatchWithIndex__SetSavePoint");
    // ALTER
    // m_batch->SetSavePoint();
    rocksdb_WriteBatchWithIndex__SetSavePoint(m_batch);
  }
  rocksdb::Status do_pop_savepoint() override {
    // ALTER
    // return m_batch->PopSavePoint();
    rocksdb_rpc_log(
        4776, "commit_no_binlog: rocksdb_WriteBatchWithIndex__PopSavePoint");
    return rocksdb_WriteBatchWithIndex__PopSavePoint(m_batch);
  }

  void do_rollback_to_savepoint() override {
    // ALTER
    // m_batch->RollbackToSavePoint();
    rocksdb_rpc_log(
        4783,
        "commit_no_binlog: rocksdb_WriteBatchWithIndex__RollbackToSavePoint");
    rocksdb_WriteBatchWithIndex__RollbackToSavePoint(m_batch);
  }

 public:
  bool is_writebatch_trx() const override { return true; }

  void set_lock_timeout(int timeout_sec_arg) override {
    // Nothing to do here.
  }

  void set_sync(bool sync) override { write_opts.sync = sync; }

  void release_lock(const Rdb_key_def &key_descr,
                    const std::string &rowkey) override {
    // Nothing to do here since we don't hold any row locks.
  }

  void rollback() override {
    rocksdb_rpc_log(4801, "rollback: begin");
    on_rollback();
    m_write_count = 0;
    m_insert_count = 0;
    m_update_count = 0;
    m_delete_count = 0;
    m_row_lock_count = 0;
    release_snapshot();

    reset();
    set_tx_read_only(false);
    m_rollback_only = false;
    rocksdb_rpc_log(4813, "rollback: end");
  }

  void acquire_snapshot(bool acquire_now) override {
    rocksdb_rpc_log(4817, "acquire_snapshot: start");
    // ALTER
    // if (m_read_opts.snapshot == nullptr) {
    if (rocksdb_ReadOptions__GetSnapshot(m_read_opts) == nullptr) {
      // ALTER
      // snapshot_created(rdb->GetSnapshot());
      snapshot_created(rocksdb_TransactionDB__GetSnapshot(rdb));
    }
    rocksdb_rpc_log(4825, "acquire_snapshot: end");
  }

  void release_snapshot() override {
    // ALTER
    // if (m_read_opts.snapshot != nullptr) {
    //   rdb->ReleaseSnapshot(m_read_opts.snapshot);
    //   m_read_opts.snapshot = nullptr;
    // }
    rocksdb_rpc_log(4834, "release_snapshot: start");
    if (rocksdb_ReadOptions__GetSnapshot(m_read_opts) != nullptr) {
      rocksdb_rpc_log(
          4836, "release_snapshot: rocksdb_TransactionDB__ReleaseSnapshot");
      rocksdb_TransactionDB__ReleaseSnapshot(
          rdb, rocksdb_ReadOptions__GetSnapshot(m_read_opts));
      // ALTER
      // m_read_opts.snapshot = nullptr;
      rocksdb_ReadOptions__SetSnapshot(m_read_opts, nullptr);
    }
    rocksdb_rpc_log(4843, "release_snapshot: end");
  }

  rocksdb::Status put(rocksdb::ColumnFamilyHandle *const column_family,
                      const rocksdb::Slice &key, const rocksdb::Slice &value,
                      const bool assume_tracked) override {
    rocksdb_rpc_log(4849, "put: rocksdb_WriteBatchWithIndex__Put");
    ++m_write_count;
    // ALTER
    // m_batch->Put(column_family, key, value);
    rocksdb_WriteBatchWithIndex__Put(m_batch, column_family, key, value);

    // Note Put/Delete in write batch doesn't return any error code. We simply
    // return OK here.
    return rocksdb::Status::OK();
  }

  rocksdb::Status delete_key(rocksdb::ColumnFamilyHandle *const column_family,
                             const rocksdb::Slice &key,
                             const bool assume_tracked) override {
    rocksdb_rpc_log(4868, "delete_key: start");
    ++m_write_count;

    // ALTER
    // m_batch->Delete(column_family, key);
    rocksdb_WriteBatchWithIndex__Delete(m_batch, column_family, key);
    rocksdb_rpc_log(4870, "delete_key: end");
    return rocksdb::Status::OK();
  }

  rocksdb::Status single_delete(
      rocksdb::ColumnFamilyHandle *const column_family,
      const rocksdb::Slice &key, const bool /* assume_tracked */) override {
    rocksdb_rpc_log(4876, "single_delete: start");
    ++m_write_count;

    // ALTER
    // m_batch->SingleDelete(column_family, key);
    rocksdb_rpc_log(
        4882, "single_delete: rocksdb_WriteBatchWithIndex__SingleDeleteart");
    rocksdb_WriteBatchWithIndex__SingleDelete(m_batch, column_family, key);
    rocksdb_rpc_log(4883, "single_delete: end");
    return rocksdb::Status::OK();
  }

  bool has_modifications() const override {
    // ALTER
    // return m_batch->GetWriteBatch()->Count() > 0;
    rocksdb_rpc_log(4890, "has_modifications: rocksdb_WriteBatch__Count");
    return rocksdb_WriteBatch__Count(
               rocksdb_WriteBatchWithIndex__GetWriteBatch(m_batch)) > 0;
  }

  rocksdb::WriteBatchBase *get_write_batch() override { return m_batch; }

  rocksdb::WriteBatchBase *get_indexed_write_batch() override {
    rocksdb_rpc_log(4899, "get_indexed_write_batch: start");
    ++m_write_count;
    return m_batch;
  }

  rocksdb::Status get(rocksdb::ColumnFamilyHandle *const column_family,
                      const rocksdb::Slice &key,
                      rocksdb::PinnableSlice *&value) const override {
    // ALTER
    // value->Reset();
    // return m_batch->GetFromBatchAndDB(rdb, m_read_opts, column_family, key,
    //                                   value);
    rocksdb_rpc_log(4910, "get: rocksdb_PinnableSlice__Reset");
    rocksdb_PinnableSlice__Reset(value);
    rocksdb_rpc_log(4913,
                    "get: rocksdb_WriteBatchWithIndex__GetFromBatchAndDB");
    return rocksdb_WriteBatchWithIndex__GetFromBatchAndDB(
        m_batch, rdb, m_read_opts, column_family, key, value);
  }

  rocksdb::Status get_for_update(const Rdb_key_def &key_descr,
                                 const rocksdb::Slice &key,
                                 rocksdb::PinnableSlice *&value,
                                 bool /* exclusive */,
                                 const bool /* do_validate */) override {
    rocksdb_rpc_log(4923, "get_for_update: begin");
    rocksdb::ColumnFamilyHandle *const column_family = key_descr.get_cf();
    if (value == nullptr) {
      // ALTER
      // rocksdb::PinnableSlice pin_val;
      // rocksdb::Status s = get(column_family, key, &pin_val);
      // pin_val.Reset();

      rocksdb::PinnableSlice *pin_val = rocksdb_PinnableSlice__PinnableSlice();
      rocksdb::Status s = get(column_family, key, pin_val);
      rocksdb_PinnableSlice__Reset(pin_val);

      rocksdb_rpc_log(4934, "get_for_update: end");
      return s;
    }
    rocksdb_rpc_log(4937, "get_for_update: end");
    return get(column_family, key, value);
  }

  void multi_get(rocksdb::ColumnFamilyHandle *const column_family,
                 const size_t num_keys, const rocksdb::Slice *keys,
                 rocksdb::PinnableSlice **values, rocksdb::Status *statuses,
                 const bool sorted_input) const override {
    // TODO: ALTER
    // m_batch->MultiGetFromBatchAndDB(rdb, m_read_opts, column_family,
    // num_keys,
    //                                 keys, values, statuses, sorted_input);
    rocksdb_rpc_log(
        4949, "multi_get: rocksdb_WriteBatchWithIndex__MultiGetFromBatchAndDB");
    rocksdb_WriteBatchWithIndex__MultiGetFromBatchAndDB(
        rdb, m_read_opts, m_batch, column_family, num_keys, keys, values,
        statuses, sorted_input);
  }

  // ALTER
  rocksdb::Iterator *get_iterator(
      rocksdb::ReadOptions *options,
      rocksdb::ColumnFamilyHandle *const /* column_family */) override {
    // const auto it = rdb->NewIterator(options);
    // return m_batch->NewIteratorWithBase(it);
    auto it = rocksdb_DB__NewIterator(rdb, options);
    rocksdb_rpc_log(
        4961, "multi_get: rocksdb_WriteBatchWithIndex__NewIteratorWithBase");
    return rocksdb_WriteBatchWithIndex__NewIteratorWithBase(m_batch, it);
  }

  bool is_tx_started() const override { return (m_batch != nullptr); }

  void start_tx() override {
    rocksdb_rpc_log(4967, "start_tx: begin");
    reset();
    write_opts.sync = (rocksdb_flush_log_at_trx_commit == FLUSH_LOG_SYNC);
    write_opts.disableWAL = THDVAR(m_thd, write_disable_wal);
    write_opts.ignore_missing_column_families =
        THDVAR(m_thd, write_ignore_missing_column_families);

    set_initial_savepoint();
    rocksdb_rpc_log(4975, "start_tx: end");
  }

  void set_name() override {}

  void start_stmt() override {}

  void rollback_stmt() override {
    rocksdb_rpc_log(4983, "start_tx: start");
    if (m_batch) rollback_to_stmt_savepoint();
    rocksdb_rpc_log(4985, "start_tx: end");
  }

  explicit Rdb_writebatch_impl(THD *const thd)
      : Rdb_transaction(thd), m_batch(nullptr) {
    // ALTER
    // m_batch = new rocksdb::WriteBatchWithIndex(rocksdb::BytewiseComparator(),
    // 0,
    //                                            true);
    rocksdb_rpc_log(4995,
                    "Rdb_writebatch_impl: "
                    "rocksdb_WriteBatchWithIndex__WriteBatchWithIndex");
    m_batch = rocksdb_WriteBatchWithIndex__WriteBatchWithIndex(
        rocksdb_BytewiseComparator(), 0, true);
  }

  virtual ~Rdb_writebatch_impl() override {
    // Remove from the global list before all other processing is started.
    // Otherwise, information_schema.rocksdb_trx can crash on this object.
    rocksdb_rpc_log(5021, " ~Rdb_writebatch_impl: start");

    Rdb_transaction::remove_from_global_trx_list();

    rollback();
    // ALTER
    // delete m_batch;
    rocksdb_rpc_log(
        5028, " ~Rdb_writebatch_impl: rocksdb_WriteBatchWithIndex__delete");
    rocksdb_WriteBatchWithIndex__delete(m_batch);
  }
};

void Rdb_snapshot_notifier::SnapshotCreated(
    const rocksdb::Snapshot *const snapshot) {
  if (m_owning_tx != nullptr) {
    m_owning_tx->snapshot_created(snapshot);
  }
}

std::multiset<Rdb_transaction *> Rdb_transaction::s_tx_list;
mysql_mutex_t Rdb_transaction::s_tx_list_mutex;

Rdb_transaction *&get_tx_from_thd(THD *const thd) {
  return *reinterpret_cast<Rdb_transaction **>(
      my_core::thd_ha_data(thd, rocksdb_hton));
}

class Rdb_perf_context_guard {
  Rdb_io_perf m_io_perf;
  Rdb_io_perf *m_io_perf_ptr;
  Rdb_transaction *m_tx;
  uint m_level;

 public:
  Rdb_perf_context_guard(const Rdb_perf_context_guard &) = delete;
  Rdb_perf_context_guard &operator=(const Rdb_perf_context_guard &) = delete;

  explicit Rdb_perf_context_guard(Rdb_io_perf *io_perf, uint level)
      : m_io_perf_ptr(io_perf), m_tx(nullptr), m_level(level) {
    m_io_perf_ptr->start(m_level);
  }

  explicit Rdb_perf_context_guard(Rdb_transaction *tx, uint level)
      : m_io_perf_ptr(nullptr), m_tx(tx), m_level(level) {
    /*
      if perf_context information is already being recorded, this becomes a
      no-op
    */
    if (tx != nullptr) {
      tx->io_perf_start(&m_io_perf);
    }
  }

  ~Rdb_perf_context_guard() {
    if (m_tx != nullptr) {
      m_tx->io_perf_end_and_record();
    } else if (m_io_perf_ptr != nullptr) {
      m_io_perf_ptr->end_and_record(m_level);
    }
  }
};

/*
  TODO: maybe, call this in external_lock() and store in ha_rocksdb..
*/

static Rdb_transaction *get_or_create_tx(THD *const thd) {
  rocksdb_rpc_log(5088, " get_or_create_tx: begin");
  Rdb_transaction *&tx = get_tx_from_thd(thd);
  // TODO: this is called too many times.. O(#rows)
  if (tx == nullptr) {
    if ((rpl_skip_tx_api && thd->rli_slave) ||
        (THDVAR(thd, master_skip_tx_api) && !thd->rli_slave)) {
      tx = new Rdb_writebatch_impl(thd);
    } else {
      tx = new Rdb_transaction_impl(thd);
    }
    tx->set_params(THDVAR(thd, lock_wait_timeout), rocksdb_max_row_locks);
    tx->start_tx();

    // Add the transaction to the global list of transactions
    // once it is fully constructed.
    tx->add_to_global_trx_list();
  } else {
    tx->set_params(THDVAR(thd, lock_wait_timeout), rocksdb_max_row_locks);
    if (!tx->is_tx_started()) {
      tx->start_tx();
    }
  }
  rocksdb_rpc_log(5088, " get_or_create_tx: end");
  return tx;
}

static int rocksdb_close_connection(handlerton *const hton, THD *const thd) {
  rocksdb_rpc_log(5115, " rocksdb_close_connection: start");
  Rdb_transaction *&tx = get_tx_from_thd(thd);
  if (tx != nullptr) {
    bool is_critical_error;
    int rc = tx->finish_bulk_load(&is_critical_error, false);
    if (rc != 0 && is_critical_error) {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Error %d finalizing last SST file while "
          "disconnecting",
          rc);
    }

    delete tx;
    tx = nullptr;
  }
  rocksdb_rpc_log(5131, " rocksdb_close_connection: end");
  return HA_EXIT_SUCCESS;
}

/**
  Called by hton->flush_logs after MySQL group commit prepares a set of
  transactions.
*/
static bool rocksdb_flush_wal(handlerton *const hton MY_ATTRIBUTE((__unused__)),
                              ulonglong target_lsn MY_ATTRIBUTE((__unused__))) {
  DBUG_ASSERT(rdb != nullptr);
  rocksdb_rpc_log(5142, " rocksdb_flush_wal: start");

  rocksdb::Status s;
  /*
    target_lsn is set to 0 when MySQL wants to sync the wal files
  */
  if ((target_lsn == 0 && !rocksdb_DBOptions__GetBoolOptions(
                              rocksdb_db_options, "allow_mmap_writes")) ||
      rocksdb_flush_log_at_trx_commit != FLUSH_LOG_NEVER) {
    rocksdb_wal_group_syncs++;

    // ALTER
    // s = rdb->FlushWAL(target_lsn == 0 ||
    //                   rocksdb_flush_log_at_trx_commit == FLUSH_LOG_SYNC);
    rocksdb_rpc_log(5156,
                    " rocksdb_flush_wal: rocksdb_TransactionDB__FlushWAL");
    s = rocksdb_TransactionDB__FlushWAL(
        rdb,
        target_lsn == 0 || rocksdb_flush_log_at_trx_commit == FLUSH_LOG_SYNC);
  }

  if (!s.ok()) {
    rdb_log_status_error(s);
    rocksdb_rpc_log(5164, " rocksdb_flush_wal: end");
    return HA_EXIT_FAILURE;
  }
  rocksdb_rpc_log(5167, " rocksdb_flush_wal: end");
  return HA_EXIT_SUCCESS;
}

/**
  For a slave, prepare() updates the slave_gtid_info table which tracks the
  replication progress.
*/
static int rocksdb_prepare(handlerton *const hton, THD *const thd,
                           bool prepare_tx, bool async) {
  rocksdb_rpc_log(5177, " rocksdb_prepare: start");
  Rdb_transaction *&tx = get_tx_from_thd(thd);
  if (!tx->can_prepare()) {
    return HA_EXIT_FAILURE;
  }
  if (prepare_tx ||
      (!my_core::thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {
    /* We were instructed to prepare the whole transaction, or
    this is an SQL statement end and autocommit is on */
    std::vector<st_slave_gtid_info> slave_gtid_info;
    my_core::thd_slave_gtid_info(thd, &slave_gtid_info);
    for (const auto &it : slave_gtid_info) {
      rocksdb::WriteBatchBase *const write_batch = tx->get_blind_write_batch();
      binlog_manager.update_slave_gtid_info(it.id, it.db, it.gtid, write_batch);
    }

    if (tx->is_two_phase()) {
      if (thd->durability_property == HA_IGNORE_DURABILITY || async) {
        tx->set_sync(false);
      }
      if (rocksdb_write_policy != rocksdb::TxnDBWritePolicy::WRITE_UNPREPARED) {
        tx->set_name();
      }
      if (!tx->prepare()) {
        rocksdb_rpc_log(5201, " rocksdb_prepare: end");
        return HA_EXIT_FAILURE;
      }
      if (thd->durability_property == HA_IGNORE_DURABILITY &&
          (rocksdb_flush_log_at_trx_commit != FLUSH_LOG_NEVER)) {
        /**
          we set the log sequence as '1' just to trigger hton->flush_logs
        */
        thd_store_lsn(thd, 1, DB_TYPE_ROCKSDB);
      }
    }

    DEBUG_SYNC(thd, "rocksdb.prepared");
  } else {
    tx->make_stmt_savepoint_permanent();
  }
  rocksdb_rpc_log(5217, " rocksdb_prepare: end");
  return HA_EXIT_SUCCESS;
}

/**
 do nothing for prepare/commit by xid
 this is needed to avoid crashes in XA scenarios
*/
static int rocksdb_commit_by_xid(handlerton *const hton, XID *const xid) {
  rocksdb_rpc_log(5226, " rocksdb_commit_by_xid: start");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(hton != nullptr);
  DBUG_ASSERT(xid != nullptr);
  DBUG_ASSERT(commit_latency_stats != nullptr);

  // ALTER
  auto clock = rocksdb::Env::Default()->GetSystemClock().get();
  // auto clock = rocksdb_Env__GetSystemClock_get(rocksdb_Env_Default());

  rocksdb::StopWatchNano timer(clock, true);

  const auto name = rdb_xid_to_string(*xid);
  DBUG_ASSERT(!name.empty());

  rocksdb_rpc_log(
      5245,
      " rocksdb_commit_by_xid: rocksdb_TransactionDB__GetTransactionByName");
  // ALTER
  // rocksdb::Transaction *const trx = rdb->GetTransactionByName(name);
  rocksdb::Transaction *const trx =
      rocksdb_TransactionDB__GetTransactionByName(rdb, name);

  if (trx == nullptr) {
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  // ALTER
  // const rocksdb::Status s = trx->Commit();
  rocksdb_rpc_log(5255, " rocksdb_commit_by_xid: rocksdb_Transaction__Commit");
  const rocksdb::Status s = rocksdb_Transaction__Commit(trx);

  if (!s.ok()) {
    rdb_log_status_error(s);
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rocksdb_rpc_log(5262, " rocksdb_commit_by_xid: rocksdb_Transaction__delete");
  // ALTER
  // delete trx;
  rocksdb_Transaction__delete(trx);

  // `Add()` is implemented in a thread-safe manner.
  commit_latency_stats->Add(timer.ElapsedNanos() / 1000);

  DBUG_RETURN(HA_EXIT_SUCCESS);
  rocksdb_rpc_log(5271, " rocksdb_commit_by_xid: end");
}

static int rocksdb_rollback_by_xid(
    handlerton *const hton MY_ATTRIBUTE((__unused__)), XID *const xid) {
  rocksdb_rpc_log(5276, " rocksdb_rollback_by_xid: start");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(hton != nullptr);
  DBUG_ASSERT(xid != nullptr);
  DBUG_ASSERT(rdb != nullptr);

  const auto name = rdb_xid_to_string(*xid);

  // ALTER
  // rocksdb::Transaction *const trx = rdb->GetTransactionByName(name);
  rocksdb_rpc_log(
      5288,
      " rocksdb_rollback_by_xid: rocksdb_TransactionDB__GetTransactionByName");
  rocksdb::Transaction *const trx =
      rocksdb_TransactionDB__GetTransactionByName(rdb, name);

  if (trx == nullptr) {
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rocksdb_rpc_log(5298,
                  " rocksdb_rollback_by_xid: rocksdb_Transaction__Rollback");
  // ALTER
  // const rocksdb::Status s = trx->Rollback();
  const rocksdb::Status s = rocksdb_Transaction__Rollback(trx);

  if (!s.ok()) {
    rdb_log_status_error(s);
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  // ALTER
  // delete trx;
  rocksdb_Transaction__delete(trx);

  DBUG_RETURN(HA_EXIT_SUCCESS);
  rocksdb_rpc_log(5288, " rocksdb_rollback_by_xid: end");
}

/**
  Rebuilds an XID from a serialized version stored in a string.
*/
static void rdb_xid_from_string(const std::string &src, XID *const dst) {
  rocksdb_rpc_log(5288, " rdb_xid_from_string: start");
  DBUG_ASSERT(dst != nullptr);
  uint offset = 0;
  uint64 raw_fid8 =
      rdb_netbuf_to_uint64(reinterpret_cast<const uchar *>(src.data()));
  const int64 signed_fid8 = *reinterpret_cast<int64 *>(&raw_fid8);
  dst->formatID = signed_fid8;
  offset += RDB_FORMATID_SZ;
  dst->gtrid_length = src.at(offset);
  offset += RDB_GTRID_SZ;
  dst->bqual_length = src.at(offset);
  offset += RDB_BQUAL_SZ;

  DBUG_ASSERT(dst->gtrid_length >= 0 && dst->gtrid_length <= MAXGTRIDSIZE);
  DBUG_ASSERT(dst->bqual_length >= 0 && dst->bqual_length <= MAXBQUALSIZE);

  memset(dst->data, 0, XIDDATASIZE);
  src.copy(dst->data, (dst->gtrid_length) + (dst->bqual_length),
           RDB_XIDHDR_LEN);
  rocksdb_rpc_log(5343, " rdb_xid_from_string: end");
}

/**
  Reading last committed binary log info from RocksDB system row.
  The info is needed for crash safe slave/master to work.
*/
static int rocksdb_recover(handlerton *const hton, XID *const xid_list,
                           uint len, char *const binlog_file,
                           my_off_t *const binlog_pos,
                           Gtid *const binlog_max_gtid) {
  rocksdb_rpc_log(5354, " rocksdb_recover: start");

  if (binlog_file && binlog_pos) {
    char file_buf[FN_REFLEN + 1] = {0};
    my_off_t pos;
    char gtid_buf[FN_REFLEN + 1] = {0};
    if (binlog_manager.read(file_buf, &pos, gtid_buf)) {
      if (is_binlog_advanced(binlog_file, *binlog_pos, file_buf, pos)) {
        memcpy(binlog_file, file_buf, FN_REFLEN + 1);
        *binlog_pos = pos;
        // NO_LINT_DEBUG
        fprintf(stderr,
                "RocksDB: Last binlog file position %llu,"
                " file name %s\n",
                pos, file_buf);
        if (*gtid_buf) {
          global_sid_lock->rdlock();
          binlog_max_gtid->parse(global_sid_map, gtid_buf);
          global_sid_lock->unlock();
          // NO_LINT_DEBUG
          fprintf(stderr, "RocksDB: Last MySQL Gtid %s\n", gtid_buf);
        }
      }
    }
  }

  if (len == 0 || xid_list == nullptr) {
    return HA_EXIT_SUCCESS;
  }

  std::vector<rocksdb::Transaction *> trans_list;

  rocksdb_rpc_log(
      5386,
      " rocksdb_recover: rocksdb_TransactionDB__GetAllPreparedTransactions");
  // ALTER
  // rdb->GetAllPreparedTransactions(&trans_list);
  rocksdb_TransactionDB__GetAllPreparedTransactions(rdb, trans_list);
  uint count = 0;

  for (auto &trans : trans_list) {
    if (count >= len) {
      break;
    }
    // ALTER
    // auto name = trans->GetName();
    auto name = rocksdb_Transaction__GetName(trans);

    rdb_xid_from_string(name, &xid_list[count]);
    count++;
  }
  rocksdb_rpc_log(5403, " rocksdb_recover: end");
  return count;
}

static int rocksdb_commit(handlerton *const hton, THD *const thd,
                          bool commit_tx, bool) {
  rocksdb_rpc_log(5410, " rocksdb_commit: begin");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(hton != nullptr);
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(commit_latency_stats != nullptr);

  // ALTER
  auto clock = rocksdb::Env::Default()->GetSystemClock().get();
  // auto clock = rocksdb_Env__GetSystemClock_get(rocksdb_Env_Default());

  rocksdb::StopWatchNano timer(clock, true);

  /* note: h->external_lock(F_UNLCK) is called after this function is called) */
  Rdb_transaction *&tx = get_tx_from_thd(thd);

  /* this will trigger saving of perf_context information */
  Rdb_perf_context_guard guard(tx, rocksdb_perf_context_level(thd));

  if (tx != nullptr) {
    if (commit_tx || (!my_core::thd_test_options(
                         thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))) {
      /*
        We get here
         - For a COMMIT statement that finishes a multi-statement transaction
         - For a statement that has its own transaction
      */
      if (tx->commit()) {
        DBUG_RETURN(HA_ERR_ROCKSDB_COMMIT_FAILED);
      }
    } else {
      /*
        We get here when committing a statement within a transaction.
      */
      tx->set_tx_failed(false);
      tx->make_stmt_savepoint_permanent();
    }

    if (my_core::thd_tx_isolation(thd) <= ISO_READ_COMMITTED) {
      // For READ_COMMITTED, we release any existing snapshot so that we will
      // see any changes that occurred since the last statement.
      tx->release_snapshot();
    }
  }

  // `Add()` is implemented in a thread-safe manner.
  commit_latency_stats->Add(timer.ElapsedNanos() / 1000);

  DBUG_RETURN(HA_EXIT_SUCCESS);
  rocksdb_rpc_log(5459, " rocksdb_commit: begin");
}

static int rocksdb_rollback(handlerton *const hton, THD *const thd,
                            bool rollback_tx) {
  rocksdb_rpc_log(5464, " rocksdb_rollback: begin");
  Rdb_transaction *&tx = get_tx_from_thd(thd);
  Rdb_perf_context_guard guard(tx, rocksdb_perf_context_level(thd));

  if (tx != nullptr) {
    if (rollback_tx) {
      /*
        We get here, when
        - ROLLBACK statement is issued.

        Discard the changes made by the transaction
      */
      tx->rollback();
    } else {
      /*
        We get here when
        - a statement with AUTOCOMMIT=1 is being rolled back (because of some
          error)
        - a statement inside a transaction is rolled back
      */

      tx->rollback_stmt();
      tx->set_tx_failed(true);
    }

    if (my_core::thd_tx_isolation(thd) <= ISO_READ_COMMITTED) {
      // For READ_COMMITTED, we release any existing snapshot so that we will
      // see any changes that occurred since the last statement.
      tx->release_snapshot();
    }
  }
  rocksdb_rpc_log(5495, " rocksdb_rollback: end");
  return HA_EXIT_SUCCESS;
}

static bool print_stats(THD *const thd, std::string const &type,
                        std::string const &name, std::string const &status,
                        stat_print_fn *stat_print) {
  return stat_print(thd, type.c_str(), type.size(), name.c_str(), name.size(),
                    status.c_str(), status.size());
}

static std::string format_string(const char *const format, ...) {
  std::string res;
  va_list args;
  va_list args_copy;
  char static_buff[256];

  DBUG_ASSERT(format != nullptr);

  va_start(args, format);
  va_copy(args_copy, args);

  // Calculate how much space we will need
  int len = vsnprintf(nullptr, 0, format, args);
  va_end(args);

  if (len < 0) {
    res = std::string("<format error>");
  } else if (len == 0) {
    // Shortcut for an empty string
    res = std::string("");
  } else {
    // For short enough output use a static buffer
    char *buff = static_buff;
    std::unique_ptr<char[]> dynamic_buff = nullptr;

    len++;  // Add one for null terminator

    // for longer output use an allocated buffer
    if (static_cast<uint>(len) > sizeof(static_buff)) {
      dynamic_buff.reset(new char[len]);
      buff = dynamic_buff.get();
    }

    // Now re-do the vsnprintf with the buffer which is now large enough
    (void)vsnprintf(buff, len, format, args_copy);

    // Convert to a std::string.  Note we could have created a std::string
    // large enough and then converted the buffer to a 'char*' and created
    // the output in place.  This would probably work but feels like a hack.
    // Since this isn't code that needs to be super-performant we are going
    // with this 'safer' method.
    res = std::string(buff);
  }

  va_end(args_copy);

  return res;
}

class Rdb_snapshot_status : public Rdb_tx_list_walker {
 private:
  std::string m_data;

  static std::string current_timestamp(void) {
    static const char *const format = "%d-%02d-%02d %02d:%02d:%02d";
    time_t currtime;
    struct tm currtm;

    time(&currtime);

    localtime_r(&currtime, &currtm);

    return format_string(format, currtm.tm_year + 1900, currtm.tm_mon + 1,
                         currtm.tm_mday, currtm.tm_hour, currtm.tm_min,
                         currtm.tm_sec);
  }

  static std::string get_header(void) {
    return "\n============================================================\n" +
           current_timestamp() +
           " ROCKSDB TRANSACTION MONITOR OUTPUT\n"
           "============================================================\n"
           "---------\n"
           "SNAPSHOTS\n"
           "---------\n"
           "LIST OF SNAPSHOTS FOR EACH SESSION:\n";
  }

  static std::string get_footer(void) {
    return "-----------------------------------------\n"
           "END OF ROCKSDB TRANSACTION MONITOR OUTPUT\n"
           "=========================================\n";
  }

  static Rdb_deadlock_info::Rdb_dl_trx_info get_dl_txn_info(
      const rocksdb::DeadlockInfo &txn, const GL_INDEX_ID &gl_index_id) {
    rocksdb_rpc_log(5593, " get_dl_txn_info: start");
    Rdb_deadlock_info::Rdb_dl_trx_info txn_data;

    txn_data.trx_id = txn.m_txn_id;

    txn_data.table_name = ddl_manager.safe_get_table_name(gl_index_id);
    if (txn_data.table_name.empty()) {
      txn_data.table_name =
          "NOT FOUND; INDEX_ID: " + std::to_string(gl_index_id.index_id);
    }

    auto kd = ddl_manager.safe_find(gl_index_id);
    txn_data.index_name =
        (kd) ? kd->get_name()
             : "NOT FOUND; INDEX_ID: " + std::to_string(gl_index_id.index_id);

    // ALTER
    // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
    //     cf_manager.get_cf(txn.m_cf_id);
    rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(txn.m_cf_id);

    // Retrieve CF name from CF handle object, and it is safe if the CF is
    // removed from cf_manager at this point.

    // ALTER
    // txn_data.cf_name = (cfh)
    //                        ? cfh->GetName()
    //                        : "NOT FOUND; CF_ID: " +
    //                        std::to_string(txn.m_cf_id);
    rocksdb_rpc_log(5622,
                    " get_dl_txn_info: rocksdb_ColumnFamilyHandle__GetName");
    txn_data.cf_name = (cfh)
                           ? rocksdb_ColumnFamilyHandle__GetName(cfh)
                           : "NOT FOUND; CF_ID: " + std::to_string(txn.m_cf_id);

    txn_data.waiting_key =
        rdb_hexdump(txn.m_waiting_key.c_str(), txn.m_waiting_key.length());

    txn_data.exclusive_lock = txn.m_exclusive;
    rocksdb_rpc_log(5632, " get_dl_txn_info: end");
    return txn_data;
  }

  static Rdb_deadlock_info get_dl_path_trx_info(
      const rocksdb::DeadlockPath &path_entry) {
    rocksdb_rpc_log(5637, " get_dl_path_trx_info: start");
    Rdb_deadlock_info deadlock_info;

    for (auto it = path_entry.path.begin(); it != path_entry.path.end(); it++) {
      const auto &txn = *it;
      const GL_INDEX_ID gl_index_id = {
          txn.m_cf_id, rdb_netbuf_to_uint32(reinterpret_cast<const uchar *>(
                           txn.m_waiting_key.c_str()))};
      deadlock_info.path.push_back(get_dl_txn_info(txn, gl_index_id));
    }
    DBUG_ASSERT_IFF(path_entry.limit_exceeded, path_entry.path.empty());
    /* print the first txn in the path to display the full deadlock cycle */
    if (!path_entry.path.empty() && !path_entry.limit_exceeded) {
      const auto &deadlocking_txn = *(path_entry.path.end() - 1);
      deadlock_info.victim_trx_id = deadlocking_txn.m_txn_id;
      deadlock_info.deadlock_time = path_entry.deadlock_time;
    }
    rocksdb_rpc_log(5654, " get_dl_path_trx_info: start");
    return deadlock_info;
  }

 public:
  Rdb_snapshot_status() : m_data(get_header()) {}

  std::string getResult() { return m_data + get_footer(); }

  /* Implement Rdb_transaction interface */
  /* Create one row in the snapshot status table */
  void process_tran(const Rdb_transaction *const tx) override {
    DBUG_ASSERT(tx != nullptr);
    rocksdb_rpc_log(5667, " process_tran: start");
    /* Calculate the duration the snapshot has existed */
    int64_t snapshot_timestamp = tx->m_snapshot_timestamp;
    if (snapshot_timestamp != 0) {
      int64_t curr_time;
      rdb->GetEnv()->GetCurrentTime(&curr_time);

      THD *thd = tx->get_thd();
      char buffer[1024];
      thd_security_context_internal(thd, buffer, sizeof buffer, 0,
                                    current_thd->variables.show_query_digest);
      m_data += format_string(
          "---SNAPSHOT, ACTIVE %lld sec\n"
          "%s\n"
          "lock count %llu, write count %llu\n"
          "insert count %llu, update count %llu, delete count %llu\n",
          curr_time - snapshot_timestamp, buffer, tx->get_row_lock_count(),
          tx->get_write_count(), tx->get_insert_count(), tx->get_update_count(),
          tx->get_delete_count());
    }
    rocksdb_rpc_log(5687, " process_tran: end");
  }

  void populate_deadlock_buffer() {
    // ALTER
    // auto dlock_buffer = rdb->GetDeadlockInfoBuffer();
    rocksdb_rpc_log(5693, " populate_deadlock_buffer: start");
    auto dlock_buffer = rocksdb_TransactionDB__GetDeadlockInfoBuffer(rdb);
    m_data += "----------LATEST DETECTED DEADLOCKS----------\n";

    for (const auto &path_entry : dlock_buffer) {
      std::string path_data;
      if (path_entry.limit_exceeded) {
        path_data += "\n-------DEADLOCK EXCEEDED MAX DEPTH-------\n";
      } else {
        path_data +=
            "\n*** DEADLOCK PATH\n"
            "=========================================\n";
        const auto dl_info = get_dl_path_trx_info(path_entry);
        const auto deadlock_time = dl_info.deadlock_time;
        for (auto it = dl_info.path.begin(); it != dl_info.path.end(); it++) {
          const auto &trx_info = *it;
          path_data += format_string(
              "TIMESTAMP: %" PRId64
              "\n"
              "TRANSACTION ID: %u\n"
              "COLUMN FAMILY NAME: %s\n"
              "WAITING KEY: %s\n"
              "LOCK TYPE: %s\n"
              "INDEX NAME: %s\n"
              "TABLE NAME: %s\n",
              deadlock_time, trx_info.trx_id, trx_info.cf_name.c_str(),
              trx_info.waiting_key.c_str(),
              trx_info.exclusive_lock ? "EXCLUSIVE" : "SHARED",
              trx_info.index_name.c_str(), trx_info.table_name.c_str());
          if (it != dl_info.path.end() - 1) {
            path_data += "---------------WAITING FOR---------------\n";
          }
        }
        path_data += format_string(
            "\n--------TRANSACTION ID: %u GOT DEADLOCK---------\n",
            dl_info.victim_trx_id);
      }
      m_data += path_data;
    }
    rocksdb_rpc_log(5732, " populate_deadlock_buffer: start");
  }

  std::vector<Rdb_deadlock_info> get_deadlock_info() {
    std::vector<Rdb_deadlock_info> deadlock_info;
    rocksdb_rpc_log(5737, " get_deadlock_info: start");

    // ALTER
    // auto dlock_buffer = rdb->GetDeadlockInfoBuffer();
    rocksdb_rpc_log(
        5741,
        " get_deadlock_info: rocksdb_TransactionDB__GetDeadlockInfoBuffer");
    auto dlock_buffer = rocksdb_TransactionDB__GetDeadlockInfoBuffer(rdb);

    for (const auto &path_entry : dlock_buffer) {
      if (!path_entry.limit_exceeded) {
        deadlock_info.push_back(get_dl_path_trx_info(path_entry));
      }
    }
    rocksdb_rpc_log(5749, " get_deadlock_info: end");
    return deadlock_info;
  }
};

/**
 * @brief
 * walks through all non-replication transactions and copies
 * out relevant information for information_schema.rocksdb_trx
 */
class Rdb_trx_info_aggregator : public Rdb_tx_list_walker {
 private:
  std::vector<Rdb_trx_info> *m_trx_info;

 public:
  explicit Rdb_trx_info_aggregator(std::vector<Rdb_trx_info> *const trx_info)
      : m_trx_info(trx_info) {}

  void process_tran(const Rdb_transaction *const tx) override {
    rocksdb_rpc_log(5768, " process_tran: start");
    static const std::map<int, std::string> state_map = {
        {rocksdb::Transaction::STARTED, "STARTED"},
        {rocksdb::Transaction::AWAITING_PREPARE, "AWAITING_PREPARE"},
        {rocksdb::Transaction::PREPARED, "PREPARED"},
        {rocksdb::Transaction::AWAITING_COMMIT, "AWAITING_COMMIT"},
        {rocksdb::Transaction::COMMITED, "COMMITED"},
        {rocksdb::Transaction::AWAITING_ROLLBACK, "AWAITING_ROLLBACK"},
        {rocksdb::Transaction::ROLLEDBACK, "ROLLEDBACK"},
    };

    DBUG_ASSERT(tx != nullptr);

    THD *const thd = tx->get_thd();
    ulong thread_id = thd_thread_id(thd);

    if (tx->is_writebatch_trx()) {
      const auto wb_impl = static_cast<const Rdb_writebatch_impl *>(tx);
      DBUG_ASSERT(wb_impl);
      m_trx_info->push_back(
          {"",                            /* name */
           0,                             /* trx_id */
           wb_impl->get_write_count(), 0, /* lock_count */
           0,                             /* timeout_sec */
           "",                            /* state */
           "",                            /* waiting_key */
           0,                             /* waiting_cf_id */
           1,                             /*is_replication */
           1,                             /* skip_trx_api */
           wb_impl->is_tx_read_only(), 0, /* deadlock detection */
           wb_impl->num_ongoing_bulk_load(), thread_id, "" /* query string */});
    } else {
      const auto tx_impl = static_cast<const Rdb_transaction_impl *>(tx);
      DBUG_ASSERT(tx_impl);
      const rocksdb::Transaction *rdb_trx = tx_impl->get_rdb_trx();

      if (rdb_trx == nullptr) {
        return;
      }

      std::string query_str;
      LEX_STRING *const lex_str = thd_query_string(thd);
      if (lex_str != nullptr && lex_str->str != nullptr) {
        query_str = std::string(lex_str->str);
      }

      // ALTER
      // const auto state_it = state_map.find(rdb_trx->GetState());
      rocksdb_rpc_log(5816, " process_tran: rocksdb_Transaction__GetState");
      const auto state_it =
          state_map.find(rocksdb_Transaction__GetState(rdb_trx));

      DBUG_ASSERT(state_it != state_map.end());
      const int is_replication = (thd->rli_slave != nullptr);
      uint32_t waiting_cf_id;
      std::string waiting_key;

      // ALTER
      // rdb_trx->GetWaitingTxns(&waiting_cf_id, &waiting_key),
      rocksdb_rpc_log(5827,
                      " process_tran: rocksdb_Transaction__GetWaitingTxns");
      rocksdb_Transaction__GetWaitingTxns(rdb_trx, &waiting_cf_id,
                                          &waiting_key);
      // ALTER
      // m_trx_info->push_back(
      //     {rdb_trx->GetName(), rdb_trx->GetID(), tx_impl->get_write_count(),
      //      tx_impl->get_row_lock_count(), tx_impl->get_timeout_sec(),
      //      state_it->second, waiting_key, waiting_cf_id, is_replication,
      //      0, /* skip_trx_api */
      //      tx_impl->is_tx_read_only(), rdb_trx->IsDeadlockDetect(),
      //      tx_impl->num_ongoing_bulk_load(), thread_id, query_str});
      m_trx_info->push_back(
          {rocksdb_Transaction__GetName(rdb_trx),
           rocksdb_Transaction__GetID(rdb_trx), tx_impl->get_write_count(),
           tx_impl->get_row_lock_count(), tx_impl->get_timeout_sec(),
           state_it->second, waiting_key, waiting_cf_id, is_replication,
           0, /* skip_trx_api */
           tx_impl->is_tx_read_only(),
           rocksdb_Transaction__IsDeadlockDetect(rdb_trx),
           tx_impl->num_ongoing_bulk_load(), thread_id, query_str});
    }

    rocksdb_rpc_log(5853, " process_tran: end");
  }
};

/*
  returns a vector of info for all non-replication threads
  for use by information_schema.rocksdb_trx
*/
std::vector<Rdb_trx_info> rdb_get_all_trx_info() {
  rocksdb_rpc_log(5862, " rdb_get_all_trx_info: start");
  std::vector<Rdb_trx_info> trx_info;
  Rdb_trx_info_aggregator trx_info_agg(&trx_info);
  Rdb_transaction::walk_tx_list(&trx_info_agg);
  rocksdb_rpc_log(5866, " rdb_get_all_trx_info: end");
  return trx_info;
}

/*
  returns a vector of info of recent deadlocks
  for use by information_schema.rocksdb_deadlock
*/
std::vector<Rdb_deadlock_info> rdb_get_deadlock_info() {
  rocksdb_rpc_log(5875, " rdb_get_deadlock_info: start");
  Rdb_snapshot_status showStatus;
  Rdb_transaction::walk_tx_list(&showStatus);
  rocksdb_rpc_log(5878, " rdb_get_deadlock_info: end");
  return showStatus.get_deadlock_info();
}

/* Generate the snapshot status table */
static bool rocksdb_show_snapshot_status(handlerton *const hton, THD *const thd,
                                         stat_print_fn *const stat_print) {
  rocksdb_rpc_log(5885, " rocksdb_show_snapshot_status: start");
  Rdb_snapshot_status showStatus;

  Rdb_transaction::walk_tx_list(&showStatus);
  showStatus.populate_deadlock_buffer();

  /* Send the result data back to MySQL */
  rocksdb_rpc_log(5892, " rocksdb_show_snapshot_status: end");
  return print_stats(thd, "rocksdb", "", showStatus.getResult(), stat_print);
}

/*
  This is called for SHOW ENGINE ROCKSDB STATUS | LOGS | etc.

  For now, produce info about live files (which gives an imprecise idea about
  what column families are there).
*/
static bool rocksdb_show_status(handlerton *const hton, THD *const thd,
                                stat_print_fn *const stat_print,
                                enum ha_stat_type stat_type) {
  rocksdb_rpc_log(5905, " rocksdb_show_status: start");
  DBUG_ASSERT(hton != nullptr);
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(stat_print != nullptr);

  bool res = false;
  char buf[100] = {'\0'};

  if (stat_type == HA_ENGINE_STATUS) {
    DBUG_ASSERT(rdb != nullptr);

    std::string str;

    /* Global DB Statistics */
    if (rocksdb_stats) {
      // ALTER
      // str = rocksdb_stats->ToString();
      str = rocksdb_Statistics__ToString(rocksdb_stats);

      // Use the same format as internal RocksDB statistics entries to make
      // sure that output will look unified.
      DBUG_ASSERT(commit_latency_stats != nullptr);

      snprintf(buf, sizeof(buf),
               "rocksdb.commit_latency statistics "
               "Percentiles :=> 50 : %.2f 95 : %.2f "
               "99 : %.2f 100 : %.2f\n",
               commit_latency_stats->Percentile(50),
               commit_latency_stats->Percentile(95),
               commit_latency_stats->Percentile(99),
               commit_latency_stats->Percentile(100));

      // snprintf(buf, sizeof(buf),
      //          "rocksdb.commit_latency statistics "
      //          "Percentiles :=> 50 : %.2f 95 : %.2f "
      //          "99 : %.2f 100 : %.2f\n",
      //          rocksdb_HistogramImpl__Percentile(commit_latency_stats, 50),
      //          rocksdb_HistogramImpl__Percentile(commit_latency_stats, 95),
      //          rocksdb_HistogramImpl__Percentile(commit_latency_stats, 99),
      //          rocksdb_HistogramImpl__Percentile(commit_latency_stats, 100));
      str.append(buf);

      uint64_t v = 0;

      // Retrieve additional stalling related numbers from RocksDB and append
      // them to the buffer meant for displaying detailed statistics. The intent
      // here is to avoid adding another row to the query output because of
      // just two numbers.
      //
      // NB! We're replacing hyphens with underscores in output to better match
      // the existing naming convention.

      // ALTER
      // if (rdb->GetIntProperty("rocksdb.is-write-stopped", &v)) {
      //   snprintf(buf, sizeof(buf), "rocksdb.is_write_stopped COUNT : %lu\n",
      //   v); str.append(buf);
      // }
      rocksdb_rpc_log(
          5962, " rocksdb_show_status: rocksdb_TransactionDB__GetIntProperty");
      if (rocksdb_TransactionDB__GetIntProperty(rdb, "rocksdb.is-write-stopped",
                                                &v)) {
        snprintf(buf, sizeof(buf), "rocksdb.is_write_stopped COUNT : %lu\n", v);
        str.append(buf);
      }

      // ALTER
      // if (rdb->GetIntProperty("rocksdb.actual-delayed-write-rate", &v)) {
      //   snprintf(buf, sizeof(buf),
      //            "rocksdb.actual_delayed_write_rate "
      //            "COUNT : %lu\n",
      //            v);
      //   str.append(buf);
      // }

      rocksdb_rpc_log(
          5979, " rocksdb_show_status: rocksdb_TransactionDB__GetIntProperty");
      if (rocksdb_TransactionDB__GetIntProperty(
              rdb, "rocksdb.actual-delayed-write-rate", &v)) {
        snprintf(buf, sizeof(buf),
                 "rocksdb.actual_delayed_write_rate "
                 "COUNT : %lu\n",
                 v);
        str.append(buf);
      }
      res |= print_stats(thd, "STATISTICS", "rocksdb", str, stat_print);
    }

    /* Per DB stats */
    // ALTER
    // if (rdb->GetProperty("rocksdb.dbstats", &str)) {
    //   res |= print_stats(thd, "DBSTATS", "rocksdb", str, stat_print);
    // }
    rocksdb_rpc_log(5997,
                    " rocksdb_show_status: rocksdb_TransactionDB__GetProperty");
    if (rocksdb_TransactionDB__GetProperty(rdb, "rocksdb.dbstats", &str)) {
      res |= print_stats(thd, "DBSTATS", "rocksdb", str, stat_print);
    }

    /* Per column family stats */
    for (const auto &cf_name : cf_manager.get_cf_names()) {
      // ALTER
      // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
      //     cf_manager.get_cf(cf_name);
      rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(cf_name);
      if (!cfh) {
        continue;
      }

      // Retrieve information from CF handle object.
      // Even if the CF is removed from CF_manager, the handle object
      // is valid.

      // ALTER
      // if (!rdb->GetProperty(cfh.get(), "rocksdb.cfstats", &str)) {
      //   continue;
      // }

      rocksdb_rpc_log(
          6021,
          " rocksdb_show_status: rocksdb_TransactionDB__GetPropertyWithCFH");
      if (!rocksdb_TransactionDB__GetPropertyWithCFH(rdb, cfh,
                                                     "rocksdb.cfstats", &str)) {
        continue;
      }

      res |= print_stats(thd, "CF_COMPACTION", cf_name, str, stat_print);
    }

    /* Memory Statistics */
    std::vector<rocksdb::DB *> dbs;
    std::unordered_set<const rocksdb::Cache *> cache_set;
    size_t internal_cache_count = 0;
    size_t kDefaultInternalCacheSize = 8 * 1024 * 1024;

    dbs.push_back(rdb);
    // ALTER
    // cache_set.insert(rocksdb_tbl_options->block_cache.get());
    rocksdb_rpc_log(
        6039,
        " rocksdb_show_status: rocksdb_BlockBasedTableOptions__BlockCachePtr");
    cache_set.insert(
        rocksdb_BlockBasedTableOptions__BlockCachePtr(rocksdb_tbl_options));

    for (const auto &cf_handle : cf_manager.get_all_cf()) {
      // It is safe if the CF handle is removed from cf_manager
      // at this point.
      rocksdb::ColumnFamilyDescriptor *cf_desc;

      // ALTER
      // cf_handle->GetDescriptor(&cf_desc);
      rocksdb_rpc_log(
          6051,
          " rocksdb_show_status: rocksdb_ColumnFamilyHandle__GetDescriptorPtr");
      rocksdb_ColumnFamilyHandle__GetDescriptorPtr(cf_handle, cf_desc);

      // ALTER
      // auto *const table_factory = cf_desc.options.table_factory.get();
      auto *const table_factory =
          rocksdb_ColumnFamilyDescriptor__TableFactoryPtr(cf_desc);

      if (table_factory != nullptr) {
        // ALTER
        // std::string tf_name = table_factory->Name();
        rocksdb_rpc_log(6051,
                        " rocksdb_show_status: rocksdb_TableFactory__Name");
        std::string tf_name = rocksdb_TableFactory__Name(table_factory);

        if (tf_name.find("BlockBasedTable") != std::string::npos) {
          // ALTER
          // const auto bbt_opt =
          //     table_factory->GetOptions<rocksdb::BlockBasedTableOptions>();
          const auto bbt_opt = rocksdb_TableFactory__GetOptions(
              table_factory, "rocksdb::BlockBasedTableOptions");

          if (bbt_opt != nullptr) {
            rocksdb::Cache *cache =
                rocksdb_BlockBasedTableOptions__BlockCachePtr(bbt_opt);

            // ALTER
            // if (bbt_opt->block_cache.get() != nullptr) {
            //   cache_set.insert(bbt_opt->block_cache.get());
            // } else {
            //   internal_cache_count++;
            // }
            if (cache != nullptr) {
              cache_set.insert(cache);
            } else {
              internal_cache_count++;
            }
            // ALTER
            // cache_set.insert(bbt_opt->block_cache_compressed.get());

            rocksdb_rpc_log(
                6089,
                " rocksdb_show_status: "
                "rocksdb_BlockBasedTableOptions__BlockCacheCompressedPtr");
            cache_set.insert(
                rocksdb_BlockBasedTableOptions__BlockCacheCompressedPtr(
                    bbt_opt));
          }
        }
      }
    }

    std::map<rocksdb::MemoryUtil::UsageType, uint64_t> temp_usage_by_type;
    str.clear();

    // ALTER
    // rocksdb::MemoryUtil::GetApproximateMemoryUsageByType(dbs, cache_set,
    //                                                      &temp_usage_by_type);
    rocksdb_rpc_log(6143,
                    "rocksdb_show_status: "
                    "rocksdb_MemoryUtil_GetApproximateMemoryUsageByType");
    rocksdb_MemoryUtil_GetApproximateMemoryUsageByType(dbs, cache_set,
                                                       temp_usage_by_type);

    snprintf(buf, sizeof(buf), "\nMemTable Total: %lu",
             temp_usage_by_type[rocksdb::MemoryUtil::kMemTableTotal]);
    str.append(buf);
    snprintf(buf, sizeof(buf), "\nMemTable Unflushed: %lu",
             temp_usage_by_type[rocksdb::MemoryUtil::kMemTableUnFlushed]);
    str.append(buf);
    snprintf(buf, sizeof(buf), "\nTable Readers Total: %lu",
             temp_usage_by_type[rocksdb::MemoryUtil::kTableReadersTotal]);
    str.append(buf);
    snprintf(buf, sizeof(buf), "\nCache Total: %lu",
             temp_usage_by_type[rocksdb::MemoryUtil::kCacheTotal]);
    str.append(buf);
    snprintf(buf, sizeof(buf), "\nDefault Cache Capacity: %lu",
             internal_cache_count * kDefaultInternalCacheSize);
    str.append(buf);
    res |= print_stats(thd, "MEMORY_STATS", "rocksdb", str, stat_print);

    /* Show the background thread status */
    std::vector<rocksdb::ThreadStatus> thread_list;

    // ALTER
    // rocksdb::Status s = rdb->GetEnv()->GetThreadList(&thread_list);
    rocksdb_rpc_log(6143,
                    " rocksdb_show_status: "
                    "rocksdb_Env__GetThreadList");
    rocksdb::Status s = rocksdb_Env__GetThreadList(
        rocksdb_TransactionDB__GetEnv(rdb), thread_list);

    if (!s.ok()) {
      // NO_LINT_DEBUG
      sql_print_error("RocksDB: Returned error (%s) from GetThreadList.\n",
                      s.ToString().c_str());
      res |= true;
    } else {
      /* For each background thread retrieved, print out its information */
      for (auto &it : thread_list) {
        /* Only look at background threads. Ignore user threads, if any. */
        if (it.thread_type > rocksdb::ThreadStatus::LOW_PRIORITY) {
          continue;
        }

        str = "\nthread_type: " + it.GetThreadTypeName(it.thread_type) +
              "\ncf_name: " + it.cf_name +
              "\noperation_type: " + it.GetOperationName(it.operation_type) +
              "\noperation_stage: " +
              it.GetOperationStageName(it.operation_stage) +
              "\nelapsed_time_ms: " + it.MicrosToString(it.op_elapsed_micros);

        for (auto &it_props : it.InterpretOperationProperties(
                 it.operation_type, it.op_properties)) {
          str += "\n" + it_props.first + ": " + std::to_string(it_props.second);
        }

        str += "\nstate_type: " + it.GetStateName(it.state_type);

        res |= print_stats(thd, "BG_THREADS", std::to_string(it.thread_id), str,
                           stat_print);
      }
    }

    /* Explicit snapshot information */
    str = Rdb_explicit_snapshot::dump_snapshots();
    if (!str.empty()) {
      res |= print_stats(thd, "EXPLICIT_SNAPSHOTS", "rocksdb", str, stat_print);
    }
  } else if (stat_type == HA_ENGINE_TRX) {
    /* Handle the SHOW ENGINE ROCKSDB TRANSACTION STATUS command */
    res |= rocksdb_show_snapshot_status(hton, thd, stat_print);
  }
  rocksdb_rpc_log(6143, "rocksdb_show_status: end");
  return res;
}

static inline void rocksdb_register_tx(handlerton *const hton, THD *const thd,
                                       Rdb_transaction *const tx) {
  rocksdb_rpc_log(6193, "rocksdb_register_tx: start");
  DBUG_ASSERT(tx != nullptr);

  trans_register_ha(thd, FALSE, rocksdb_hton);
  if (rocksdb_write_policy == rocksdb::TxnDBWritePolicy::WRITE_UNPREPARED) {
    // Some internal operations will call trans_register_ha, but they do not
    // go through 2pc. In this case, the xid is set with query_id == 0, which
    // means that rocksdb will receive transactions with duplicate names.
    //
    // Skip setting name in these cases.
    if (thd->query_id != 0) {
      tx->set_name();
    }
  }
  if (my_core::thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
    tx->start_stmt();
    trans_register_ha(thd, TRUE, rocksdb_hton);
  }
  rocksdb_rpc_log(6212, "rocksdb_register_tx: end");
}

static bool rocksdb_explicit_snapshot(
    handlerton *const /* hton */, /*!< in: RocksDB handlerton */
    THD *const thd,               /*!< in: MySQL thread handle */
    snapshot_info_st *ss_info)    /*!< out: Snapshot information */
{
  rocksdb_rpc_log(6220, "rocksdb_explicit_snapshot: start");
  switch (ss_info->op) {
    case snapshot_operation::SNAPSHOT_CREATE: {
      if (mysql_bin_log_is_open()) {
        mysql_bin_log_lock_commits(ss_info);
      }
      auto s = Rdb_explicit_snapshot::create(ss_info, rdb, rdb->GetSnapshot());
      if (mysql_bin_log_is_open()) {
        mysql_bin_log_unlock_commits(ss_info);
      }

      thd->set_explicit_snapshot(s);
      rocksdb_rpc_log(6232, "rocksdb_explicit_snapshot: end");
      return s == nullptr;
    }
    case snapshot_operation::SNAPSHOT_ATTACH: {
      auto s = Rdb_explicit_snapshot::get(ss_info->snapshot_id);
      if (!s) {
        rocksdb_rpc_log(6240, "rocksdb_explicit_snapshot: end");
        return true;
      }
      *ss_info = s->ss_info;
      thd->set_explicit_snapshot(s);
      rocksdb_rpc_log(6245, "rocksdb_explicit_snapshot: end");
      return false;
    }
    case snapshot_operation::SNAPSHOT_RELEASE: {
      if (!thd->get_explicit_snapshot()) {
        rocksdb_rpc_log(6250, "rocksdb_explicit_snapshot: end");
        return true;
      }
      *ss_info = thd->get_explicit_snapshot()->ss_info;
      thd->set_explicit_snapshot(nullptr);
      rocksdb_rpc_log(6255, "rocksdb_explicit_snapshot: end");
      return false;
    }
    default:
      DBUG_ASSERT(false);
      rocksdb_rpc_log(6261, "rocksdb_explicit_snapshot: end");
      return true;
  }
  rocksdb_rpc_log(6263, "rocksdb_explicit_snapshot: end");
  return true;
}

/*
    Supporting START TRANSACTION WITH CONSISTENT [ROCKSDB] SNAPSHOT

    Features:
    1. Supporting START TRANSACTION WITH CONSISTENT SNAPSHOT
    2. Getting current binlog position in addition to #1.

    The second feature is done by START TRANSACTION WITH
    CONSISTENT ROCKSDB SNAPSHOT. This is Facebook's extension, and
    it works like existing START TRANSACTION WITH CONSISTENT INNODB SNAPSHOT.

    - When not setting engine, START TRANSACTION WITH CONSISTENT SNAPSHOT
    takes both InnoDB and RocksDB snapshots, and both InnoDB and RocksDB
    participate in transaction. When executing COMMIT, both InnoDB and
    RocksDB modifications are committed. Remember that XA is not supported yet,
    so mixing engines is not recommended anyway.

    - When setting engine, START TRANSACTION WITH CONSISTENT.. takes
    snapshot for the specified engine only. But it starts both
    InnoDB and RocksDB transactions.
*/
static int rocksdb_start_tx_and_assign_read_view(
    handlerton *const hton,    /*!< in: RocksDB handlerton */
    THD *const thd,            /*!< in: MySQL thread handle of the
                               user for whom the transaction should
                               be committed */
    snapshot_info_st *ss_info) /*!< in/out: Snapshot info like binlog file, pos,
                               gtid executed and snapshot ID */
{
  rocksdb_rpc_log(6296, "rocksdb_start_tx_and_assign_read_view: start");
  ulong const tx_isolation = my_core::thd_tx_isolation(thd);

  if (tx_isolation != ISO_REPEATABLE_READ) {
    my_error(ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT, MYF(0));
    rocksdb_rpc_log(6302, "rocksdb_start_tx_and_assign_read_view: end");
    return HA_EXIT_FAILURE;
  }

  if (ss_info) {
    if (mysql_bin_log_is_open()) {
      mysql_bin_log_lock_commits(ss_info);
    } else {
      rocksdb_rpc_log(6309, "rocksdb_start_tx_and_assign_read_view: end");

      return HA_EXIT_FAILURE;
    }
  }

  Rdb_transaction *const tx = get_or_create_tx(thd);
  Rdb_perf_context_guard guard(tx, rocksdb_perf_context_level(thd));

  DBUG_ASSERT(!tx->has_snapshot());
  tx->set_tx_read_only(true);
  rocksdb_register_tx(hton, thd, tx);
  tx->acquire_snapshot(true);

  if (ss_info) {
    mysql_bin_log_unlock_commits(ss_info);
  }
  rocksdb_rpc_log(6326, "rocksdb_start_tx_and_assign_read_view: end");

  return HA_EXIT_SUCCESS;
}

static int rocksdb_start_tx_with_shared_read_view(
    handlerton *const hton,    /*!< in: RocksDB handlerton */
    THD *const thd,            /*!< in: MySQL thread handle of the
                               user for whom the transaction should
                               be committed */
    snapshot_info_st *ss_info) /*!< out: Snapshot info like binlog file, pos,
                               gtid executed and snapshot ID */
{
  rocksdb_rpc_log(6309, "rocksdb_start_tx_with_shared_read_view: start");
  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(ss_info != nullptr);

  int error = HA_EXIT_SUCCESS;

  ulong const tx_isolation = my_core::thd_tx_isolation(thd);
  if (tx_isolation != ISO_REPEATABLE_READ) {
    my_error(ER_ISOLATION_LEVEL_WITH_CONSISTENT_SNAPSHOT, MYF(0));
    return HA_EXIT_FAILURE;
  }

  std::shared_ptr<Rdb_explicit_snapshot> explicit_snapshot;
  const auto op = ss_info->op;
  Rdb_transaction *tx = nullptr;

  DBUG_ASSERT(op == snapshot_operation::SNAPSHOT_CREATE ||
              op == snapshot_operation::SNAPSHOT_ATTACH);

  // case: if binlogs are available get binlog file/pos and gtid info
  if (op == snapshot_operation::SNAPSHOT_CREATE && mysql_bin_log_is_open()) {
    mysql_bin_log_lock_commits(ss_info);
  }

  if (op == snapshot_operation::SNAPSHOT_ATTACH) {
    explicit_snapshot = Rdb_explicit_snapshot::get(ss_info->snapshot_id);
    if (!explicit_snapshot) {
      my_printf_error(ER_UNKNOWN_ERROR, "Snapshot %llu does not exist", MYF(0),
                      ss_info->snapshot_id);
      error = HA_EXIT_FAILURE;
    }
  }

  // case: all good till now
  if (error == HA_EXIT_SUCCESS) {
    tx = get_or_create_tx(thd);
    Rdb_perf_context_guard guard(tx, rocksdb_perf_context_level(thd));

    if (explicit_snapshot) {
      tx->m_explicit_snapshot = explicit_snapshot;
    }

    DBUG_ASSERT(!tx->has_snapshot());
    tx->set_tx_read_only(true);
    rocksdb_register_tx(hton, thd, tx);
    tx->acquire_snapshot(true);

    // case: an explicit snapshot was not assigned to this transaction
    if (!tx->m_explicit_snapshot) {
      // ALTER
      // tx->m_explicit_snapshot =
      //     Rdb_explicit_snapshot::create(ss_info, rdb,
      //     tx->m_read_opts.snapshot);
      rocksdb_rpc_log(6392,
                      "rocksdb_start_tx_with_shared_read_view: "
                      "rocksdb_ReadOptions__GetSnapshot");
      tx->m_explicit_snapshot = Rdb_explicit_snapshot::create(
          ss_info, rdb, rocksdb_ReadOptions__GetSnapshot(tx->m_read_opts));
      if (!tx->m_explicit_snapshot) {
        my_printf_error(ER_UNKNOWN_ERROR, "Could not create snapshot", MYF(0));
        error = HA_EXIT_FAILURE;
      }
    }
  }

  // case: unlock the binlog
  if (op == snapshot_operation::SNAPSHOT_CREATE && mysql_bin_log_is_open()) {
    mysql_bin_log_unlock_commits(ss_info);
  }

  DBUG_ASSERT(error == HA_EXIT_FAILURE || tx->m_explicit_snapshot);

  // copy over the snapshot details to pass to the upper layers
  if (tx->m_explicit_snapshot) {
    *ss_info = tx->m_explicit_snapshot->ss_info;
    ss_info->op = op;
  }
  rocksdb_rpc_log(6416, "rocksdb_start_tx_with_shared_read_view: end");

  return error;
}

/* Dummy SAVEPOINT support. This is needed for long running transactions
 * like mysqldump (https://bugs.mysql.com/bug.php?id=71017).
 * Current SAVEPOINT does not correctly handle ROLLBACK and does not return
 * errors. This needs to be addressed in future versions (Issue#96).
 */
static int rocksdb_savepoint(handlerton *const hton, THD *const thd,
                             void *const savepoint) {
  return HA_EXIT_SUCCESS;
}

static int rocksdb_rollback_to_savepoint(handlerton *const hton, THD *const thd,
                                         void *const savepoint) {
  rocksdb_rpc_log(6433, "rocksdb_rollback_to_savepoint: start");
  Rdb_transaction *&tx = get_tx_from_thd(thd);
  rocksdb_rpc_log(6435, "rocksdb_rollback_to_savepoint: end");
  return tx->rollback_to_savepoint(savepoint);
}

static bool rocksdb_rollback_to_savepoint_can_release_mdl(
    handlerton *const /* hton */, THD *const /* thd */) {
  return true;
}

/*
  This is called for INFORMATION_SCHEMA
*/
static void rocksdb_update_table_stats(
    /* per-table stats callback */
    void (*cb)(const char *_db, const char *_tbl, bool _is_partition,
               my_io_perf_t *_r, my_io_perf_t *w, my_io_perf_t *_r_blob,
               my_io_perf_t *_r_primary, my_io_perf_t *_r_secondary,
               page_stats_t *_page_stats, comp_stats_t *_comp_stats,
               int _n_lock_wait, int _n_lock_wait_timeout, int _n_lock_deadlock,
               const char *_engine)) {
  rocksdb_rpc_log(6433, "rocksdb_update_table_stats: start");
  my_io_perf_t io_perf_read;
  my_io_perf_t io_perf_write;
  my_io_perf_t io_perf;
  page_stats_t page_stats;
  comp_stats_t comp_stats;
  uint lock_wait_timeout_stats;
  uint deadlock_stats;
  uint lock_wait_stats;
  std::vector<std::string> tablenames;

  /*
    Most of these are for innodb, so setting them to 0.
    TODO: possibly separate out primary vs. secondary index reads
   */
  memset(&io_perf, 0, sizeof(io_perf));
  memset(&page_stats, 0, sizeof(page_stats));
  memset(&comp_stats, 0, sizeof(comp_stats));
  memset(&io_perf_write, 0, sizeof(io_perf_write));

  tablenames = rdb_open_tables.get_table_names();

  for (const auto &it : tablenames) {
    Rdb_table_handler *table_handler;
    std::string str, dbname, tablename, partname;
    char dbname_sys[NAME_LEN + 1];
    char tablename_sys[NAME_LEN + 1];
    bool is_partition;

    if (rdb_normalize_tablename(it, &str) != HA_EXIT_SUCCESS) {
      /* Function needs to return void because of the interface and we've
       * detected an error which shouldn't happen. There's no way to let
       * caller know that something failed.
       */
      SHIP_ASSERT(false);
      return;
    }

    if (rdb_split_normalized_tablename(str, &dbname, &tablename, &partname)) {
      continue;
    }

    is_partition = (partname.size() != 0);

    table_handler = rdb_open_tables.get_table_handler(it.c_str());
    if (table_handler == nullptr) {
      continue;
    }

    io_perf_read.bytes = table_handler->m_io_perf_read.bytes.load();
    io_perf_read.requests = table_handler->m_io_perf_read.requests.load();
    io_perf_write.bytes = table_handler->m_io_perf_write.bytes.load();
    io_perf_write.requests = table_handler->m_io_perf_write.requests.load();
    lock_wait_timeout_stats = table_handler->m_lock_wait_timeout_counter.load();
    deadlock_stats = table_handler->m_deadlock_counter.load();
    lock_wait_stats =
        table_handler->m_table_perf_context.m_value[PC_KEY_LOCK_WAIT_COUNT]
            .load();

    /*
      Convert from rocksdb timer to mysql timer. RocksDB values are
      in nanoseconds, but table statistics expect the value to be
      in my_timer format.
     */
    io_perf_read.svc_time = my_core::microseconds_to_my_timer(
        table_handler->m_io_perf_read.svc_time.load() / 1000);
    io_perf_read.svc_time_max = my_core::microseconds_to_my_timer(
        table_handler->m_io_perf_read.svc_time_max.load() / 1000);
    io_perf_read.wait_time = my_core::microseconds_to_my_timer(
        table_handler->m_io_perf_read.wait_time.load() / 1000);
    io_perf_read.wait_time_max = my_core::microseconds_to_my_timer(
        table_handler->m_io_perf_read.wait_time_max.load() / 1000);
    io_perf_read.slow_ios = table_handler->m_io_perf_read.slow_ios.load();
    rdb_open_tables.release_table_handler(table_handler);

    /*
      Table stats expects our database and table name to be in system encoding,
      not filename format. Convert before calling callback.
     */
    my_core::filename_to_tablename(dbname.c_str(), dbname_sys,
                                   sizeof(dbname_sys));
    my_core::filename_to_tablename(tablename.c_str(), tablename_sys,
                                   sizeof(tablename_sys));
    (*cb)(dbname_sys, tablename_sys, is_partition, &io_perf_read,
          &io_perf_write, &io_perf, &io_perf, &io_perf, &page_stats,
          &comp_stats, lock_wait_stats, lock_wait_timeout_stats, deadlock_stats,
          rocksdb_hton_name);
  }
  rocksdb_rpc_log(6543, "rocksdb_update_table_stats: end");
}

// TODO: ALTER this func
static rocksdb::Status check_rocksdb_options_compatibility(
    const char *const dbpath, const rocksdb::Options &main_opts,
    const std::vector<rocksdb::ColumnFamilyDescriptor> &cf_descr) {
  DBUG_ASSERT(rocksdb_datadir != nullptr);

  rocksdb::DBOptions loaded_db_opt;
  std::vector<rocksdb::ColumnFamilyDescriptor> loaded_cf_descs;
  rocksdb::Status status =
      LoadLatestOptions(dbpath, rocksdb::Env::Default(), &loaded_db_opt,
                        &loaded_cf_descs, rocksdb_ignore_unknown_options);

  // If we're starting from scratch and there are no options saved yet then this
  // is a valid case. Therefore we can't compare the current set of options to
  // anything.
  if (status.IsNotFound()) {
    return rocksdb::Status::OK();
  }

  if (!status.ok()) {
    return status;
  }

  if (loaded_cf_descs.size() != cf_descr.size()) {
    return rocksdb::Status::NotSupported(
        "Mismatched size of column family "
        "descriptors.");
  }

  // Please see RocksDB documentation for more context about why we need to set
  // user-defined functions and pointer-typed options manually.
  for (size_t i = 0; i < loaded_cf_descs.size(); i++) {
    loaded_cf_descs[i].options.compaction_filter =
        cf_descr[i].options.compaction_filter;
    loaded_cf_descs[i].options.compaction_filter_factory =
        cf_descr[i].options.compaction_filter_factory;
    loaded_cf_descs[i].options.comparator = cf_descr[i].options.comparator;
    loaded_cf_descs[i].options.memtable_factory =
        cf_descr[i].options.memtable_factory;
    loaded_cf_descs[i].options.merge_operator =
        cf_descr[i].options.merge_operator;
    loaded_cf_descs[i].options.prefix_extractor =
        cf_descr[i].options.prefix_extractor;
    loaded_cf_descs[i].options.table_factory =
        cf_descr[i].options.table_factory;
  }

  // This is the essence of the function - determine if it's safe to open the
  // database or not.
  status = CheckOptionsCompatibility(dbpath, rocksdb::Env::Default(), main_opts,
                                     loaded_cf_descs,
                                     rocksdb_ignore_unknown_options);

  return status;
}

/* Clean up tables leftover from truncation */
void rocksdb_truncation_table_cleanup(void) {
  rocksdb_rpc_log(6604, "rocksdb_truncation_table_cleanup: start");
  /* Scan for tables that have the truncation prefix */
  struct Rdb_truncate_tbls : public Rdb_tables_scanner {
   public:
    std::vector<Rdb_tbl_def *> m_tbl_list;
    int add_table(Rdb_tbl_def *tdef) override {
      DBUG_ASSERT(tdef != nullptr);
      if (tdef->base_tablename().find(TRUNCATE_TABLE_PREFIX) !=
          std::string::npos) {
        m_tbl_list.push_back(tdef);
      }
      return HA_EXIT_SUCCESS;
    }
  } collector;
  ddl_manager.scan_for_tables(&collector);

  /*
    For now, delete any table found. It's possible to rename them back,
    but there's a risk the rename can potentially lead to other inconsistencies.
    Removing the old table (which is being truncated anyway) seems to be the
    safest solution.
  */
  ha_rocksdb table(rocksdb_hton, nullptr);
  for (Rdb_tbl_def *tbl_def : collector.m_tbl_list) {
    // NO_LINT_DEBUG
    sql_print_warning("MyRocks: Removing truncated leftover table %s",
                      tbl_def->full_tablename().c_str());
    table.delete_table(tbl_def);
  }
  rocksdb_rpc_log(6633, "rocksdb_truncation_table_cleanup: end");
}

/*
  Storage Engine initialization function, invoked when plugin is loaded.
*/

static int rocksdb_init_func(void *const p) {
  rocksdb_rpc_log(6641, "rocksdb_init_func: start");
  DBUG_ENTER_FUNC();

  if (rdb_check_rocksdb_corruption()) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: There was a corruption detected in RockDB files. "
        "Check error log emitted earlier for more details.");
    if (rocksdb_allow_to_start_after_corruption) {
      // NO_LINT_DEBUG
      sql_print_information(
          "RocksDB: Remove rocksdb_allow_to_start_after_corruption to prevent "
          "server operating if RocksDB corruption is detected.");
    } else {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: The server will exit normally and stop restart "
          "attempts. Remove %s file from data directory and "
          "start mysqld manually.",
          rdb_corruption_marker_file_name().c_str());
      exit(0);
    }
  }
  rocksdb_rpc_log(6664,
                  "rocksdb_init_func: finish rdb_check_rocksdb_corruption");

  // Validate the assumption about the size of ROCKSDB_SIZEOF_HIDDEN_PK_COLUMN.
  static_assert(sizeof(longlong) == 8, "Assuming that longlong is 8 bytes.");

  init_rocksdb_psi_keys();

  rocksdb_rpc_log(6671, "rocksdb_init_func: finish init_rocksdb_psi_keys");

  rocksdb_hton = (handlerton *)p;

  rdb_open_tables.init();
  rocksdb_rpc_log(6675, "rocksdb_init_func: finish rdb_open_tables.init()");

  Ensure_cleanup rdb_open_tables_cleanup([]() { rdb_open_tables.free(); });

#ifdef HAVE_PSI_INTERFACE
  rdb_bg_thread.init(rdb_signal_bg_psi_mutex_key, rdb_signal_bg_psi_cond_key);
  rdb_drop_idx_thread.init(rdb_signal_drop_idx_psi_mutex_key,
                           rdb_signal_drop_idx_psi_cond_key);
  rdb_is_thread.init(rdb_signal_is_psi_mutex_key, rdb_signal_is_psi_cond_key);
  rdb_mc_thread.init(rdb_signal_mc_psi_mutex_key, rdb_signal_mc_psi_cond_key);
#else
  rdb_bg_thread.init();
  rdb_drop_idx_thread.init();
  rdb_is_thread.init();
  rdb_mc_thread.init();
#endif
  rocksdb_rpc_log(6692, "rocksdb_init_func: finish thread init");

  mysql_mutex_init(rdb_collation_data_mutex_key, &rdb_collation_data_mutex,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(rdb_mem_cmp_space_mutex_key, &rdb_mem_cmp_space_mutex,
                   MY_MUTEX_INIT_FAST);

#if defined(HAVE_PSI_INTERFACE)
  rdb_collation_exceptions =
      new Regex_list_handler(key_rwlock_collation_exception_list);
#else
  rdb_collation_exceptions = new Regex_list_handler();
#endif

  mysql_mutex_init(rdb_sysvars_psi_mutex_key, &rdb_sysvars_mutex,
                   MY_MUTEX_INIT_FAST);
  mysql_mutex_init(rdb_block_cache_resize_mutex_key,
                   &rdb_block_cache_resize_mutex, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(rdb_bottom_pri_background_compactions_resize_mutex_key,
                   &rdb_bottom_pri_background_compactions_resize_mutex,
                   MY_MUTEX_INIT_FAST);
  Rdb_transaction::init_mutex();

  rocksdb_rpc_log(6715, "rocksdb_init_func: finish mutex init");

  rocksdb_hton->state = SHOW_OPTION_YES;
  rocksdb_hton->create = rocksdb_create_handler;
  rocksdb_hton->close_connection = rocksdb_close_connection;
  rocksdb_hton->prepare = rocksdb_prepare;
  rocksdb_hton->commit_by_xid = rocksdb_commit_by_xid;
  rocksdb_hton->rollback_by_xid = rocksdb_rollback_by_xid;
  rocksdb_hton->recover = rocksdb_recover;
  rocksdb_hton->commit = rocksdb_commit;
  rocksdb_hton->rollback = rocksdb_rollback;
  rocksdb_hton->db_type = DB_TYPE_ROCKSDB;
  rocksdb_hton->show_status = rocksdb_show_status;
  rocksdb_hton->explicit_snapshot = rocksdb_explicit_snapshot;
  rocksdb_hton->start_consistent_snapshot =
      rocksdb_start_tx_and_assign_read_view;
  rocksdb_hton->start_shared_snapshot = rocksdb_start_tx_with_shared_read_view;
  rocksdb_hton->savepoint_set = rocksdb_savepoint;
  rocksdb_hton->savepoint_rollback = rocksdb_rollback_to_savepoint;
  rocksdb_hton->savepoint_rollback_can_release_mdl =
      rocksdb_rollback_to_savepoint_can_release_mdl;
  rocksdb_hton->update_table_stats = rocksdb_update_table_stats;
  rocksdb_hton->flush_logs = rocksdb_flush_wal;
  rocksdb_hton->handle_single_table_select = rocksdb_handle_single_table_select;

  rocksdb_hton->flags = HTON_TEMPORARY_NOT_SUPPORTED |
                        HTON_SUPPORTS_EXTENDED_KEYS | HTON_CAN_RECREATE;

  DBUG_ASSERT(!mysqld_embedded);
  rocksdb_rpc_log(6745, "rocksdb_init_func: finish hton set");

  // ALTER
  // if (rocksdb_db_options->max_open_files > (long)open_files_limit) {
  //   // NO_LINT_DEBUG
  //   sql_print_information(
  //       "RocksDB: rocksdb_max_open_files should not be "
  //       "greater than the open_files_limit, effective value "
  //       "of rocksdb_max_open_files is being set to "
  //       "open_files_limit / 2.");
  //   rocksdb_db_options->max_open_files = open_files_limit / 2;
  // } else if (rocksdb_db_options->max_open_files == -2) {
  //   rocksdb_db_options->max_open_files = open_files_limit / 2;
  // }
  if (rocksdb_DBOptions__GetIntOptions(rocksdb_db_options, "max_open_files") >
      (long)open_files_limit) {
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: rocksdb_max_open_files should not be "
        "greater than the open_files_limit, effective value "
        "of rocksdb_max_open_files is being set to "
        "open_files_limit / 2.");
    rocksdb_rpc_log(6767,
                    "rocksdb_init_func: rocksdb_DBOptions__SetIntOptions");
    rocksdb_DBOptions__SetIntOptions(rocksdb_db_options, "max_open_files",
                                     open_files_limit / 2);
  } else if (rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
                                              "max_open_files") == -2) {
    rocksdb_rpc_log(6773,
                    "rocksdb_init_func: rocksdb_DBOptions__SetIntOptions");
    rocksdb_DBOptions__SetIntOptions(rocksdb_db_options, "max_open_files",
                                     open_files_limit / 2);
  }

  rdb_read_free_regex_handler.set_patterns(DEFAULT_READ_FREE_RPL_TABLES);

  // ALTER
  // rocksdb_stats = rocksdb::CreateDBStatistics();
  // rocksdb_stats->set_stats_level(
  //     static_cast<rocksdb::StatsLevel>(rocksdb_stats_level));
  // rocksdb_stats_level = rocksdb_stats->get_stats_level();
  // rocksdb_db_options->statistics = rocksdb_stats;

  rocksdb_rpc_log(6786, "rocksdb_init_func: rocksdb_CreateDBStatistics");
  rocksdb_stats = rocksdb_CreateDBStatistics();
  rocksdb_Statistics__set_stats_level(
      rocksdb_stats, static_cast<rocksdb::StatsLevel>(rocksdb_stats_level));
  rocksdb_stats_level = rocksdb_Statistics__get_stats_level(rocksdb_stats);
  rocksdb_rpc_log(6791, "rocksdb_init_func: rocksdb_DBOptions__SetStatistics");
  rocksdb_DBOptions__SetStatistics(rocksdb_db_options, rocksdb_stats);

  if (rocksdb_rate_limiter_bytes_per_sec != 0) {
    // ALTER
    // rocksdb_rate_limiter.reset(
    //     rocksdb::NewGenericRateLimiter(rocksdb_rate_limiter_bytes_per_sec));
    rocksdb_rpc_log(6798, "rocksdb_init_func: rocksdb_NewGenericRateLimiter");
    rocksdb_rate_limiter =
        rocksdb_NewGenericRateLimiter(rocksdb_rate_limiter_bytes_per_sec);

    // ALTER
    // rocksdb_db_options->rate_limiter = rocksdb_rate_limiter;
    rocksdb_rpc_log(6804,
                    "rocksdb_init_func: rocksdb_DBOptions__SetRateLimiter");
    rocksdb_DBOptions__SetRateLimiter(rocksdb_db_options, rocksdb_rate_limiter);
  }

  rocksdb_rpc_log(6808,
                  "rocksdb_init_func: rocksdb_DBOptions__SetUInt64Options");
  // ALTER
  // rocksdb_db_options->delayed_write_rate = rocksdb_delayed_write_rate;
  rocksdb_DBOptions__SetUInt64Options(rocksdb_db_options, "delayed_write_rate",
                                      rocksdb_delayed_write_rate);
  std::shared_ptr<Rdb_logger> myrocks_logger = std::make_shared<Rdb_logger>();

  // ALTER
  // rocksdb::Status s = rocksdb::CreateLoggerFromOptions(
  //     rocksdb_datadir, *rocksdb_db_options, &rocksdb_db_options->info_log);
  rocksdb_rpc_log(6818, "rocksdb_init_func: rocksdb_CreateLoggerFromOptions");
  rocksdb::Status s =
      rocksdb_CreateLoggerFromOptions(rocksdb_datadir, rocksdb_db_options);

  if (s.ok()) {
    // ALTER
    // myrocks_logger->SetRocksDBLogger(rocksdb_db_options->info_log);

    rocksdb_rpc_log(6826, "rocksdb_init_func: SetRocksDBLogger");
    // ALTER
    // myrocks_logger->SetRocksDBLogger(
    //     rocksdb_DBOptions__GetLoggerPtr(rocksdb_db_options));
  }

  // ALTER
  // not set the logger
  // rocksdb_db_options->info_log = myrocks_logger;

  // ALTER
  // myrocks_logger->SetInfoLogLevel(
  //     static_cast<rocksdb::InfoLogLevel>(rocksdb_info_log_level));

  // ALTER
  // rocksdb_db_options->wal_dir = rocksdb_wal_dir;
  rocksdb_rpc_log(
      6841, "rocksdb_init_func: rocksdb_DBOptions__SetStringOptions waldir");
  rocksdb_DBOptions__SetStringOptions(rocksdb_db_options, "wal_dir", "");

  // ALTER
  // rocksdb_db_options->wal_recovery_mode =
  //     static_cast<rocksdb::WALRecoveryMode>(rocksdb_wal_recovery_mode);
  rocksdb_rpc_log(6848,
                  "rocksdb_init_func: rocksdb_DBOptions__SetWALModeOptions");
  rocksdb_DBOptions__SetWALModeOptions(
      rocksdb_db_options, "wal_recovery_mode",
      static_cast<rocksdb::WALRecoveryMode>(rocksdb_wal_recovery_mode));

  // ALTER
  // rocksdb_db_options->track_and_verify_wals_in_manifest =
  //     rocksdb_track_and_verify_wals_in_manifest;
  rocksdb_rpc_log(6856,
                  "rocksdb_init_func: rocksdb_DBOptions__SetBoolOptions "
                  "track_and_verify_wals_in_manifest");
  rocksdb_DBOptions__SetBoolOptions(rocksdb_db_options,
                                    "track_and_verify_wals_in_manifest",
                                    rocksdb_track_and_verify_wals_in_manifest);

  // ALTER
  // rocksdb_db_options->access_hint_on_compaction_start =
  //     static_cast<rocksdb::Options::AccessHint>(
  //         rocksdb_access_hint_on_compaction_start);
  rocksdb_rpc_log(6865,
                  "rocksdb_init_func: rocksdb_DBOptions__SetAccessHint "
                  "access_hint_on_compaction_start");
  rocksdb_DBOptions__SetAccessHint(
      rocksdb_db_options, "access_hint_on_compaction_start",
      static_cast<rocksdb::Options::AccessHint>(
          rocksdb_access_hint_on_compaction_start));

  // ALTER
  // if (rocksdb_db_options->allow_mmap_reads &&
  //     rocksdb_db_options->use_direct_reads) {
  //   // allow_mmap_reads implies !use_direct_reads and RocksDB will not open
  //   if
  //   // mmap_reads and direct_reads are both on.   (NO_LINT_DEBUG)
  //   sql_print_error(
  //       "RocksDB: Can't enable both use_direct_reads "
  //       "and allow_mmap_reads\n");
  //   DBUG_RETURN(HA_EXIT_FAILURE);
  // }

  if (rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                        "allow_mmap_reads") &&
      rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                        "use_direct_reads")) {
    rocksdb_rpc_log(6886, "rocksdb_init_func: failed");
    sql_print_error(
        "RocksDB: Can't enable both use_direct_reads "
        "and allow_mmap_reads\n");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  // Check whether the filesystem backing rocksdb_datadir allows O_DIRECT

  // ALTER
  // if (rocksdb_db_options->use_direct_reads ||
  //     rocksdb_db_options->use_direct_io_for_flush_and_compaction) {
  if (rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                        "use_direct_reads") ||
      rocksdb_DBOptions__GetBoolOptions(
          rocksdb_db_options, "use_direct_io_for_flush_and_compaction")) {
    rocksdb::EnvOptions soptions;
    rocksdb::Status check_status;

    // ALTER
    // rocksdb::Env *const env = rocksdb_db_options->env;

    rocksdb_rpc_log(6908, "rocksdb_init_func: rocksdb_DBOptions__GetEnv");
    rocksdb::Env *const env = rocksdb_DBOptions__GetEnv(rocksdb_db_options);

    std::string fname = format_string("%s/DIRECT_CHECK", rocksdb_datadir);

    // ALTER
    // if (env->FileExists(fname).ok()) {
    if (rocksdb_Env__FileExists(env, fname).ok()) {
      // ALTER
      // std::unique_ptr<rocksdb::SequentialFile> file;
      std::unique_ptr<rocksdb::SequentialFile> *file;

      soptions.use_direct_reads = true;

      // ALTER
      // check_status = env->NewSequentialFile(fname, &file, soptions);
      rocksdb_rpc_log(6924,
                      "rocksdb_init_func: rocksdb_Env__NewSequentialFile");
      check_status = rocksdb_Env__NewSequentialFile(env, fname, file, soptions);
    } else {
      // ALTER
      // std::unique_ptr<rocksdb::WritableFile> file;
      std::unique_ptr<rocksdb::WritableFile> *file;

      soptions.use_direct_writes = true;

      // ALTER
      // check_status = env->ReopenWritableFile(fname, &file, soptions);
      rocksdb_rpc_log(6946,
                      "rocksdb_init_func: rocksdb_Env__ReopenWritableFile");
      check_status =
          rocksdb_Env__ReopenWritableFile(env, fname, file, soptions);

      if (!rocksdb_File__IsWritableFileNull(file)) {
        // ALTER
        // file->Close();
        rocksdb_rpc_log(6954,
                        "rocksdb_init_func: rocksdb_File__CloseWritableFile");
        rocksdb_File__CloseWritableFile(file);
      }

      // ALTER
      // env->DeleteFile(fname);
      rocksdb_rpc_log(6961, "rocksdb_init_func: rocksdb_Env__DeleteFile");
      rocksdb_Env__DeleteFile(env, fname);
    }

    if (!check_status.ok()) {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Unable to use direct io in rocksdb-datadir:"
          "(%s)",
          check_status.getState());
      rocksdb_rpc_log(6972, "rocksdb_init_func: failed");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }
  }

  // ALTER
  // if (rocksdb_db_options->allow_mmap_writes &&
  //     rocksdb_db_options->use_direct_io_for_flush_and_compaction) {
  // See above comment for allow_mmap_reads. (NO_LINT_DEBUG)

  if (rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                        "allow_mmap_writes") ||
      rocksdb_DBOptions__GetBoolOptions(
          rocksdb_db_options, "use_direct_io_for_flush_and_compaction")) {
    rocksdb_rpc_log(6985, "rocksdb_init_func: failed");
    sql_print_error(
        "RocksDB: Can't enable both "
        "use_direct_io_for_flush_and_compaction and "
        "allow_mmap_writes\n");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  // ALTER
  // if (rocksdb_db_options->allow_mmap_writes &&
  //     rocksdb_flush_log_at_trx_commit != FLUSH_LOG_NEVER)
  if (rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                        "allow_mmap_writes") &&
      rocksdb_flush_log_at_trx_commit != FLUSH_LOG_NEVER) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: rocksdb_flush_log_at_trx_commit needs to be 0 "
        "to use allow_mmap_writes");
    rocksdb_rpc_log(7004, "rocksdb_init_func: failed");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rocksdb_rpc_log(7007, "rocksdb_init_func: init sst file manager");
  // sst_file_manager will move deleted rocksdb sst files to trash_dir
  // to be deleted in a background thread.
  std::string trash_dir = std::string(rocksdb_datadir) + "/trash";

  // ALTER
  // rocksdb_db_options->sst_file_manager.reset(NewSstFileManager(
  //     rocksdb_db_options->env, myrocks_logger, trash_dir,
  //     rocksdb_sst_mgr_rate_bytes_per_sec, true /* delete_existing_trash */));

  rocksdb_DBOptions__SetSstFileManager(
      rocksdb_db_options,
      rocksdb_NewSstFileManager(rocksdb_DBOptions__GetEnv(rocksdb_db_options),
                                nullptr, trash_dir,
                                rocksdb_sst_mgr_rate_bytes_per_sec, true));

  std::vector<std::string> cf_names;
  rocksdb::Status status;
  // ALTER
  // status = rocksdb::DB::ListColumnFamilies(*rocksdb_db_options,
  // rocksdb_datadir,
  //                                          &cf_names);

  rocksdb_rpc_log(7030, "rocksdb_init_func: rocksdb_DB_ListColumnFamilies");

  status = rocksdb_DB_ListColumnFamilies(rocksdb_db_options, rocksdb_datadir,
                                         cf_names);
  if (!status.ok()) {
    /*
      When we start on an empty datadir, ListColumnFamilies returns IOError,
      and RocksDB doesn't provide any way to check what kind of error it was.
      Checking system errno happens to work right now.
    */
    if (status.IsIOError()) {
      // NO_LINT_DEBUG
      sql_print_information("RocksDB: Got ENOENT when listing column families");

      // NO_LINT_DEBUG
      sql_print_information(
          "RocksDB:   assuming that we're creating a new database");
    } else {
      rdb_log_status_error(status, "Error listing column families");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }
  } else {
    // NO_LINT_DEBUG
    sql_print_information("RocksDB: %ld column families found",
                          cf_names.size());
  }

  // ALTER
  // std::vector<rocksdb::ColumnFamilyDescriptor> cf_descr;
  std::vector<rocksdb::ColumnFamilyDescriptor *> cf_descr;
  std::vector<rocksdb::ColumnFamilyHandle *> cf_handles;

  // ALTER
  // rocksdb_tbl_options->index_type =
  //     (rocksdb::BlockBasedTableOptions::IndexType)rocksdb_index_type;
  rocksdb_rpc_log(
      7065, "rocksdb_init_func: rocksdb_BlockBasedTableOptions__SetIndexType");

  rocksdb_BlockBasedTableOptions__SetIndexType(
      rocksdb_tbl_options,
      (rocksdb::BlockBasedTableOptions::IndexType)rocksdb_index_type);

  rocksdb_rpc_log(
      7075,
      "rocksdb_init_func: rocksdb_BlockBasedTableOptions__GetBoolOptions");
  // ALTER
  // if (!rocksdb_tbl_options->no_block_cache) {
  if (!rocksdb_BlockBasedTableOptions__GetBoolOptions(rocksdb_tbl_options,
                                                      "no_block_cache")) {
    // ALTER
    // std::shared_ptr<rocksdb::MemoryAllocator> memory_allocator;
    rocksdb_rpc_log(7085, "rocksdb_init_func: rocksdb_MemoryAllocator_New");

    std::shared_ptr<rocksdb::MemoryAllocator> *memory_allocator =
        rocksdb_MemoryAllocator_New();

    // ALTER: temperarily ignore it
    //     if (!rocksdb_cache_dump) {
    // // TODO: ALTER
    // #ifdef HAVE_JEMALLOC
    //       size_t block_size = rocksdb_tbl_options->block_size;
    //       rocksdb::JemallocAllocatorOptions alloc_opt;
    //       // Limit jemalloc tcache memory usage. The range
    //       // [block_size/4, block_size] should be enough to cover most of
    //       // block cache allocation sizes.
    //       alloc_opt.limit_tcache_size = true;
    //       alloc_opt.tcache_size_lower_bound = block_size / 4;
    //       alloc_opt.tcache_size_upper_bound = block_size;
    //       rocksdb::Status new_alloc_status =
    //           rocksdb::NewJemallocNodumpAllocator(alloc_opt,
    //           &memory_allocator);
    //       if (!new_alloc_status.ok()) {
    //         // Fallback to use default malloc/free.
    //         rdb_log_status_error(new_alloc_status,
    //                              "Error excluding block cache from core
    //                              dump");
    //         memory_allocator = nullptr;
    //         DBUG_RETURN(HA_EXIT_FAILURE);
    //       }
    // #else
    //       // NO_LINT_DEBUG
    //       sql_print_warning(
    //           "Ignoring rocksdb_cache_dump because jemalloc is missing.");
    // #endif  // HAVE_JEMALLOC
    //     }
    // ALTER
    // std::shared_ptr<rocksdb::Cache> block_cache =
    //     rocksdb_use_clock_cache
    //         ? rocksdb::NewClockCache(rocksdb_block_cache_size)
    //         : rocksdb::NewLRUCache(
    //               rocksdb_block_cache_size, -1 /*num_shard_bits*/,
    //               false /*strict_capcity_limit*/,
    //               rocksdb_cache_high_pri_pool_ratio, memory_allocator);

    rocksdb_rpc_log(7125, "rocksdb_init_func: init block cache");
    std::shared_ptr<rocksdb::Cache> *block_cache =
        rocksdb_use_clock_cache
            ? rocksdb_NewClockCache(rocksdb_block_cache_size)
            : rocksdb_NewLRUCache(
                  rocksdb_block_cache_size, -1 /*num_shard_bits*/,
                  false /*strict_capcity_limit*/,
                  rocksdb_cache_high_pri_pool_ratio, memory_allocator);

    if (rocksdb_sim_cache_size > 0) {
      // Simulated cache enabled
      // Wrap block cache inside a simulated cache and pass it to RocksDB

      // ALTER
      // rocksdb_tbl_options->block_cache =
      //     rocksdb::NewSimCache(block_cache, rocksdb_sim_cache_size, 6);
      rocksdb_rpc_log(7145, "rocksdb_init_func: init sim cache");
      rocksdb_BlockBasedTableOptions__SetBlockCache(
          rocksdb_tbl_options,
          rocksdb_NewSimCache(block_cache, rocksdb_sim_cache_size, 6));

    } else {
      // ALTER
      // Pass block cache to RocksDB
      // rocksdb_tbl_options->block_cache = block_cache;
      rocksdb_rpc_log(
          7154,
          "rocksdb_init_func: rocksdb_BlockBasedTableOptions__SetBlockCache");
      rocksdb_BlockBasedTableOptions__SetBlockCache(rocksdb_tbl_options,
                                                    block_cache);
    }
  }

  if (rocksdb_collect_sst_properties) {
    rocksdb_rpc_log(7154, "rocksdb_init_func: init sst properties factory");
    properties_collector_factory =
        std::make_shared<Rdb_tbl_prop_coll_factory>(&ddl_manager);

    rocksdb_set_compaction_options(nullptr, nullptr, nullptr, nullptr);

    RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

    DBUG_ASSERT(rocksdb_table_stats_sampling_pct <=
                RDB_TBL_STATS_SAMPLE_PCT_MAX);
    properties_collector_factory->SetTableStatsSamplingPct(
        rocksdb_table_stats_sampling_pct);

    RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  }

  // TODO: ALTER
  // not enable the rocksdb_persistent_cache
  // if (rocksdb_persistent_cache_size_mb > 0) {
  //   // ALTER
  //   // std::shared_ptr<rocksdb::PersistentCache> pcache;
  //   std::shared_ptr<rocksdb::PersistentCache> *pcache =
  //       rocksdb_Cache__NewPersistentCache();

  //   uint64_t cache_size_bytes = rocksdb_persistent_cache_size_mb * 1024 *
  //   1024; status = rocksdb::NewPersistentCache(
  //       rocksdb::Env::Default(), std::string(rocksdb_persistent_cache_path),
  //       cache_size_bytes, myrocks_logger, true, &pcache);
  //   if (!status.ok()) {
  //     // NO_LINT_DEBUG
  //     sql_print_error("RocksDB: Persistent cache returned error: (%s)",
  //                     status.getState());
  //     DBUG_RETURN(HA_EXIT_FAILURE);
  //   }
  //   rocksdb_tbl_options->persistent_cache = pcache;
  // } else if (strlen(rocksdb_persistent_cache_path)) {
  //   // NO_LINT_DEBUG
  //   sql_print_error("RocksDB: Must specify
  //   rocksdb_persistent_cache_size_mb"); DBUG_RETURN(HA_EXIT_FAILURE);
  // }

  std::unique_ptr<Rdb_cf_options> cf_options_map(new Rdb_cf_options());

  // TODO: ALTER
  // not set this properties_collector_factory
  // if (!cf_options_map->init(*rocksdb_tbl_options,
  // properties_collector_factory,
  //                           rocksdb_default_cf_options,
  //                           rocksdb_override_cf_options)) {
  rocksdb_rpc_log(
      7208, "rocksdb_init_func: rocksdb_BlockBasedTableOptions__SetBlockCache");
  if (!cf_options_map->init(rocksdb_tbl_options, nullptr,
                            rocksdb_default_cf_options,
                            rocksdb_override_cf_options)) {
    rocksdb_rpc_log(7212, "rocksdb_init_func: failed");
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Failed to initialize CF options map.");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  /*
    If there are no column families, we're creating the new database.
    Create one column family named "default".
  */
  if (cf_names.size() == 0) cf_names.push_back(DEFAULT_CF_NAME);

  std::vector<int> compaction_enabled_cf_indices;

  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Column Families at start:");
  for (size_t i = 0; i < cf_names.size(); ++i) {
    // ALTER
    // rocksdb::ColumnFamilyOptions opts;
    rocksdb_rpc_log(7234, "rocksdb_init_func: rocksdb_ColumnFamilyOptions");
    rocksdb::ColumnFamilyOptions *opts = rocksdb_ColumnFamilyOptions();

    cf_options_map->get_cf_options(cf_names[i], opts);

    // NO_LINT_DEBUG
    sql_print_information("  cf=%s", cf_names[i].c_str());

    // ALTER
    // NO_LINT_DEBUG
    // sql_print_information("    write_buffer_size=%ld",
    // opts.write_buffer_size);
    rocksdb_rpc_log(
        7249,
        "rocksdb_init_func: rocksdb_ColumnFamilyOptions__GetSizeTProp "
        "write_buffer_size");
    sql_print_information(
        "    write_buffer_size=%ld",
        rocksdb_ColumnFamilyOptions__GetSizeTProp(opts, "write_buffer_size"));

    // ALTER
    // NO_LINT_DEBUG
    // sql_print_information("    target_file_size_base=%" PRIu64,
    //                       opts.target_file_size_base);
    rocksdb_rpc_log(
        7259,
        "rocksdb_init_func: rocksdb_ColumnFamilyOptions__GetUInt64Prop "
        "target_file_size_base");
    sql_print_information("    target_file_size_base=%" PRIu64,
                          rocksdb_ColumnFamilyOptions__GetUInt64Prop(
                              opts, "target_file_size_base"));

    /*
      Temporarily disable compactions to prevent a race condition where
      compaction starts before compaction filter is ready.
    */
    // ALTER
    // if (!opts.disable_auto_compactions) {
    //   compaction_enabled_cf_indices.push_back(i);
    //   opts.disable_auto_compactions = true;
    // }
    if (!rocksdb_ColumnFamilyOptions__GetBoolProp(opts,
                                                  "disable_auto_compactions")) {
      compaction_enabled_cf_indices.push_back(i);
      rocksdb_ColumnFamilyOptions__SetBoolProp(opts, "disable_auto_compactions",
                                               true);
    }

    rocksdb_rpc_log(7284,
                    "rocksdb_init_func: "
                    "rocksdb_ColumnFamilyDescriptor__ColumnFamilyDescriptor");
    // ALTER
    // cf_descr.push_back(rocksdb::ColumnFamilyDescriptor(cf_names[i], opts));
    cf_descr.push_back(rocksdb_ColumnFamilyDescriptor__ColumnFamilyDescriptor(
        cf_names[i], opts));
  }

  rocksdb_rpc_log(7294,
                  "rocksdb_init_func: "
                  "rocksdb_Options__Options");
  // ALTER
  // rocksdb::Options main_opts(*rocksdb_db_options,
  //                            cf_options_map->get_defaults());
  rocksdb::Options *main_opts = rocksdb_Options__Options(
      rocksdb_db_options, cf_options_map->get_defaults());

  // ALTER
  // rocksdb::TransactionDBOptions tx_db_options;
  // tx_db_options.transaction_lock_timeout = 2000;  // 2 seconds
  // tx_db_options.custom_mutex_factory = std::make_shared<Rdb_mutex_factory>();
  // tx_db_options.write_policy =
  //     static_cast<rocksdb::TxnDBWritePolicy>(rocksdb_write_policy);

  rocksdb_rpc_log(7308,
                  "rocksdb_init_func: "
                  "myrocks_InitTxDBOptions");
  rocksdb::TransactionDBOptions *tx_db_options = myrocks_InitTxDBOptions(
      2000, static_cast<rocksdb::TxnDBWritePolicy>(rocksdb_write_policy));

  // TODO: ALTER
  // temperarily not check the compatibility
  // status =
  //     check_rocksdb_options_compatibility(rocksdb_datadir, main_opts,
  //     cf_descr);
  // // We won't start if we'll determine that there's a chance of data
  // corruption
  // // because of incompatible options.
  // if (!status.ok()) {
  //   rdb_log_status_error(
  //       status, "Compatibility check against existing database options
  //       failed");
  //   DBUG_RETURN(HA_EXIT_FAILURE);
  // }

  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Opening TransactionDB...");

  // ALTER
  // status = rocksdb::TransactionDB::Open(
  //     main_opts, tx_db_options, rocksdb_datadir, cf_descr, &cf_handles,
  //     &rdb);
  rocksdb_rpc_log(7335,
                  "rocksdb_init_func: "
                  "rocksdb_TransactionDB_Open");
  status = rocksdb_TransactionDB_Open(main_opts, tx_db_options, rocksdb_datadir,
                                      cf_descr, &cf_handles, &rdb);

  if (!status.ok()) {
    rdb_log_status_error(status, "Error opening instance");
    rocksdb_rpc_log(7342, "rocksdb_init_func: failed");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }
  cf_manager.init(std::move(cf_options_map), &cf_handles);

  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Initializing data dictionary...");

  rocksdb_rpc_log(7349, "rocksdb_init_func: Initializing data dictionary...");
  // if (st_rdb_exec_time.exec("Rdb_dict_manager::init", [&]() {
  //       return dict_manager.init(rdb, &cf_manager,
  //                                rocksdb_enable_remove_orphaned_dropped_cfs);
  //     })) {
  if (dict_manager.init(rdb, &cf_manager,
                        rocksdb_enable_remove_orphaned_dropped_cfs)) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Failed to initialize data dictionary.");
    rocksdb_rpc_log(7357,
                    "rocksdb_init_func: Failed to initialize data dictionary.");

    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  // NO_LINT_DEBUG
  rocksdb_rpc_log(7363, "rocksdb_init_func: Initializing binlog manager...");
  sql_print_information("RocksDB: Initializing binlog manager...");

  // if (st_rdb_exec_time.exec("Rdb_binlog_manager::init", [&]() {
  //       return binlog_manager.init(&dict_manager);
  //     })) {
  if (binlog_manager.init(&dict_manager)) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Failed to initialize binlog manager.");
    rocksdb_rpc_log(7370,
                    "rocksdb_init_func: Failed to initialize binlog manager.");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rocksdb_rpc_log(7376, "rocksdb_init_func: Initializing binlog manager...");
  // NO_LINT_DEBUG
  sql_print_information("RocksDB: Initializing DDL Manager...");

  // if (st_rdb_exec_time.exec("Rdb_ddl_manager::init", [&]() {
  //       return ddl_manager.init(&dict_manager, &cf_manager,
  //                               rocksdb_validate_tables);
  //     })) {
  if (ddl_manager.init(&dict_manager, &cf_manager, rocksdb_validate_tables)) {
    // NO_LINT_DEBUG
    rocksdb_rpc_log(7385,
                    "rocksdb_init_func: Failed to initialize DDL manager.");
    sql_print_error("RocksDB: Failed to initialize DDL manager.");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  Rdb_sst_info::init(rdb);

  /*
    Enable auto compaction, things needed for compaction filter are finished
    initializing
  */
  rocksdb_rpc_log(7385, "rocksdb_init_func: compaction_enabled_cf_handles");
  std::vector<rocksdb::ColumnFamilyHandle *> compaction_enabled_cf_handles;
  compaction_enabled_cf_handles.reserve(compaction_enabled_cf_indices.size());
  for (const auto &index : compaction_enabled_cf_indices) {
    compaction_enabled_cf_handles.push_back(cf_handles[index]);
  }

  rocksdb_rpc_log(
      7405, "rocksdb_init_func: rocksdb_TransactionDB__EnableAutoCompaction");
  // ALTER
  // status = rdb->EnableAutoCompaction(compaction_enabled_cf_handles);
  status = rocksdb_TransactionDB__EnableAutoCompaction(
      rdb, compaction_enabled_cf_handles);

  if (!status.ok()) {
    rdb_log_status_error(status, "Error enabling compaction");
    rocksdb_rpc_log(7405, "rocksdb_init_func: Error enabling compaction");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

#ifndef HAVE_PSI_INTERFACE
  auto err = rdb_bg_thread.create_thread(BG_THREAD_NAME);
#else
  auto err = rdb_bg_thread.create_thread(BG_THREAD_NAME,
                                         rdb_background_psi_thread_key);
#endif
  rocksdb_rpc_log(7419, "rocksdb_init_func: rdb_bg_thread.create_thread");

  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Couldn't start the background thread: (errno=%d)",
                    err);
    rocksdb_rpc_log(7429,
                    "rocksdb_init_func: Couldn't start the background thread:");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

#ifndef HAVE_PSI_INTERFACE
  err = rdb_drop_idx_thread.create_thread(INDEX_THREAD_NAME);
#else
  err = rdb_drop_idx_thread.create_thread(INDEX_THREAD_NAME,
                                          rdb_drop_idx_psi_thread_key);
#endif

  rocksdb_rpc_log(7439, "rocksdb_init_func: rdb_drop_idx_thread.create_thread");

  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Couldn't start the drop index thread: (errno=%d)",
                    err);
    rocksdb_rpc_log(7446,
                    "rocksdb_init_func: Couldn't start the drop index thread:");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

#ifndef HAVE_PSI_INTERFACE
  err = rdb_is_thread.create_thread(INDEX_STATS_THREAD_NAME);
#else
  err = rdb_is_thread.create_thread(INDEX_STATS_THREAD_NAME,
                                    rdb_is_psi_thread_key);
#endif
  rocksdb_rpc_log(7455, "rocksdb_init_func: rdb_is_thread.create_thread");

  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: Couldn't start the index stats calculation thread: "
        "(errno=%d)",
        err);
    rocksdb_rpc_log(7463,
                    "rocksdb_init_func: Couldn't start the index stats "
                    "calculation thread: ");

    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rocksdb_rpc_log(7468, "rocksdb_init_func: rdb_mc_thread.create_thread");
  err = rdb_mc_thread.create_thread(MANUAL_COMPACTION_THREAD_NAME
#ifdef HAVE_PSI_INTERFACE
                                    ,
                                    rdb_mc_psi_thread_key
#endif
  );
  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: Couldn't start the manual compaction thread: (errno=%d)",
        err);
    rocksdb_rpc_log(
        7480, "rocksdb_init_func: Couldn't start the manual compaction thread");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rdb_set_collation_exception_list(rocksdb_strict_collation_exceptions);
  rocksdb_rpc_log(7485, "rocksdb_init_func: rdb_set_collation_exception_list");

  if (rocksdb_pause_background_work) {
    // ALTER
    // rdb->PauseBackgroundWork();
    rocksdb_rpc_log(
        7490, "rocksdb_init_func: rocksdb_TransactionDB__PauseBackgroundWork");
    rocksdb_TransactionDB__PauseBackgroundWork(rdb);
  }

  rocksdb_rpc_log(7501, "rocksdb_init_func: sched_getcpu");

  // NO_LINT_DEBUG
  sql_print_information("RocksDB: global statistics using %s indexer",
                        STRINGIFY_ARG(RDB_INDEXER));
#if defined(HAVE_SCHED_GETCPU)
  if (sched_getcpu() == -1) {
    // NO_LINT_DEBUG
    sql_print_information(
        "RocksDB: sched_getcpu() failed - "
        "global statistics will use thread_id_indexer_t instead");
  }
#endif

  err = my_error_register(rdb_get_error_message, HA_ERR_ROCKSDB_FIRST,
                          HA_ERR_ROCKSDB_LAST);
  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Couldn't initialize error messages");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  // Creating an instance of HistogramImpl should only happen after RocksDB
  // has been successfully initialized.

  rocksdb_rpc_log(7525, "rocksdb_init_func: new rocksdb::HistogramImpl()");
  commit_latency_stats = new rocksdb::HistogramImpl();
  // commit_latency_stats = rocksdb_HistogramImpl__HistogramImpl();

  // Construct a list of directories which will be monitored by I/O watchdog
  // to make sure that we won't lose write access to them.
  std::vector<std::string> directories;

  // 1. Data directory.
  directories.push_back(mysql_real_data_home);

  // 2. Transaction logs.
  if (myrocks_rpc::rocksdb_wal_dir && *myrocks_rpc::rocksdb_wal_dir) {
    directories.push_back(myrocks_rpc::rocksdb_wal_dir);
  }

  rocksdb_rpc_log(7525, "rocksdb_init_func: new Rdb_io_watchdog");
  io_watchdog = new Rdb_io_watchdog(std::move(directories));
  io_watchdog->reset_timeout(rocksdb_io_write_timeout_secs);

  // Remove tables that may have been leftover during truncation
  rocksdb_truncation_table_cleanup();

  // NO_LINT_DEBUG
  sql_print_information(
      "MyRocks storage engine plugin has been successfully "
      "initialized.");

  st_rdb_exec_time.report();

  // Skip cleaning up rdb_open_tables as we've succeeded
  rdb_open_tables_cleanup.skip();

  rocksdb_rpc_log(7558,
                  "rocksdb_init_func: "
                  "rocksdb_set_max_bottom_pri_background_compactions_internal");
  rocksdb_set_max_bottom_pri_background_compactions_internal(
      rocksdb_max_bottom_pri_background_compactions);

  DBUG_RETURN(HA_EXIT_SUCCESS);
  rocksdb_rpc_log(7563, "rocksdb_init_func: end");
}

/*
  Storage Engine deinitialization function, invoked when plugin is unloaded.
*/

static int rocksdb_done_func(void *const p) {
  rocksdb_rpc_log(7575, "rocksdb_done_func: begin");

  DBUG_ENTER_FUNC();

  int error = 0;

  // signal the drop index thread to stop
  rdb_drop_idx_thread.signal(true);

  // Flush all memtables for not losing data, even if WAL is disabled.
  rocksdb_rpc_log(7584, "rocksdb_done_func: rocksdb_flush_all_memtables");
  rocksdb_flush_all_memtables();

  // Stop all rocksdb background work
  // ALTER
  // CancelAllBackgroundWork(rdb->GetBaseDB(), true);
  rocksdb_rpc_log(7590, "rocksdb_done_func: rocksdb_CancelAllBackgroundWork");
  rocksdb_CancelAllBackgroundWork(rocksdb_TransactionDB__GetBaseDB(rdb), true);

  // Signal the background thread to stop and to persist all stats collected
  // from background flushes and compactions. This will add more keys to a new
  // memtable, but since the memtables were just flushed, it should not trigger
  // a flush that can stall due to background threads being stopped. As long
  // as these keys are stored in a WAL file, they can be retrieved on restart.
  rdb_bg_thread.signal(true);

  // signal the index stats calculation thread to stop
  rdb_is_thread.signal(true);

  // signal the manual compaction thread to stop
  rdb_mc_thread.signal(true);

  // Wait for the background thread to finish.
  rocksdb_rpc_log(7606, "rocksdb_done_func: rdb_bg_thread.join");
  auto err = rdb_bg_thread.join();
  if (err != 0) {
    // We'll log the message and continue because we're shutting down and
    // continuation is the optimal strategy.
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Couldn't stop the background thread: (errno=%d)",
                    err);
  }

  // Wait for the drop index thread to finish.
  rocksdb_rpc_log(7617, "rocksdb_done_func: rdb_drop_idx_thread.join");
  err = rdb_drop_idx_thread.join();
  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error("RocksDB: Couldn't stop the index thread: (errno=%d)", err);
  }

  // Wait for the index stats calculation thread to finish.
  rocksdb_rpc_log(7626, "rocksdb_done_func: rdb_is_thread.join");
  err = rdb_is_thread.join();
  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: Couldn't stop the index stats calculation thread: (errno=%d)",
        err);
  }

  // Wait for the manual compaction thread to finish.
  rocksdb_rpc_log(7636, "rocksdb_done_func: rdb_mc_thread.join");
  err = rdb_mc_thread.join();
  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: Couldn't stop the manual compaction thread: (errno=%d)", err);
  }

  rocksdb_rpc_log(7644, "rocksdb_done_func: rdb_open_tables.count");
  if (rdb_open_tables.count()) {
    // Looks like we are getting unloaded and yet we have some open tables
    // left behind.
    error = 1;
  }

  rdb_open_tables.free();
  rocksdb_rpc_log(7653, "rocksdb_done_func: mysql_mutex_destroy");

  mysql_mutex_destroy(&rdb_sysvars_mutex);
  mysql_mutex_destroy(&rdb_block_cache_resize_mutex);
  mysql_mutex_destroy(&rdb_bottom_pri_background_compactions_resize_mutex);

  delete rdb_collation_exceptions;
  mysql_mutex_destroy(&rdb_collation_data_mutex);
  mysql_mutex_destroy(&rdb_mem_cmp_space_mutex);

  rocksdb_rpc_log(7661, "rocksdb_done_func: Rdb_transaction::term_mutex");
  Rdb_transaction::term_mutex();

  rocksdb_rpc_log(7664, "rocksdb_done_func: delete it");

  for (auto &it : rdb_collation_data) {
    delete it;
    it = nullptr;
  }

  rocksdb_rpc_log(7671, "rocksdb_done_func: ddl_manager.cleanup();");
  ddl_manager.cleanup();
  rocksdb_rpc_log(7673, "rocksdb_done_func: binlog_manager.cleanup();");
  binlog_manager.cleanup();
  rocksdb_rpc_log(7675, "rocksdb_done_func: dict_manager.cleanup();");
  dict_manager.cleanup();
  rocksdb_rpc_log(7677, "rocksdb_done_func: cf_manager.cleanup();");
  cf_manager.cleanup();

  // ALTER
  // delete rdb;
  rocksdb_rpc_log(7682, "rocksdb_done_func: rocksdb_TransactionDB__delete;");
  rocksdb_TransactionDB__delete(rdb);
  rdb = nullptr;

  delete commit_latency_stats;
  // rocksdb_HistogramImpl__delete(commit_latency_stats);
  commit_latency_stats = nullptr;

  delete io_watchdog;
  io_watchdog = nullptr;

  // Disown the cache data since we're shutting down.
  // This results in memory leaks but it improved the shutdown time.
  // Don't disown when running under valgrind

#ifndef HAVE_purify
  // ALTER
  // if (rocksdb_tbl_options->block_cache) {
  //   rocksdb_tbl_options->block_cache->DisownData();
  // }
  // rocksdb_rpc_log(
  //     7702, "rocksdb_done_func:
  //     rocksdb_BlockBasedTableOptions__DisdownData;");
  // rocksdb_BlockBasedTableOptions__DisdownData(rocksdb_tbl_options);
#endif /* HAVE_purify */

  rocksdb_db_options = nullptr;
  rocksdb_tbl_options = nullptr;
  rocksdb_stats = nullptr;

  my_error_unregister(HA_ERR_ROCKSDB_FIRST, HA_ERR_ROCKSDB_LAST);

  DBUG_RETURN(error);
  rocksdb_rpc_log(7713, "rocksdb_done_func: end;");
}

// If the iterator is not valid it might be because of EOF but might be due
// to IOError or corruption. The good practice is always check it.
// https://github.com/facebook/rocksdb/wiki/Iterator#error-handling
inline bool is_valid_iterator(rocksdb::Iterator *scan_it) {
  rocksdb_rpc_log(7720, "is_valid_iterator: start");

  rocksdb_rpc_log(7725, "is_valid_iterator: rocksdb_Iterator__Valid");
  // ALTER
  // if (scan_it->Valid()) {
  if (rocksdb_Iterator__Valid(scan_it)) {
    rocksdb_rpc_log(7726, "is_valid_iterator: begin");
    return true;
  } else {
    // ALTER
    // rocksdb::Status s = scan_it->status();
    rocksdb_rpc_log(7732, "is_valid_iterator: rocksdb_Iterator__status");
    rocksdb::Status s = rocksdb_Iterator__status(scan_it);
    DBUG_EXECUTE_IF("rocksdb_return_status_corrupted",
                    dbug_change_status_to_corrupted(&s););
    if (s.IsIOError() || s.IsCorruption()) {
      if (s.IsCorruption()) {
        rdb_persist_corruption_marker();
      }
      rdb_handle_io_error(s, RDB_IO_ERROR_GENERAL);
    }
    rocksdb_rpc_log(7740, "is_valid_iterator: end");
    return false;
  }
}

/**
  @brief
  Example of simple lock controls. The "table_handler" it creates is a
  structure we will pass to each ha_rocksdb handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

Rdb_table_handler *Rdb_open_tables_map::get_table_handler(
    const char *const table_name) {
  rocksdb_rpc_log(7758, "get_table_handler: begin");
  DBUG_ASSERT(table_name != nullptr);

  Rdb_table_handler *table_handler;

  std::string table_name_str(table_name);

  // First, look up the table in the hash map.
  RDB_MUTEX_LOCK_CHECK(m_mutex);
  const auto it = m_table_map.find(table_name_str);
  if (it != m_table_map.end()) {
    // Found it
    table_handler = it->second;
    rocksdb_rpc_log(7771, "get_table_handler: Found it");
  } else {
    char *tmp_name;

    // Since we did not find it in the hash map, attempt to create and add it
    // to the hash map.
    if (!(table_handler = reinterpret_cast<Rdb_table_handler *>(my_multi_malloc(
              MYF(MY_WME | MY_ZEROFILL), &table_handler, sizeof(*table_handler),
              &tmp_name, table_name_str.length() + 1, NullS)))) {
      // Allocating a new Rdb_table_handler and a new table name failed.
      RDB_MUTEX_UNLOCK_CHECK(m_mutex);
      rocksdb_rpc_log(7781, "get_table_handler: end");

      return nullptr;
    }

    table_handler->m_ref_count = 0;
    table_handler->m_table_name_length = table_name_str.length();
    table_handler->m_table_name = tmp_name;
    strmov(table_handler->m_table_name, table_name);

    m_table_map.emplace(table_name_str, table_handler);

    thr_lock_init(&table_handler->m_thr_lock);
    table_handler->m_io_perf_read.init();
    table_handler->m_io_perf_write.init();
  }
  DBUG_ASSERT(table_handler->m_ref_count >= 0);
  table_handler->m_ref_count++;

  RDB_MUTEX_UNLOCK_CHECK(m_mutex);
  rocksdb_rpc_log(7801, "get_table_handler: end");

  return table_handler;
}

std::vector<std::string> rdb_get_open_table_names(void) {
  rocksdb_rpc_log(7807, "rdb_get_open_table_names: start");
  return rdb_open_tables.get_table_names();
}

std::vector<std::string> Rdb_open_tables_map::get_table_names(void) const {
  rocksdb_rpc_log(7812, "get_table_names: start");
  const Rdb_table_handler *table_handler;
  std::vector<std::string> names;

  RDB_MUTEX_LOCK_CHECK(m_mutex);
  for (const auto &kv : m_table_map) {
    table_handler = kv.second;
    DBUG_ASSERT(table_handler != nullptr);
    names.push_back(table_handler->m_table_name);
  }
  RDB_MUTEX_UNLOCK_CHECK(m_mutex);
  rocksdb_rpc_log(7823, "get_table_names: end");

  return names;
}

/*
  Inspired by innobase_get_int_col_max_value from InnoDB. This returns the
  maximum value a type can take on.
*/
static ulonglong rdb_get_int_col_max_value(const Field *field) {
  rocksdb_rpc_log(7833, "rdb_get_int_col_max_value: start");
  ulonglong max_value = 0;
  switch (field->key_type()) {
    case HA_KEYTYPE_BINARY:
      max_value = 0xFFULL;
      break;
    case HA_KEYTYPE_INT8:
      max_value = 0x7FULL;
      break;
    case HA_KEYTYPE_USHORT_INT:
      max_value = 0xFFFFULL;
      break;
    case HA_KEYTYPE_SHORT_INT:
      max_value = 0x7FFFULL;
      break;
    case HA_KEYTYPE_UINT24:
      max_value = 0xFFFFFFULL;
      break;
    case HA_KEYTYPE_INT24:
      max_value = 0x7FFFFFULL;
      break;
    case HA_KEYTYPE_ULONG_INT:
      max_value = 0xFFFFFFFFULL;
      break;
    case HA_KEYTYPE_LONG_INT:
      max_value = 0x7FFFFFFFULL;
      break;
    case HA_KEYTYPE_ULONGLONG:
      max_value = 0xFFFFFFFFFFFFFFFFULL;
      break;
    case HA_KEYTYPE_LONGLONG:
      max_value = 0x7FFFFFFFFFFFFFFFULL;
      break;
    case HA_KEYTYPE_FLOAT:
      max_value = 0x1000000ULL;
      break;
    case HA_KEYTYPE_DOUBLE:
      max_value = 0x20000000000000ULL;
      break;
    default:
      abort();
  }
  rocksdb_rpc_log(7875, "rdb_get_int_col_max_value: end");
  return max_value;
}

void ha_rocksdb::load_auto_incr_value() {
  rocksdb_rpc_log(7880, "load_auto_incr_value: start");
  ulonglong auto_incr = 0;
  bool validate_last = false, use_datadic = true;
#ifndef DBUG_OFF
  DBUG_EXECUTE_IF("myrocks_autoinc_upgrade", use_datadic = false;);
  validate_last = true;
#endif

  if (use_datadic && dict_manager.get_auto_incr_val(
                         m_tbl_def->get_autoincr_gl_index_id(), &auto_incr)) {
    update_auto_incr_val(auto_incr);
  }

  // If we find nothing in the data dictionary, or if we are in debug mode,
  // then call index_last to get the last value.
  //
  // This is needed when upgrading from a server that did not support
  // persistent auto_increment, of if the table is empty.
  //
  // For debug mode, we are just verifying that the data dictionary value is
  // greater than or equal to the maximum value in the table.
  if (auto_incr == 0 || validate_last) {
    auto_incr = load_auto_incr_value_from_index();
    update_auto_incr_val(auto_incr);
  }

  // If we failed to find anything from the data dictionary and index, then
  // initialize auto_increment to 1.
  if (m_tbl_def->m_auto_incr_val == 0) {
    update_auto_incr_val(1);
  }
  rocksdb_rpc_log(7880, "load_auto_incr_value: end");
}

ulonglong ha_rocksdb::load_auto_incr_value_from_index() {
  rocksdb_rpc_log(7915, "load_auto_incr_value_from_index: start");
  const int save_active_index = active_index;
  active_index = table->s->next_number_index;
  const uint8 save_table_status = table->status;
  ulonglong last_val = 0;

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  const bool is_new_snapshot = !tx->has_snapshot();
  if (is_new_snapshot) {
    tx->acquire_snapshot(true);
  }

  // Do a lookup. We only need index column, so it should be index-only.
  // (another reason to make it index-only is that table->read_set is not set
  // appropriately and non-index-only lookup will not read the value)
  const bool save_keyread_only = m_keyread_only;
  m_keyread_only = true;
  m_converter->set_is_key_requested(true);

  if (!index_last(table->record[0])) {
    Field *field =
        table->key_info[table->s->next_number_index].key_part[0].field;
    ulonglong max_val = rdb_get_int_col_max_value(field);
    my_bitmap_map *const old_map =
        dbug_tmp_use_all_columns(table, table->read_set);
    last_val = field->val_int();
    if (last_val != max_val) {
      last_val++;
    }
#ifndef DBUG_OFF
    ulonglong dd_val;
    if (last_val <= max_val) {
      const auto &gl_index_id = m_tbl_def->get_autoincr_gl_index_id();
      if (dict_manager.get_auto_incr_val(gl_index_id, &dd_val) &&
          tx->get_auto_incr(gl_index_id) == 0) {
        DBUG_ASSERT(dd_val >= last_val);
      }
    }
#endif
    dbug_tmp_restore_column_map(table->read_set, old_map);
  }

  m_keyread_only = save_keyread_only;
  if (is_new_snapshot) {
    tx->release_snapshot();
  }

  table->status = save_table_status;
  active_index = save_active_index;

  /*
    Do what ha_rocksdb::index_end() does.
    (Why don't we use index_init/index_end? class handler defines index_init
    as private, for some reason).
    */
  release_scan_iterator();

  rocksdb_rpc_log(7972, "load_auto_incr_value_from_index: end");
  return last_val;
}

void ha_rocksdb::update_auto_incr_val(ulonglong val) {
  rocksdb_rpc_log(7977, "update_auto_incr_val: start");
  ulonglong auto_incr_val = m_tbl_def->m_auto_incr_val;
  while (
      auto_incr_val < val &&
      !m_tbl_def->m_auto_incr_val.compare_exchange_weak(auto_incr_val, val)) {
    // Do nothing - just loop until auto_incr_val is >= val or we successfully
    // set it
  }
  rocksdb_rpc_log(7985, "update_auto_incr_val: end");
}

void ha_rocksdb::update_auto_incr_val_from_field() {
  rocksdb_rpc_log(7989, "update_auto_incr_val_from_field: start");
  Field *field;
  ulonglong new_val, max_val;
  field = table->key_info[table->s->next_number_index].key_part[0].field;
  max_val = rdb_get_int_col_max_value(field);

  my_bitmap_map *const old_map =
      dbug_tmp_use_all_columns(table, table->read_set);
  new_val = field->val_int();
  // don't increment if we would wrap around
  if (new_val != max_val) {
    new_val++;
  }

  dbug_tmp_restore_column_map(table->read_set, old_map);

  // Only update if positive value was set for auto_incr column.
  if (new_val <= max_val) {
    Rdb_transaction *const tx = get_or_create_tx(table->in_use);
    tx->set_auto_incr(m_tbl_def->get_autoincr_gl_index_id(), new_val);

    // Update the in memory auto_incr value in m_tbl_def.
    update_auto_incr_val(new_val);
  }
  rocksdb_rpc_log(8013, "update_auto_incr_val_from_field: end");
}

int ha_rocksdb::load_hidden_pk_value() {
  rocksdb_rpc_log(8017, "load_hidden_pk_value: start");
  const int save_active_index = active_index;
  active_index = m_tbl_def->m_key_count - 1;
  const uint8 save_table_status = table->status;

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  const bool is_new_snapshot = !tx->has_snapshot();

  longlong hidden_pk_id = 1;
  // Do a lookup.
  if (!index_last(table->record[0])) {
    /*
      Decode PK field from the key
    */
    auto err = read_hidden_pk_id_from_rowkey(&hidden_pk_id);
    if (err) {
      if (is_new_snapshot) {
        tx->release_snapshot();
      }
      return err;
    }

    hidden_pk_id++;
  }

  longlong old = m_tbl_def->m_hidden_pk_val;
  while (old < hidden_pk_id &&
         !m_tbl_def->m_hidden_pk_val.compare_exchange_weak(old, hidden_pk_id)) {
  }

  if (is_new_snapshot) {
    tx->release_snapshot();
  }

  table->status = save_table_status;
  active_index = save_active_index;

  release_scan_iterator();

  rocksdb_rpc_log(8056, "load_hidden_pk_value: end");

  return HA_EXIT_SUCCESS;
}

/* Get PK value from m_tbl_def->m_hidden_pk_info. */
longlong ha_rocksdb::update_hidden_pk_val() {
  rocksdb_rpc_log(8063, "update_hidden_pk_val: start");
  DBUG_ASSERT(has_hidden_pk(table));
  const longlong new_val = m_tbl_def->m_hidden_pk_val++;
  rocksdb_rpc_log(8066, "update_hidden_pk_val: end");
  return new_val;
}

/* Get the id of the hidden pk id from m_last_rowkey */
int ha_rocksdb::read_hidden_pk_id_from_rowkey(longlong *const hidden_pk_id) {
  rocksdb_rpc_log(8072, "read_hidden_pk_id_from_rowkey: start");

  DBUG_ASSERT(table != nullptr);
  DBUG_ASSERT(has_hidden_pk(table));

  rocksdb::Slice rowkey_slice(m_last_rowkey.ptr(), m_last_rowkey.length());

  // Get hidden primary key from old key slice
  Rdb_string_reader reader(&rowkey_slice);
  if ((!reader.read(Rdb_key_def::INDEX_NUMBER_SIZE))) {
    rocksdb_rpc_log(8082, "read_hidden_pk_id_from_rowkey: start");
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  const int length = Field_longlong::PACK_LENGTH;
  const uchar *from = reinterpret_cast<const uchar *>(reader.read(length));
  if (from == nullptr) {
    /* Mem-comparable image doesn't have enough bytes */
    rocksdb_rpc_log(8089, "read_hidden_pk_id_from_rowkey: start");
    return HA_ERR_ROCKSDB_CORRUPT_DATA;
  }

  *hidden_pk_id = rdb_netbuf_read_uint64(&from);
  rocksdb_rpc_log(8093, "read_hidden_pk_id_from_rowkey: start");

  return HA_EXIT_SUCCESS;
}

/**
  @brief
  Free lock controls. We call this whenever we close a table. If the table had
  the last reference to the table_handler, then we free the memory associated
  with it.
*/

void Rdb_open_tables_map::release_table_handler(
    Rdb_table_handler *const table_handler) {
  rocksdb_rpc_log(8109, "release_table_handler: start");

  RDB_MUTEX_LOCK_CHECK(m_mutex);

  DBUG_ASSERT(table_handler != nullptr);
  DBUG_ASSERT(table_handler->m_ref_count > 0);
  if (!--table_handler->m_ref_count) {
    // Last rereference was released. Tear down the hash entry.
    const auto ret MY_ATTRIBUTE((__unused__)) =
        m_table_map.erase(std::string(table_handler->m_table_name));
    DBUG_ASSERT(ret == 1);  // the hash entry must actually be found and deleted
    my_core::thr_lock_delete(&table_handler->m_thr_lock);
    my_free(table_handler);
  }

  RDB_MUTEX_UNLOCK_CHECK(m_mutex);
  rocksdb_rpc_log(8125, "release_table_handler: end");
}

static handler *rocksdb_create_handler(my_core::handlerton *const hton,
                                       my_core::TABLE_SHARE *const table_arg,
                                       my_core::MEM_ROOT *const mem_root) {
  rocksdb_rpc_log(8131, "rocksdb_create_handler: start");
  return new (mem_root) ha_rocksdb(hton, table_arg);
}

ha_rocksdb::ha_rocksdb(my_core::handlerton *const hton,
                       my_core::TABLE_SHARE *const table_arg)
    : handler(hton, table_arg),
      m_table_handler(nullptr),
      m_scan_it(nullptr),
      m_scan_it_skips_bloom(false),
      m_scan_it_snapshot(nullptr),
      m_scan_it_lower_bound(nullptr),
      m_scan_it_upper_bound(nullptr),
      m_tbl_def(nullptr),
      m_pk_descr(nullptr),
      m_key_descr_arr(nullptr),
      m_pk_can_be_decoded(false),
      m_pk_tuple(nullptr),
      m_pk_packed_tuple(nullptr),
      m_sk_packed_tuple(nullptr),
      m_end_key_packed_tuple(nullptr),
      m_sk_match_prefix(nullptr),
      m_sk_match_prefix_buf(nullptr),
      m_sk_packed_tuple_old(nullptr),
      m_dup_sk_packed_tuple(nullptr),
      m_dup_sk_packed_tuple_old(nullptr),
      m_pack_buffer(nullptr),
      m_lock_rows(RDB_LOCK_NONE),
      m_keyread_only(false),
      m_insert_with_update(false),
      m_dup_key_found(false),
      mrr_rowid_reader(nullptr),
      mrr_n_elements(0),
      mrr_enabled_keyread(false),
      mrr_used_cpk(false),
      m_in_rpl_delete_rows(false),
      m_in_rpl_update_rows(false),
      m_force_skip_unique_check(false),
      m_need_build_decoder(false) {}

ha_rocksdb::~ha_rocksdb() {
  rocksdb_rpc_log(8172, "ha_rocksdb: start");
  int err MY_ATTRIBUTE((__unused__));
  err = finalize_bulk_load(false);
  if (err != 0) {
    // NO_LINT_DEBUG
    sql_print_error(
        "RocksDB: Error %d finalizing bulk load while closing "
        "handler.",
        err);
  }
}

static const char *ha_rocksdb_exts[] = {NullS};

const char **ha_rocksdb::bas_ext() const {
  rocksdb_rpc_log(8187, "bas_ext: start");
  DBUG_ENTER_FUNC();

  DBUG_RETURN(ha_rocksdb_exts);
  rocksdb_rpc_log(8191, "bas_ext: end");
}

const std::string &ha_rocksdb::get_table_basename() const {
  rocksdb_rpc_log(8195, "get_table_basename: start");
  return m_tbl_def->base_tablename();
}

/**
  @return
    false  OK
    other  Error inpacking the data
*/
bool ha_rocksdb::init_with_fields() {
  rocksdb_rpc_log(8205, "init_with_fields: start");
  DBUG_ENTER_FUNC();

  const uint pk = table_share->primary_key;
  if (pk != MAX_KEY) {
    const uint key_parts = table_share->key_info[pk].user_defined_key_parts;
    check_keyread_allowed(pk /*PK*/, key_parts - 1, true);
  } else {
    m_pk_can_be_decoded = false;
  }
  cached_table_flags = table_flags();

  DBUG_RETURN(false); /* Ok */
  rocksdb_rpc_log(8218, "init_with_fields: end");
}

/*
  If the key is a TTL key, we may need to filter it out.

  The purpose of read filtering for tables with TTL is to ensure that
  during a transaction a key which has expired already but not removed by
  compaction yet is not returned to the user.

  Without this the user might be hit with problems such as disappearing
  rows within a transaction, etc, because the compaction filter ignores
  snapshots when filtering keys.
*/
bool ha_rocksdb::should_hide_ttl_rec(const Rdb_key_def &kd,
                                     const rocksdb::Slice &ttl_rec_val,
                                     const int64_t curr_ts) {
  rocksdb_rpc_log(8235, "should_hide_ttl_rec: start");
  DBUG_ASSERT(kd.has_ttl());
  DBUG_ASSERT(kd.m_ttl_rec_offset != UINT_MAX);

  /*
    Curr_ts can only be 0 if there are no snapshots open.
    should_hide_ttl_rec can only be called when there is >=1 snapshots, unless
    we are filtering on the write path (single INSERT/UPDATE) in which case
    we are passed in the current time as curr_ts.

    In the event curr_ts is 0, we always decide not to filter the record. We
    also log a warning and increment a diagnostic counter.
  */
  if (curr_ts == 0) {
    update_row_stats(ROWS_HIDDEN_NO_SNAPSHOT);
    rocksdb_rpc_log(8250, "should_hide_ttl_rec: end");
    return false;
  }

  if (!rdb_is_ttl_read_filtering_enabled() || !rdb_is_ttl_enabled()) {
    rocksdb_rpc_log(8255, "should_hide_ttl_rec: end");
    return false;
  }
  rocksdb_rpc_log(8256, "should_hide_ttl_rec: init reader");

  Rdb_string_reader reader(&ttl_rec_val);

  /*
    Find where the 8-byte ttl is for each record in this index.
  */
  uint64 ts;
  if (!reader.read(kd.m_ttl_rec_offset) || reader.read_uint64(&ts)) {
    /*
      This condition should never be reached since all TTL records have an
      8 byte ttl field in front. Don't filter the record out, and log an error.
    */
    std::string buf;
    buf = rdb_hexdump(ttl_rec_val.data(), ttl_rec_val.size(),
                      RDB_MAX_HEXDUMP_LEN);
    const GL_INDEX_ID gl_index_id = kd.get_gl_index_id();
    // NO_LINT_DEBUG
    sql_print_error(
        "Decoding ttl from PK value failed, "
        "for index (%u,%u), val: %s",
        gl_index_id.cf_id, gl_index_id.index_id, buf.c_str());
    DBUG_ASSERT(0);
    rocksdb_rpc_log(8281, "should_hide_ttl_rec: end");
    return false;
  }

  /* Hide record if it has expired before the current snapshot time. */
  uint64 read_filter_ts = 0;
#ifndef DBUG_OFF
  read_filter_ts += rdb_dbug_set_ttl_read_filter_ts();
#endif
  bool is_hide_ttl =
      ts + kd.m_ttl_duration + read_filter_ts <= static_cast<uint64>(curr_ts);
  if (is_hide_ttl) {
    update_row_stats(ROWS_FILTERED);

    /* increment examined row count when rows are skipped */
    THD *thd = ha_thd();
    thd->inc_examined_row_count(1);
    DEBUG_SYNC(thd, "rocksdb.ttl_rows_examined");
  }
  rocksdb_rpc_log(8300, "should_hide_ttl_rec: end");
  return is_hide_ttl;
}

int ha_rocksdb::rocksdb_skip_expired_records(const Rdb_key_def &kd,
                                             rocksdb::Iterator *const iter,
                                             bool seek_backward) {
  rocksdb_rpc_log(8307, "rocksdb_skip_expired_records: start");
  if (kd.has_ttl()) {
    THD *thd = ha_thd();

    rocksdb_rpc_log(8311,
                    "rocksdb_skip_expired_records: rocksdb_Iterator__Valid");
    // ALTER
    // while (iter->Valid() &&
    while (rocksdb_Iterator__Valid(iter) &&
           should_hide_ttl_rec(
               kd, rocksdb_Iterator__value(iter),
               get_or_create_tx(table->in_use)->m_snapshot_timestamp)) {
      DEBUG_SYNC(thd, "rocksdb.check_flags_ser");
      if (thd && thd->killed) {
        rocksdb_rpc_log(8322, "rocksdb_skip_expired_records: end");
        return HA_ERR_QUERY_INTERRUPTED;
      }
      rocksdb_smart_next(seek_backward, iter);
    }
  }
  rocksdb_rpc_log(8329, "rocksdb_skip_expired_records: end");
  return HA_EXIT_SUCCESS;
}

#ifndef DBUG_OFF
void dbug_append_garbage_at_end(rocksdb::PinnableSlice *on_disk_rec) {
  // ALTER
  // std::string str(on_disk_rec->data(), on_disk_rec->size());
  // on_disk_rec->Reset();
  // str.append("abc");
  // on_disk_rec->PinSelf(rocksdb::Slice(str));
  rocksdb_rpc_log(8341, "dbug_append_garbage_at_end: start");
  rocksdb_rpc_log(8342,
                  "dbug_append_garbage_at_end: rocksdb_PinnableSlice__data "
                  "rocksdb_PinnableSlice__size");
  std::string str(rocksdb_PinnableSlice__data(on_disk_rec),
                  rocksdb_PinnableSlice__size(on_disk_rec));
  rocksdb_rpc_log(8343,
                  "dbug_append_garbage_at_end: rocksdb_PinnableSlice__Reset");
  rocksdb_PinnableSlice__Reset(on_disk_rec);
  str.append("abc");
  rocksdb_rpc_log(8346,
                  "dbug_append_garbage_at_end: rocksdb_PinnableSlice__PinSelf");
  rocksdb_PinnableSlice__PinSelf(on_disk_rec, rocksdb::Slice(str));
  rocksdb_rpc_log(8347, "dbug_append_garbage_at_end: end");
}

void dbug_truncate_record(rocksdb::PinnableSlice *on_disk_rec) {
  // on_disk_rec->remove_suffix(on_disk_rec->size());
  rocksdb_rpc_log(8352,
                  "dbug_truncate_record: rocksdb_PinnableSlice__remove_suffix");
  rocksdb_PinnableSlice__remove_suffix(
      on_disk_rec, rocksdb_PinnableSlice__size(on_disk_rec));
  rocksdb_rpc_log(8352, "dbug_truncate_record: end");
}

void dbug_modify_rec_varchar12(rocksdb::PinnableSlice *on_disk_rec) {
  rocksdb_rpc_log(8359, "dbug_modify_rec_varchar12: start");
  std::string res;
  // The record is NULL-byte followed by VARCHAR(10).
  // Put the NULL-byte
  res.append("\0", 1);
  // Then, add a valid VARCHAR(12) value.
  res.append("\xC", 1);
  res.append("123456789ab", 12);

  // on_disk_rec->Reset();
  // on_disk_rec->PinSelf(rocksdb::Slice(res));
  rocksdb_rpc_log(8371,
                  "dbug_modify_rec_varchar12: rocksdb_PinnableSlice__Reset "
                  "rocksdb_PinnableSlice__PinSelf");
  rocksdb_PinnableSlice__Reset(on_disk_rec);
  rocksdb_PinnableSlice__PinSelf(on_disk_rec, rocksdb::Slice(res));
}

void dbug_create_err_inplace_alter() {
  my_printf_error(ER_UNKNOWN_ERROR,
                  "Intentional failure in inplace alter occurred.", MYF(0));
}
#endif

int ha_rocksdb::convert_record_from_storage_format(
    const rocksdb::Slice *const key, uchar *const buf) {
  rocksdb_rpc_log(8383, "convert_record_from_storage_format: start");

  DBUG_EXECUTE_IF("myrocks_simulate_bad_row_read1",
                  dbug_append_garbage_at_end(&m_retrieved_record););
  DBUG_EXECUTE_IF("myrocks_simulate_bad_row_read2",
                  dbug_truncate_record(&m_retrieved_record););
  DBUG_EXECUTE_IF("myrocks_simulate_bad_row_read3",
                  dbug_modify_rec_varchar12(&m_retrieved_record););

  // ALTER
  // return convert_record_from_storage_format(key, &m_retrieved_record, buf);
  rocksdb_rpc_log(
      8394,
      "convert_record_from_storage_format: convert_record_from_storage_format");
  // ALTER
  rocksdb::Slice s = rocksdb_PinnableSlice__Slice(m_retrieved_record);
  return convert_record_from_storage_format(key, &s, buf);
}

/*
  @brief
  Unpack the record in this->m_retrieved_record and this->m_last_rowkey from
  storage format into buf (which can be table->record[0] or table->record[1]).

  @param  key   Table record's key in mem-comparable form.
  @param  buf   Store record in table->record[0] format here

  @detail
    If the table has blobs, the unpacked data in buf may keep pointers to the
    data in this->m_retrieved_record.

    The key is only needed to check its checksum value (the checksum is in
    m_retrieved_record).

  @seealso
    rdb_converter::setup_read_decoders()  Sets up data structures which tell
  which columns to decode.

  @return
    0      OK
    other  Error inpacking the data
*/

int ha_rocksdb::convert_record_from_storage_format(
    const rocksdb::Slice *const key, const rocksdb::Slice *const value,
    uchar *const buf) {
  rocksdb_rpc_log(8425, "convert_record_from_storage_format: start");
  return m_converter->decode(m_pk_descr, buf, key, value);
}

int ha_rocksdb::alloc_key_buffers(const TABLE *const table_arg,
                                  const Rdb_tbl_def *const tbl_def_arg,
                                  bool alloc_alter_buffers) {
  rocksdb_rpc_log(8433, "alloc_key_buffers: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(m_pk_tuple == nullptr);

  std::shared_ptr<Rdb_key_def> *const kd_arr = tbl_def_arg->m_key_descr_arr;

  uint key_len = 0;
  uint max_packed_sk_len = 0;
  uint pack_key_len = 0;

  m_pk_descr = kd_arr[pk_index(table_arg, tbl_def_arg)];
  if (has_hidden_pk(table_arg)) {
    m_pk_key_parts = 1;
  } else {
    m_pk_key_parts =
        table->key_info[table->s->primary_key].user_defined_key_parts;
    key_len = table->key_info[table->s->primary_key].key_length;
  }

  // move this into get_table_handler() ??
  m_pk_descr->setup(table_arg, tbl_def_arg);

  m_pk_tuple = reinterpret_cast<uchar *>(my_malloc(key_len, MYF(0)));

  pack_key_len = m_pk_descr->max_storage_fmt_length();
  m_pk_packed_tuple =
      reinterpret_cast<uchar *>(my_malloc(pack_key_len, MYF(0)));

  /* Sometimes, we may use m_sk_packed_tuple for storing packed PK */
  max_packed_sk_len = pack_key_len;
  for (uint i = 0; i < table_arg->s->keys; i++) {
    /* Primary key was processed above */
    if (i == table_arg->s->primary_key) continue;

    // TODO: move this into get_table_handler() ??
    kd_arr[i]->setup(table_arg, tbl_def_arg);

    const uint packed_len = kd_arr[i]->max_storage_fmt_length();
    if (packed_len > max_packed_sk_len) {
      max_packed_sk_len = packed_len;
    }
  }

  m_sk_packed_tuple =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
  m_sk_match_prefix_buf =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
  m_sk_packed_tuple_old =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
  m_end_key_packed_tuple =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
  m_pack_buffer =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));

  m_scan_it_lower_bound =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
  m_scan_it_upper_bound =
      reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));

  /*
    If inplace alter is happening, allocate special buffers for unique
    secondary index duplicate checking.
  */
  if (alloc_alter_buffers) {
    m_dup_sk_packed_tuple =
        reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
    m_dup_sk_packed_tuple_old =
        reinterpret_cast<uchar *>(my_malloc(max_packed_sk_len, MYF(0)));
  }

  if (m_pk_tuple == nullptr || m_pk_packed_tuple == nullptr ||
      m_sk_packed_tuple == nullptr || m_sk_packed_tuple_old == nullptr ||
      m_end_key_packed_tuple == nullptr || m_pack_buffer == nullptr ||
      m_scan_it_upper_bound == nullptr || m_scan_it_lower_bound == nullptr ||
      (alloc_alter_buffers && (m_dup_sk_packed_tuple == nullptr ||
                               m_dup_sk_packed_tuple_old == nullptr))) {
    // One or more of the above allocations failed.  Clean up and exit
    free_key_buffers();
    rocksdb_rpc_log(8513, "alloc_key_buffers: end");
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  rocksdb_rpc_log(8516, "alloc_key_buffers: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

void ha_rocksdb::free_key_buffers() {
  rocksdb_rpc_log(8521, "free_key_buffers: start");

  my_free(m_pk_tuple);
  m_pk_tuple = nullptr;

  my_free(m_pk_packed_tuple);
  m_pk_packed_tuple = nullptr;

  my_free(m_sk_packed_tuple);
  m_sk_packed_tuple = nullptr;

  my_free(m_sk_match_prefix_buf);
  m_sk_match_prefix_buf = nullptr;

  my_free(m_sk_packed_tuple_old);
  m_sk_packed_tuple_old = nullptr;

  my_free(m_end_key_packed_tuple);
  m_end_key_packed_tuple = nullptr;

  my_free(m_pack_buffer);
  m_pack_buffer = nullptr;

  my_free(m_dup_sk_packed_tuple);
  m_dup_sk_packed_tuple = nullptr;

  my_free(m_dup_sk_packed_tuple_old);
  m_dup_sk_packed_tuple_old = nullptr;

  my_free(m_scan_it_lower_bound);
  m_scan_it_lower_bound = nullptr;

  my_free(m_scan_it_upper_bound);
  m_scan_it_upper_bound = nullptr;
}

void ha_rocksdb::set_skip_unique_check_tables(const char *const whitelist) {
  rocksdb_rpc_log(8558, "set_skip_unique_check_tables: start");
  const char *const wl =
      whitelist ? whitelist : DEFAULT_SKIP_UNIQUE_CHECK_TABLES;

#if defined(HAVE_PSI_INTERFACE)
  Regex_list_handler regex_handler(key_rwlock_skip_unique_check_tables);
#else
  Regex_list_handler regex_handler;
#endif

  if (!regex_handler.set_patterns(wl)) {
    warn_about_bad_patterns(&regex_handler, "skip_unique_check_tables");
  }

  m_skip_unique_check = regex_handler.matches(m_tbl_def->base_tablename());
  rocksdb_rpc_log(8573, "set_skip_unique_check_tables: end");
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::open(const char *const name, int mode, uint test_if_locked) {
  rocksdb_rpc_log(8582, "open: start");

  DBUG_ENTER_FUNC();

  int err = close();
  if (err) {
    DBUG_RETURN(err);
  }

  rocksdb_rpc_log(8591, "open: rdb_open_tables.get_table_handler");
  m_table_handler = rdb_open_tables.get_table_handler(name);

  if (m_table_handler == nullptr) {
    rocksdb_rpc_log(8595, "open: end");
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }

  rocksdb_rpc_log(8601, "open: thr_lock_data_init");

  my_core::thr_lock_data_init(&m_table_handler->m_thr_lock, &m_db_lock,
                              nullptr);
  m_io_perf.init(&m_table_handler->m_table_perf_context,
                 &m_table_handler->m_io_perf_read,
                 &m_table_handler->m_io_perf_write, &stats);
  Rdb_perf_context_guard guard(&m_io_perf,
                               rocksdb_perf_context_level(ha_thd()));

  rocksdb_rpc_log(8617, "open: rdb_normalize_tablename");

  std::string fullname;
  err = rdb_normalize_tablename(name, &fullname);
  if (err != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(8617, "open: failed");
    DBUG_RETURN(err);
  }

  m_tbl_def = ddl_manager.find(fullname);
  if (m_tbl_def == nullptr) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Attempt to open a table that is not present in RocksDB-SE data "
             "dictionary");
    rocksdb_rpc_log(8632, "open: failed");
    DBUG_RETURN(HA_ERR_ROCKSDB_INVALID_TABLE);
  }

  m_lock_rows = RDB_LOCK_NONE;
  m_key_descr_arr = m_tbl_def->m_key_descr_arr;

  /*
    Full table scan actually uses primary key
    (UPDATE needs to know this, otherwise it will go into infinite loop on
    queries like "UPDATE tbl SET pk=pk+100")
  */
  key_used_on_scan = table->s->primary_key;

  rocksdb_rpc_log(8645, "open: get primary_key; alloc_key_buffers");
  // close() above has already called free_key_buffers(). No need to do it here.
  err = alloc_key_buffers(table, m_tbl_def);

  if (err) {
    rocksdb_rpc_log(8651, "open: failed");
    DBUG_RETURN(err);
  }

  /*
    init_with_fields() is used to initialize table flags based on the field
    definitions in table->field[].
    It is called by open_binary_frm(), but that function calls the method for
    a temporary ha_rocksdb object which is later destroyed.

    If we are here in ::open(), then init_with_fields() has not been called
    for this object. Call it ourselves, we want all member variables to be
    properly initialized.
  */
  init_with_fields();

  /* Initialize decoder */
  rocksdb_rpc_log(8668, "open: m_converter.reset");
  m_converter.reset(new Rdb_converter(ha_thd(), m_tbl_def, table));

  /*
     Update m_ttl_bytes address to same as Rdb_converter's m_ttl_bytes.
     Remove this code after moving convert_record_to_storage_format() into
     Rdb_converter class.
  */
  rocksdb_rpc_log(8675, "open:m_converter->get_ttl_bytes_buffer");
  m_ttl_bytes = m_converter->get_ttl_bytes_buffer();

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

  /*
    The following load_XXX code calls row decode functions, and they do
    that without having done ::external_lock() or index_init()/rnd_init().
    (Note: this also means we're doing a read when there was no
    rdb_converter::setup_field_encoders() call)

    Initialize the necessary variables for them:
  */

  /* Load auto_increment value only once on first use. */
  if (table->found_next_number_field && m_tbl_def->m_auto_incr_val == 0) {
    load_auto_incr_value();
  }

  /* Load hidden pk only once on first use. */
  if (has_hidden_pk(table) && m_tbl_def->m_hidden_pk_val == 0 &&
      (err = load_hidden_pk_value()) != HA_EXIT_SUCCESS) {
    free_key_buffers();
    rocksdb_rpc_log(8698, "open:m_converter->get_ttl_bytes_buffer");

    DBUG_RETURN(err);
  }

  /* Index block size in MyRocks: used by MySQL in query optimization */

  rocksdb_rpc_log(8705,
                  "open: rocksdb_BlockBasedTableOptions__GetSizeTOptions");
  // ALTER
  // stats.block_size = rocksdb_tbl_options->block_size;
  stats.block_size = rocksdb_BlockBasedTableOptions__GetSizeTOptions(
      rocksdb_tbl_options, "block_size");
  /* Determine at open whether we should skip unique checks for this table */
  set_skip_unique_check_tables(THDVAR(ha_thd(), skip_unique_check_tables));

  rocksdb_rpc_log(8713, "open: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

int ha_rocksdb::close(void) {
  DBUG_ENTER_FUNC();
  rocksdb_rpc_log(8719, "close: start");

  m_pk_descr = nullptr;
  m_key_descr_arr = nullptr;
  m_converter = nullptr;
  free_key_buffers();

  if (m_table_handler != nullptr) {
    rdb_open_tables.release_table_handler(m_table_handler);
    m_table_handler = nullptr;
  }

  // These are needed to suppress valgrind errors in rocksdb.partition
  m_last_rowkey.free();
  m_sk_tails.free();
  m_sk_tails_old.free();
  m_pk_unpack_info.free();

  rocksdb_rpc_log(8737, "close: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

rpc_logger l_25(8742, "init rdb_error_messages");
static const char *rdb_error_messages[] = {
    "Table must have a PRIMARY KEY.",
    "Specifying DATA DIRECTORY for an individual table is not supported.",
    "Specifying INDEX DIRECTORY for an individual table is not supported.",
    "RocksDB commit failed.",
    "Failure during bulk load operation.",
    "Found data corruption.",
    "CRC checksum mismatch.",
    "Invalid table.",
    "Could not access RocksDB properties.",
    "File I/O error during merge/sort operation.",
    "RocksDB status: not found.",
    "RocksDB status: corruption.",
    "RocksDB status: not supported.",
    "RocksDB status: invalid argument.",
    "RocksDB status: io error.",
    "RocksDB status: no space.",
    "RocksDB status: merge in progress.",
    "RocksDB status: incomplete.",
    "RocksDB status: shutdown in progress.",
    "RocksDB status: timed out.",
    "RocksDB status: aborted.",
    "RocksDB status: lock limit reached.",
    "RocksDB status: busy.",
    "RocksDB status: deadlock.",
    "RocksDB status: expired.",
    "RocksDB status: try again.",
};

rpc_logger l_26(8772, "static_assert");

static_assert((sizeof(rdb_error_messages) / sizeof(rdb_error_messages[0])) ==
                  ((HA_ERR_ROCKSDB_LAST - HA_ERR_ROCKSDB_FIRST) + 1),
              "Number of error messages doesn't match number of error codes");

static const char *rdb_get_error_message(int nr) {
  return rdb_error_messages[nr - HA_ERR_ROCKSDB_FIRST];
}

bool ha_rocksdb::get_error_message(const int error, String *const buf) {
  DBUG_ENTER_FUNC();
  rocksdb_rpc_log(8782, "get_error_message: start");

  static_assert(HA_ERR_ROCKSDB_LAST > HA_ERR_FIRST,
                "HA_ERR_ROCKSDB_LAST > HA_ERR_FIRST");
  static_assert(HA_ERR_ROCKSDB_LAST > HA_ERR_LAST,
                "HA_ERR_ROCKSDB_LAST > HA_ERR_LAST");

  if (error == HA_ERR_LOCK_WAIT_TIMEOUT || error == HA_ERR_LOCK_DEADLOCK ||
      error == HA_ERR_ROCKSDB_STATUS_BUSY) {
    Rdb_transaction *const tx = get_tx_from_thd(ha_thd());
    DBUG_ASSERT(tx != nullptr);
    buf->append(tx->m_detailed_error);
    rocksdb_rpc_log(8794, "get_error_message: end");
    DBUG_RETURN(true);
  }

  if (error >= HA_ERR_ROCKSDB_FIRST && error <= HA_ERR_ROCKSDB_LAST) {
    buf->append(rdb_error_messages[error - HA_ERR_ROCKSDB_FIRST]);
  }

  // We can be called with the values which are < HA_ERR_FIRST because most
  // MySQL internal functions will just return HA_EXIT_FAILURE in case of
  // an error.

  rocksdb_rpc_log(8806, "get_error_message: end");
  DBUG_RETURN(false);
}

/*
  Generalized way to convert RocksDB status errors into MySQL error code, and
  print error message.

  Each error code below maps to a RocksDB status code found in:
  rocksdb/include/rocksdb/status.h
*/
int ha_rocksdb::rdb_error_to_mysql(const rocksdb::Status &s,
                                   const char *opt_msg) {
  rocksdb_rpc_log(8821, "rdb_error_to_mysql: start");
  DBUG_ASSERT(!s.ok());

  int err;
  switch (s.code()) {
    case rocksdb::Status::Code::kOk:
      err = HA_EXIT_SUCCESS;
      break;
    case rocksdb::Status::Code::kNotFound:
      err = HA_ERR_ROCKSDB_STATUS_NOT_FOUND;
      break;
    case rocksdb::Status::Code::kCorruption:
      err = HA_ERR_ROCKSDB_STATUS_CORRUPTION;
      break;
    case rocksdb::Status::Code::kNotSupported:
      err = HA_ERR_ROCKSDB_STATUS_NOT_SUPPORTED;
      break;
    case rocksdb::Status::Code::kInvalidArgument:
      err = HA_ERR_ROCKSDB_STATUS_INVALID_ARGUMENT;
      break;
    case rocksdb::Status::Code::kIOError:
      err = (s.IsNoSpace()) ? HA_ERR_ROCKSDB_STATUS_NO_SPACE
                            : HA_ERR_ROCKSDB_STATUS_IO_ERROR;
      break;
    case rocksdb::Status::Code::kMergeInProgress:
      err = HA_ERR_ROCKSDB_STATUS_MERGE_IN_PROGRESS;
      break;
    case rocksdb::Status::Code::kIncomplete:
      err = HA_ERR_ROCKSDB_STATUS_INCOMPLETE;
      break;
    case rocksdb::Status::Code::kShutdownInProgress:
      err = HA_ERR_ROCKSDB_STATUS_SHUTDOWN_IN_PROGRESS;
      break;
    case rocksdb::Status::Code::kTimedOut:
      err = HA_ERR_ROCKSDB_STATUS_TIMED_OUT;
      break;
    case rocksdb::Status::Code::kAborted:
      err = (s.IsLockLimit()) ? HA_ERR_ROCKSDB_STATUS_LOCK_LIMIT
                              : HA_ERR_ROCKSDB_STATUS_ABORTED;
      break;
    case rocksdb::Status::Code::kBusy:
      err = (s.IsDeadlock()) ? HA_ERR_ROCKSDB_STATUS_DEADLOCK
                             : HA_ERR_ROCKSDB_STATUS_BUSY;
      break;
    case rocksdb::Status::Code::kExpired:
      err = HA_ERR_ROCKSDB_STATUS_EXPIRED;
      break;
    case rocksdb::Status::Code::kTryAgain:
      err = HA_ERR_ROCKSDB_STATUS_TRY_AGAIN;
      break;
    default:
      DBUG_ASSERT(0);
      return -1;
  }

  std::string errMsg;
  if (s.IsLockLimit()) {
    errMsg =
        "Operation aborted: Failed to acquire lock due to "
        "rocksdb_max_row_locks limit";
  } else {
    errMsg = s.ToString();
  }

  if (opt_msg) {
    std::string concatenated_error = errMsg + " (" + std::string(opt_msg) + ")";
    my_error(ER_GET_ERRMSG, MYF(0), s.code(), concatenated_error.c_str(),
             rocksdb_hton_name);
  } else {
    my_error(ER_GET_ERRMSG, MYF(0), s.code(), errMsg.c_str(),
             rocksdb_hton_name);
  }

  rocksdb_rpc_log(8893, "rdb_error_to_mysql: end");
  return err;
}

rpc_logger l_28(8892, "RDB_INDEX_COLLATIONS");

/* MyRocks supports only the following collations for indexed columns */
static const std::set<const my_core::CHARSET_INFO *> RDB_INDEX_COLLATIONS = {
    &my_charset_bin, &my_charset_utf8_bin, &my_charset_latin1_bin};

static bool rdb_is_index_collation_supported(
    const my_core::Field *const field) {
  rocksdb_rpc_log(8903, "rdb_is_index_collation_supported: start");
  const my_core::enum_field_types type = field->real_type();
  /* Handle [VAR](CHAR|BINARY) or TEXT|BLOB */
  if (type == MYSQL_TYPE_VARCHAR || type == MYSQL_TYPE_STRING ||
      type == MYSQL_TYPE_BLOB) {
    rocksdb_rpc_log(8908, "rdb_is_index_collation_supported: end");
    return RDB_INDEX_COLLATIONS.find(field->charset()) !=
           RDB_INDEX_COLLATIONS.end();
  }
  rocksdb_rpc_log(8912, "rdb_is_index_collation_supported: end");
  return true;
}

/*
  Create structures needed for storing data in rocksdb. This is called when the
  table is created. The structures will be shared by all TABLE* objects.

  @param
    table_arg        Table with definition
    db_table         "dbname.tablename"
    len              strlen of the above
    tbl_def_arg      tbl_def whose key_descr is being created/populated
    old_tbl_def_arg  tbl_def from which keys are being copied over from
                     (for use during inplace alter)

  @return
    0      - Ok
    other  - error, either given table ddl is not supported by rocksdb or OOM.
*/
int ha_rocksdb::create_key_defs(
    const TABLE *const table_arg, Rdb_tbl_def *const tbl_def_arg,
    const TABLE *const old_table_arg /* = nullptr */,
    const Rdb_tbl_def *const old_tbl_def_arg
    /* = nullptr */) const {
  rocksdb_rpc_log(8936, "create_key_defs: start");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(table_arg->s != nullptr);

  DBUG_EXECUTE_IF("rocksdb_truncate_failure", {
    my_error(ER_INTERNAL_ERROR, MYF(0), "Simulated truncation failure.");
    DBUG_RETURN(HA_EXIT_FAILURE);
  });

  DBUG_EXECUTE_IF("rocksdb_truncate_failure_crash", DBUG_SUICIDE(););

  /*
    These need to be one greater than MAX_INDEXES since the user can create
    MAX_INDEXES secondary keys and no primary key which would cause us
    to generate a hidden one.
  */
  std::array<key_def_cf_info, MAX_INDEXES + 1> cfs;

  /*
    NOTE: All new column families must be created before new index numbers are
    allocated to each key definition. See below for more details.
    http://github.com/MySQLOnRocksDB/mysql-5.6/issues/86#issuecomment-138515501
  */
  if (create_cfs(table_arg, tbl_def_arg, &cfs)) {
    rocksdb_rpc_log(8961, "create_key_defs: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  uint64 ttl_duration = 0;
  std::string ttl_column;
  uint ttl_field_offset;

  uint err;
  if ((err = Rdb_key_def::extract_ttl_duration(table_arg, tbl_def_arg,
                                               &ttl_duration))) {
    rocksdb_rpc_log(8972, "create_key_defs: end");
    DBUG_RETURN(err);
  }

  if ((err = Rdb_key_def::extract_ttl_col(table_arg, tbl_def_arg, &ttl_column,
                                          &ttl_field_offset))) {
    rocksdb_rpc_log(8978, "create_key_defs: end");
    DBUG_RETURN(err);
  }

  /* We don't currently support TTL on tables with hidden primary keys. */
  if (ttl_duration > 0 && has_hidden_pk(table_arg)) {
    my_error(ER_RDB_TTL_UNSUPPORTED, MYF(0));
    rocksdb_rpc_log(8986, "create_key_defs: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  /*
    If TTL duration is not specified but TTL column was specified, throw an
    error because TTL column requires duration.
  */
  if (ttl_duration == 0 && !ttl_column.empty()) {
    my_error(ER_RDB_TTL_COL_FORMAT, MYF(0), ttl_column.c_str());
    rocksdb_rpc_log(8995, "create_key_defs: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  if (!old_tbl_def_arg) {
    /*
      old_tbl_def doesn't exist. this means we are in the process of creating
      a new table.

      Get the index numbers (this will update the next_index_number)
      and create Rdb_key_def structures.
    */
    for (uint i = 0; i < tbl_def_arg->m_key_count; i++) {
      if (create_key_def(table_arg, i, tbl_def_arg, &m_key_descr_arr[i], cfs[i],
                         ttl_duration, ttl_column)) {
        rocksdb_rpc_log(9010, "create_key_defs: end");
        DBUG_RETURN(HA_EXIT_FAILURE);
      }
    }
  } else {
    /*
      old_tbl_def exists.  This means we are creating a new tbl_def as part of
      in-place alter table.  Copy over existing keys from the old_tbl_def and
      generate the necessary new key definitions if any.
    */
    if (create_inplace_key_defs(table_arg, tbl_def_arg, old_table_arg,
                                old_tbl_def_arg, cfs, ttl_duration,
                                ttl_column)) {
      rocksdb_rpc_log(9024, "create_key_defs: end");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }
  }
  rocksdb_rpc_log(9027, "create_key_defs: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
  Checks index parameters and creates column families needed for storing data
  in rocksdb if necessary.

  @param in
    table_arg     Table with definition
    db_table      Table name
    tbl_def_arg   Table def structure being populated

  @param out
    cfs           CF info for each key definition in 'key_info' order

  @return
    0      - Ok
    other  - error
*/
int ha_rocksdb::create_cfs(
    const TABLE *const table_arg, Rdb_tbl_def *const tbl_def_arg,
    std::array<struct key_def_cf_info, MAX_INDEXES + 1> *const cfs) const {
  rocksdb_rpc_log(9051, "create_cfs: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(table_arg->s != nullptr);

  char tablename_sys[NAME_LEN + 1];

  my_core::filename_to_tablename(tbl_def_arg->base_tablename().c_str(),
                                 tablename_sys, sizeof(tablename_sys));

  uint primary_key_index = pk_index(table_arg, tbl_def_arg);
  /*
    The first loop checks the index parameters and creates
    column families if necessary.
  */
  THD *const thd = my_core::thd_get_current_thd();
  for (uint i = 0; i < tbl_def_arg->m_key_count; i++) {
    // ALTER
    // std::shared_ptr<rocksdb::ColumnFamilyHandle> cf_handle;
    rocksdb::ColumnFamilyHandle *cf_handle;

    /*
      Skip collation checks on truncation since we might be recreating the
      table that had unsupported collations and we don't want to fail the
      truncation.
    */
    if (rocksdb_strict_collation_check &&
        thd->lex->sql_command != SQLCOM_TRUNCATE &&
        !is_hidden_pk(i, table_arg, tbl_def_arg) &&
        tbl_def_arg->base_tablename().find(tmp_file_prefix) != 0) {
      for (uint part = 0; part < table_arg->key_info[i].actual_key_parts;
           part++) {
        if (!rdb_is_index_collation_supported(
                table_arg->key_info[i].key_part[part].field) &&
            !rdb_collation_exceptions->matches(tablename_sys)) {
          std::string collation_err;
          for (const auto &coll : RDB_INDEX_COLLATIONS) {
            if (collation_err != "") {
              collation_err += ", ";
            }
            collation_err += coll->name;
          }

          if (rocksdb_error_on_suboptimal_collation) {
            my_error(ER_UNSUPPORTED_COLLATION, MYF(0),
                     tbl_def_arg->full_tablename().c_str(),
                     table_arg->key_info[i].key_part[part].field->field_name,
                     collation_err.c_str());
            rocksdb_rpc_log(9100, "create_cfs: end");

            DBUG_RETURN(HA_EXIT_FAILURE);
          } else {
            push_warning_printf(
                ha_thd(), Sql_condition::WARN_LEVEL_WARN, ER_WRONG_ARGUMENTS,
                "Unsupported collation on string indexed column %s.%s Use "
                "binary collation (%s).",
                tbl_def_arg->full_tablename().c_str(),
                table_arg->key_info[i].key_part[part].field->field_name,
                collation_err.c_str());
          }
        }
      }
    }

    // Internal consistency check to make sure that data in TABLE and
    // Rdb_tbl_def structures matches. Either both are missing or both are
    // specified. Yes, this is critical enough to make it into SHIP_ASSERT.
    SHIP_ASSERT(!table_arg->part_info == tbl_def_arg->base_partition().empty());

    // Generate the name for the column family to use.
    bool per_part_match_found = false;
    std::string cf_name =
        generate_cf_name(i, table_arg, tbl_def_arg, &per_part_match_found);

    // Prevent create from using the system column family.
    if (cf_name == DEFAULT_SYSTEM_CF_NAME) {
      my_error(ER_WRONG_ARGUMENTS, MYF(0),
               "column family not valid for storing index data.");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }

    DBUG_EXECUTE_IF("rocksdb_create_primary_cf", {
      if (cf_name == "cf_primary_key") {
        THD *const thd = my_core::thd_get_current_thd();
        const char act[] =
            "now signal ready_to_mark_cf_dropped_in_create_cfs "
            "wait_for mark_cf_dropped_done_in_create_cfs";
        DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
      }
    });

    DBUG_EXECUTE_IF("rocksdb_create_secondary_cf", {
      if (cf_name == "cf_secondary_key") {
        THD *const thd = my_core::thd_get_current_thd();
        const char act[] =
            "now signal ready_to_mark_cf_dropped_in_create_cfs "
            "wait_for mark_cf_dropped_done_in_create_cfs";
        DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
      }
    });

    // if not specified, use default CF name
    if (cf_name.empty()) {
      if (i != primary_key_index && rocksdb_use_default_sk_cf)
        cf_name = DEFAULT_SK_CF_NAME;
      else
        cf_name = DEFAULT_CF_NAME;
    }

    // Here's how `get_or_create_cf` will use the input parameters:
    //
    // `cf_name` - will be used as a CF name.
    {
      std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
      cf_handle = cf_manager.get_or_create_cf(rdb, cf_name);
      if (!cf_handle) {
        rocksdb_rpc_log(9168, "create_cfs: rocksdb_ColumnFamilyHandle__GetID");
        DBUG_RETURN(HA_EXIT_FAILURE);
      }

      rocksdb_rpc_log(9173, "create_cfs: rocksdb_ColumnFamilyHandle__GetID");
      // uint32 cf_id = cf_handle->GetID();
      uint32 cf_id = rocksdb_ColumnFamilyHandle__GetID(cf_handle);

      // If the cf is marked as dropped, we fail it here.
      // The cf can be dropped after this point, we will
      // check again when committing metadata changes.
      if (dict_manager.get_dropped_cf(cf_id)) {
        my_error(ER_CF_DROPPED, MYF(0), cf_name.c_str());
        DBUG_RETURN(HA_EXIT_FAILURE);
      }

      rocksdb_rpc_log(9188, "create_cfs: rocksdb_ColumnFamilyHandle__GetID");

      // ALTER
      // if (cf_manager.create_cf_flags_if_needed(&dict_manager,
      //                                          cf_handle->GetID(), cf_name,
      //                                          per_part_match_found)) {
      if (cf_manager.create_cf_flags_if_needed(
              &dict_manager, rocksdb_ColumnFamilyHandle__GetID(cf_handle),
              cf_name, per_part_match_found)) {
        rocksdb_rpc_log(9189, "create_cfs: end");

        DBUG_RETURN(HA_EXIT_FAILURE);
      }
    }

    // The CF can be dropped from cf_manager at this point. This is part of
    // create table or alter table. If the drop happens before metadata are
    // written, create table or alter table will fail.
    auto &cf = (*cfs)[i];

    cf.cf_handle = cf_handle;
    cf.is_reverse_cf = Rdb_cf_manager::is_cf_name_reverse(cf_name.c_str());
    cf.is_per_partition_cf = per_part_match_found;
  }
  rocksdb_rpc_log(9209, "create_cfs: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
  Create key definition needed for storing data in rocksdb during ADD index
  inplace operations.

  @param in
    table_arg         Table with definition
    tbl_def_arg       New table def structure being populated
    old_tbl_def_arg   Old(current) table def structure
    cfs               Struct array which contains column family information

  @return
    0      - Ok
    other  - error, either given table ddl is not supported by rocksdb or OOM.
*/
int ha_rocksdb::create_inplace_key_defs(
    const TABLE *const table_arg, Rdb_tbl_def *const tbl_def_arg,
    const TABLE *const old_table_arg, const Rdb_tbl_def *const old_tbl_def_arg,
    const std::array<key_def_cf_info, MAX_INDEXES + 1> &cfs,
    uint64 ttl_duration, const std::string &ttl_column) const {
  rocksdb_rpc_log(9231, "create_inplace_key_defs: start");
  DBUG_ENTER_FUNC();

  std::shared_ptr<Rdb_key_def> *const old_key_descr =
      old_tbl_def_arg->m_key_descr_arr;
  std::shared_ptr<Rdb_key_def> *const new_key_descr =
      tbl_def_arg->m_key_descr_arr;
  const std::unordered_map<std::string, uint> old_key_pos =
      get_old_key_positions(table_arg, tbl_def_arg, old_table_arg,
                            old_tbl_def_arg);

  uint i;
  rocksdb_rpc_log(9244, "create_inplace_key_defs:  tbl_def_arg->");
  for (i = 0; i < tbl_def_arg->m_key_count; i++) {
    const auto &it = old_key_pos.find(get_key_name(i, table_arg, tbl_def_arg));

    if (it != old_key_pos.end()) {
      /*
        Found matching index in old table definition, so copy it over to the
        new one created.
      */
      const Rdb_key_def &okd = *old_key_descr[it->second];

      const GL_INDEX_ID gl_index_id = okd.get_gl_index_id();
      struct Rdb_index_info index_info;
      if (!dict_manager.get_index_info(gl_index_id, &index_info)) {
        // NO_LINT_DEBUG
        sql_print_error(
            "RocksDB: Could not get index information "
            "for Index Number (%u,%u), table %s",
            gl_index_id.cf_id, gl_index_id.index_id,
            old_tbl_def_arg->full_tablename().c_str());
        rocksdb_rpc_log(9252, "create_inplace_key_defs: end");

        DBUG_RETURN(HA_EXIT_FAILURE);
      }

      uint32 ttl_rec_offset =
          Rdb_key_def::has_index_flag(index_info.m_index_flags,
                                      Rdb_key_def::TTL_FLAG)
              ? Rdb_key_def::calculate_index_flag_offset(
                    index_info.m_index_flags, Rdb_key_def::TTL_FLAG)
              : UINT_MAX;

      /*
        We can't use the copy constructor because we need to update the
        keynr within the pack_info for each field and the keyno of the keydef
        itself.
      */
      rocksdb_rpc_log(9281,
                      "create_inplace_key_defs: std::make_shared<Rdb_key_def>");
      new_key_descr[i] = std::make_shared<Rdb_key_def>(
          okd.get_index_number(), i, okd.get_shared_cf(),
          index_info.m_index_dict_version, index_info.m_index_type,
          index_info.m_kv_version, okd.m_is_reverse_cf,
          okd.m_is_per_partition_cf, okd.m_name.c_str(),
          dict_manager.get_stats(gl_index_id), index_info.m_index_flags,
          ttl_rec_offset, index_info.m_ttl_duration);
    } else if (create_key_def(table_arg, i, tbl_def_arg, &new_key_descr[i],
                              cfs[i], ttl_duration, ttl_column)) {
      rocksdb_rpc_log(9291, "create_inplace_key_defs: end");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }

    DBUG_ASSERT(new_key_descr[i] != nullptr);
    new_key_descr[i]->setup(table_arg, tbl_def_arg);
  }

  tbl_def_arg->m_tbl_stats.set(new_key_descr[0]->m_stats.m_rows, 0, 0);
  rocksdb_rpc_log(9300, "create_inplace_key_defs: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

std::unordered_map<std::string, uint> ha_rocksdb::get_old_key_positions(
    const TABLE *const table_arg, const Rdb_tbl_def *const tbl_def_arg,
    const TABLE *const old_table_arg,
    const Rdb_tbl_def *const old_tbl_def_arg) const {
  rocksdb_rpc_log(9308, "get_old_key_positions: begin");

  DBUG_ENTER_FUNC();

  std::shared_ptr<Rdb_key_def> *const old_key_descr =
      old_tbl_def_arg->m_key_descr_arr;
  std::unordered_map<std::string, uint> old_key_pos;
  std::unordered_map<std::string, uint> new_key_pos;
  uint i;

  for (i = 0; i < tbl_def_arg->m_key_count; i++) {
    new_key_pos[get_key_name(i, table_arg, tbl_def_arg)] = i;
  }

  for (i = 0; i < old_tbl_def_arg->m_key_count; i++) {
    if (is_hidden_pk(i, old_table_arg, old_tbl_def_arg)) {
      old_key_pos[old_key_descr[i]->m_name] = i;
      continue;
    }

    /*
      In case of matching key name, need to check key parts of keys as well,
      in case a simultaneous drop + add is performed, where the key name is the
      same but the key parts are different.

      Example:
      CREATE TABLE t1 (a INT, b INT, KEY ka(a)) ENGINE=RocksDB;
      ALTER TABLE t1 DROP INDEX ka, ADD INDEX ka(b), ALGORITHM=INPLACE;
    */
    const KEY *const old_key = &old_table_arg->key_info[i];
    const auto &it = new_key_pos.find(old_key->name);
    if (it == new_key_pos.end()) {
      continue;
    }

    KEY *const new_key = &table_arg->key_info[it->second];

    /*
      Check that the key is identical between old and new tables.
      If not, we still need to create a new index.

      The exception is if there is an index changed from unique to non-unique,
      in these cases we don't need to rebuild as they are stored the same way in
      RocksDB.
    */
    bool unique_to_non_unique =
        ((old_key->flags ^ new_key->flags) == HA_NOSAME) &&
        (old_key->flags & HA_NOSAME);

    if (compare_keys(old_key, new_key) && !unique_to_non_unique) {
      continue;
    }

    /* Check to make sure key parts match. */
    if (compare_key_parts(old_key, new_key)) {
      continue;
    }

    old_key_pos[old_key->name] = i;
  }
  rocksdb_rpc_log(9368, "create_inplace_key_defs: end");
  DBUG_RETURN(old_key_pos);
}

/* Check to see if two keys are identical. */
int ha_rocksdb::compare_keys(const KEY *const old_key,
                             const KEY *const new_key) const {
  rocksdb_rpc_log(9375, "compare_keys: start");
  DBUG_ENTER_FUNC();

  /* Check index name. */
  if (strcmp(old_key->name, new_key->name) != 0) {
    rocksdb_rpc_log(9380, "compare_keys: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  /* If index algorithms are different then keys are different. */
  if (old_key->algorithm != new_key->algorithm) {
    rocksdb_rpc_log(9386, "compare_keys: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  /* Check that the key is identical between old and new tables.  */
  if ((old_key->flags ^ new_key->flags) & HA_KEYFLAG_MASK) {
    rocksdb_rpc_log(9392, "compare_keys: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  /* Check index comment. (for column family changes) */
  std::string old_comment(old_key->comment.str, old_key->comment.length);
  std::string new_comment(new_key->comment.str, new_key->comment.length);
  if (old_comment.compare(new_comment) != 0) {
    rocksdb_rpc_log(9400, "compare_keys: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  rocksdb_rpc_log(9404, "compare_keys: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/* Check two keys to ensure that key parts within keys match */
int ha_rocksdb::compare_key_parts(const KEY *const old_key,
                                  const KEY *const new_key) const {
  rocksdb_rpc_log(9411, "compare_key_parts: start");
  DBUG_ENTER_FUNC();

  /* Skip if key parts do not match, as it is a different key */
  if (new_key->user_defined_key_parts != old_key->user_defined_key_parts) {
    rocksdb_rpc_log(9416, "compare_key_parts: end");
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  /* Check to see that key parts themselves match */
  for (uint i = 0; i < old_key->user_defined_key_parts; i++) {
    if (strcmp(old_key->key_part[i].field->field_name,
               new_key->key_part[i].field->field_name) != 0) {
      rocksdb_rpc_log(9424, "compare_key_parts: end");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }

    /* Check if prefix index key part length has changed */
    if (old_key->key_part[i].length != new_key->key_part[i].length) {
      rocksdb_rpc_log(9430, "compare_key_parts: end");
      DBUG_RETURN(HA_EXIT_FAILURE);
    }
  }

  rocksdb_rpc_log(9435, "compare_key_parts: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
  Create key definition needed for storing data in rocksdb.
  This can be called either during CREATE table or doing ADD index operations.

  @param in
    table_arg     Table with definition
    i             Position of index being created inside table_arg->key_info
    tbl_def_arg   Table def structure being populated
    cf_info       Struct which contains column family information

  @param out
    new_key_def  Newly created index definition.

  @return
    0      - Ok
    other  - error, either given table ddl is not supported by rocksdb or OOM.
*/
int ha_rocksdb::create_key_def(const TABLE *const table_arg, const uint i,
                               const Rdb_tbl_def *const tbl_def_arg,
                               std::shared_ptr<Rdb_key_def> *const new_key_def,
                               const struct key_def_cf_info &cf_info,
                               uint64 ttl_duration,
                               const std::string &ttl_column) const {
  rocksdb_rpc_log(9463, "create_key_def: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(*new_key_def == nullptr);

  const uint index_id = ddl_manager.get_and_update_next_number(&dict_manager);
  const uint16_t index_dict_version = Rdb_key_def::INDEX_INFO_VERSION_LATEST;
  uchar index_type;
  uint16_t kv_version;

  if (is_hidden_pk(i, table_arg, tbl_def_arg)) {
    index_type = Rdb_key_def::INDEX_TYPE_HIDDEN_PRIMARY;
    kv_version = Rdb_key_def::PRIMARY_FORMAT_VERSION_LATEST;
  } else if (i == table_arg->s->primary_key) {
    index_type = Rdb_key_def::INDEX_TYPE_PRIMARY;
    uint16 pk_latest_version = Rdb_key_def::PRIMARY_FORMAT_VERSION_LATEST;
    kv_version = pk_latest_version;
  } else {
    index_type = Rdb_key_def::INDEX_TYPE_SECONDARY;
    uint16 sk_latest_version = Rdb_key_def::SECONDARY_FORMAT_VERSION_LATEST;
    kv_version = sk_latest_version;
  }

  // Use PRIMARY_FORMAT_VERSION_UPDATE1 here since it is the same value as
  // SECONDARY_FORMAT_VERSION_UPDATE1 so it doesn't matter if this is a
  // primary key or secondary key.
  DBUG_EXECUTE_IF("MYROCKS_LEGACY_VARBINARY_FORMAT", {
    kv_version = Rdb_key_def::PRIMARY_FORMAT_VERSION_UPDATE1;
  });

  DBUG_EXECUTE_IF("MYROCKS_NO_COVERED_BITMAP_FORMAT", {
    if (index_type == Rdb_key_def::INDEX_TYPE_SECONDARY) {
      kv_version = Rdb_key_def::SECONDARY_FORMAT_VERSION_UPDATE2;
    }
  });

  uint32 index_flags = (ttl_duration > 0 ? Rdb_key_def::TTL_FLAG : 0);

  uint32 ttl_rec_offset =
      Rdb_key_def::has_index_flag(index_flags, Rdb_key_def::TTL_FLAG)
          ? Rdb_key_def::calculate_index_flag_offset(index_flags,
                                                     Rdb_key_def::TTL_FLAG)
          : UINT_MAX;

  rocksdb_rpc_log(9508, "create_key_def: get_key_name");

  const char *const key_name = get_key_name(i, table_arg, m_tbl_def);
  *new_key_def = std::make_shared<Rdb_key_def>(
      index_id, i, cf_info.cf_handle, index_dict_version, index_type,
      kv_version, cf_info.is_reverse_cf, cf_info.is_per_partition_cf, key_name,
      Rdb_index_stats(), index_flags, ttl_rec_offset, ttl_duration);

  if (!ttl_column.empty()) {
    (*new_key_def)->m_ttl_column = ttl_column;
  }
  // initialize key_def
  (*new_key_def)->setup(table_arg, tbl_def_arg);
  rocksdb_rpc_log(9521, "create_key_def: get_key_name");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

bool rdb_is_tablename_normalized(const std::string &tablename) {
  rocksdb_rpc_log(9526, "rdb_is_tablename_normalized: begin");
  return tablename.size() < 2 || (tablename[0] != '.' && tablename[1] != '/');
}

int rdb_normalize_tablename(const std::string &tablename,
                            std::string *const strbuf) {
  rocksdb_rpc_log(9532, "rdb_normalize_tablename: begin");

  if (tablename.size() < 2 || tablename[0] != '.' || tablename[1] != '/') {
    DBUG_ASSERT(0);  // We were not passed table name?

    return HA_ERR_ROCKSDB_INVALID_TABLE;
  }

  size_t pos = tablename.find_first_of('/', 2);
  if (pos == std::string::npos) {
    DBUG_ASSERT(0);  // We were not passed table name?
    return HA_ERR_ROCKSDB_INVALID_TABLE;
  }

  *strbuf = tablename.substr(2, pos - 2) + "." + tablename.substr(pos + 1);

  rocksdb_rpc_log(9548, "rdb_normalize_tablename: end");
  return HA_EXIT_SUCCESS;
}

/*
  Check to see if the user's original statement includes foreign key
  references
*/
bool ha_rocksdb::contains_foreign_key(THD *const thd) {
  rocksdb_rpc_log(9557, "contains_foreign_key: begin");
  bool success;
  const char *str = thd_query_string(thd)->str;

  DBUG_ASSERT(str != nullptr);

  while (*str != '\0') {
    // Scan from our current pos looking for 'FOREIGN'
    str = rdb_find_in_string(str, "FOREIGN", &success);
    if (!success) {
      rocksdb_rpc_log(9567, "contains_foreign_key: end");

      return false;
    }

    // Skip past the found "FOREIGN'
    str = rdb_check_next_token(&my_charset_bin, str, "FOREIGN", &success);
    DBUG_ASSERT(success);

    if (!my_isspace(&my_charset_bin, *str)) {
      rocksdb_rpc_log(9577, "contains_foreign_key: end");
      return false;
    }

    // See if the next token is 'KEY'
    str = rdb_check_next_token(&my_charset_bin, str, "KEY", &success);
    if (!success) {
      continue;
    }

    // See if the next token is '('
    str = rdb_check_next_token(&my_charset_bin, str, "(", &success);
    if (!success) {
      // There is an optional index id after 'FOREIGN KEY', skip it
      str = rdb_skip_id(&my_charset_bin, str);

      // Now check for '(' again
      str = rdb_check_next_token(&my_charset_bin, str, "(", &success);
    }

    // If we have found 'FOREIGN KEY [<word>] (' we can be confident we have
    // a foreign key clause.
    rocksdb_rpc_log(9599, "contains_foreign_key: end");
    return success;
  }

  // We never found a valid foreign key clause
  rocksdb_rpc_log(9604, "contains_foreign_key: end");
  return false;
}

/**
  @brief
  splits the normalized table name of <dbname>.<tablename>#P#<part_no> into
  the <dbname>, <tablename> and <part_no> components.

  @param dbbuf returns database name/table_schema
  @param tablebuf returns tablename
  @param partitionbuf returns partition suffix if there is one
  @return HA_EXIT_SUCCESS on success, non-zero on failure to split
*/
int rdb_split_normalized_tablename(const std::string &fullname,
                                   std::string *const db,
                                   std::string *const table,
                                   std::string *const partition) {
  rocksdb_rpc_log(9622, "rdb_split_normalized_tablename: start");

  DBUG_ASSERT(!fullname.empty());

#define RDB_PARTITION_STR "#P#"

  /* Normalize returns dbname.tablename. */
  size_t dotpos = fullname.find('.');

  /* Invalid table name? */
  if (dotpos == std::string::npos) {
    rocksdb_rpc_log(9633, "rdb_split_normalized_tablename: end");
    return HA_ERR_ROCKSDB_INVALID_TABLE;
  }

  // Table must have a database name associated with it.
  DBUG_ASSERT(dotpos > 0);

  if (db != nullptr) {
    *db = fullname.substr(0, dotpos);
  }

  dotpos++;

  const size_t partpos =
      fullname.find(RDB_PARTITION_STR, dotpos, strlen(RDB_PARTITION_STR));

  if (partpos != std::string::npos) {
    DBUG_ASSERT(partpos >= dotpos);

    if (table != nullptr) {
      *table = fullname.substr(dotpos, partpos - dotpos);
    }

    if (partition != nullptr) {
      *partition = fullname.substr(partpos + strlen(RDB_PARTITION_STR));
    }
  } else if (table != nullptr) {
    *table = fullname.substr(dotpos);
  }
  rocksdb_rpc_log(9662, "rdb_split_normalized_tablename: end");
  return HA_EXIT_SUCCESS;
}

/*
  Generates the normalized tablename using as many of the given arguments as
  possible. Any of the three arguments to <db>.<table>#P#<partition> can be
  null/empty, but return string will only ever be of the form
  <db>
  <db>.<table>
  <db>.<table>#P#<partition>
  <table>
  <table>#P#<partition>
*/
void rdb_gen_normalized_tablename(const std::string *db,
                                  const std::string *table,
                                  const std::string *partition,
                                  std::string *fullname) {
  rocksdb_rpc_log(9662, "rdb_gen_normalized_tablename: start");
  if (!fullname) return;
  fullname->clear();
  if (db && db->length() > 0) *fullname += *db;

  /* If table was not passed in, the partition will be ignored too */
  if (!table || table->length() == 0) return;

  if (fullname->length() > 0) *fullname += ".";
  *fullname += *table;

  if (partition && partition->length() > 0) {
    *fullname += std::string(RDB_PARTITION_STR) + *partition;
  }
  rocksdb_rpc_log(9694, "rdb_gen_normalized_tablename: end");
}

/*
 Create a table's Rdb_tbl_def and its Rdb_key_defs and store table information
 into MyRocks Data Dictionary
 The method is called during create table/partition, truncate table/partition

 @param table_name            IN      table's name formated as
 'dbname.tablename'
 @param table_arg             IN      sql table
 @param auto_increment_value  IN      specified table's auto increment value

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::create_table(const std::string &table_name,
                             const TABLE *table_arg,
                             ulonglong auto_increment_value) {
  rocksdb_rpc_log(9714, "create_table: start");
  DBUG_ENTER_FUNC();

  int err;

  rocksdb_rpc_log(9719, "create_table: dict_manager.begin()");
  // ALTER
  // const std::unique_ptr<rocksdb::WriteBatch> wb = dict_manager.begin();
  // rocksdb::WriteBatch *const batch = wb.get();
  rocksdb::WriteBatch *const batch = dict_manager.begin();

  /* Create table/key descriptions and put them into the data dictionary */
  m_tbl_def = new Rdb_tbl_def(table_name);

  uint n_keys = table_arg->s->keys;

  /*
    If no primary key found, create a hidden PK and place it inside table
    definition
  */
  if (has_hidden_pk(table_arg)) {
    n_keys += 1;
    // reset hidden pk id
    // the starting valid value for hidden pk is 1
    m_tbl_def->m_hidden_pk_val = 1;
  }

  rocksdb_rpc_log(9719,
                  "create_table: new std::shared_ptr<Rdb_key_def>[n_keys]");
  m_key_descr_arr = new std::shared_ptr<Rdb_key_def>[n_keys];
  m_tbl_def->m_key_count = n_keys;
  m_tbl_def->m_key_descr_arr = m_key_descr_arr;

  err = create_key_defs(table_arg, m_tbl_def);
  if (err != HA_EXIT_SUCCESS) {
    goto error;
  }

  m_pk_descr = m_key_descr_arr[pk_index(table_arg, m_tbl_def)];

  if (auto_increment_value) {
    bool autoinc_upgrade_test = false;
    m_tbl_def->m_auto_incr_val = auto_increment_value;
    DBUG_EXECUTE_IF("myrocks_autoinc_upgrade", autoinc_upgrade_test = true;);
    if (!autoinc_upgrade_test) {
      auto s = dict_manager.put_auto_incr_val(
          batch, m_tbl_def->get_autoincr_gl_index_id(),
          m_tbl_def->m_auto_incr_val);
      if (!s.ok()) {
        goto error;
      }
    }
  }

  DBUG_EXECUTE_IF("rocksdb_create_table", {
    THD *const thd = my_core::thd_get_current_thd();
    const char act[] =
        "now signal ready_to_mark_cf_dropped_in_create_table "
        "wait_for mark_cf_dropped_done_in_create_table";
    DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
  });

  {
    std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
    err = ddl_manager.put_and_write(m_tbl_def, batch);
    if (err != HA_EXIT_SUCCESS) {
      goto error;
    }

    err = dict_manager.commit(batch);
    if (err != HA_EXIT_SUCCESS) {
      goto error;
    }
  }
  rocksdb_rpc_log(9787, "create_table: succcess");
  DBUG_RETURN(HA_EXIT_SUCCESS);

error:
  /* Delete what we have allocated so far */
  delete m_tbl_def;
  m_tbl_def = nullptr;
  m_key_descr_arr = nullptr;
  rocksdb_rpc_log(9795, "create_table: failed");
  DBUG_RETURN(err);
}

/**
  @brief
  create() is called to create a table. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)

  @see
  ha_create_table() in handle.cc
*/

int ha_rocksdb::create(const char *const name, TABLE *const table_arg,
                       HA_CREATE_INFO *const create_info) {
  rocksdb_rpc_log(9824, "create: start");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(table_arg != nullptr);
  DBUG_ASSERT(create_info != nullptr);

  if (create_info->data_file_name) {
    // DATA DIRECTORY is used to create tables under a specific location
    // outside the MySQL data directory. We don't support this for MyRocks.
    // The `rocksdb_datadir` setting should be used to configure RocksDB data
    // directory.
    rocksdb_rpc_log(9836, "create: end");

    DBUG_RETURN(HA_ERR_ROCKSDB_TABLE_DATA_DIRECTORY_NOT_SUPPORTED);
  }

  if (create_info->index_file_name) {
    // Similar check for INDEX DIRECTORY as well.
    rocksdb_rpc_log(9843, "create: end");
    DBUG_RETURN(HA_ERR_ROCKSDB_TABLE_INDEX_DIRECTORY_NOT_SUPPORTED);
  }

  int err;
  /*
    Construct dbname.tablename ourselves, because parititioning
    passes strings like "./test/t14#P#p0" for individual partitions,
    while table_arg->s->table_name has none of that.
  */
  std::string str;
  err = rdb_normalize_tablename(name, &str);
  if (err != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(9856, "create: end");
    DBUG_RETURN(err);
  }

  // FOREIGN KEY isn't supported yet
  THD *const thd = my_core::thd_get_current_thd();
  if (contains_foreign_key(thd)) {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "FOREIGN KEY for the RocksDB storage engine");
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }

  // Check whether Data Dictionary contain information
  Rdb_tbl_def *old_tbl = ddl_manager.find(str);
  if (old_tbl != nullptr) {
    if (thd->lex->sql_command == SQLCOM_TRUNCATE) {
      rocksdb_rpc_log(9873, "create: end");
      DBUG_RETURN(truncate_table(old_tbl, table_arg,
                                 create_info->auto_increment_value));
    } else {
      my_error(ER_METADATA_INCONSISTENCY, MYF(0), str.c_str(), name);
      rocksdb_rpc_log(9877, "create: end");
      DBUG_RETURN(HA_ERR_ROCKSDB_CORRUPT_DATA);
    }
  }
  rocksdb_rpc_log(9883, "create: end");

  DBUG_RETURN(create_table(str, table_arg, create_info->auto_increment_value));
}

/*
  Fast truncates a table by renaming the old table, creating a new one and
  restoring or deleting the old table based on the results from creation.

  @param tbl_def               IN      MyRocks's table structure
  @param table_arg             IN      sql table
  @param auto_increment_value  IN      specified table's auto increment value

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::truncate_table(Rdb_tbl_def *tbl_def_arg, TABLE *table_arg,
                               ulonglong auto_increment_value) {
  rocksdb_rpc_log(9900, "truncate_table: start");
  DBUG_ENTER_FUNC();

  /*
    Fast table truncation involves deleting the table and then recreating
    it. However, it is possible recreating the table fails. In this case, a
    table inconsistency might result between SQL and MyRocks where MyRocks is
    missing a table. Since table creation involves modifying keys with the
    original table name, renaming the original table first, and then renaming
    it back in case of creation failure can help restore the pre-truncation
    state.

    If the server were to crash during truncation, the system will end up with
    an inconsistency. Future changes for atomic ddl will resolve this. For now,
    if there are any truncation renamed tables found during startup, MyRocks
    will automatically remove them.
  */
  std::string orig_tablename = tbl_def_arg->full_tablename();
  std::string dbname, tblname, partition;

  /*
    Rename the table in the data dictionary. Since this thread should be
    holding the MDL for this tablename, it is safe to perform these renames
    should be locked via MDL, no other process thread be able to access this
    table.
  */
  int err = rdb_split_normalized_tablename(orig_tablename, &dbname, &tblname,
                                           &partition);
  DBUG_ASSERT(err == 0);
  if (err != HA_EXIT_SUCCESS) DBUG_RETURN(err);
  tblname = std::string(TRUNCATE_TABLE_PREFIX) + tblname;

  std::string tmp_tablename;
  rdb_gen_normalized_tablename(&dbname, &tblname, &partition, &tmp_tablename);

  rocksdb_rpc_log(9935, "truncate_table: rename table");
  err = rename_table(orig_tablename.c_str(), tmp_tablename.c_str());
  if (err != HA_EXIT_SUCCESS) DBUG_RETURN(err);

  /*
    Attempt to create the table. If this succeeds, then drop the old table.
    Otherwise, try to restore it.
  */
  err = create_table(orig_tablename, table_arg, auto_increment_value);
  bool should_remove_old_table = true;

  /* Restore the old table being truncated if creating the new table failed */
  if (err != HA_EXIT_SUCCESS) {
    int rename_err =
        rename_table(tmp_tablename.c_str(), orig_tablename.c_str());

    /*
      If the rename also fails, we are out of options, but at least try to drop
      the old table contents.
    */
    if (rename_err == HA_EXIT_SUCCESS) {
      should_remove_old_table = false;
    } else {
      // NO_LINT_DEBUG
      sql_print_error(
          "MyRocks: Failure during truncation of table %s "
          "being renamed from %s",
          orig_tablename.c_str(), tmp_tablename.c_str());
      err = rename_err;
    }
  }

  /*
    Since the table was successfully truncated or the name restore failed, no
    error should be returned at this point from trying to delete the old
    table. If the delete_table fails, log it instead.
  */
  rocksdb_rpc_log(9935, "truncate_table: ddl_manager.find");
  Rdb_tbl_def *old_tbl_def = ddl_manager.find(tmp_tablename);
  if (should_remove_old_table && old_tbl_def) {
    m_tbl_def = old_tbl_def;
    if (delete_table(old_tbl_def) != HA_EXIT_SUCCESS) {
      // NO_LINT_DEBUG
      sql_print_error(
          "Failure when trying to drop table %s during "
          "truncation of table %s",
          tmp_tablename.c_str(), orig_tablename.c_str());
    }
  }

  /* Update the local m_tbl_def reference */
  m_tbl_def = ddl_manager.find(orig_tablename);
  rocksdb_rpc_log(9987, "truncate_table: Rdb_converter.reset");
  m_converter.reset(new Rdb_converter(ha_thd(), m_tbl_def, table_arg));
  rocksdb_rpc_log(9989, "truncate_table: end");
  DBUG_RETURN(err);
}

/**
  @note
  This function is used only when the table has not yet been opened, and
  keyread_allowed bitmap doesn't have the correct values yet.

  See comment in ha_rocksdb::index_flags() for details.
*/

bool ha_rocksdb::check_keyread_allowed(uint inx, uint part,
                                       bool all_parts) const {
  rocksdb_rpc_log(10003, "check_keyread_allowed: start");
  bool res = true;
  KEY *const key_info = &table_share->key_info[inx];

  Rdb_field_packing dummy1;
  res = dummy1.setup(nullptr, key_info->key_part[part].field, inx, part,
                     key_info->key_part[part].length);

  if (res && all_parts) {
    for (uint i = 0; i < part; i++) {
      Field *field;
      if ((field = key_info->key_part[i].field)) {
        Rdb_field_packing dummy;
        if (!dummy.setup(nullptr, field, inx, i,
                         key_info->key_part[i].length)) {
          /* Cannot do index-only reads for this column */
          res = false;
          break;
        }
      }
    }
  }

  const uint pk = table_share->primary_key;
  if (inx == pk && all_parts &&
      part + 1 == table_share->key_info[pk].user_defined_key_parts) {
    m_pk_can_be_decoded = res;
  }
  rocksdb_rpc_log(10031, "check_keyread_allowed: end");

  return res;
}

int ha_rocksdb::read_key_exact(const Rdb_key_def &kd,
                               rocksdb::Iterator *const iter,
                               const bool /* unused */,
                               const rocksdb::Slice &key_slice,
                               const int64_t ttl_filter_ts) {
  rocksdb_rpc_log(10041, "read_key_exact: start");
  THD *thd = ha_thd();
  /*
    We are looking for the first record such that
      index_tuple= lookup_tuple.
    lookup_tuple may be a prefix of the index.
  */
  rocksdb_smart_seek(kd.m_is_reverse_cf, iter, key_slice);

  // ALTER
  // while (iter->Valid() && kd.value_matches_prefix(iter->key(), key_slice)) {
  rocksdb_rpc_log(10053, "read_key_exact: rocksdb_Iterator__Valid");
  while (rocksdb_Iterator__Valid(iter) &&
         kd.value_matches_prefix(rocksdb_Iterator__key(iter), key_slice)) {
    if (thd && thd->killed) {
      rocksdb_rpc_log(10056, "read_key_exact: end");
      return HA_ERR_QUERY_INTERRUPTED;
    }
    /*
      If TTL is enabled we need to check if the given key has already expired
      from the POV of the current transaction.  If it has, try going to the next
      key.
    */
    if (kd.has_ttl() &&
        should_hide_ttl_rec(kd, rocksdb_Iterator__value(iter), ttl_filter_ts)) {
      rocksdb_smart_next(kd.m_is_reverse_cf, iter);
      continue;
    }
    rocksdb_rpc_log(10069, "read_key_exact: end");

    return HA_EXIT_SUCCESS;
  }

  /*
    Got a record that is not equal to the lookup value, or even a record
    from another table.index.
  */
  rocksdb_rpc_log(10078, "read_key_exact: end");
  return HA_ERR_KEY_NOT_FOUND;
}

int ha_rocksdb::read_before_key(const Rdb_key_def &kd,
                                const bool full_key_match,
                                const rocksdb::Slice &key_slice,
                                const int64_t ttl_filter_ts) {
  rocksdb_rpc_log(10086, "read_before_key: start");
  THD *thd = ha_thd();
  /*
    We are looking for record with the biggest t.key such that
    t.key < lookup_tuple.
  */
  rocksdb_smart_seek(!kd.m_is_reverse_cf, m_scan_it, key_slice);

  rocksdb_rpc_log(10094, "read_before_key: is_valid_iterator");

  while (is_valid_iterator(m_scan_it)) {
    if (thd && thd->killed) {
      return HA_ERR_QUERY_INTERRUPTED;
    }
    /*
      We are using full key and we've hit an exact match, or...

      If TTL is enabled we need to check if the given key has already expired
      from the POV of the current transaction.  If it has, try going to the next
      key.
    */
    if ((full_key_match &&
         kd.value_matches_prefix(m_scan_it->key(), key_slice)) ||
        (kd.has_ttl() &&
         should_hide_ttl_rec(kd, m_scan_it->value(), ttl_filter_ts))) {
      rocksdb_smart_next(!kd.m_is_reverse_cf, m_scan_it);
      continue;
    }

    rocksdb_rpc_log(10115, "read_before_key: end");
    return HA_EXIT_SUCCESS;
  }
  rocksdb_rpc_log(10118, "read_before_key: end");
  return HA_ERR_KEY_NOT_FOUND;
}

int ha_rocksdb::read_after_key(const Rdb_key_def &kd,
                               const rocksdb::Slice &key_slice,
                               const int64_t ttl_filter_ts) {
  rocksdb_rpc_log(10125, "read_after_key: begin");
  THD *thd = ha_thd();
  /*
    We are looking for the first record such that

      index_tuple $GT lookup_tuple

    with HA_READ_AFTER_KEY, $GT = '>',
    with HA_READ_KEY_OR_NEXT, $GT = '>='
  */
  rocksdb_smart_seek(kd.m_is_reverse_cf, m_scan_it, key_slice);

  /*
    If TTL is enabled we need to check if the given key has already expired
    from the POV of the current transaction.  If it has, try going to the next
    key.
  */
  while (is_valid_iterator(m_scan_it) && kd.has_ttl() &&
         /*ALTER should_hide_ttl_rec(kd, m_scan_it->value()*/
         should_hide_ttl_rec(kd, rocksdb_Iterator__value(m_scan_it),
                             ttl_filter_ts)) {
    if (thd && thd->killed) {
      rocksdb_rpc_log(10147, "read_after_key: end");

      return HA_ERR_QUERY_INTERRUPTED;
    }
    rocksdb_smart_next(kd.m_is_reverse_cf, m_scan_it);
  }
  rocksdb_rpc_log(10153, "read_after_key: end");

  return is_valid_iterator(m_scan_it) ? HA_EXIT_SUCCESS : HA_ERR_KEY_NOT_FOUND;
}

int ha_rocksdb::position_to_correct_key(
    const Rdb_key_def &kd, const enum ha_rkey_function &find_flag,
    const bool full_key_match, const uchar *const key,
    const key_part_map &keypart_map, const rocksdb::Slice &key_slice,
    bool *const move_forward, const int64_t ttl_filter_ts) {
  rocksdb_rpc_log(10163, "position_to_correct_key: start");
  int rc = 0;

  *move_forward = true;

  switch (find_flag) {
    case HA_READ_KEY_EXACT:
      rc = read_key_exact(kd, m_scan_it, full_key_match, key_slice,
                          ttl_filter_ts);
      break;
    case HA_READ_BEFORE_KEY:
      *move_forward = false;
      rc = read_before_key(kd, full_key_match, key_slice, ttl_filter_ts);
      // ALTER
      if (rc == 0 && !kd.covers_key(rocksdb_Iterator__key(m_scan_it))) {
        /* The record we've got is not from this index */
        rc = HA_ERR_KEY_NOT_FOUND;
      }
      break;
    case HA_READ_AFTER_KEY:
    case HA_READ_KEY_OR_NEXT:
      rc = read_after_key(kd, key_slice, ttl_filter_ts);
      // ALTER
      if (rc == 0 && !kd.covers_key(rocksdb_Iterator__key(m_scan_it))) {
        /* The record we've got is not from this index */
        rc = HA_ERR_KEY_NOT_FOUND;
      }
      break;
    case HA_READ_KEY_OR_PREV:
    case HA_READ_PREFIX:
      /* This flag is not used by the SQL layer, so we don't support it yet. */
      rc = HA_ERR_UNSUPPORTED;
      break;
    case HA_READ_PREFIX_LAST:
    case HA_READ_PREFIX_LAST_OR_PREV:
      *move_forward = false;
      /*
        Find the last record with the specified index prefix lookup.
        - HA_READ_PREFIX_LAST requires that the record has the
          prefix=lookup (if there are no such records,
          HA_ERR_KEY_NOT_FOUND should be returned).
        - HA_READ_PREFIX_LAST_OR_PREV has no such requirement. If there are no
          records with prefix=lookup, we should return the last record
          before that.
      */
      rc = read_before_key(kd, full_key_match, key_slice, ttl_filter_ts);
      if (rc == 0) {
        // ALTER
        // const rocksdb::Slice &rkey = m_scan_it->key();
        const rocksdb::Slice &rkey = rocksdb_Iterator__key(m_scan_it);

        if (!kd.covers_key(rkey)) {
          /* The record we've got is not from this index */
          rc = HA_ERR_KEY_NOT_FOUND;
        } else if (find_flag == HA_READ_PREFIX_LAST) {
          uint size = kd.pack_index_tuple(table, m_pack_buffer,
                                          m_sk_packed_tuple, key, keypart_map);
          rocksdb::Slice lookup_tuple(
              reinterpret_cast<char *>(m_sk_packed_tuple), size);

          // We need to compare the key we've got with the original search
          // prefix.
          if (!kd.value_matches_prefix(rkey, lookup_tuple)) {
            rc = HA_ERR_KEY_NOT_FOUND;
          }
        }
      }
      break;
    default:
      DBUG_ASSERT(0);
      break;
  }
  rocksdb_rpc_log(10235, "position_to_correct_key: end");
  return rc;
}

int ha_rocksdb::calc_eq_cond_len(const Rdb_key_def &kd,
                                 const enum ha_rkey_function &find_flag,
                                 const rocksdb::Slice &slice,
                                 const int bytes_changed_by_succ,
                                 const key_range *const end_key,
                                 uint *const end_key_packed_size) {
  rocksdb_rpc_log(10245, "calc_eq_cond_len: start");
  if (find_flag == HA_READ_KEY_EXACT) return slice.size();

  if (find_flag == HA_READ_PREFIX_LAST) {
    /*
      We have made the kd.successor(m_sk_packed_tuple) call above.

      The slice is at least Rdb_key_def::INDEX_NUMBER_SIZE bytes long.
    */
    rocksdb_rpc_log(10256, "calc_eq_cond_len: end");

    return slice.size() - bytes_changed_by_succ;
  }

  if (end_key) {
    *end_key_packed_size =
        kd.pack_index_tuple(table, m_pack_buffer, m_end_key_packed_tuple,
                            end_key->key, end_key->keypart_map);

    /*
      Calculating length of the equal conditions here. 4 byte index id is
      included.
      Example1: id1 BIGINT, id2 INT, id3 BIGINT, PRIMARY KEY (id1, id2, id3)
       WHERE id1=1 AND id2=1 AND id3>=2 => eq_cond_len= 4+8+4= 16
       WHERE id1=1 AND id2>=1 AND id3>=2 => eq_cond_len= 4+8= 12
      Example2: id1 VARCHAR(30), id2 INT, PRIMARY KEY (id1, id2)
       WHERE id1 = 'AAA' and id2 < 3; => eq_cond_len=13 (varchar used 9 bytes)
    */
    rocksdb::Slice end_slice(reinterpret_cast<char *>(m_end_key_packed_tuple),
                             *end_key_packed_size);
    rocksdb_rpc_log(10275, "calc_eq_cond_len: end");
    return slice.difference_offset(end_slice);
  }

  /*
    On range scan without any end key condition, there is no
    eq cond, and eq cond length is the same as index_id size (4 bytes).
    Example1: id1 BIGINT, id2 INT, id3 BIGINT, PRIMARY KEY (id1, id2, id3)
     WHERE id1>=1 AND id2 >= 2 and id2 <= 5 => eq_cond_len= 4
  */
  rocksdb_rpc_log(10285, "calc_eq_cond_len: end");
  return Rdb_key_def::INDEX_NUMBER_SIZE;
}

int ha_rocksdb::read_row_from_primary_key(uchar *const buf) {
  rocksdb_rpc_log(10290, "read_row_from_primary_key: start");
  int rc;

  // ALTER
  // const rocksdb::Slice &rkey = m_scan_it->key();
  rocksdb_rpc_log(10295, "read_row_from_primary_key: rocksdb_Iterator__key");
  const rocksdb::Slice &rkey = rocksdb_Iterator__key(m_scan_it);
  const uint pk_size = rkey.size();
  const char *pk_data = rkey.data();

  memcpy(m_pk_packed_tuple, pk_data, pk_size);
  m_last_rowkey.copy(pk_data, pk_size, &my_charset_bin);

  if (m_lock_rows != RDB_LOCK_NONE) {
    /* We need to put a lock and re-read */
    rc = get_row_by_rowid(buf, m_pk_packed_tuple, pk_size);
  } else {
    /* Unpack from the row we've read */
    // ALTER
    // const rocksdb::Slice &value = m_scan_it->value();
    const rocksdb::Slice &value = rocksdb_Iterator__value(m_scan_it);

    rc = convert_record_from_storage_format(&rkey, &value, buf);
  }

  rocksdb_rpc_log(10315, "read_row_from_primary_key: end");
  return rc;
}

int ha_rocksdb::read_row_from_secondary_key(uchar *const buf,
                                            const Rdb_key_def &kd,
                                            bool move_forward) {
  rocksdb_rpc_log(10322, "read_row_from_secondary_key: start");
  int rc = 0;
  uint pk_size;

  /* Get the key columns and primary key value */
  // const rocksdb::Slice &rkey = m_scan_it->key();
  // const rocksdb::Slice &value = m_scan_it->value();
  const rocksdb::Slice &rkey = rocksdb_Iterator__key(m_scan_it);
  const rocksdb::Slice &value = rocksdb_Iterator__value(m_scan_it);

#ifndef DBUG_OFF
  bool save_keyread_only = m_keyread_only;
#endif
  DBUG_EXECUTE_IF("dbug.rocksdb.HA_EXTRA_KEYREAD", { m_keyread_only = true; });

  bool covered_lookup =
      (m_keyread_only && kd.can_cover_lookup()) ||
      kd.covers_lookup(&value, m_converter->get_lookup_bitmap());

#ifndef DBUG_OFF
  m_keyread_only = save_keyread_only;
#endif
  // Due to MRR, now an index-only scan have pushed index condition.
  // (If it does, we follow non-index only code path here, except that
  //  we don't fetch the row).
  bool have_icp = (pushed_idx_cond && pushed_idx_cond_keyno == active_index);

  if (covered_lookup && m_lock_rows == RDB_LOCK_NONE && !have_icp) {
    pk_size =
        kd.get_primary_key_tuple(table, *m_pk_descr, &rkey, m_pk_packed_tuple);
    if (pk_size == RDB_INVALID_KEY_LEN) {
      rc = HA_ERR_ROCKSDB_CORRUPT_DATA;
    } else {
      rc = kd.unpack_record(table, buf, &rkey, &value,
                            m_converter->get_verify_row_debug_checksums());
    }
  } else {
    if (kd.m_is_reverse_cf) move_forward = !move_forward;

    rc = find_icp_matching_index_rec(move_forward, buf);
    if (!rc) {
      // ALTER
      // const rocksdb::Slice &rkey = m_scan_it->key();
      const rocksdb::Slice &rkey = rocksdb_Iterator__key(m_scan_it);

      pk_size = kd.get_primary_key_tuple(table, *m_pk_descr, &rkey,
                                         m_pk_packed_tuple);
      if (pk_size == RDB_INVALID_KEY_LEN) {
        rc = HA_ERR_ROCKSDB_CORRUPT_DATA;
      } else {
        if (!covered_lookup || m_lock_rows != RDB_LOCK_NONE)
          rc = get_row_by_rowid(buf, m_pk_packed_tuple, pk_size);
      }
    }
  }

  if (!rc) {
    m_last_rowkey.copy((const char *)m_pk_packed_tuple, pk_size,
                       &my_charset_bin);
  }

  rocksdb_rpc_log(10383, "read_row_from_secondary_key: end");
  return rc;
}

/**
  @note
    The problem with this function is that SQL layer calls it, when
     - the table has not been yet opened (no ::open() call done)
     - this->table_share already exists, but it is in the process of being
       filled, so some of fields are still NULL.
     - In particular, table_share->key_info[inx].key_part[] is filled only up
       to part #part. Subsequent key parts are not yet filled.

    To complicate things further, SQL layer will call index_flags() with
    all_parts=TRUE. Essentially, we're asked to provide flags for reading
    keyparts whose datatype is not yet known.

    We walk around this problem by using check_keyread_allowed(), which uses
    table_share object and is careful not to step on unitialized data.

    When we get a call with all_parts=TRUE, we try to analyze all parts but
    ignore those that have key_part->field==nullptr (these are not initialized
    yet).
*/

ulong ha_rocksdb::index_flags(uint inx, uint part, bool all_parts) const {
  rocksdb_rpc_log(10409, "index_flags: start");
  DBUG_ENTER_FUNC();

  ulong base_flags = HA_READ_NEXT |  // doesn't seem to be used
                     HA_READ_ORDER | HA_READ_RANGE | HA_READ_PREV;

  if (check_keyread_allowed(inx, part, all_parts)) {
    base_flags |= HA_KEYREAD_ONLY;
  }

  if (inx == table_share->primary_key) {
    /*
      Index-only reads on primary key are the same as table scan for us. Still,
      we need to explicitly "allow" them, otherwise SQL layer will miss some
      plans.
    */
    base_flags |= HA_KEYREAD_ONLY;
  } else {
    /*
      We can Index Condition Pushdown any key except the primary. With primary
      key, we get (pk, record) pair immediately, there is no place to put the
      ICP check.
    */
    base_flags |= HA_DO_INDEX_COND_PUSHDOWN;
  }
  rocksdb_rpc_log(10434, "index_flags: end");
  DBUG_RETURN(base_flags);
}

/**
  @brief
  Read next index tuple through the secondary index.

  @details
  m_scan_it points at the index key-value pair that we should read the (pk,row)
  pair for.
*/
int ha_rocksdb::secondary_index_read(const int keyno, uchar *const buf) {
  rocksdb_rpc_log(10447, "secondary_index_read: start");
  DBUG_ASSERT(table != nullptr);

  stats.rows_requested++;

  /* Use STATUS_NOT_FOUND when record not found or some error occurred */
  table->status = STATUS_NOT_FOUND;

  if (is_valid_iterator(m_scan_it)) {
    // ALTER
    // rocksdb::Slice key = m_scan_it->key();
    rocksdb_rpc_log(10460, "secondary_index_read: rocksdb_Iterator__key");

    rocksdb::Slice key = rocksdb_Iterator__key(m_scan_it);

    /* Check if we've ran out of records of this index */
    if (m_key_descr_arr[keyno]->covers_key(key)) {
      int rc = 0;

      // TODO: We could here check if we have ran out of range we're scanning
      const uint size = m_key_descr_arr[keyno]->get_primary_key_tuple(
          table, *m_pk_descr, &key, m_pk_packed_tuple);
      if (size == RDB_INVALID_KEY_LEN) {
        return HA_ERR_ROCKSDB_CORRUPT_DATA;
      }

      m_last_rowkey.copy((const char *)m_pk_packed_tuple, size,
                         &my_charset_bin);

      // ALTER
      // rocksdb::Slice value = m_scan_it->value();
      rocksdb_rpc_log(10479, "secondary_index_read: rocksdb_Iterator__value");
      rocksdb::Slice value = rocksdb_Iterator__value(m_scan_it);

      bool covered_lookup =
          (m_keyread_only && m_key_descr_arr[keyno]->can_cover_lookup()) ||
          m_key_descr_arr[keyno]->covers_lookup(
              &value, m_converter->get_lookup_bitmap());
      if (covered_lookup && m_lock_rows == RDB_LOCK_NONE) {
        rc = m_key_descr_arr[keyno]->unpack_record(
            table, buf, &key, &value,
            m_converter->get_verify_row_debug_checksums());
        inc_covered_sk_lookup();
      } else {
        DEBUG_SYNC(ha_thd(), "rocksdb_concurrent_delete_sk");
        rc = get_row_by_rowid(buf, m_pk_packed_tuple, size);
      }

      if (!rc) {
        table->status = 0;
        stats.rows_read++;
        stats.rows_index_next++;
        update_row_stats(ROWS_READ);
      }
      rocksdb_rpc_log(10501, "secondary_index_read: end");
      return rc;
    }
  }
  rocksdb_rpc_log(10505, "secondary_index_read: end");
  return HA_ERR_END_OF_FILE;
}

/*
  ha_rocksdb::read_range_first overrides handler::read_range_first.
  The only difference from handler::read_range_first is that
  ha_rocksdb::read_range_first passes end_key to
  ha_rocksdb::index_read_map_impl function.

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::read_range_first(const key_range *const start_key,
                                 const key_range *const end_key,
                                 bool eq_range_arg, bool sorted) {
  rocksdb_rpc_log(10522, "read_range_first: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  int result;

  eq_range = eq_range_arg;
  set_end_range(end_key, RANGE_SCAN_ASC);

  range_key_part = table->key_info[active_index].key_part;

  if (!start_key) {
    // Read first record
    result = ha_index_first(table->record[0]);
  } else {
    if (is_using_prohibited_gap_locks(
            table,
            is_using_full_unique_key(active_index, start_key->keypart_map,
                                     start_key->flag))) {
      rocksdb_rpc_log(10542, "read_range_first: end");

      DBUG_RETURN(HA_ERR_LOCK_DEADLOCK);
    }

    MYSQL_TABLE_IO_WAIT(m_psi, PSI_TABLE_FETCH_ROW, active_index, 0, {
      result =
          index_read_map_impl(table->record[0], start_key->key,
                              start_key->keypart_map, start_key->flag, end_key);
    })
  }
  if (result) {
    rocksdb_rpc_log(10554, "read_range_first: end");
    DBUG_RETURN((result == HA_ERR_KEY_NOT_FOUND) ? HA_ERR_END_OF_FILE : result);
  }

  if (compare_key(end_range) <= 0) {
    rocksdb_rpc_log(10559, "read_range_first: end");
    DBUG_RETURN(HA_EXIT_SUCCESS);
  } else {
    /*
      The last read row does not fall in the range. So request
      storage engine to release row lock if possible.
    */
    unlock_row();
    rocksdb_rpc_log(10567, "read_range_first: end");
    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }
}

/**
   @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_read_map(uchar *const buf, const uchar *const key,
                               key_part_map keypart_map,
                               enum ha_rkey_function find_flag) {
  rocksdb_rpc_log(10580, "index_read_map: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  DBUG_RETURN(index_read_map_impl(buf, key, keypart_map, find_flag, nullptr));
  rocksdb_rpc_log(10586, "index_read_map: end");
}

/*
   See storage/rocksdb/rocksdb-range-access.txt for description of how MySQL
   index navigation commands are converted into RocksDB lookup commands.

   This function takes end_key as an argument, and it is set on range scan.
   MyRocks needs to decide whether prefix bloom filter can be used or not.
   To decide to use prefix bloom filter or not, calculating equal condition
   length
   is needed. On equal lookups (find_flag == HA_READ_KEY_EXACT), equal
   condition length is the same as rocksdb::Slice.size() of the start key.
   On range scan, equal condition length is MIN(start_key, end_key) of the
   rocksdb::Slice expression.

   @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_read_map_impl(uchar *const buf, const uchar *const key,
                                    key_part_map keypart_map,
                                    enum ha_rkey_function find_flag,
                                    const key_range *end_key) {
  rocksdb_rpc_log(10610, "index_read_map_impl: start");
  DBUG_ENTER_FUNC();

  DBUG_EXECUTE_IF(
      "myrocks_busy_loop_on_row_read", int debug_i = 0;
      while (1) { debug_i++; });

  int rc = 0;

  THD *thd = ha_thd();
  DEBUG_SYNC(thd, "rocksdb.check_flags_rmi");
  if (thd && thd->killed) {
    rc = HA_ERR_QUERY_INTERRUPTED;
    rocksdb_rpc_log(10623, "index_read_map_impl: end");
    DBUG_RETURN(rc);
  }

  ha_statistic_increment(&SSV::ha_read_key_count);
  const Rdb_key_def &kd = *m_key_descr_arr[active_index];
  const uint actual_key_parts = kd.get_key_parts();
  bool using_full_key = is_using_full_key(keypart_map, actual_key_parts);

  if (!end_key) end_key = end_range;

  /* By default, we don't need the retrieved records to match the prefix */
  m_sk_match_prefix = nullptr;
  stats.rows_requested++;

  if (active_index == table->s->primary_key && find_flag == HA_READ_KEY_EXACT &&
      using_full_key) {
    /*
      Equality lookup over primary key, using full tuple.
      This is a special case, use DB::Get.
    */
    const uint size = kd.pack_index_tuple(table, m_pack_buffer,
                                          m_pk_packed_tuple, key, keypart_map);
    bool skip_lookup = is_blind_delete_enabled();

    rc = get_row_by_rowid(buf, m_pk_packed_tuple, size, skip_lookup, false);

    if (!rc && !skip_lookup) {
      stats.rows_read++;
      stats.rows_index_first++;
      update_row_stats(ROWS_READ);
    }
    rocksdb_rpc_log(10655, "index_read_map_impl: end");
    DBUG_RETURN(rc);
  }

  /*
    Unique secondary index performs lookups without the extended key fields
  */
  uint packed_size;
  if (active_index != table->s->primary_key &&
      table->key_info[active_index].flags & HA_NOSAME &&
      find_flag == HA_READ_KEY_EXACT && using_full_key) {
    key_part_map tmp_map = (key_part_map(1) << table->key_info[active_index]
                                                   .user_defined_key_parts) -
                           1;
    packed_size = kd.pack_index_tuple(table, m_pack_buffer, m_sk_packed_tuple,
                                      key, tmp_map);
    if (table->key_info[active_index].user_defined_key_parts !=
        kd.get_key_parts()) {
      using_full_key = false;
    }

    if (m_insert_with_update && m_dup_key_found &&
        active_index == m_dupp_errkey) {
      /*
        We are in INSERT ... ON DUPLICATE KEY UPDATE, and this is a read
        that SQL layer does to read the duplicate key.
        Its rowid is saved in m_last_rowkey. Get the full record and return it.
      */

      DBUG_ASSERT(m_dup_key_retrieved_record.length() >= packed_size);
      DBUG_ASSERT(memcmp(m_dup_key_retrieved_record.ptr(), m_sk_packed_tuple,
                         packed_size) == 0);

      rc = get_row_by_rowid(buf, m_last_rowkey.ptr(), m_last_rowkey.length());
      rocksdb_rpc_log(10690, "index_read_map_impl: end");
      DBUG_RETURN(rc);
    }

  } else {
    packed_size = kd.pack_index_tuple(table, m_pack_buffer, m_sk_packed_tuple,
                                      key, keypart_map);
  }
  rocksdb_rpc_log(10697, "index_read_map_impl: end");

  if ((pushed_idx_cond && pushed_idx_cond_keyno == active_index) &&
      (find_flag == HA_READ_KEY_EXACT || find_flag == HA_READ_PREFIX_LAST)) {
    /*
      We are doing a point index lookup, and ICP is enabled. It is possible
      that this call will be followed by ha_rocksdb->index_next_same() call.

      Do what InnoDB does: save the lookup tuple now. We will need it in
      index_next_same/find_icp_matching_index_rec in order to stop scanning
      as soon as index record doesn't match the lookup tuple.

      When not using ICP, handler::index_next_same() will make sure that rows
      that don't match the lookup prefix are not returned.
      row matches the lookup prefix.
    */
    m_sk_match_prefix = m_sk_match_prefix_buf;
    m_sk_match_length = packed_size;
    memcpy(m_sk_match_prefix, m_sk_packed_tuple, packed_size);
  }

  rocksdb_rpc_log(10718, "index_read_map_impl: bytes_changed_by_succ");
  int bytes_changed_by_succ = 0;
  if (find_flag == HA_READ_PREFIX_LAST_OR_PREV ||
      find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_AFTER_KEY) {
    /* See below */
    bytes_changed_by_succ = kd.successor(m_sk_packed_tuple, packed_size);
  }

  rocksdb::Slice slice(reinterpret_cast<const char *>(m_sk_packed_tuple),
                       packed_size);

  uint end_key_packed_size = 0;
  const uint eq_cond_len =
      calc_eq_cond_len(kd, find_flag, slice, bytes_changed_by_succ, end_key,
                       &end_key_packed_size);

  bool use_all_keys = false;
  if (find_flag == HA_READ_KEY_EXACT &&
      my_count_bits(keypart_map) == kd.get_key_parts()) {
    use_all_keys = true;
  }

  rocksdb_rpc_log(10718, "index_read_map_impl: get_or_create_tx");
  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  const bool is_new_snapshot = !tx->has_snapshot();
  // Loop as long as we get a deadlock error AND we end up creating the
  // snapshot here (i.e. it did not exist prior to this)
  for (;;) {
    DEBUG_SYNC(thd, "rocksdb.check_flags_rmi_scan");
    if (thd && thd->killed) {
      rc = HA_ERR_QUERY_INTERRUPTED;
      break;
    }
    /*
      This will open the iterator and position it at a record that's equal or
      greater than the lookup tuple.
    */
    setup_scan_iterator(kd, &slice, use_all_keys, eq_cond_len);

    /*
      Once we are positioned on from above, move to the position we really
      want: See storage/rocksdb/rocksdb-range-access.txt
    */
    bool move_forward;
    rc =
        position_to_correct_key(kd, find_flag, using_full_key, key, keypart_map,
                                slice, &move_forward, tx->m_snapshot_timestamp);

    if (rc) {
      break;
    }

    m_skip_scan_it_next_call = false;

    /*
      Now get the data for the row into 'buf'.  If we were using a primary key
      then we have all the rows we need.  For a secondary key we now need to
      lookup the primary key.
    */
    if (active_index == table->s->primary_key) {
      rc = read_row_from_primary_key(buf);
    } else {
      rc = read_row_from_secondary_key(buf, kd, move_forward);
    }

    if (!should_recreate_snapshot(rc, is_new_snapshot)) {
      break; /* Exit the loop */
    }

    // release the snapshot and iterator so they will be regenerated
    rocksdb_rpc_log(10790, "index_read_map_impl: release_snapshot");
    tx->release_snapshot();
    release_scan_iterator();
  }

  if (rc) {
    /*
      This status is returned on any error
      the only possible error condition is record-not-found
    */
    table->status = STATUS_NOT_FOUND;
  } else {
    table->status = 0;
    stats.rows_read++;
    stats.rows_index_first++;
    update_row_stats(ROWS_READ);
  }

  rocksdb_rpc_log(10806, "index_read_map_impl: end");

  DBUG_RETURN(rc);
}

/*
  @brief
  Scan the secondary index until we find an index record that satisfies ICP

  @param move_forward   TRUE  <=> move m_scan_it forward
                        FALSE <=> move m_scan_it backward
  @param buf            Record buffer (must be the same buffer that
                        pushed index condition points to, in practice
                        it is table->record[0])

  @detail
  Move the current iterator m_scan_it until we get an index tuple that
  satisfies the pushed Index Condition.
  (if there is no pushed index condition, return right away)

  @return
    0     - Index tuple satisfies ICP, can do index read.
    other - error code
*/

int ha_rocksdb::find_icp_matching_index_rec(const bool move_forward,
                                            uchar *const buf) {
  rocksdb_rpc_log(10833, "find_icp_matching_index_rec: start");
  if (pushed_idx_cond && pushed_idx_cond_keyno == active_index) {
    const Rdb_key_def &kd = *m_key_descr_arr[active_index];
    THD *thd = ha_thd();

    while (1) {
      int rc = rocksdb_skip_expired_records(kd, m_scan_it, !move_forward);
      if (rc != HA_EXIT_SUCCESS) {
        return rc;
      }

      if (thd && thd->killed) {
        return HA_ERR_QUERY_INTERRUPTED;
      }

      if (!is_valid_iterator(m_scan_it)) {
        table->status = STATUS_NOT_FOUND;
        return HA_ERR_END_OF_FILE;
      }
      rocksdb_rpc_log(10853, "find_icp_matching_index_rec: m_scan_it->key()");

      const rocksdb::Slice rkey = m_scan_it->key();

      if (!kd.covers_key(rkey)) {
        table->status = STATUS_NOT_FOUND;
        return HA_ERR_END_OF_FILE;
      }

      if (m_sk_match_prefix) {
        const rocksdb::Slice prefix((const char *)m_sk_match_prefix,
                                    m_sk_match_length);
        if (!kd.value_matches_prefix(rkey, prefix)) {
          table->status = STATUS_NOT_FOUND;
          return HA_ERR_END_OF_FILE;
        }
      }

      rocksdb_rpc_log(10853, "find_icp_matching_index_rec: m_scan_it->value()");

      const rocksdb::Slice value = m_scan_it->value();
      int err = kd.unpack_record(table, buf, &rkey, &value,
                                 m_converter->get_verify_row_debug_checksums());
      if (err != HA_EXIT_SUCCESS) {
        return err;
      }

      const enum icp_result icp_status = check_index_cond();
      if (icp_status == ICP_NO_MATCH) {
        rocksdb_smart_next(!move_forward, m_scan_it);
        continue; /* Get the next (or prev) index tuple */
      } else if (icp_status == ICP_OUT_OF_RANGE) {
        /* We have walked out of range we are scanning */
        table->status = STATUS_NOT_FOUND;
        rocksdb_rpc_log(10886, "find_icp_matching_index_rec: end");
        return HA_ERR_END_OF_FILE;
      } else /* icp_status == ICP_MATCH */
      {
        /* Index Condition is satisfied. We have rc==0, proceed to fetch the
         * row. */
        break;
      }
    }
  }
  rocksdb_rpc_log(10896, "find_icp_matching_index_rec: end");
  return HA_EXIT_SUCCESS;
}

/**
   @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_read_last_map(uchar *const buf, const uchar *const key,
                                    key_part_map keypart_map) {
  rocksdb_rpc_log(10907, "index_read_last_map: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  rocksdb_rpc_log(10912, "index_read_last_map: end");
  DBUG_RETURN(index_read_map(buf, key, keypart_map, HA_READ_PREFIX_LAST));
}

/**
   @return
    HA_ADMIN_OK      OK
    other            HA_ADMIN error code
*/
int ha_rocksdb::check(THD *const thd, HA_CHECK_OPT *const check_opt) {
  rocksdb_rpc_log(10922, "check: start");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(check_opt != nullptr);

  const uint pk = pk_index(table, m_tbl_def);
  String rowkey_copy;
  String sec_key_copy;
  const char *const table_name = table->s->table_name.str;

  // Only when debugging: don't use snapshot when reading
  // Rdb_transaction *tx= get_or_create_tx(table->in_use);
  // tx->snapshot= nullptr;

  rocksdb_rpc_log(10937, "check: m_converter->get_verify_row_debug_checksums");
  bool save_verify_row_debug_checksums =
      m_converter->get_verify_row_debug_checksums();
  m_converter->set_verify_row_debug_checksums(true);
  /* For each secondary index, check that we can get a PK value from it */
  // NO_LINT_DEBUG

  sql_print_information("CHECKTABLE %s: Checking table %s", table_name,
                        table_name);
  ha_rows UNINIT_VAR(row_checksums_at_start);  // set/used iff first_index==true
  ha_rows row_checksums = ha_rows(-1);
  bool first_index = true;

  for (uint keyno = 0; keyno < table->s->keys; keyno++) {
    if (keyno != pk) {
      extra(HA_EXTRA_KEYREAD);
      ha_index_init(keyno, true);
      ha_rows rows = 0;
      ha_rows checksums = 0;
      if (first_index) {
        row_checksums_at_start = m_converter->get_row_checksums_checked();
      }
      int res;
      // NO_LINT_DEBUG
      sql_print_information("CHECKTABLE %s:   Checking index %s", table_name,
                            table->key_info[keyno].name);
      while (1) {
        if (!rows) {
          res = index_first(table->record[0]);
        } else {
          res = index_next(table->record[0]);
        }

        if (res == HA_ERR_END_OF_FILE) break;
        if (res) {
          // error
          // NO_LINT_DEBUG
          sql_print_error("CHECKTABLE %s:   .. row %lld: index scan error %d",
                          table_name, rows, res);
          goto error;
        }
        rocksdb_rpc_log(10983, "check: rocksdb_Iterator__key");

        // ALTER
        // rocksdb::Slice key = m_scan_it->key();
        rocksdb::Slice key = rocksdb_Iterator__key(m_scan_it);

        sec_key_copy.copy(key.data(), key.size(), &my_charset_bin);
        rowkey_copy.copy(m_last_rowkey.ptr(), m_last_rowkey.length(),
                         &my_charset_bin);

        // if (m_key_descr_arr[keyno]->unpack_info_has_checksum(
        //         m_scan_it->value())) {
        //   checksums++;
        // }
        rocksdb_rpc_log(10992, "check: rocksdb_Iterator__value");
        if (m_key_descr_arr[keyno]->unpack_info_has_checksum(
                rocksdb_Iterator__value(m_scan_it))) {
          checksums++;
        }

        if ((res = get_row_by_rowid(table->record[0], rowkey_copy.ptr(),
                                    rowkey_copy.length()))) {
          // NO_LINT_DEBUG
          sql_print_error(
              "CHECKTABLE %s:   .. row %lld: "
              "failed to fetch row by rowid",
              table_name, rows);
          goto error;
        }

        longlong hidden_pk_id = 0;
        if (has_hidden_pk(table) &&
            read_hidden_pk_id_from_rowkey(&hidden_pk_id)) {
          goto error;
        }

        /* Check if we get the same PK value */
        uint packed_size = m_pk_descr->pack_record(
            table, m_pack_buffer, table->record[0], m_pk_packed_tuple, nullptr,
            false, hidden_pk_id);
        if (packed_size != rowkey_copy.length() ||
            memcmp(m_pk_packed_tuple, rowkey_copy.ptr(), packed_size)) {
          // NO_LINT_DEBUG
          sql_print_error("CHECKTABLE %s:   .. row %lld: PK value mismatch",
                          table_name, rows);
          goto print_and_error;
        }

        /* Check if we get the same secondary key value */
        packed_size = m_key_descr_arr[keyno]->pack_record(
            table, m_pack_buffer, table->record[0], m_sk_packed_tuple,
            &m_sk_tails, false, hidden_pk_id);
        if (packed_size != sec_key_copy.length() ||
            memcmp(m_sk_packed_tuple, sec_key_copy.ptr(), packed_size)) {
          // NO_LINT_DEBUG
          sql_print_error(
              "CHECKTABLE %s:   .. row %lld: "
              "secondary index value mismatch",
              table_name, rows);
          goto print_and_error;
        }
        rows++;
        continue;

      print_and_error : {
        std::string buf;
        buf = rdb_hexdump(rowkey_copy.ptr(), rowkey_copy.length(),
                          RDB_MAX_HEXDUMP_LEN);
        // NO_LINT_DEBUG
        sql_print_error("CHECKTABLE %s:   rowkey: %s", table_name, buf.c_str());

        // ALTER
        // buf = rdb_hexdump(m_retrieved_record.data(),
        // m_retrieved_record.size(),
        //                   RDB_MAX_HEXDUMP_LEN);

        rocksdb_rpc_log(
            11042,
            "check: rocksdb_PinnableSlice__data rocksdb_PinnableSlice__size");

        buf = rdb_hexdump(rocksdb_PinnableSlice__data(m_retrieved_record),
                          rocksdb_PinnableSlice__size(m_retrieved_record),
                          RDB_MAX_HEXDUMP_LEN);

        // NO_LINT_DEBUG
        sql_print_error("CHECKTABLE %s:   record: %s", table_name, buf.c_str());

        buf = rdb_hexdump(sec_key_copy.ptr(), sec_key_copy.length(),
                          RDB_MAX_HEXDUMP_LEN);
        // NO_LINT_DEBUG
        sql_print_error("CHECKTABLE %s:   index: %s", table_name, buf.c_str());

        goto error;
      }
      }
      // NO_LINT_DEBUG
      sql_print_information(
          "CHECKTABLE %s:   ... %lld index entries checked "
          "(%lld had checksums)",
          table_name, rows, checksums);

      if (first_index) {
        row_checksums =
            m_converter->get_row_checksums_checked() - row_checksums_at_start;
        first_index = false;
      }
      rocksdb_rpc_log(11082, "check: ha_index_end");
      ha_index_end();
    }
  }
  if (row_checksums != ha_rows(-1)) {
    // NO_LINT_DEBUG
    sql_print_information("CHECKTABLE %s:   %lld table records had checksums",
                          table_name, row_checksums);
  }
  extra(HA_EXTRA_NO_KEYREAD);

  rocksdb_rpc_log(11095, "check: set_verify_row_debug_checksums");

  m_converter->set_verify_row_debug_checksums(save_verify_row_debug_checksums);
  /*
    TODO(sergiy): we should check also for PK records that are missing in
    the secondary indexes.
    For that, need to walk through the PK and check that every PK record has a
    proper counterpart in each secondary index.
  */
  rocksdb_rpc_log(11095, "check: end");
  DBUG_RETURN(HA_ADMIN_OK);
error:
  m_converter->set_verify_row_debug_checksums(save_verify_row_debug_checksums);
  ha_index_or_rnd_end();
  extra(HA_EXTRA_NO_KEYREAD);
  rocksdb_rpc_log(11110, "check: end");
  DBUG_RETURN(HA_ADMIN_CORRUPT);
}

static void dbug_dump_str(FILE *const out, const char *const str, int len) {
  fprintf(out, "\"");
  for (int i = 0; i < len; i++) {
    if (str[i] > 32) {
      fprintf(out, "%c", str[i]);
    } else {
      fprintf(out, "\\%d", str[i]);
    }
  }
  fprintf(out, "\"");
}

/*
  Debugging help: dump the whole database into a human-readable file.
  Usage:
    dbug_dump_database(rdb);
*/

void dbug_dump_database(rocksdb::DB *const db) {
  rocksdb_rpc_log(11133, "dbug_dump_database: sstart");

  FILE *const out = fopen("/tmp/rocksdb.dump", "wt");
  if (!out) return;

  // ALTER
  // rocksdb::Iterator *it = db->NewIterator(rocksdb::ReadOptions());
  // for (it->SeekToFirst(); it->Valid(); it->Next()) {
  //   rocksdb::Slice key = it->key();
  //   rocksdb::Slice val = it->value();
  //   dbug_dump_str(out, key.data(), key.size());
  //   fprintf(out, " -> ");
  //   dbug_dump_str(out, val.data(), val.size());
  //   fprintf(out, "\n");
  // }

  rocksdb::ReadOptions *readopt = rocksdb_ReadOptions__NewReadOptions();
  rocksdb::Iterator *it = rocksdb_DB__NewIterator(db, readopt);
  for (rocksdb_Iterator__SeekToFirst(it); rocksdb_Iterator__Valid(it);
       rocksdb_Iterator__Next(it)) {
    rocksdb::Slice key = rocksdb_Iterator__key(it);
    rocksdb::Slice val = rocksdb_Iterator__value(it);
    dbug_dump_str(out, key.data(), key.size());
    fprintf(out, " -> ");
    dbug_dump_str(out, val.data(), val.size());
    fprintf(out, "\n");
  }

  // delete it;
  rocksdb_Iterator__delete(it);

  fclose(out);
  rocksdb_rpc_log(11163, "dbug_dump_database: end");
}

rocksdb::Status ha_rocksdb::get_for_update(
    Rdb_transaction *const tx, const Rdb_key_def &key_descr,
    const rocksdb::Slice &key, rocksdb::PinnableSlice *&value) const {
  rocksdb_rpc_log(11169, "get_for_update: start");
  DBUG_ASSERT(m_lock_rows != RDB_LOCK_NONE);

  bool exclusive = m_lock_rows != RDB_LOCK_READ;
  bool do_validate = my_core::thd_tx_isolation(ha_thd()) > ISO_READ_COMMITTED;
  rocksdb_rpc_log(11169, "get_for_update: tx->get_for_update");

  rocksdb::Status s =
      tx->get_for_update(key_descr, key, value, exclusive, do_validate);

#ifndef DBUG_OFF
  ++rocksdb_num_get_for_update_calls;
#endif
  rocksdb_rpc_log(11182, "get_for_update: end");
  return s;
}

bool ha_rocksdb::is_blind_delete_enabled() {
  rocksdb_rpc_log(11187, "is_blind_delete_enabled: start");
  THD *thd = ha_thd();
  return (THDVAR(thd, blind_delete_primary_key) &&
          thd->lex->sql_command == SQLCOM_DELETE &&
          thd->lex->table_count == 1 && table->s->keys == 1 &&
          !has_hidden_pk(table) && !thd->rli_slave);
}

/*
  Given a rowid (i.e. packed PK) as a parameter, get the record.

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/

int ha_rocksdb::get_row_by_rowid(uchar *const buf, const char *const rowid,
                                 const uint rowid_size, const bool skip_lookup,
                                 const bool skip_ttl_check) {
  rocksdb_rpc_log(11206, "get_row_by_rowid: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(table != nullptr);

  int rc;

  rocksdb::Slice key_slice(rowid, rowid_size);

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  DBUG_ASSERT(tx != nullptr);

  DEBUG_SYNC(ha_thd(), "rocksdb.get_row_by_rowid");
  DBUG_EXECUTE_IF("dbug.rocksdb.get_row_by_rowid", {
    THD *thd = ha_thd();
    const char act[] =
        "now signal Reached "
        "wait_for signal.rocksdb.get_row_by_rowid_let_running";
    DBUG_ASSERT(opt_debug_sync_timeout > 0);
    DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
  };);

  bool found;
  rocksdb::Status s;

  /* Pretend row found without looking up */
  if (skip_lookup) {
    stats.rows_deleted_blind++;
    update_row_stats(ROWS_DELETED_BLIND);
    m_last_rowkey.copy((const char *)rowid, rowid_size, &my_charset_bin);
    table->status = 0;
    rocksdb_rpc_log(11238, "get_row_by_rowid: end");
    DBUG_RETURN(0);
  }

  if (m_lock_rows == RDB_LOCK_NONE) {
    tx->acquire_snapshot(true);

    // ALTER
    // s = tx->get(m_pk_descr->get_cf(), key_slice, &m_retrieved_record);
    rocksdb_rpc_log(11247, "get_row_by_rowid: tx->get");
    s = tx->get(m_pk_descr->get_cf(), key_slice, m_retrieved_record);

  } else if (m_insert_with_update && m_dup_key_found &&
             m_pk_descr->get_keyno() == m_dupp_errkey) {
    // ALTER
    // DBUG_ASSERT(m_dup_key_retrieved_record.length() ==
    //             m_retrieved_record.size());
    // DBUG_ASSERT(memcmp(m_dup_key_retrieved_record.ptr(),
    //                    m_retrieved_record.data(),
    //                    m_retrieved_record.size()) == 0);
    DBUG_ASSERT(m_dup_key_retrieved_record.length() ==
                rocksdb_PinnableSlice__size(m_retrieved_record));
    DBUG_ASSERT(memcmp(m_dup_key_retrieved_record.ptr(),
                       rocksdb_PinnableSlice__data(m_retrieved_record),
                       rocksdb_PinnableSlice__size(m_retrieved_record)) == 0);

    // do nothing - we already have the result in m_retrieved_record and
    // already taken the lock
    rocksdb_rpc_log(11267, "get_row_by_rowid: rocksdb::Status::OK()");
    s = rocksdb::Status::OK();
  } else {
    // ALTER
    // s = get_for_update(tx, *m_pk_descr, key_slice, &m_retrieved_record);
    rocksdb_rpc_log(11272, "get_row_by_rowid: get_for_update");
    s = get_for_update(tx, *m_pk_descr, key_slice, m_retrieved_record);
  }

  DBUG_EXECUTE_IF("rocksdb_return_status_corrupted",
                  dbug_change_status_to_corrupted(&s););

  if (!s.IsNotFound() && !s.ok()) {
    DBUG_RETURN(tx->set_status_error(table->in_use, s, *m_pk_descr, m_tbl_def,
                                     m_table_handler));
  }
  found = !s.IsNotFound();

  rocksdb_rpc_log(11286, "get_row_by_rowid: table->status = STATUS_NOT_FOUND");

  table->status = STATUS_NOT_FOUND;
  if (found) {
    /* If we found the record, but it's expired, pretend we didn't find it.  */
    // ALTER
    // if (!skip_ttl_check && m_pk_descr->has_ttl() &&
    //     should_hide_ttl_rec(*m_pk_descr, m_retrieved_record,
    //                         tx->m_snapshot_timestamp)) {
    //   DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
    // }
    if (!skip_ttl_check && m_pk_descr->has_ttl() &&
        should_hide_ttl_rec(*m_pk_descr,
                            rocksdb_PinnableSlice__Slice(m_retrieved_record),
                            tx->m_snapshot_timestamp)) {
      rocksdb_rpc_log(11286, "get_row_by_rowid: rocksdb_PinnableSlice__Slice");
      DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
    }

    m_last_rowkey.copy((const char *)rowid, rowid_size, &my_charset_bin);
    rc = convert_record_from_storage_format(&key_slice, buf);

    if (!rc) {
      table->status = 0;
    }
  } else {
    /*
      Note: we don't need to unlock the row. It is intentional that we keep
      locks on rows that don't exist.
    */
    rc = HA_ERR_KEY_NOT_FOUND;
  }
  rocksdb_rpc_log(11318, "get_row_by_rowid: end");

  DBUG_RETURN(rc);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_next(uchar *const buf) {
  rocksdb_rpc_log(11327, "index_next: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  bool moves_forward = true;
  ha_statistic_increment(&SSV::ha_read_next_count);
  if (m_key_descr_arr[active_index]->m_is_reverse_cf) {
    moves_forward = false;
  }

  int rc = index_next_with_direction(buf, moves_forward);
  if (rc == HA_ERR_KEY_NOT_FOUND) rc = HA_ERR_END_OF_FILE;
  rocksdb_rpc_log(11340, "index_next: end");
  DBUG_RETURN(rc);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_prev(uchar *const buf) {
  rocksdb_rpc_log(11350, "index_prev: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  bool moves_forward = false;
  ha_statistic_increment(&SSV::ha_read_prev_count);
  if (m_key_descr_arr[active_index]->m_is_reverse_cf) {
    moves_forward = true;
  }

  int rc = index_next_with_direction(buf, moves_forward);
  if (rc == HA_ERR_KEY_NOT_FOUND) rc = HA_ERR_END_OF_FILE;

  rocksdb_rpc_log(11364, "index_prev: end");
  DBUG_RETURN(rc);
}

int ha_rocksdb::index_next_with_direction(uchar *const buf, bool move_forward) {
  rocksdb_rpc_log(11369, "index_next_with_direction: start");
  DBUG_ENTER_FUNC();

  int rc;

  if (active_index == pk_index(table, m_tbl_def)) {
    rc = rnd_next_with_direction(buf, move_forward);
  } else {
    THD *thd = ha_thd();
    for (;;) {
      DEBUG_SYNC(thd, "rocksdb.check_flags_inwd");
      if (thd && thd->killed) {
        rc = HA_ERR_QUERY_INTERRUPTED;
        break;
      }
      if (m_skip_scan_it_next_call) {
        m_skip_scan_it_next_call = false;
      } else {
        if (move_forward) {
          // ALTER
          // m_scan_it->Next(); /* this call cannot fail */
          rocksdb_rpc_log(11390,
                          "index_next_with_direction: rocksdb_Iterator__Next");
          rocksdb_Iterator__Next(m_scan_it);
        } else {
          // ALTER
          // m_scan_it->Prev();
          rocksdb_rpc_log(11396,
                          "index_next_with_direction: rocksdb_Iterator__Prev");
          rocksdb_Iterator__Prev(m_scan_it);
        }
      }
      rc = rocksdb_skip_expired_records(*m_key_descr_arr[active_index],
                                        m_scan_it, !move_forward);
      if (rc != HA_EXIT_SUCCESS) {
        break;
      }
      rc = find_icp_matching_index_rec(move_forward, buf);
      if (!rc) rc = secondary_index_read(active_index, buf);
      if (!should_skip_invalidated_record(rc)) {
        break;
      }
    }
  }

  DBUG_RETURN(rc);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_first(uchar *const buf) {
  rocksdb_rpc_log(11423, "index_first: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  m_sk_match_prefix = nullptr;
  ha_statistic_increment(&SSV::ha_read_first_count);
  int rc = m_key_descr_arr[active_index]->m_is_reverse_cf
               ? index_last_intern(buf)
               : index_first_intern(buf);
  if (rc == HA_ERR_KEY_NOT_FOUND) rc = HA_ERR_END_OF_FILE;

  rocksdb_rpc_log(11435, "index_first: end");

  DBUG_RETURN(rc);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_last(uchar *const buf) {
  rocksdb_rpc_log(11446, "index_last: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  m_sk_match_prefix = nullptr;
  ha_statistic_increment(&SSV::ha_read_last_count);
  int rc = m_key_descr_arr[active_index]->m_is_reverse_cf
               ? index_first_intern(buf)
               : index_last_intern(buf);
  if (rc == HA_ERR_KEY_NOT_FOUND) rc = HA_ERR_END_OF_FILE;

  rocksdb_rpc_log(11458, "index_last: end");
  DBUG_RETURN(rc);
}

/*
  Start scanning from the "first" value.

  The 'first' here means "the first from start of the key space".
  For reverse-ordered key spaces, the first value will be the biggest, 'zzzz'.

  An picture of a forward-ordered keyspace (remember, the keys have form
  'indexnr-keyval'. Suppose the index we are at has number n)

      (n-1) - ...
      ( n )          <--- 1. (n) doesn't exist in the db but it would be here.
      ( n ) - aaa       <--- 2. Seek("n") will put us here on the first index
      ( n ) - bbb               record.
      ( n ) - cc

  So, need to do: Seek(n);

  A backward-ordered keyspace:

      (n+1) - bbb
      (n+1) - aaa
      (n+1)        <--- (n+1) doesn't exist in the db but would be here.
      ( n ) - ccc       <--- 1. We need to be here.
      ( n ) - bbb
      ( n ) - aaa
      ( n )

  So, need to: Seek(n+1);

*/

int ha_rocksdb::index_first_intern(uchar *const buf) {
  rocksdb_rpc_log(11494, "index_first_intern: start");
  DBUG_ENTER_FUNC();

  uchar *key;
  uint key_size;
  int rc;

  if (is_pk(active_index, table, m_tbl_def)) {
    key = m_pk_packed_tuple;
  } else {
    key = m_sk_packed_tuple;
  }

  DBUG_ASSERT(key != nullptr);

  const Rdb_key_def &kd = *m_key_descr_arr[active_index];
  int key_start_matching_bytes = kd.get_first_key(key, &key_size);

  rocksdb::Slice index_key((const char *)key, key_size);

  rocksdb_rpc_log(11514, "index_first_intern: get_or_create_tx");

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  DBUG_ASSERT(tx != nullptr);

  const bool is_new_snapshot = !tx->has_snapshot();
  // Loop as long as we get a deadlock error AND we end up creating the
  // snapshot here (i.e. it did not exist prior to this)
  for (;;) {
    setup_scan_iterator(kd, &index_key, false, key_start_matching_bytes);

    // ALTER
    // m_scan_it->Seek(index_key);
    // m_scan_it->Seek(index_key);
    rocksdb_rpc_log(11528, "index_first_intern: rocksdb_Iterator__Seek");
    rocksdb_Iterator__Seek(m_scan_it, index_key);
    m_skip_scan_it_next_call = true;

    rc = index_next_with_direction(buf, true);
    if (!should_recreate_snapshot(rc, is_new_snapshot)) {
      break; /* exit the loop */
    }

    // release the snapshot and iterator so they will be regenerated
    rocksdb_rpc_log(11538, "index_first_intern: tx->release_snapshot()");
    tx->release_snapshot();
    release_scan_iterator();
  }

  if (!rc) {
    /*
      index_next is always incremented on success, so decrement if it is
      index_first instead
     */
    stats.rows_index_first++;
    stats.rows_index_next--;
  }
  rocksdb_rpc_log(11553, "index_first_intern: end");

  DBUG_RETURN(rc);
}

/**
  @details
  Start scanning from the "last" value

  The 'last' here means "the last from start of the key space".
  For reverse-ordered key spaces, we will actually read the smallest value.

  An picture of a forward-ordered keyspace (remember, the keys have form
  'indexnr-keyval'. Suppose the we are at a key that has number n)

     (n-1)-something
     ( n )-aaa
     ( n )-bbb
     ( n )-ccc            <----------- Need to seek to here.
     (n+1)      <---- Doesn't exist, but would be here.
     (n+1)-smth, or no value at all

   RocksDB's Iterator::SeekForPrev($val) seeks to "at $val or last value that's
   smaller". We can't seek to "(n)-ccc" directly, because we don't know what
   is the value of 'ccc' (the biggest record with prefix (n)). Instead, we seek
   to "(n+1)", which is the least possible value that's greater than any value
   in index #n.

   So, need to:  it->SeekForPrev(n+1)

   A backward-ordered keyspace:

      (n+1)-something
      ( n ) - ccc
      ( n ) - bbb
      ( n ) - aaa       <---------------- (*) Need to seek here.
      ( n ) <--- Doesn't exist, but would be here.
      (n-1)-smth, or no value at all

   So, need to:  it->SeekForPrev(n)
*/

int ha_rocksdb::index_last_intern(uchar *const buf) {
  rocksdb_rpc_log(11553, "index_last_intern: start");
  DBUG_ENTER_FUNC();

  uchar *key;
  uint key_size;
  int rc;

  if (is_pk(active_index, table, m_tbl_def)) {
    key = m_pk_packed_tuple;
  } else {
    key = m_sk_packed_tuple;
  }

  rocksdb_rpc_log(11607, "index_last_intern: start");
  DBUG_ASSERT(key != nullptr);

  const Rdb_key_def &kd = *m_key_descr_arr[active_index];
  int key_end_matching_bytes = kd.get_last_key(key, &key_size);

  rocksdb::Slice index_key((const char *)key, key_size);

  rocksdb_rpc_log(11617, "index_last_intern: get_or_create_tx");
  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  DBUG_ASSERT(tx != nullptr);

  bool is_new_snapshot = !tx->has_snapshot();
  // Loop as long as we get a deadlock error AND we end up creating the
  // snapshot here (i.e. it did not exist prior to this)
  for (;;) {
    setup_scan_iterator(kd, &index_key, false, key_end_matching_bytes);

    // ALTER
    // m_scan_it->SeekForPrev(index_key);
    rocksdb_rpc_log(11627, "index_last_intern: rocksdb_Iterator__SeekForPrev");
    rocksdb_Iterator__SeekForPrev(m_scan_it, index_key);
    m_skip_scan_it_next_call = false;

    if (is_pk(active_index, table, m_tbl_def)) {
      m_skip_scan_it_next_call = true;
      rc = rnd_next_with_direction(buf, false);
    } else {
      rc = find_icp_matching_index_rec(false /*move_forward*/, buf);
      if (!rc) rc = secondary_index_read(active_index, buf);
    }

    if (!should_recreate_snapshot(rc, is_new_snapshot)) {
      break; /* exit the loop */
    }

    // release the snapshot and iterator so they will be regenerated
    rocksdb_rpc_log(11644, "index_last_intern: release_snapshot");
    tx->release_snapshot();
    release_scan_iterator();
  }

  if (!rc) {
    /*
      index_next is always incremented on success, so decrement if it is
      index_first instead
     */
    stats.rows_index_first++;
    stats.rows_index_next--;
  }
  rocksdb_rpc_log(11659, "index_last_intern: end");

  DBUG_RETURN(rc);
}

void ha_rocksdb::unlock_row() {
  rocksdb_rpc_log(11663, "unlock_row: start");
  DBUG_ENTER_FUNC();

  if (m_lock_rows != RDB_LOCK_NONE) {
    Rdb_transaction *const tx = get_or_create_tx(table->in_use);
    tx->release_lock(*m_pk_descr,
                     std::string(m_last_rowkey.ptr(), m_last_rowkey.length()));
  }

  rocksdb_rpc_log(11672, "unlock_row: end");
  DBUG_VOID_RETURN;
}

/*
  Returning true if SingleDelete can be used.
  - Secondary Indexes can always use SingleDelete.
  - If the index is PRIMARY KEY, and if all of the columns of the table
    are covered by the PRIMARY KEY, SingleDelete can be used.
*/
bool ha_rocksdb::can_use_single_delete(const uint index) const {
  rocksdb_rpc_log(11683, "can_use_single_delete: begin");
  return (index != pk_index(table, m_tbl_def) ||
          (!has_hidden_pk(table) &&
           table->key_info[index].actual_key_parts == table->s->fields));
}

bool ha_rocksdb::skip_unique_check() const {
  rocksdb_rpc_log(11690, "skip_unique_check: begin");
  /*
    We want to skip unique checks if:
      1) bulk_load is on
      2) this table is in the whitelist of tables to skip and the replication
         lag has reached a large enough value (see unique_check_lag_threshold
         and unique_check_lage_reset_threshold)
      3) the user set unique_checks option to 0, and the table does not have
         any indexes. If the table has secondary keys, then those might becomes
         inconsisted/corrupted
      4) We're using read-free replication
  */
  return THDVAR(table->in_use, bulk_load) ||
         (m_force_skip_unique_check && m_skip_unique_check) ||
         (my_core::thd_test_options(table->in_use,
                                    OPTION_RELAXED_UNIQUE_CHECKS) &&
          m_tbl_def->m_key_count == 1) ||
         use_read_free_rpl();
}

void ha_rocksdb::set_force_skip_unique_check(bool skip) {
  rocksdb_rpc_log(11711, "set_force_skip_unique_check: begin");
  DBUG_ENTER_FUNC();

  m_force_skip_unique_check = skip;

  rocksdb_rpc_log(11716, "set_force_skip_unique_check: end");
  DBUG_VOID_RETURN;
}

bool ha_rocksdb::commit_in_the_middle() {
  rocksdb_rpc_log(11721, "commit_in_the_middle: begin");
  return THDVAR(table->in_use, bulk_load) ||
         THDVAR(table->in_use, commit_in_the_middle);
}

/*
  Executing bulk commit if it should.
  @retval true if bulk commit failed
  @retval false if bulk commit was skipped or succeeded
*/
bool ha_rocksdb::do_bulk_commit(Rdb_transaction *const tx) {
  rocksdb_rpc_log(11732, "do_bulk_commit: begin");
  return commit_in_the_middle() &&
         tx->get_write_count() >= THDVAR(table->in_use, bulk_load_size) &&
         tx->flush_batch();
}

/*
  If table was created without primary key, SQL layer represents the primary
  key number as MAX_INDEXES.  Hence, this function returns true if the table
  does not contain a primary key. (In which case we generate a hidden
  'auto-incremented' pk.)
*/
bool ha_rocksdb::has_hidden_pk(const TABLE *const table) const {
  rocksdb_rpc_log(11745, "has_hidden_pk: begin");
  return Rdb_key_def::table_has_hidden_pk(table);
}

/*
  Returns true if given index number is a hidden_pk.
  - This is used when a table is created with no primary key.
*/
bool ha_rocksdb::is_hidden_pk(const uint index, const TABLE *const table_arg,
                              const Rdb_tbl_def *const tbl_def_arg) {
  rocksdb_rpc_log(11755, "has_hidden_pk: begin");
  DBUG_ASSERT(table_arg->s != nullptr);

  return (table_arg->s->primary_key == MAX_INDEXES &&
          index == tbl_def_arg->m_key_count - 1);
}

/* Returns index of primary key */
uint ha_rocksdb::pk_index(const TABLE *const table_arg,
                          const Rdb_tbl_def *const tbl_def_arg) {
  rocksdb_rpc_log(11765, "pk_index: begin");
  DBUG_ASSERT(table_arg->s != nullptr);

  return table_arg->s->primary_key == MAX_INDEXES ? tbl_def_arg->m_key_count - 1
                                                  : table_arg->s->primary_key;
}

/* Returns true if given index number is a primary key */
bool ha_rocksdb::is_pk(const uint index, const TABLE *const table_arg,
                       const Rdb_tbl_def *const tbl_def_arg) {
  rocksdb_rpc_log(11775, "is_pk: begin");
  DBUG_ASSERT(table_arg->s != nullptr);

  return index == table_arg->s->primary_key ||
         is_hidden_pk(index, table_arg, tbl_def_arg);
}

uint ha_rocksdb::max_supported_key_part_length() const {
  rocksdb_rpc_log(11783, "max_supported_key_part_length: begin");
  DBUG_ENTER_FUNC();
  DBUG_RETURN(rocksdb_large_prefix ? MAX_INDEX_COL_LEN_LARGE
                                   : MAX_INDEX_COL_LEN_SMALL);
}

const char *ha_rocksdb::get_key_name(const uint index,
                                     const TABLE *const table_arg,
                                     const Rdb_tbl_def *const tbl_def_arg) {
  rocksdb_rpc_log(11792, "get_key_name: begin");

  if (is_hidden_pk(index, table_arg, tbl_def_arg)) {
    rocksdb_rpc_log(11795, "get_key_name: end");
    return HIDDEN_PK_NAME;
  }

  DBUG_ASSERT(table_arg->key_info != nullptr);
  DBUG_ASSERT(table_arg->key_info[index].name != nullptr);

  rocksdb_rpc_log(11802, "get_key_name: end");
  return table_arg->key_info[index].name;
}

const char *ha_rocksdb::get_key_comment(const uint index,
                                        const TABLE *const table_arg,
                                        const Rdb_tbl_def *const tbl_def_arg) {
  rocksdb_rpc_log(11802, "get_key_name: begin");

  if (is_hidden_pk(index, table_arg, tbl_def_arg)) {
    rocksdb_rpc_log(11812, "get_key_name: end");
    return nullptr;
  }

  DBUG_ASSERT(table_arg->key_info != nullptr);
  rocksdb_rpc_log(11817, "get_key_name: end");
  return table_arg->key_info[index].comment.str;
}

const std::string ha_rocksdb::generate_cf_name(
    const uint index, const TABLE *const table_arg,
    const Rdb_tbl_def *const tbl_def_arg, bool *per_part_match_found) {
  rocksdb_rpc_log(11824, "generate_cf_name: start");
  DBUG_ASSERT(table_arg != nullptr);
  DBUG_ASSERT(tbl_def_arg != nullptr);
  DBUG_ASSERT(per_part_match_found != nullptr);

  // When creating CF-s the caller needs to know if there was a custom CF name
  // specified for a given paritition.
  *per_part_match_found = false;

  // Index comment is used to define the column family name specification(s).
  // If there was no comment, we get an emptry string, and it means "use the
  // default column family".
  const char *const comment = get_key_comment(index, table_arg, tbl_def_arg);

  // `get_key_comment` can return `nullptr`, that's why this.
  std::string key_comment = comment ? comment : "";

  std::string cf_name = Rdb_key_def::parse_comment_for_qualifier(
      key_comment, table_arg, tbl_def_arg, per_part_match_found,
      RDB_CF_NAME_QUALIFIER);

  if (table_arg->part_info != nullptr && !*per_part_match_found) {
    // At this point we tried to search for a custom CF name for a partition,
    // but none was specified. Therefore default one will be used.
    rocksdb_rpc_log(11848, "generate_cf_name: end");
    return "";
  }

  // If we didn't find any partitioned/non-partitioned qualifiers, return the
  // comment itself.  NOTE: this currently handles returning the cf name
  // specified in the index comment in the case of no partitions, which doesn't
  // use any qualifiers at the moment. (aka its a special case)
  if (cf_name.empty() && !key_comment.empty()) {
    rocksdb_rpc_log(11857, "generate_cf_name: end");

    return key_comment;
  }
  rocksdb_rpc_log(11861, "generate_cf_name: end");

  return cf_name;
}

const std::string ha_rocksdb::get_table_comment(const TABLE *const table_arg) {
  DBUG_ASSERT(table_arg->s != nullptr);
  rocksdb_rpc_log(11868, "get_table_comment: begin");

  return table_arg->s->comment.str;
}

/**
  Write a new row

  @param[in] buf                new row data to write
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::write_row(uchar *const buf) {
  rocksdb_rpc_log(11882, "write_row: begin");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(buf != nullptr);
  DBUG_ASSERT(buf == table->record[0]);
  DBUG_ASSERT(m_lock_rows == RDB_LOCK_WRITE);

  ha_statistic_increment(&SSV::ha_write_count);
  /*
    Note: "buf == table->record[0]" is copied from innodb. I am not aware of
    any use cases where this condition is not true.
  */
  if (table->next_number_field && buf == table->record[0]) {
    int err;
    if ((err = update_auto_increment())) {
      rocksdb_rpc_log(11898, "write_row: end");

      DBUG_RETURN(err);
    }
  }

  // clear cache at beginning of write for INSERT ON DUPLICATE
  // we may get multiple write->fail->read->update if there are multiple
  // values from INSERT
  m_dup_key_found = false;

  rocksdb_rpc_log(11911, "write_row: update_write_row");

  const int rv = update_write_row(nullptr, buf, skip_unique_check());

  if (rv == 0) {
    stats.rows_inserted++;

    // Not protected by ddl_manger lock for performance
    // reasons. This is an estimate value anyway.
    inc_table_n_rows();
    update_table_stats_if_needed();

    update_row_stats(ROWS_INSERTED);
  }

  rocksdb_rpc_log(11924, "write_row: end");

  DBUG_RETURN(rv);
}

// Increment the number of rows in the table by one.
// This operation is not protected by ddl manager lock.
// The number is estimated.
void ha_rocksdb::inc_table_n_rows() {
  rocksdb_rpc_log(11924, "inc_table_n_rows: start");

  if (!rocksdb_table_stats_use_table_scan) {
    rocksdb_rpc_log(11936, "inc_table_n_rows: end");

    return;
  }

  uint64 n_rows = m_tbl_def->m_tbl_stats.m_stat_n_rows;
  if (n_rows < std::numeric_limits<ulonglong>::max()) {
    m_tbl_def->m_tbl_stats.m_stat_n_rows = n_rows + 1;
  }
  rocksdb_rpc_log(11945, "inc_table_n_rows: end");
}

// Decrement the number of rows in the table by one.
// This operation is not protected by ddl manager lock.
// The number is estimated.
void ha_rocksdb::dec_table_n_rows() {
  rocksdb_rpc_log(11952, "dec_table_n_rows: start");

  if (!rocksdb_table_stats_use_table_scan) {
    rocksdb_rpc_log(11955, "dec_table_n_rows: end");
    return;
  }

  uint64 n_rows = m_tbl_def->m_tbl_stats.m_stat_n_rows;
  if (n_rows > 0) {
    m_tbl_def->m_tbl_stats.m_stat_n_rows = n_rows - 1;
  }
  rocksdb_rpc_log(11963, "dec_table_n_rows: end");
}

/**
  Constructing m_last_rowkey (MyRocks key expression) from
  before_update|delete image (MySQL row expression).
  m_last_rowkey is normally set during lookup phase, such as
  rnd_next_with_direction() and rnd_pos(). With Read Free Replication,
  these read functions are skipped and update_rows(), delete_rows() are
  called without setting m_last_rowkey. This function sets m_last_rowkey
  for Read Free Replication.
*/
void ha_rocksdb::set_last_rowkey(const uchar *const old_data) {
  rocksdb_rpc_log(11976, "set_last_rowkey: start");
  if (old_data && use_read_free_rpl()) {
    const int old_pk_size = m_pk_descr->pack_record(
        table, m_pack_buffer, old_data, m_pk_packed_tuple, nullptr, false);
    m_last_rowkey.copy((const char *)m_pk_packed_tuple, old_pk_size,
                       &my_charset_bin);
  }
  rocksdb_rpc_log(11983, "set_last_rowkey: end");
}

/**
  Collect update data for primary key

  @param[in, out] row_info            hold all data for update row, such as
                                      new row data/old row data
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::get_pk_for_update(struct update_row_info *const row_info) {
  rocksdb_rpc_log(11997, "get_pk_for_update: start");
  int size;

  /*
    Get new row key for any insert, and any update where the pk is not hidden.
    Row key for updates with hidden pk is handled below.
  */
  if (!has_hidden_pk(table)) {
    row_info->hidden_pk_id = 0;

    row_info->new_pk_unpack_info = &m_pk_unpack_info;

    size = m_pk_descr->pack_record(
        table, m_pack_buffer, row_info->new_data, m_pk_packed_tuple,
        row_info->new_pk_unpack_info, false, 0, 0, nullptr);
  } else if (row_info->old_data == nullptr) {
    row_info->hidden_pk_id = update_hidden_pk_val();
    size =
        m_pk_descr->pack_hidden_pk(row_info->hidden_pk_id, m_pk_packed_tuple);
  } else {
    /*
      If hidden primary key, rowkey for new record will always be the same as
      before
    */
    size = row_info->old_pk_slice.size();
    memcpy(m_pk_packed_tuple, row_info->old_pk_slice.data(), size);
    int err = read_hidden_pk_id_from_rowkey(&row_info->hidden_pk_id);
    if (err) {
      rocksdb_rpc_log(12024, "get_pk_for_update: end");

      return err;
    }
  }

  row_info->new_pk_slice =
      rocksdb::Slice((const char *)m_pk_packed_tuple, size);

  rocksdb_rpc_log(12033, "get_pk_for_update: end");

  return HA_EXIT_SUCCESS;
}

/**
   Check the specified primary key value is unique and also lock the row

  @param[in] key_id           key index
  @param[in] row_info         hold all data for update row, such as old row
                              data and new row data
  @param[out] found           whether the primary key exists before.
  @param[out] pk_changed      whether primary key is changed
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::check_and_lock_unique_pk(const uint key_id,
                                         const struct update_row_info &row_info,
                                         bool *const found,
                                         const bool skip_unique_check) {
  rocksdb_rpc_log(12054, "check_and_lock_unique_pk: start");

  DBUG_ASSERT(found != nullptr);

  DBUG_ASSERT(row_info.old_pk_slice.size() == 0 ||
              row_info.new_pk_slice.compare(row_info.old_pk_slice) != 0);

  /* Ignore PK violations if this is a optimized 'replace into' */
  rocksdb_rpc_log(12063, "check_and_lock_unique_pk: start");
  const bool ignore_pk_unique_check =
      ha_thd()->lex->blind_replace_into || skip_unique_check;

  /*
    Perform a read to determine if a duplicate entry exists. For primary
    keys, a point lookup will be sufficient.

    note: we intentionally don't set options.snapshot here. We want to read
    the latest committed data.
  */

  /*
    To prevent race conditions like below, it is necessary to
    take a lock for a target row. get_for_update() holds a gap lock if
    target key does not exist, so below conditions should never
    happen.

    1) T1 Get(empty) -> T2 Get(empty) -> T1 Put(insert) -> T1 commit
       -> T2 Put(overwrite) -> T2 commit
    2) T1 Get(empty) -> T1 Put(insert, not committed yet) -> T2 Get(empty)
       -> T2 Put(insert, blocked) -> T1 commit -> T2 commit(overwrite)
  */

  // ALTER
  // const rocksdb::Status s =
  //     get_for_update(row_info.tx, *m_pk_descr, row_info.new_pk_slice,
  //                    ignore_pk_unique_check ? nullptr : &m_retrieved_record);

  rocksdb_rpc_log(12093, "check_and_lock_unique_pk: get_for_update");

  rocksdb::PinnableSlice *ps = nullptr;
  const rocksdb::Status s =
      get_for_update(row_info.tx, *m_pk_descr, row_info.new_pk_slice,
                     ignore_pk_unique_check ? ps : m_retrieved_record);

  if (!s.ok() && !s.IsNotFound()) {
    return row_info.tx->set_status_error(
        table->in_use, s, *m_key_descr_arr[key_id], m_tbl_def, m_table_handler);
  }

  bool key_found = ignore_pk_unique_check ? false : !s.IsNotFound();

  /*
    If the pk key has ttl, we may need to pretend the row wasn't
    found if it is already expired.
  */

  // ALTER
  // if (key_found && m_pk_descr->has_ttl() &&
  //     should_hide_ttl_rec(*m_pk_descr, m_retrieved_record,
  //                         (row_info.tx->m_snapshot_timestamp
  //                              ? row_info.tx->m_snapshot_timestamp
  //                              : static_cast<int64_t>(std::time(nullptr)))))
  //                              {
  //   key_found = false;
  // }
  rocksdb_rpc_log(12118,
                  "check_and_lock_unique_pk: rocksdb_PinnableSlice__Slice");
  if (key_found && m_pk_descr->has_ttl() &&
      should_hide_ttl_rec(*m_pk_descr,
                          rocksdb_PinnableSlice__Slice(m_retrieved_record),
                          (row_info.tx->m_snapshot_timestamp
                               ? row_info.tx->m_snapshot_timestamp
                               : static_cast<int64_t>(std::time(nullptr))))) {
    key_found = false;
  }
  if (key_found && row_info.old_data == nullptr && m_insert_with_update) {
    // In INSERT ON DUPLICATE KEY UPDATE ... case, if the insert failed
    // due to a duplicate key, remember the last key and skip the check
    // next time
    m_dup_key_found = true;

#ifndef DBUG_OFF
    // save it for sanity checking later

    // ALTER
    // m_dup_key_retrieved_record.copy(m_retrieved_record.data(),
    //                                 m_retrieved_record.size(),
    //                                 &my_charset_bin);
    m_dup_key_retrieved_record.copy(
        rocksdb_PinnableSlice__data(m_retrieved_record),
        rocksdb_PinnableSlice__size(m_retrieved_record), &my_charset_bin);
#endif
  }

  *found = key_found;
  rocksdb_rpc_log(12146, "check_and_lock_unique_pk: end");

  return HA_EXIT_SUCCESS;
}

/**
   Check the specified secondary key value is unique and also lock the row

  @param[in] key_id           key index
  @param[in] row_info         hold all data for update row, such as old row
                              data and new row data
  @param[out] found           whether specified key value exists before.
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::check_and_lock_sk(const uint key_id,
                                  const struct update_row_info &row_info,
                                  bool *const found,
                                  const bool skip_unique_check) {
  rocksdb_rpc_log(12167, "check_and_lock_sk: start");

  DBUG_ASSERT(found != nullptr);
  *found = false;

  /*
    Can skip checking this key if none of the key fields have changed.
  */
  if (row_info.old_data != nullptr && !m_update_scope.is_set(key_id)) {
    rocksdb_rpc_log(12176, "check_and_lock_sk: end");

    return HA_EXIT_SUCCESS;
  }

  KEY *key_info = nullptr;
  uint n_null_fields = 0;
  uint user_defined_key_parts = 1;

  key_info = &table->key_info[key_id];
  user_defined_key_parts = key_info->user_defined_key_parts;
  /*
    If there are no uniqueness requirements, there's no need to obtain a
    lock for this key.
  */
  if (!(key_info->flags & HA_NOSAME)) {
    rocksdb_rpc_log(12192, "check_and_lock_sk: end");
    return HA_EXIT_SUCCESS;
  }

  const Rdb_key_def &kd = *m_key_descr_arr[key_id];

  /*
    Calculate the new key for obtaining the lock

    For unique secondary indexes, the key used for locking does not
    include the extended fields.
  */
  int size =
      kd.pack_record(table, m_pack_buffer, row_info.new_data, m_sk_packed_tuple,
                     nullptr, false, 0, user_defined_key_parts, &n_null_fields);
  if (n_null_fields > 0) {
    rocksdb_rpc_log(12208, "check_and_lock_sk: end");
    /*
      If any fields are marked as NULL this will never match another row as
      to NULL never matches anything else including another NULL.
     */
    return HA_EXIT_SUCCESS;
  }

  const rocksdb::Slice new_slice =
      rocksdb::Slice((const char *)m_sk_packed_tuple, size);

  /*
     Acquire lock on the old key in case of UPDATE
  */
  if (row_info.old_data != nullptr) {
    size = kd.pack_record(table, m_pack_buffer, row_info.old_data,
                          m_sk_packed_tuple_old, nullptr, false, 0,
                          user_defined_key_parts);
    const rocksdb::Slice old_slice =
        rocksdb::Slice((const char *)m_sk_packed_tuple_old, size);

    rocksdb::PinnableSlice *ps = nullptr;
    const rocksdb::Status s = get_for_update(row_info.tx, kd, old_slice, ps);
    if (!s.ok()) {
      rocksdb_rpc_log(12233, "check_and_lock_sk: end");

      return row_info.tx->set_status_error(table->in_use, s, kd, m_tbl_def,
                                           m_table_handler);
    }

    /*
      If the old and new keys are the same we're done since we've already taken
      the lock on the old key
    */
    if (!new_slice.compare(old_slice)) {
      rocksdb_rpc_log(12244, "check_and_lock_sk: end");
      return HA_EXIT_SUCCESS;
    }
  }

  /*
    Perform a read to determine if a duplicate entry exists - since this is
    a secondary indexes a range scan is needed.

    note: we intentionally don't set options.snapshot here. We want to read
    the latest committed data.
  */

  const bool all_parts_used = (user_defined_key_parts == kd.get_key_parts());

  /*
    This iterator seems expensive since we need to allocate and free
    memory for each unique index.

    If this needs to be optimized, for keys without NULL fields, the
    extended primary key fields can be migrated to the value portion of the
    key. This enables using Get() instead of Seek() as in the primary key
    case.

    The bloom filter may need to be disabled for this lookup.
  */
  uchar lower_bound_buf[Rdb_key_def::INDEX_NUMBER_SIZE];
  uchar upper_bound_buf[Rdb_key_def::INDEX_NUMBER_SIZE];
  rocksdb::Slice lower_bound_slice;
  rocksdb::Slice upper_bound_slice;

  const bool total_order_seek = !check_bloom_and_set_bounds(
      ha_thd(), kd, new_slice, all_parts_used, Rdb_key_def::INDEX_NUMBER_SIZE,
      lower_bound_buf, upper_bound_buf, &lower_bound_slice, &upper_bound_slice);
  const bool fill_cache = !THDVAR(ha_thd(), skip_fill_cache);
  rocksdb::PinnableSlice *ps = nullptr;
  rocksdb_rpc_log(12281, "check_and_lock_sk: get_for_update");
  const rocksdb::Status s = get_for_update(row_info.tx, kd, new_slice, ps);
  if (!s.ok() && !s.IsNotFound()) {
    rocksdb_rpc_log(12283, "check_and_lock_sk: end");
    return row_info.tx->set_status_error(table->in_use, s, kd, m_tbl_def,
                                         m_table_handler);
  }

  rocksdb_rpc_log(12288, "check_and_lock_sk: get_iterator");
  rocksdb::Iterator *const iter = row_info.tx->get_iterator(
      kd.get_cf(), total_order_seek, fill_cache, lower_bound_slice,
      upper_bound_slice, true /* read current data */,
      false /* acquire snapshot */);
  /*
    Need to scan the transaction to see if there is a duplicate key.
    Also need to scan RocksDB and verify the key has not been deleted
    in the transaction.
  */
  *found = !read_key_exact(kd, iter, all_parts_used, new_slice,
                           row_info.tx->m_snapshot_timestamp);

  int rc = HA_EXIT_SUCCESS;

  if (*found && m_insert_with_update) {
    // ALTER
    // const rocksdb::Slice &rkey = iter->key();
    rocksdb_rpc_log(12306, "check_and_lock_sk: rocksdb_Iterator__key");

    const rocksdb::Slice &key = rocksdb_Iterator__key(iter);
    uint pk_size =
        kd.get_primary_key_tuple(table, *m_pk_descr, &key, m_pk_packed_tuple);
    if (pk_size == RDB_INVALID_KEY_LEN) {
      rc = HA_ERR_ROCKSDB_CORRUPT_DATA;
    } else {
      m_dup_key_found = true;
      m_last_rowkey.copy((const char *)m_pk_packed_tuple, pk_size,
                         &my_charset_bin);
#ifndef DBUG_OFF
      // save it for sanity checking later
      m_dup_key_retrieved_record.copy(rkey.data(), rkey.size(),
                                      &my_charset_bin);
#endif
    }
  }

  rocksdb_rpc_log(12325, "check_and_lock_sk: rocksdb_Iterator__key");
  // TODO: ALTER
  // delete iter;
  return rc;
}

/**
   Enumerate all keys to check their uniquess and also lock it

  @param[in] row_info         hold all data for update row, such as old row
                              data and new row data
  @param[out] pk_changed      whether primary key is changed
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::check_uniqueness_and_lock(
    const struct update_row_info &row_info, bool pk_changed,
    bool skip_unique_check) {
  rocksdb_rpc_log(12344, "check_uniqueness_and_lock: begin");
  /*
    Go through each index and determine if the index has uniqueness
    requirements. If it does, then try to obtain a row lock on the new values.
    Once all locks have been obtained, then perform the changes needed to
    update/insert the row.
  */
  for (uint key_id = 0; key_id < m_tbl_def->m_key_count; key_id++) {
    bool found;
    int rc;

    if (is_pk(key_id, table, m_tbl_def)) {
      if (row_info.old_pk_slice.size() > 0 && !pk_changed) {
        found = false;
        rc = HA_EXIT_SUCCESS;
      } else {
        rc = check_and_lock_unique_pk(key_id, row_info, &found,
                                      skip_unique_check);
        DEBUG_SYNC(ha_thd(), "rocksdb.after_unique_pk_check");
      }
    } else {
      rc = check_and_lock_sk(key_id, row_info, &found, skip_unique_check);
      DEBUG_SYNC(ha_thd(), "rocksdb.after_unique_sk_check");
    }

    if (rc != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(12370, "check_uniqueness_and_lock: end");

      return rc;
    }

    if (found) {
      /* There is a row with this key already, so error out. */
      errkey = key_id;
      m_dupp_errkey = errkey;
      rocksdb_rpc_log(12381, "check_uniqueness_and_lock: end");

      return HA_ERR_FOUND_DUPP_KEY;
    }
  }
  rocksdb_rpc_log(12384, "check_uniqueness_and_lock: end");
  return HA_EXIT_SUCCESS;
}

/**
  Check whether secondary key value is duplicate or not

  @param[in] table_arg         the table currently working on
  @param[in  key_def           the key_def is being checked
  @param[in] key               secondary key storage data
  @param[out] sk_info          hold secondary key memcmp datas(new/old)
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/

int ha_rocksdb::check_duplicate_sk(const TABLE *table_arg,
                                   const Rdb_key_def &key_def,
                                   const rocksdb::Slice *key,
                                   struct unique_sk_buf_info *sk_info) {
  rocksdb_rpc_log(12404, "check_duplicate_sk: begin");

  uint n_null_fields = 0;

  // ALTER
  // const rocksdb::Comparator *index_comp = key_def.get_cf()->GetComparator();
  rocksdb_rpc_log(
      12411, "check_duplicate_sk: rocksdb_ColumnFamilyHandle__GetComparator");
  const rocksdb::Comparator *index_comp =
      rocksdb_ColumnFamilyHandle__GetComparator(key_def.get_cf());

  /* Get proper SK buffer. */
  uchar *sk_buf = sk_info->swap_and_get_sk_buf();

  /* Get memcmp form of sk without extended pk tail */
  uint sk_memcmp_size =
      key_def.get_memcmp_sk_parts(table_arg, *key, sk_buf, &n_null_fields);

  sk_info->sk_memcmp_key =
      rocksdb::Slice(reinterpret_cast<char *>(sk_buf), sk_memcmp_size);

  // ALTER
  // if (sk_info->sk_memcmp_key_old.size() > 0 && n_null_fields == 0 &&
  //     index_comp->Compare(sk_info->sk_memcmp_key, sk_info->sk_memcmp_key_old)
  //     ==
  //         0) {
  //   return 1;
  // }
  rocksdb_rpc_log(12431, "check_duplicate_sk: rocksdb_Comparator__Compare");
  if (sk_info->sk_memcmp_key_old.size() > 0 && n_null_fields == 0 &&
      rocksdb_Comparator__Compare(index_comp, sk_info->sk_memcmp_key,
                                  sk_info->sk_memcmp_key_old) == 0) {
    rocksdb_rpc_log(12435, "check_duplicate_sk: end");

    return 1;
  }

  sk_info->sk_memcmp_key_old = sk_info->sk_memcmp_key;
  rocksdb_rpc_log(12442, "check_duplicate_sk: end");
  return 0;
}

int ha_rocksdb::bulk_load_key(Rdb_transaction *const tx, const Rdb_key_def &kd,
                              const rocksdb::Slice &key,
                              const rocksdb::Slice &value, bool sort) {
  rocksdb_rpc_log(12449, "bulk_load_key: start");
  DBUG_ENTER_FUNC();
  int res;
  THD *thd = ha_thd();
  if (thd && thd->killed) {
    rocksdb_rpc_log(12454, "bulk_load_key: end");
    DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
  }

  rocksdb_rpc_log(12458, "bulk_load_key: kd.get_cf");

  rocksdb::ColumnFamilyHandle *cf = kd.get_cf();

  // In the case of unsorted inserts, m_sst_info allocated here is not
  // used to store the keys. It is still used to indicate when tables
  // are switched.
  if (m_sst_info == nullptr || m_sst_info->is_done()) {
    // ALTER
    // m_sst_info.reset(new Rdb_sst_info(rdb, m_table_handler->m_table_name,
    //                                   kd.get_name(), cf, *rocksdb_db_options,
    //                                   THDVAR(ha_thd(), trace_sst_api)));
    rocksdb_rpc_log(12472, "bulk_load_key: new Rdb_sst_info");

    m_sst_info.reset(new Rdb_sst_info(rdb, m_table_handler->m_table_name,
                                      kd.get_name(), cf, rocksdb_db_options,
                                      THDVAR(ha_thd(), trace_sst_api)));
    res = tx->start_bulk_load(this, m_sst_info);
    if (res != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(12477, "bulk_load_key: end");
      DBUG_RETURN(res);
    }
  }
  DBUG_ASSERT(m_sst_info);

  if (sort) {
    Rdb_index_merge *key_merge;
    DBUG_ASSERT(cf != nullptr);
    rocksdb_rpc_log(12486, "bulk_load_key: tx->get_key_merge");
    res = tx->get_key_merge(kd.get_gl_index_id(), cf, &key_merge);
    if (res == HA_EXIT_SUCCESS) {
      res = key_merge->add(key, value);
    }
  } else {
    rocksdb_rpc_log(12493, "bulk_load_key: m_sst_info->put");
    res = m_sst_info->put(key, value);
  }
  rocksdb_rpc_log(12497, "bulk_load_key: m_sst_info->put");
  DBUG_RETURN(res);
}

int ha_rocksdb::finalize_bulk_load(bool print_client_error) {
  rocksdb_rpc_log(12500, "finalize_bulk_load: begin");
  DBUG_ENTER_FUNC();

  int res = HA_EXIT_SUCCESS;

  /* Skip if there are no possible ongoing bulk loads */
  if (m_sst_info) {
    if (m_sst_info->is_done()) {
      m_sst_info.reset();
      rocksdb_rpc_log(12511, "finalize_bulk_load: end");
      DBUG_RETURN(res);
    }

    Rdb_sst_info::Rdb_sst_commit_info commit_info;

    // Wrap up the current work in m_sst_info and get ready to commit
    // This transfer the responsibility of commit over to commit_info
    rocksdb_rpc_log(12511, "finalize_bulk_load: m_sst_info->finish");
    res = m_sst_info->finish(&commit_info, print_client_error);
    if (res == 0) {
      // Make sure we have work to do - under race condition we could lose
      // to another thread and end up with no work
      if (commit_info.has_work()) {
        rocksdb::IngestExternalFileOptions opts;
        opts.move_files = true;
        opts.snapshot_consistency = false;
        opts.allow_global_seqno = false;
        opts.allow_blocking_flush = false;

        // ALTER
        // const rocksdb::Status s = rdb->IngestExternalFile(
        //     commit_info.get_cf(), commit_info.get_committed_files(), opts);
        rocksdb_rpc_log(
            12532,
            "finalize_bulk_load: rocksdb_TransactionDB__IngestExternalFile");

        const rocksdb::Status s = rocksdb_TransactionDB__IngestExternalFile(
            rdb, commit_info.get_cf(), commit_info.get_committed_files(), opts);

        if (!s.ok()) {
          if (print_client_error) {
            Rdb_sst_info::report_error_msg(s, nullptr);
          }
          res = HA_ERR_ROCKSDB_BULK_LOAD;
        } else {
          // Mark the list of SST files as committed, otherwise they'll get
          // cleaned up when commit_info destructs
          rocksdb_rpc_log(12548, "finalize_bulk_load: commit");
          commit_info.commit();
        }
      }
    }
    m_sst_info.reset();
  }
  rocksdb_rpc_log(12554, "finalize_bulk_load: end");

  DBUG_RETURN(res);
}

/**
  Update an existing primary key record or write a new primary key record

  @param[in] kd                the primary key is being update/write
  @param[in] update_row_info   hold all row data, such as old row data and
                               new row data
  @param[in] pk_changed        whether primary key is changed
  @return
    HA_EXIT_SUCCESS OK
    Other           HA_ERR error code (can be SE-specific)
 */
int ha_rocksdb::update_write_pk(const Rdb_key_def &kd,
                                const struct update_row_info &row_info,
                                bool pk_changed) {
  rocksdb_rpc_log(12574, "update_write_pk: start");
  uint key_id = kd.get_keyno();
  bool hidden_pk = is_hidden_pk(key_id, table, m_tbl_def);
  ulonglong bytes_written = 0;

  /*
    If the PK has changed, or if this PK uses single deletes and this is an
    update, the old key needs to be deleted. In the single delete case, it
    might be possible to have this sequence of keys: PUT(X), PUT(X), SD(X),
    resulting in the first PUT(X) showing up.
  */
  if (!hidden_pk && (pk_changed || ((row_info.old_pk_slice.size() > 0) &&
                                    can_use_single_delete(key_id)))) {
    rocksdb_rpc_log(12586, "update_write_pk: delete_or_singledelete");
    const rocksdb::Status s = delete_or_singledelete(
        key_id, row_info.tx, kd.get_cf(), row_info.old_pk_slice);
    if (!s.ok()) {
      rocksdb_rpc_log(12589, "update_write_pk: end");

      return row_info.tx->set_status_error(table->in_use, s, kd, m_tbl_def,
                                           m_table_handler);
    } else {
      bytes_written = row_info.old_pk_slice.size();
    }
  }

  if (table->found_next_number_field) {
    update_auto_incr_val_from_field();
  }

  int rc = HA_EXIT_SUCCESS;
  rocksdb::Slice value_slice;
  /* Prepare the new record to be written into RocksDB */
  rocksdb_rpc_log(12606, "update_write_pk: m_converter->encode_value_slice");
  if ((rc = m_converter->encode_value_slice(
           m_pk_descr, row_info.new_pk_slice, row_info.new_pk_unpack_info,
           !row_info.old_pk_slice.empty(), should_store_row_debug_checksums(),
           m_ttl_bytes, &m_ttl_bytes_updated, &value_slice))) {
    rocksdb_rpc_log(12611, "update_write_pk: end");
    return rc;
  }

  rocksdb_rpc_log(12616, "update_write_pk: m_pk_descr->get_cf");
  const auto cf = m_pk_descr->get_cf();
  if (rocksdb_enable_bulk_load_api && THDVAR(table->in_use, bulk_load) &&
      !hidden_pk) {
    /*
      Write the primary key directly to an SST file using an SstFileWriter
     */
    rc = bulk_load_key(row_info.tx, kd, row_info.new_pk_slice, value_slice,
                       THDVAR(table->in_use, bulk_load_allow_unsorted));
  } else if (row_info.skip_unique_check || row_info.tx->m_ddl_transaction) {
    /*
      It is responsibility of the user to make sure that the data being
      inserted doesn't violate any unique keys.
    */
    // ALTER
    // row_info.tx->get_indexed_write_batch()->Put(cf, row_info.new_pk_slice,
    //                                             value_slice);
    rocksdb_rpc_log(12633, "update_write_pk: rocksdb_WriteBatchBase__Put");
    rocksdb_WriteBatchBase__Put(row_info.tx->get_indexed_write_batch(), cf,
                                row_info.new_pk_slice, value_slice);
  } else {
    const bool assume_tracked = can_assume_tracked(ha_thd());
    const auto s = row_info.tx->put(cf, row_info.new_pk_slice, value_slice,
                                    assume_tracked);
    if (!s.ok()) {
      if (s.IsBusy()) {
        errkey = table->s->primary_key;
        m_dupp_errkey = errkey;
        rc = HA_ERR_FOUND_DUPP_KEY;
      } else {
        rc = row_info.tx->set_status_error(table->in_use, s, *m_pk_descr,
                                           m_tbl_def, m_table_handler);
      }
    }
  }

  if (rc == HA_EXIT_SUCCESS) {
    row_info.tx->update_bytes_written(
        bytes_written + row_info.new_pk_slice.size() + value_slice.size());
  }
  rocksdb_rpc_log(12655, "update_write_pk: end");
  return rc;
}

/**
  update an existing secondary key record or write a new secondary key record

  @param[in] table_arg    Table we're working on
  @param[in] kd           The secondary key being update/write
  @param[in] row_info     data structure contains old row data and new row data
  @param[in] bulk_load_sk whether support bulk load. Currently it is only
                          support for write
  @return
    HA_EXIT_SUCCESS OK
    Other           HA_ERR error code (can be SE-specific)
 */
int ha_rocksdb::update_write_sk(const TABLE *const table_arg,
                                const Rdb_key_def &kd,
                                const struct update_row_info &row_info,
                                const bool bulk_load_sk) {
  rocksdb_rpc_log(12675, "update_write_sk: start");

  int new_packed_size;
  int old_packed_size;
  int rc = HA_EXIT_SUCCESS;

  rocksdb::Slice new_key_slice;
  rocksdb::Slice new_value_slice;
  rocksdb::Slice old_key_slice;

  const uint key_id = kd.get_keyno();

  ulonglong bytes_written = 0;

  /*
    Can skip updating this key if none of the key fields have changed and, if
    this table has TTL, the TTL timestamp has not changed.
  */
  if (row_info.old_data != nullptr && !m_update_scope.is_set(key_id) &&
      (!kd.has_ttl() || !m_ttl_bytes_updated)) {
    rocksdb_rpc_log(12695, "update_write_sk: end");

    return HA_EXIT_SUCCESS;
  }

  bool store_row_debug_checksums = should_store_row_debug_checksums();
  new_packed_size =
      kd.pack_record(table_arg, m_pack_buffer, row_info.new_data,
                     m_sk_packed_tuple, &m_sk_tails, store_row_debug_checksums,
                     row_info.hidden_pk_id, 0, nullptr, m_ttl_bytes);

  if (row_info.old_data != nullptr) {
    // The old value
    old_packed_size = kd.pack_record(
        table_arg, m_pack_buffer, row_info.old_data, m_sk_packed_tuple_old,
        &m_sk_tails_old, store_row_debug_checksums, row_info.hidden_pk_id, 0,
        nullptr, m_ttl_bytes);

    /*
      Check if we are going to write the same value. This can happen when
      one does
        UPDATE tbl SET col='foo'
      and we are looking at the row that already has col='foo'.

      We also need to compare the unpack info. Suppose, the collation is
      case-insensitive, and unpack info contains information about whether
      the letters were uppercase and lowercase.  Then, both 'foo' and 'FOO'
      will have the same key value, but different data in unpack_info.

      (note: anyone changing bytewise_compare should take this code into
      account)
    */
    if (old_packed_size == new_packed_size &&
        m_sk_tails_old.get_current_pos() == m_sk_tails.get_current_pos() &&
        !(kd.has_ttl() && m_ttl_bytes_updated) &&
        memcmp(m_sk_packed_tuple_old, m_sk_packed_tuple, old_packed_size) ==
            0 &&
        memcmp(m_sk_tails_old.ptr(), m_sk_tails.ptr(),
               m_sk_tails.get_current_pos()) == 0) {
      rocksdb_rpc_log(12736, "update_write_sk: end");

      return HA_EXIT_SUCCESS;
    }

    /*
      Deleting entries from secondary index should skip locking, but
      be visible to the transaction.
      (also note that DDL statements do not delete rows, so this is not a DDL
       statement)
    */
    old_key_slice = rocksdb::Slice(
        reinterpret_cast<const char *>(m_sk_packed_tuple_old), old_packed_size);

    // ALTER
    // row_info.tx->get_indexed_write_batch()->SingleDelete(kd.get_cf(),
    //                                                      old_key_slice);
    rocksdb_rpc_log(12751,
                    "update_write_sk: rocksdb_WriteBatchBase__SingleDelete");
    rocksdb_WriteBatchBase__SingleDelete(row_info.tx->get_indexed_write_batch(),
                                         kd.get_cf(), old_key_slice);

    bytes_written = old_key_slice.size();
  }

  new_key_slice = rocksdb::Slice(
      reinterpret_cast<const char *>(m_sk_packed_tuple), new_packed_size);
  new_value_slice =
      rocksdb::Slice(reinterpret_cast<const char *>(m_sk_tails.ptr()),
                     m_sk_tails.get_current_pos());

  if (bulk_load_sk && row_info.old_data == nullptr) {
    rc = bulk_load_key(row_info.tx, kd, new_key_slice, new_value_slice, true);
  } else {
    // ALTER
    // row_info.tx->get_indexed_write_batch()->Put(kd.get_cf(), new_key_slice,
    //                                             new_value_slice);
    rocksdb_rpc_log(12770, "update_write_sk: rocksdb_WriteBatchBase__Put");
    rocksdb_WriteBatchBase__Put(row_info.tx->get_indexed_write_batch(),
                                kd.get_cf(), new_key_slice, new_value_slice);
  }

  row_info.tx->update_bytes_written(bytes_written + new_key_slice.size() +
                                    new_value_slice.size());
  rocksdb_rpc_log(12780, "update_write_sk: end");

  return rc;
}

/**
   Update existing indexes(PK/SKs) or write new indexes(PK/SKs)

   @param[in] row_info    hold all row data, such as old key/new key
   @param[in] pk_changed  whether primary key is changed
   @return
     HA_EXIT_SUCCESS OK
     Other           HA_ERR error code (can be SE-specific)
 */
int ha_rocksdb::update_write_indexes(const struct update_row_info &row_info,
                                     const bool pk_changed) {
  rocksdb_rpc_log(12794, "update_write_indexes: start");

  int rc;
  bool bulk_load_sk;

  // The PK must be updated first to pull out the TTL value.
  rc = update_write_pk(*m_pk_descr, row_info, pk_changed);
  if (rc != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(12802, "update_write_indexes: end");
    return rc;
  }

  // Update the remaining indexes. Allow bulk loading only if
  // allow_sk is enabled
  bulk_load_sk = rocksdb_enable_bulk_load_api &&
                 THDVAR(table->in_use, bulk_load) &&
                 THDVAR(table->in_use, bulk_load_allow_sk);
  for (uint key_id = 0; key_id < m_tbl_def->m_key_count; key_id++) {
    if (is_pk(key_id, table, m_tbl_def)) {
      continue;
    }

    rc = update_write_sk(table, *m_key_descr_arr[key_id], row_info,
                         bulk_load_sk);
    if (rc != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(12819, "update_write_indexes: end");

      return rc;
    }
  }
  rocksdb_rpc_log(12824, "update_write_indexes: end");

  return HA_EXIT_SUCCESS;
}

/**
  Update an existing row or write a new row

  @param[in] old_data           nullptr for write, non-null for update
  @param[in] new_data           non-null for write/update
  @param[in] skip_unique_check  whether to check uniqueness
  @return
    HA_EXIT_SUCCESS OK
    Other           HA_ERR error code (can be SE-specific)
 */
int ha_rocksdb::update_write_row(const uchar *const old_data,
                                 const uchar *const new_data,
                                 const bool skip_unique_check) {
  rocksdb_rpc_log(12842, "update_write_row: start");
  DBUG_ENTER_FUNC();

  THD *thd = ha_thd();
  if (thd && thd->killed) {
    rocksdb_rpc_log(12847, "update_write_row: end");
    DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
  }

  bool pk_changed = false;
  struct update_row_info row_info;

  row_info.old_data = old_data;
  row_info.new_data = new_data;
  row_info.skip_unique_check = skip_unique_check;
  row_info.new_pk_unpack_info = nullptr;
  set_last_rowkey(old_data);

  rocksdb_rpc_log(12847, "update_write_row: get_or_create_tx");
  row_info.tx = get_or_create_tx(table->in_use);

  if (old_data != nullptr) {
    row_info.old_pk_slice =
        rocksdb::Slice(m_last_rowkey.ptr(), m_last_rowkey.length());

    /* Determine which indexes need updating. */
    calc_updated_indexes();
  }

  /*
    Get the new row key into row_info.new_pk_slice
   */
  rocksdb_rpc_log(12847, "update_write_row: get_pk_for_update");
  int rc = get_pk_for_update(&row_info);
  if (rc != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(12877, "update_write_row: end");
    DBUG_RETURN(rc);
  }

  /*
    For UPDATEs, if the key has changed, we need to obtain a lock. INSERTs
    always require locking.
  */
  if (row_info.old_pk_slice.size() > 0) {
    pk_changed = row_info.new_pk_slice.compare(row_info.old_pk_slice) != 0;
  }

  // Case: We skip both unique checks and rows locks only when bulk load is
  // enabled or if rocksdb_skip_locks_if_skip_unique_check is ON
  if (!THDVAR(table->in_use, bulk_load) &&
      (!rocksdb_skip_locks_if_skip_unique_check || !skip_unique_check)) {
    /*
      Check to see if we are going to have failures because of unique
      keys.  Also lock the appropriate key values.
    */
    rc = check_uniqueness_and_lock(row_info, pk_changed, skip_unique_check);
    if (rc != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(12899, "update_write_row: end");
      DBUG_RETURN(rc);
    }
  }

  DEBUG_SYNC(ha_thd(), "rocksdb.update_write_row_after_unique_check");

  /*
    At this point, all locks have been obtained, and all checks for duplicate
    keys have been performed. No further errors can be allowed to occur from
    here because updates to the transaction will be made and those updates
    cannot be easily removed without rolling back the entire transaction.
  */
  rc = update_write_indexes(row_info, pk_changed);
  if (rc != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(12916, "update_write_row: end");
    DBUG_RETURN(rc);
  }

  if (old_data != nullptr) {
    row_info.tx->incr_update_count();
  } else {
    row_info.tx->incr_insert_count();
  }

  row_info.tx->log_table_write_op(m_tbl_def);

  if (do_bulk_commit(row_info.tx)) {
    rocksdb_rpc_log(12927, "update_write_row: do_bulk_commit");
    DBUG_RETURN(HA_ERR_ROCKSDB_BULK_LOAD);
  }

  rocksdb_rpc_log(12931, "update_write_row: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
 Setting iterator upper/lower bounds for Seek/SeekForPrev.
 This makes RocksDB to avoid scanning tombstones outside of
 the given key ranges, when prefix_same_as_start=true was not passed
 (when prefix bloom filter can not be used).
 Inversing upper/lower bound is necessary on reverse order CF.
 This covers HA_READ_PREFIX_LAST* case as well. For example,
 if given query eq condition was 12 bytes and condition was
 0x0000b3eb003f65c5e78858b8, and if doing HA_READ_PREFIX_LAST,
 eq_cond_len was 11 (see calc_eq_cond_len() for details).
 If the index was reverse order, upper bound would be
 0x0000b3eb003f65c5e78857, and lower bound would be
 0x0000b3eb003f65c5e78859. These cover given eq condition range.
*/
void ha_rocksdb::setup_iterator_bounds(
    const Rdb_key_def &kd, const rocksdb::Slice &eq_cond, size_t bound_len,
    uchar *const lower_bound, uchar *const upper_bound,
    rocksdb::Slice *lower_bound_slice, rocksdb::Slice *upper_bound_slice) {
  rocksdb_rpc_log(12953, "setup_iterator_bounds: start");

  // If eq_cond is shorter than Rdb_key_def::INDEX_NUMBER_SIZE, we should be
  // able to get better bounds just by using index id directly.
  if (eq_cond.size() <= Rdb_key_def::INDEX_NUMBER_SIZE) {
    DBUG_ASSERT(bound_len == Rdb_key_def::INDEX_NUMBER_SIZE);
    uint size;
    kd.get_infimum_key(lower_bound, &size);
    DBUG_ASSERT(size == Rdb_key_def::INDEX_NUMBER_SIZE);
    kd.get_supremum_key(upper_bound, &size);
    DBUG_ASSERT(size == Rdb_key_def::INDEX_NUMBER_SIZE);
  } else {
    DBUG_ASSERT(bound_len <= eq_cond.size());
    memcpy(upper_bound, eq_cond.data(), bound_len);
    kd.successor(upper_bound, bound_len);
    memcpy(lower_bound, eq_cond.data(), bound_len);
    kd.predecessor(lower_bound, bound_len);
  }

  if (kd.m_is_reverse_cf) {
    *upper_bound_slice = rocksdb::Slice((const char *)lower_bound, bound_len);
    *lower_bound_slice = rocksdb::Slice((const char *)upper_bound, bound_len);
  } else {
    *upper_bound_slice = rocksdb::Slice((const char *)upper_bound, bound_len);
    *lower_bound_slice = rocksdb::Slice((const char *)lower_bound, bound_len);
  }
  rocksdb_rpc_log(12979, "setup_iterator_bounds: end");
}

/*
  Open a cursor
*/

void ha_rocksdb::setup_scan_iterator(const Rdb_key_def &kd,
                                     rocksdb::Slice *const slice,
                                     const bool use_all_keys,
                                     const uint eq_cond_len) {
  rocksdb_rpc_log(12979, "setup_scan_iterator: start");

  DBUG_ASSERT(slice->size() >= eq_cond_len);

  rocksdb_rpc_log(12996, "setup_scan_iterator: get_or_create_tx");

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);

  bool skip_bloom = true;

  const rocksdb::Slice eq_cond(slice->data(), eq_cond_len);
  // The size of m_scan_it_lower_bound (and upper) is technically
  // max_packed_sk_len as calculated in ha_rocksdb::alloc_key_buffers.  Rather
  // than recalculating that number, we pass in the max of eq_cond_len and
  // Rdb_key_def::INDEX_NUMBER_SIZE which is guaranteed to be smaller than
  // max_packed_sk_len, hence ensuring no buffer overrun.
  //
  // See ha_rocksdb::setup_iterator_bounds on how the bound_len parameter is
  // used.
  if (check_bloom_and_set_bounds(
          ha_thd(), kd, eq_cond, use_all_keys,
          std::max(eq_cond_len, (uint)Rdb_key_def::INDEX_NUMBER_SIZE),
          m_scan_it_lower_bound, m_scan_it_upper_bound,
          &m_scan_it_lower_bound_slice, &m_scan_it_upper_bound_slice)) {
    skip_bloom = false;
  }

  /*
    In some cases, setup_scan_iterator() is called multiple times from
    the same query but bloom filter can not always be used.
    Suppose the following query example. id2 is VARCHAR(30) and PRIMARY KEY
    (id1, id2).
     select count(*) from t2 WHERE id1=100 and id2 IN ('00000000000000000000',
    '100');
    In this case, setup_scan_iterator() is called twice, the first time is for
    (id1, id2)=(100, '00000000000000000000') and the second time is for (100,
    '100').
    If prefix bloom filter length is 24 bytes, prefix bloom filter can be used
    for the
    first condition but not for the second condition.
    If bloom filter condition is changed, currently it is necessary to destroy
    and
    re-create Iterator.
  */
  if (m_scan_it_skips_bloom != skip_bloom) {
    release_scan_iterator();
  }

  /*
    SQL layer can call rnd_init() multiple times in a row.
    In that case, re-use the iterator, but re-position it at the table start.
  */
  rocksdb_rpc_log(13043, "setup_scan_iterator: m_scan_it");

  if (!m_scan_it) {
    const bool fill_cache = !THDVAR(ha_thd(), skip_fill_cache);
    if (commit_in_the_middle()) {
      DBUG_ASSERT(m_scan_it_snapshot == nullptr);

      // ALTER
      // m_scan_it_snapshot = rdb->GetSnapshot();
      m_scan_it_snapshot = rocksdb_TransactionDB__GetSnapshot(rdb);

      // ALTER
      // auto read_opts = rocksdb::ReadOptions();
      auto read_opts = rocksdb_ReadOptions__NewReadOptions();

      // TODO(mung): set based on WHERE conditions
      // ALTER
      // read_opts.total_order_seek = true;
      // read_opts.snapshot = m_scan_it_snapshot;

      rocksdb_rpc_log(
          13062, "setup_scan_iterator: rocksdb_ReadOptions__SetBoolProperty");

      rocksdb_ReadOptions__SetBoolProperty(read_opts, "total_order_seek", true);
      rocksdb_ReadOptions__SetSnapshot(read_opts, m_scan_it_snapshot);

      // ALTER
      // m_scan_it = rdb->NewIterator(read_opts, kd.get_cf());
      rocksdb_rpc_log(
          13070, "setup_scan_iterator: rocksdb_TransactionDB__NewIterator");
      m_scan_it =
          rocksdb_TransactionDB__NewIterator(rdb, read_opts, kd.get_cf());
    } else {
      m_scan_it = tx->get_iterator(kd.get_cf(), skip_bloom, fill_cache,
                                   m_scan_it_lower_bound_slice,
                                   m_scan_it_upper_bound_slice);
    }
    m_scan_it_skips_bloom = skip_bloom;
  }
  rocksdb_rpc_log(13081, "setup_scan_iterator: end");
}

void ha_rocksdb::release_scan_iterator() {
  // ALTER
  // delete m_scan_it;
  // m_scan_it = nullptr;

  rocksdb_Iterator__delete(m_scan_it);
  m_scan_it = nullptr;

  if (m_scan_it_snapshot) {
    // ALTER
    // rdb->ReleaseSnapshot(m_scan_it_snapshot);
    rocksdb_rpc_log(
        13095, "setup_scan_iterator: rocksdb_TransactionDB__ReleaseSnapshot");
    rocksdb_TransactionDB__ReleaseSnapshot(rdb, m_scan_it_snapshot);
    m_scan_it_snapshot = nullptr;
  }
  rocksdb_rpc_log(13101, "setup_scan_iterator: end");
}

void ha_rocksdb::setup_iterator_for_rnd_scan() {
  rocksdb_rpc_log(13104, "setup_iterator_for_rnd_scan: start");

  uint key_size;

  rocksdb_rpc_log(13108,
                  "setup_iterator_for_rnd_scan: m_pk_descr->get_first_key");
  int key_start_matching_bytes =
      m_pk_descr->get_first_key(m_pk_packed_tuple, &key_size);

  rocksdb::Slice table_key((const char *)m_pk_packed_tuple, key_size);

  setup_scan_iterator(*m_pk_descr, &table_key, false, key_start_matching_bytes);

  // ALTER
  // m_scan_it->Seek(table_key);
  rocksdb_rpc_log(13118, "setup_iterator_for_rnd_scan: rocksdb_Iterator__Seek");
  rocksdb_Iterator__Seek(m_scan_it, table_key);
  m_skip_scan_it_next_call = true;
  rocksdb_rpc_log(13121, "setup_iterator_for_rnd_scan: end");
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::rnd_init(bool scan) {
  rocksdb_rpc_log(13130, "rnd_init: start");
  DBUG_ENTER_FUNC();

  m_need_build_decoder = true;
  active_index = table->s->primary_key;

  THD *thd = ha_thd();
  if (thd && thd->killed) {
    rocksdb_rpc_log(13139, "rnd_init: end");
    DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
  }

  rocksdb_rpc_log(13143, "rnd_init: get_or_create_tx");

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);

  if (scan) {
    m_rnd_scan_is_new_snapshot = !tx->has_snapshot();
    setup_iterator_for_rnd_scan();
  } else {
    /* We don't need any preparations for rnd_pos() calls. */
  }

  // If m_lock_rows is on then we will be doing a get_for_update when accessing
  // the index, so don't acquire the snapshot right away.  Otherwise acquire
  // the snapshot immediately.
  tx->acquire_snapshot(m_lock_rows == RDB_LOCK_NONE);
  rocksdb_rpc_log(13160, "rnd_init: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::rnd_next(uchar *const buf) {
  rocksdb_rpc_log(13169, "rnd_next: start");
  DBUG_ENTER_FUNC();

  check_build_decoder();

  int rc;
  ha_statistic_increment(&SSV::ha_read_rnd_next_count);
  for (;;) {
    rc = rnd_next_with_direction(buf, true);
    if (!should_recreate_snapshot(rc, m_rnd_scan_is_new_snapshot)) {
      break; /* exit the loop */
    }
    // release the snapshot and iterator and then regenerate them
    Rdb_transaction *tx = get_or_create_tx(table->in_use);
    tx->release_snapshot();
    release_scan_iterator();
    setup_iterator_for_rnd_scan();
  }

  m_rnd_scan_is_new_snapshot = false;

  if (rc == HA_ERR_KEY_NOT_FOUND) rc = HA_ERR_END_OF_FILE;
  rocksdb_rpc_log(13191, "rnd_next: end");

  DBUG_RETURN(rc);
}

/*
  See also secondary_index_read().
*/
int ha_rocksdb::rnd_next_with_direction(uchar *const buf, bool move_forward) {
  rocksdb_rpc_log(13200, "rnd_next_with_direction: start");

  DBUG_ENTER_FUNC();

  int rc;
  THD *thd = ha_thd();

  table->status = STATUS_NOT_FOUND;
  stats.rows_requested++;

  if (!m_scan_it || !is_valid_iterator(m_scan_it)) {
    /*
      We can get here when SQL layer has called

        h->index_init(PRIMARY);
        h->index_read_map(full index tuple, HA_READ_KEY_EXACT);

      In this case, we should return EOF.
    */
    rocksdb_rpc_log(13219, "rnd_next_with_direction: start");

    DBUG_RETURN(HA_ERR_END_OF_FILE);
  }

  for (;;) {
    DEBUG_SYNC(thd, "rocksdb.check_flags_rnwd");
    if (thd && thd->killed) {
      rc = HA_ERR_QUERY_INTERRUPTED;
      break;
    }

    if (m_skip_scan_it_next_call) {
      m_skip_scan_it_next_call = false;
    } else {
      if (move_forward) {
        // ALTER
        // m_scan_it->Next(); /* this call cannot fail */
        rocksdb_rpc_log(13237,
                        "rnd_next_with_direction: rocksdb_Iterator__Next");
        rocksdb_Iterator__Next(m_scan_it);
      } else {
        // ALTER
        // m_scan_it->Prev(); /* this call cannot fail */
        rocksdb_rpc_log(13243,
                        "rnd_next_with_direction: rocksdb_Iterator__Next");
        rocksdb_Iterator__Prev(m_scan_it);
      }
    }

    if (!is_valid_iterator(m_scan_it)) {
      rc = HA_ERR_END_OF_FILE;
      break;
    }

    /* check if we're out of this table */

    // ALTER
    // const rocksdb::Slice key = m_scan_it->key();
    rocksdb_rpc_log(13258, "rnd_next_with_direction: rocksdb_Iterator__key");
    const rocksdb::Slice key = rocksdb_Iterator__key(m_scan_it);
    if (!m_pk_descr->covers_key(key)) {
      rc = HA_ERR_END_OF_FILE;
      break;
    }

    if (m_lock_rows != RDB_LOCK_NONE) {
      /*
        Lock the row we've just read.

        Now we call get_for_update which will 1) Take a lock and 2) Will fail
        if the row was deleted since the snapshot was taken.
      */
      rocksdb_rpc_log(13274, "rnd_next_with_direction: get_or_create_tx");

      Rdb_transaction *const tx = get_or_create_tx(table->in_use);
      DEBUG_SYNC(ha_thd(), "rocksdb_concurrent_delete");

      if (m_pk_descr->has_ttl() &&
          should_hide_ttl_rec(
              *m_pk_descr,
              /*ALTER m_scan_it->value()*/ rocksdb_Iterator__value(m_scan_it),
              tx->m_snapshot_timestamp)) {
        continue;
      }

      // ALTER
      // const rocksdb::Status s =
      //     get_for_update(tx, *m_pk_descr, key, &m_retrieved_record);
      rocksdb_rpc_log(13288, "rnd_next_with_direction: get_for_update");

      const rocksdb::Status s =
          get_for_update(tx, *m_pk_descr, key, m_retrieved_record);

      if (s.IsNotFound() &&
          should_skip_invalidated_record(HA_ERR_KEY_NOT_FOUND)) {
        continue;
      }

      if (!s.ok()) {
        DBUG_RETURN(tx->set_status_error(table->in_use, s, *m_pk_descr,
                                         m_tbl_def, m_table_handler));
      }

      // If we called get_for_update() use the value from that call not from
      // the iterator as it may be stale since we don't have a snapshot
      // when m_lock_rows is not RDB_LOCK_NONE.
      m_last_rowkey.copy(key.data(), key.size(), &my_charset_bin);
      rc = convert_record_from_storage_format(&key, buf);
    } else {
      // Use the value from the iterator

      rocksdb_rpc_log(13311,
                      "rnd_next_with_direction: rocksdb_Iterator__value");

      // ALTER
      // rocksdb::Slice value = m_scan_it->value();
      rocksdb::Slice value = rocksdb_Iterator__value(m_scan_it);

      if (m_pk_descr->has_ttl() &&
          should_hide_ttl_rec(
              *m_pk_descr, value,
              get_or_create_tx(table->in_use)->m_snapshot_timestamp)) {
        continue;
      }

      m_last_rowkey.copy(key.data(), key.size(), &my_charset_bin);
      rc = convert_record_from_storage_format(&key, &value, buf);
    }

    table->status = 0;
    break;
  }

  if (!rc) {
    stats.rows_read++;
    stats.rows_index_next++;
    update_row_stats(ROWS_READ);
  }

  rocksdb_rpc_log(13338, "rnd_next_with_direction: end");
  DBUG_RETURN(rc);
}

int ha_rocksdb::rnd_end() {
  rocksdb_rpc_log(13343, "rnd_end: start");
  DBUG_ENTER_FUNC();

  m_need_build_decoder = false;

  release_scan_iterator();

  rocksdb_rpc_log(13353, "rnd_end: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

void ha_rocksdb::build_decoder() {
  rocksdb_rpc_log(13357, "build_decoder: start");
  m_converter->setup_field_decoders(table->read_set, active_index,
                                    m_keyread_only,
                                    m_lock_rows == RDB_LOCK_WRITE);
}

void ha_rocksdb::check_build_decoder() {
  rocksdb_rpc_log(13364, "check_build_decoder: start");
  if (m_need_build_decoder) {
    build_decoder();
    m_need_build_decoder = false;
  }
  rocksdb_rpc_log(13369, "check_build_decoder: end");
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::index_init(uint idx, bool sorted) {
  rocksdb_rpc_log(13378, "index_init: start");
  DBUG_ENTER_FUNC();

  m_need_build_decoder = true;
  active_index = idx;

  THD *thd = ha_thd();
  if (thd && thd->killed) {
    rocksdb_rpc_log(13386, "index_init: end");
    DBUG_RETURN(HA_ERR_QUERY_INTERRUPTED);
  }

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  DBUG_ASSERT(tx != nullptr);

  // If m_lock_rows is not RDB_LOCK_NONE then we will be doing a get_for_update
  // when accessing the index, so don't acquire the snapshot right away.
  // Otherwise acquire the snapshot immediately.
  tx->acquire_snapshot(m_lock_rows == RDB_LOCK_NONE);

  rocksdb_rpc_log(13398, "index_init: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/**
  @return
    HA_EXIT_SUCCESS      OK
*/
int ha_rocksdb::index_end() {
  rocksdb_rpc_log(13407, "index_end: start");

  DBUG_ENTER_FUNC();

  m_need_build_decoder = false;

  release_scan_iterator();

  active_index = MAX_KEY;
  in_range_check_pushed_down = FALSE;

  if (mrr_rowid_reader) mrr_free();

  rocksdb_rpc_log(13420, "index_end: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/**
  Called by the partition manager for truncating tables.

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::truncate() {
  rocksdb_rpc_log(13433, "truncate: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(m_tbl_def != nullptr);

  // Reset auto_increment_value to 1 if auto-increment feature is enabled
  // By default, the starting valid value for auto_increment_value is 1
  rocksdb_rpc_log(13441, "truncate: end");

  DBUG_RETURN(truncate_table(
      m_tbl_def, table,
      table->found_next_number_field ? 1 : 0 /* auto_increment_value */));
}

/*
  Delete the row we've last read. The row is also passed as parameter.

  @detail
    The caller guarantees table buf points to the row that was just read.
    The row is either table->record[0] or table->record[1].
    (Check out InnoDB: row_update_for_mysql() has "UT_NOT_USED(mysql_rec)"

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::delete_row(const uchar *const buf) {
  rocksdb_rpc_log(13461, "delete_row: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(buf != nullptr);

  ha_statistic_increment(&SSV::ha_delete_count);
  set_last_rowkey(buf);

  rocksdb::Slice key_slice(m_last_rowkey.ptr(), m_last_rowkey.length());
  rocksdb_rpc_log(13473, "delete_row: get_or_create_tx");

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);
  ulonglong bytes_written = 0;

  const uint index = pk_index(table, m_tbl_def);
  rocksdb::Status s =
      delete_or_singledelete(index, tx, m_pk_descr->get_cf(), key_slice);
  if (!s.ok()) {
    DBUG_RETURN(tx->set_status_error(table->in_use, s, *m_pk_descr, m_tbl_def,
                                     m_table_handler));
  } else {
    bytes_written = key_slice.size();
  }

  rocksdb_rpc_log(13486, "delete_row: hidden_pk_id");
  longlong hidden_pk_id = 0;
  if (m_tbl_def->m_key_count > 1 && has_hidden_pk(table)) {
    int err = read_hidden_pk_id_from_rowkey(&hidden_pk_id);
    if (err) {
      DBUG_RETURN(err);
    }
  }

  // Delete the record for every secondary index
  for (uint i = 0; i < m_tbl_def->m_key_count; i++) {
    if (!is_pk(i, table, m_tbl_def)) {
      int packed_size;
      const Rdb_key_def &kd = *m_key_descr_arr[i];

      // The unique key should be locked so that behavior is
      // similar to InnoDB and reduce conflicts. The key
      // used for locking does not include the extended fields.
      const KEY *key_info = &table->key_info[i];
      if (key_info->flags & HA_NOSAME) {
        uint user_defined_key_parts = key_info->user_defined_key_parts;
        uint n_null_fields = 0;

        packed_size = kd.pack_record(table, m_pack_buffer, buf,
                                     m_sk_packed_tuple, nullptr, false, 0,
                                     user_defined_key_parts, &n_null_fields);

        // NULL fields are considered unique, so no lock is needed
        if (n_null_fields == 0) {
          rocksdb::Slice sk_slice(
              reinterpret_cast<const char *>(m_sk_packed_tuple), packed_size);
          rocksdb_rpc_log(13519, "delete_row: get_for_update");

          rocksdb::PinnableSlice *ps = nullptr;
          const rocksdb::Status s = get_for_update(tx, kd, sk_slice, ps);
          if (!s.ok()) {
            DBUG_RETURN(tx->set_status_error(table->in_use, s, kd, m_tbl_def,
                                             m_table_handler));
          }
        }
      }

      packed_size = kd.pack_record(table, m_pack_buffer, buf, m_sk_packed_tuple,
                                   nullptr, false, hidden_pk_id);
      rocksdb::Slice secondary_key_slice(
          reinterpret_cast<const char *>(m_sk_packed_tuple), packed_size);

      rocksdb_rpc_log(13553,
                      "delete_row: rocksdb_WriteBatchBase__SingleDelete");

      // ALTER
      // tx->get_indexed_write_batch()->SingleDelete(kd.get_cf(),
      //                                             secondary_key_slice);
      rocksdb_WriteBatchBase__SingleDelete(tx->get_indexed_write_batch(),
                                           kd.get_cf(), secondary_key_slice);

      bytes_written += secondary_key_slice.size();
    }
  }

  tx->incr_delete_count();
  tx->log_table_write_op(m_tbl_def);

  if (do_bulk_commit(tx)) {
    DBUG_RETURN(HA_ERR_ROCKSDB_BULK_LOAD);
  }
  stats.rows_deleted++;

  // Not protected by ddl_manger lock for performance
  // reasons. This is an estimate value anyway.
  dec_table_n_rows();
  update_table_stats_if_needed();
  update_row_stats(ROWS_DELETED);
  rocksdb_rpc_log(13560, "delete_row: update_bytes_written");

  tx->update_bytes_written(bytes_written);

  rocksdb_rpc_log(13564, "delete_row: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

rocksdb::Status ha_rocksdb::delete_or_singledelete(
    uint index, Rdb_transaction *const tx,
    rocksdb::ColumnFamilyHandle *const column_family,
    const rocksdb::Slice &key) {
  rocksdb_rpc_log(13564, "delete_row: end");

  const bool assume_tracked = can_assume_tracked(ha_thd());
  if (can_use_single_delete(index)) {
    rocksdb_rpc_log(13575, "delete_row: tx->single_delete");
    return tx->single_delete(column_family, key, assume_tracked);
  }

  rocksdb_rpc_log(13581, "delete_row: tx->delete_key");

  return tx->delete_key(column_family, key, assume_tracked);
}

void ha_rocksdb::update_stats(void) {
  rocksdb_rpc_log(13585, "delete_row: tx->delete_key");
  DBUG_ENTER_FUNC();

  stats.records = 0;
  stats.index_file_length = 0ul;
  stats.data_file_length = 0ul;
  stats.mean_rec_length = 0;

  for (uint i = 0; i < m_tbl_def->m_key_count; i++) {
    if (is_pk(i, table, m_tbl_def)) {
      stats.data_file_length = m_pk_descr->m_stats.m_actual_disk_size;
      stats.records = m_pk_descr->m_stats.m_rows;
    } else {
      stats.index_file_length += m_key_descr_arr[i]->m_stats.m_actual_disk_size;
    }
  }
  rocksdb_rpc_log(13601, "delete_row: end");

  DBUG_VOID_RETURN;
}

int ha_rocksdb::adjust_handler_stats_table_scan() {
  rocksdb_rpc_log(13607, "adjust_handler_stats_table_scan: start");
  DBUG_ENTER_FUNC();

  bool should_recalc_stats = false;
  if (static_cast<longlong>(stats.data_file_length) < 0) {
    stats.data_file_length = 0;
    should_recalc_stats = true;
  }

  if (static_cast<longlong>(stats.index_file_length) < 0) {
    stats.index_file_length = 0;
    should_recalc_stats = true;
  }

  if (static_cast<longlong>(stats.records) < 0) {
    stats.records = 1;
    should_recalc_stats = true;
  }

  if (should_recalc_stats) {
    // If any of the stats is corrupt, add the table to the index stats
    // recalc queue.
    rdb_is_thread.add_index_stats_request(m_tbl_def->full_tablename());
  }
  rocksdb_rpc_log(13632, "adjust_handler_stats_table_scan: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    HA_EXIT_FAILURE  Error
*/
int ha_rocksdb::info(uint flag) {
  rocksdb_rpc_log(13642, "info: start");

  DBUG_ENTER_FUNC();

  if (!table) {
    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  if (flag & HA_STATUS_VARIABLE) {
    /*
      Test only to simulate corrupted stats
    */
    DBUG_EXECUTE_IF("myrocks_simulate_negative_stats",
                    m_pk_descr->m_stats.m_actual_disk_size =
                        -m_pk_descr->m_stats.m_actual_disk_size;);

    update_stats();
    if (rocksdb_table_stats_use_table_scan) {
      int ret = adjust_handler_stats_table_scan();
      if (ret != HA_EXIT_SUCCESS) {
        rocksdb_rpc_log(13662, "info: end");

        return ret;
      }
    } else {
      int ret = adjust_handler_stats_sst_and_memtable();
      if (ret != HA_EXIT_SUCCESS) {
        rocksdb_rpc_log(13669, "info: end");
        return ret;
      }
    }

    if (rocksdb_debug_optimizer_n_rows > 0) {
      stats.records = rocksdb_debug_optimizer_n_rows;
    }

    if (stats.records != 0) {
      stats.mean_rec_length = stats.data_file_length / stats.records;
    }

    stats.mrr_length_per_rec = mrr_get_length_per_rec();
  }

  if (flag & HA_STATUS_CONST) {
    ref_length = m_pk_descr->max_storage_fmt_length();

    for (uint i = 0; i < m_tbl_def->m_key_count; i++) {
      if (is_hidden_pk(i, table, m_tbl_def)) {
        continue;
      }
      KEY *const k = &table->key_info[i];
      for (uint j = 0; j < k->actual_key_parts; j++) {
        const Rdb_index_stats &k_stats = m_key_descr_arr[i]->m_stats;
        uint x;

        if (k_stats.m_distinct_keys_per_prefix.size() > j &&
            k_stats.m_distinct_keys_per_prefix[j] > 0) {
          x = k_stats.m_rows / k_stats.m_distinct_keys_per_prefix[j];
          /*
            If the number of rows is less than the number of prefixes (due to
            sampling), the average number of rows with the same prefix is 1.
           */
          if (x == 0) {
            x = 1;
          }
        } else {
          x = 0;
        }
        if (x > stats.records) x = stats.records;
        if ((x == 0 && rocksdb_debug_optimizer_no_zero_cardinality) ||
            rocksdb_debug_optimizer_n_rows > 0) {
          // Fake cardinality implementation. For example, (idx1, idx2, idx3)
          // index
          // will have rec_per_key for (idx1)=4, (idx1,2)=2, and (idx1,2,3)=1.
          // rec_per_key for the whole index is 1, and multiplied by 2^n if
          // n suffix columns of the index are not used.
          x = 1 << (k->actual_key_parts - j - 1);
        }
        k->rec_per_key[j] = x;
      }
    }

    stats.create_time = m_tbl_def->get_create_time();
  }

  if (flag & HA_STATUS_TIME) {
    stats.update_time = m_tbl_def->m_update_time;
  }

  if (flag & HA_STATUS_ERRKEY) {
    /*
      Currently we support only primary keys so we know which key had a
      uniqueness violation.
    */
    errkey = m_dupp_errkey;
    dup_ref = m_pk_tuple;  // TODO(?): this should store packed PK.
  }

  if (flag & HA_STATUS_AUTO) {
    stats.auto_increment_value = m_tbl_def->m_auto_incr_val;
  }

  rocksdb_rpc_log(13744, "info: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

void ha_rocksdb::position(const uchar *const record) {
  rocksdb_rpc_log(13750, "info: start");
  DBUG_ENTER_FUNC();

  longlong hidden_pk_id = 0;
  if (has_hidden_pk(table) && read_hidden_pk_id_from_rowkey(&hidden_pk_id)) {
    DBUG_ASSERT(false);  // should never reach here
  }

  /*
    Get packed primary key value from the record.

    (Note: m_last_rowkey has the packed PK of last-read-row, which allows to
    handle most cases, but there is an exception to this: when slave applies
    RBR events, it fills the record and then calls position(); rnd_pos())

    Also note that we *can* rely on PK column values being available. This is
    because table_flags() includes HA_PRIMARY_KEY_REQUIRED_FOR_POSITION bit.
    When that is true, table->prepare_for_position() adds PK columns into the
    read set (this may potentially disable index-only access when PK column
    cannot be restored from its mem-comparable form in the secondary indexes).
  */
  rocksdb_rpc_log(13771, "info: m_pk_descr->pack_record");

  const uint packed_size = m_pk_descr->pack_record(
      table, m_pack_buffer, record, ref, nullptr, false, hidden_pk_id);

  /*
    It could be that mem-comparable form of PK occupies less than ref_length
    bytes. Fill the remainder with zeros.
  */
  if (ref_length > packed_size) {
    memset(ref + packed_size, 0, ref_length - packed_size);
  }
  rocksdb_rpc_log(13783, "info: end");

  DBUG_VOID_RETURN;
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::rnd_pos(uchar *const buf, uchar *const pos) {
  rocksdb_rpc_log(13795, "rnd_pos: start");

  DBUG_ENTER_FUNC();

  check_build_decoder();

  int rc;
  size_t len;

  ha_statistic_increment(&SSV::ha_read_rnd_count);
  stats.rows_requested++;
  len = m_pk_descr->key_length(table,
                               rocksdb::Slice((const char *)pos, ref_length));
  if (len == size_t(-1)) {
    rocksdb_rpc_log(13808, "rnd_pos: end");

    DBUG_RETURN(HA_ERR_ROCKSDB_CORRUPT_DATA); /* Data corruption? */
  }

  rocksdb_rpc_log(13813, "rnd_pos: end");

  rc = get_row_by_rowid(buf, pos, len);

  if (!rc) {
    stats.rows_read++;
    update_row_stats(ROWS_READ);
  }

  rocksdb_rpc_log(13822, "rnd_pos: end");
  DBUG_RETURN(rc);
}

/*
  @brief
    Calculate (if needed) the bitmap of indexes that are modified by the
    current query

  @detail
    The calculation done by checking index definitions against the
  table->write_set
    column bitmap.
*/

void ha_rocksdb::calc_updated_indexes() {
  rocksdb_rpc_log(13838, "calc_updated_indexes: start");
  m_update_scope.clear_all();

  for (uint keynr = 0; keynr < table->s->keys; keynr++) {
    const Rdb_key_def &kd = *m_key_descr_arr[keynr];
    /* Walk over all key parts, including the "extended key" suffix */
    const uint key_parts = kd.get_key_parts();
    for (uint kp = 0; kp < key_parts; kp++) {
      if (has_hidden_pk(table) && kp + 1 == key_parts) break;

      Field *const field = kd.get_table_field_for_part_no(table, kp);
      if (bitmap_is_set(table->write_set, field->field_index)) {
        m_update_scope.set_bit(keynr);
        break;
      }
    }
  }
  rocksdb_rpc_log(13855, "calc_updated_indexes: end");
}

/**
  Update an existing row
  @param[in] old_data           nullptr for write, non-null for update
  @param[in] new_data           non-null for write/update
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::update_row(const uchar *const old_data, uchar *const new_data) {
  rocksdb_rpc_log(13867, "update_row: start");
  DBUG_ENTER_FUNC();

  DBUG_ASSERT(old_data != nullptr);
  DBUG_ASSERT(new_data != nullptr);
  DBUG_ASSERT(m_lock_rows == RDB_LOCK_WRITE);
  /*
    old_data points to record we're updating. It is the same as the record
    we've just read (for multi-table UPDATE, too, because SQL layer will make
    an rnd_pos() call to re-read the record before calling update_row())
  */
  DBUG_ASSERT(new_data == table->record[0]);

  ha_statistic_increment(&SSV::ha_update_count);
  const int rv = update_write_row(old_data, new_data, skip_unique_check());

  if (rv == 0) {
    stats.rows_updated++;
    update_table_stats_if_needed();
    update_row_stats(ROWS_UPDATED);
  }
  rocksdb_rpc_log(13888, "update_row: end");
  DBUG_RETURN(rv);
}

void ha_rocksdb::update_table_stats_if_needed() {
  rocksdb_rpc_log(13888, "update_table_stats_if_needed: start");
  DBUG_ENTER_FUNC();

  if (!rocksdb_table_stats_use_table_scan) {
    rocksdb_rpc_log(13897, "update_table_stats_if_needed: end");

    DBUG_VOID_RETURN;
  }

  /*
    InnoDB performs a similar operation to update counters during query
    processing. Because the changes in MyRocks are made to a write batch,
    it is possible for the table scan cardinality calculation to trigger
    before the transaction performing the update commits. Hence the
    cardinality scan might miss the keys for these pending transactions.
  */
  uint64 counter = m_tbl_def->m_tbl_stats.m_stat_modified_counter++;
  uint64 n_rows = m_tbl_def->m_tbl_stats.m_stat_n_rows;

  if (counter > std::max(rocksdb_table_stats_recalc_threshold_count,
                         static_cast<uint64>(
                             n_rows * rocksdb_table_stats_recalc_threshold_pct /
                             100.0))) {
    // Add the table to the recalc queue
    rdb_is_thread.add_index_stats_request(m_tbl_def->full_tablename());
    m_tbl_def->m_tbl_stats.m_stat_modified_counter = 0;
  }

  rocksdb_rpc_log(13919, "update_table_stats_if_needed: end");
  DBUG_VOID_RETURN;
}

/* The following function was copied from ha_blackhole::store_lock: */
THR_LOCK_DATA **ha_rocksdb::store_lock(THD *const thd, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type) {
  rocksdb_rpc_log(13928, "store_lock: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(to != nullptr);

  bool in_lock_tables = my_core::thd_in_lock_tables(thd);

  /* First, make a decision about MyRocks's internal locking */
  if (lock_type >= TL_WRITE_ALLOW_WRITE) {
    m_lock_rows = RDB_LOCK_WRITE;
  } else if (lock_type == TL_READ_WITH_SHARED_LOCKS) {
    m_lock_rows = RDB_LOCK_READ;
  } else if (lock_type != TL_IGNORE) {
    m_lock_rows = RDB_LOCK_NONE;
    if (THDVAR(thd, lock_scanned_rows)) {
      /*
        The following logic was copied directly from
        ha_innobase::store_lock_with_x_type() in
        storage/innobase/handler/ha_innodb.cc and causes MyRocks to leave
        locks in place on rows that are in a table that is not being updated.
      */
      const uint sql_command = my_core::thd_sql_command(thd);
      if ((lock_type == TL_READ && in_lock_tables) ||
          (lock_type == TL_READ_HIGH_PRIORITY && in_lock_tables) ||
          can_hold_read_locks_on_select(thd, lock_type)) {
        ulong tx_isolation = my_core::thd_tx_isolation(thd);
        if (sql_command != SQLCOM_CHECKSUM &&
            ((my_core::thd_test_options(thd, OPTION_BIN_LOG) &&
              tx_isolation > ISO_READ_COMMITTED) ||
             tx_isolation == ISO_SERIALIZABLE ||
             (lock_type != TL_READ && lock_type != TL_READ_NO_INSERT) ||
             (sql_command != SQLCOM_INSERT_SELECT &&
              sql_command != SQLCOM_REPLACE_SELECT &&
              sql_command != SQLCOM_UPDATE && sql_command != SQLCOM_DELETE &&
              sql_command != SQLCOM_CREATE_TABLE))) {
          m_lock_rows = RDB_LOCK_READ;
        }
      }
    }
  }

  /* Then, tell the SQL layer what kind of locking it should use: */
  if (lock_type != TL_IGNORE && m_db_lock.type == TL_UNLOCK) {
    /*
      Here is where we get into the guts of a row level lock.
      If TL_UNLOCK is set
      If we are not doing a LOCK TABLE or DISCARD/IMPORT
      TABLESPACE, then allow multiple writers
    */

    if ((lock_type >= TL_WRITE_CONCURRENT_INSERT && lock_type <= TL_WRITE) &&
        !in_lock_tables && !my_core::thd_tablespace_op(thd)) {
      lock_type = TL_WRITE_ALLOW_WRITE;
    }

    /*
      In queries of type INSERT INTO t1 SELECT ... FROM t2 ...
      MySQL would use the lock TL_READ_NO_INSERT on t2, and that
      would conflict with TL_WRITE_ALLOW_WRITE, blocking all inserts
      to t2. Convert the lock to a normal read lock to allow
      concurrent inserts to t2.
    */

    if (lock_type == TL_READ_NO_INSERT && !in_lock_tables) {
      lock_type = TL_READ;
    }

    m_db_lock.type = lock_type;
  }

  *to++ = &m_db_lock;
  rocksdb_rpc_log(14001, "store_lock: end");

  DBUG_RETURN(to);
}

void ha_rocksdb::read_thd_vars(THD *const thd) {
  rocksdb_rpc_log(14007, "read_thd_vars: start");

  m_store_row_debug_checksums = THDVAR(thd, store_row_debug_checksums);
  m_converter->set_verify_row_debug_checksums(
      THDVAR(thd, verify_row_debug_checksums));
  m_checksums_pct = THDVAR(thd, checksums_pct);
  rocksdb_rpc_log(14013, "read_thd_vars: end");
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (cannot be SE-specific)
*/
int ha_rocksdb::external_lock(THD *const thd, int lock_type) {
  rocksdb_rpc_log(14022, "external_lock: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);

  int res = HA_EXIT_SUCCESS;
  int binlog_format = my_core::thd_binlog_format(thd);
  bool unsafe_for_binlog = THDVAR(ha_thd(), unsafe_for_binlog);
  if (lock_type == F_WRLCK && !thd->rli_slave && !unsafe_for_binlog &&
      binlog_format != BINLOG_FORMAT_ROW &&
      binlog_format != BINLOG_FORMAT_UNSPEC &&
      my_core::thd_binlog_filter_ok(thd)) {
    my_error(ER_REQUIRE_ROW_BINLOG_FORMAT, MYF(0));
    rocksdb_rpc_log(14036, "external_lock: end");
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  }

  if (lock_type == F_UNLCK) {
    rocksdb_rpc_log(14041, "external_lock: get_tx_from_thd");

    Rdb_transaction *const tx = get_tx_from_thd(thd);

    if (tx) {
      tx->io_perf_end_and_record(&m_io_perf);
      tx->m_n_mysql_tables_in_use--;
      if (tx->m_n_mysql_tables_in_use == 0 &&
          !my_core::thd_test_options(thd,
                                     OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)) {
        /*
          Do like InnoDB: when we get here, it's time to commit a
          single-statement transaction.

          If the statement involved multiple tables, this code will be executed
          for each of them, but that's ok because non-first tx->commit() calls
          will be no-ops.
        */
        rocksdb_rpc_log(14061, "external_lock: commit_or_rollback");

        if (tx->commit_or_rollback()) {
          res = HA_ERR_INTERNAL_ERROR;
        }
      }
    }
  } else {
    if (my_core::thd_tx_isolation(thd) < ISO_READ_COMMITTED ||
        my_core::thd_tx_isolation(thd) > ISO_REPEATABLE_READ) {
      my_error(ER_ISOLATION_MODE_NOT_SUPPORTED, MYF(0),
               tx_isolation_names[my_core::thd_tx_isolation(thd)]);
      rocksdb_rpc_log(14071, "external_lock: end");

      DBUG_RETURN(HA_ERR_UNSUPPORTED);
    }

    /*
      It's nice to do the following on start of every statement. The problem
      is, handler->start_stmt() is not called for INSERTs.
      So, we put this code here.
    */
    rocksdb_rpc_log(14081, "external_lock: get_or_create_tx");

    Rdb_transaction *const tx = get_or_create_tx(thd);
    read_thd_vars(thd);

    if (skip_unique_check()) {
      if ((thd->lex->sql_command == SQLCOM_INSERT ||
           thd->lex->sql_command == SQLCOM_LOAD ||
           thd->lex->sql_command == SQLCOM_REPLACE) &&
          (thd->lex->duplicates == DUP_REPLACE ||
           thd->lex->duplicates == DUP_UPDATE)) {
        my_error(ER_ON_DUPLICATE_DISABLED, MYF(0), thd->query());
        rocksdb_rpc_log(14093, "external_lock: end");

        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      }
    }

    if (lock_type == F_WRLCK) {
      if (tx->is_tx_read_only()) {
        my_error(ER_UPDATES_WITH_CONSISTENT_SNAPSHOT, MYF(0));
        rocksdb_rpc_log(14102, "external_lock: end");

        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      }

      if (thd->get_explicit_snapshot()) {
        my_error(ER_UPDATES_WITH_EXPLICIT_SNAPSHOT, MYF(0));
        rocksdb_rpc_log(14110, "external_lock: end");

        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      }

      /*
        SQL layer signals us to take a write lock. It does so when starting DML
        statement. We should put locks on the rows we're reading.

        Note: sometimes, external_lock() can be called without a prior
        ::store_lock call.  That's why we need to set lock_* members here, too.
      */
      m_lock_rows = RDB_LOCK_WRITE;

      if (thd->lex->sql_command == SQLCOM_CREATE_INDEX ||
          thd->lex->sql_command == SQLCOM_DROP_INDEX ||
          thd->lex->sql_command == SQLCOM_ALTER_TABLE) {
        tx->m_ddl_transaction = true;
      }
    }
    tx->m_n_mysql_tables_in_use++;
    rocksdb_register_tx(rocksdb_hton, thd, tx);
    tx->io_perf_start(&m_io_perf);
  }

  rocksdb_rpc_log(14134, "external_lock: end");

  DBUG_RETURN(res);
}

/**
  @note
  A quote from ha_innobase::start_stmt():
  <quote>
  MySQL calls this function at the start of each SQL statement inside LOCK
  TABLES. Inside LOCK TABLES the ::external_lock method does not work to
  mark SQL statement borders.
  </quote>

  @return
    HA_EXIT_SUCCESS  OK
*/

int ha_rocksdb::start_stmt(THD *const thd, thr_lock_type lock_type) {
  rocksdb_rpc_log(14153, "start_stmt: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);

  rocksdb_rpc_log(14159, "start_stmt: get_or_create_tx");

  Rdb_transaction *const tx = get_or_create_tx(thd);
  read_thd_vars(thd);
  rocksdb_register_tx(ht, thd, tx);
  tx->io_perf_start(&m_io_perf);

  DBUG_RETURN(HA_EXIT_SUCCESS);
  rocksdb_rpc_log(14168, "start_stmt: end");
}

rocksdb::Range get_range(uint32_t i,
                         uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2],
                         int offset1, int offset2) {
  rocksdb_rpc_log(14174, "get_range: start");

  uchar *buf_begin = buf;
  uchar *buf_end = buf + Rdb_key_def::INDEX_NUMBER_SIZE;
  rdb_netbuf_store_index(buf_begin, i + offset1);
  rdb_netbuf_store_index(buf_end, i + offset2);

  rocksdb_rpc_log(14182, "get_range: end");

  return rocksdb::Range(
      rocksdb::Slice((const char *)buf_begin, Rdb_key_def::INDEX_NUMBER_SIZE),
      rocksdb::Slice((const char *)buf_end, Rdb_key_def::INDEX_NUMBER_SIZE));
}

static rocksdb::Range get_range(const Rdb_key_def &kd,
                                uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2],
                                int offset1, int offset2) {
  rocksdb_rpc_log(14191, "get_range: start");
  return get_range(kd.get_index_number(), buf, offset1, offset2);
}

rocksdb::Range get_range(const Rdb_key_def &kd,
                         uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2]) {
  rocksdb_rpc_log(14198, "get_range: start");

  if (kd.m_is_reverse_cf) {
    rocksdb_rpc_log(14201, "get_range: end");

    return myrocks_rpc::get_range(kd, buf, 1, 0);
  } else {
    rocksdb_rpc_log(141203, "get_range: end");

    return myrocks_rpc::get_range(kd, buf, 0, 1);
  }
}

rocksdb::Range ha_rocksdb::get_range(
    const int i, uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2]) const {
  rocksdb_rpc_log(14211, "get_range: start");

  return myrocks_rpc::get_range(*m_key_descr_arr[i], buf);
}

/*
 This function is called with total_order_seek=true, but
 upper/lower bound setting is not necessary.
 Boundary set is useful when there is no matching key,
 but in drop_index_thread's case, it means index is marked as removed,
 so no further seek will happen for the index id.
*/
static bool is_myrocks_index_empty(rocksdb::ColumnFamilyHandle *cfh,
                                   const bool is_reverse_cf,
                                   rocksdb::ReadOptions *read_opts,
                                   const uint index_id) {
  rocksdb_rpc_log(14227, "is_myrocks_index_empty: start");

  bool index_removed = false;
  uchar key_buf[Rdb_key_def::INDEX_NUMBER_SIZE] = {0};
  rdb_netbuf_store_uint32(key_buf, index_id);
  const rocksdb::Slice key =
      rocksdb::Slice(reinterpret_cast<char *>(key_buf), sizeof(key_buf));

  // ALTER
  // std::unique_ptr<rocksdb::Iterator> it(rdb->NewIterator(read_opts, cfh));
  rocksdb_rpc_log(14239,
                  "is_myrocks_index_empty: rocksdb_TransactionDB__NewIterator");

  rocksdb::Iterator *it =
      rocksdb_TransactionDB__NewIterator(rdb, read_opts, cfh);

  rocksdb_smart_seek(is_reverse_cf, it, key);

  rocksdb_rpc_log(14249, "is_myrocks_index_empty: rocksdb_Iterator__Valid");

  // ALTER
  // if (!it->Valid()) {
  if (!rocksdb_Iterator__Valid(it)) {
    index_removed = true;
  } else {
    rocksdb_rpc_log(14252, "is_myrocks_index_empty: rocksdb_Iterator__key");

    // ALTER
    // if (memcmp(it->key().data(), key_buf, Rdb_key_def::INDEX_NUMBER_SIZE)) {
    if (memcmp(rocksdb_Iterator__key(it).data(), key_buf,
               Rdb_key_def::INDEX_NUMBER_SIZE)) {
      // Key does not have same prefix
      index_removed = true;
    }
  }
  rocksdb_rpc_log(14262, "is_myrocks_index_empty: index_removed");

  return index_removed;
}

/*
  Drop index thread's main logic
*/

void Rdb_drop_index_thread::run() {
  rocksdb_rpc_log(14272, "run: begin");
  RDB_MUTEX_LOCK_CHECK(m_signal_mutex);

  for (;;) {
    // The stop flag might be set by shutdown command
    // after drop_index_thread releases signal_mutex
    // (i.e. while executing expensive Seek()). To prevent drop_index_thread
    // from entering long cond_timedwait, checking if stop flag
    // is true or not is needed, with drop_index_interrupt_mutex held.
    if (m_killed) {
      break;
    }

    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += dict_manager.is_drop_index_empty()
                     ? 24 * 60 * 60  // no filtering
                     : 60;           // filtering

    const auto ret MY_ATTRIBUTE((__unused__)) =
        mysql_cond_timedwait(&m_signal_cond, &m_signal_mutex, &ts);
    if (m_killed) {
      break;
    }
    // make sure, no program error is returned
    DBUG_ASSERT(ret == 0 || ret == ETIMEDOUT);
    RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);

    rocksdb_rpc_log(14302, "run: get_ongoing_drop_indexes");

    std::unordered_set<GL_INDEX_ID> indices;
    dict_manager.get_ongoing_drop_indexes(&indices);
    if (!indices.empty()) {
      std::unordered_set<GL_INDEX_ID> finished;

      // ALTER
      // rocksdb::ReadOptions read_opts;
      // read_opts.total_order_seek = true;  // disable bloom filter
      rocksdb_rpc_log(14310, "run: rocksdb_ReadOptions__NewReadOptions");

      rocksdb::ReadOptions *read_opts = rocksdb_ReadOptions__NewReadOptions();
      rocksdb_ReadOptions__SetBoolProperty(read_opts, "total_order_seek", true);

      for (const auto d : indices) {
        uint32 cf_flags = 0;
        if (!dict_manager.get_cf_flags(d.cf_id, &cf_flags)) {
          // NO_LINT_DEBUG
          sql_print_error(
              "RocksDB: Failed to get column family flags "
              "from cf id %u. MyRocks data dictionary may "
              "get corrupted.",
              d.cf_id);
          abort();
        }

        // ALTER
        // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
        //     cf_manager.get_cf(d.cf_id);
        rocksdb_rpc_log(14332, "run: cf_manager.get_cf");

        rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(d.cf_id);
        DBUG_ASSERT(cfh);

        if (dict_manager.get_dropped_cf(d.cf_id)) {
          finished.insert(d);
          continue;
        }

        const bool is_reverse_cf = cf_flags & Rdb_key_def::REVERSE_CF_FLAG;

        uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2];
        rocksdb::Range range = get_range(d.index_id, buf, is_reverse_cf ? 1 : 0,
                                         is_reverse_cf ? 0 : 1);

        rocksdb_rpc_log(14346, "run: rocksdb_DeleteFilesInRange");

        // ALTER
        // rocksdb::Status status = DeleteFilesInRange(rdb->GetBaseDB(), cfh,
        //                                             &range.start,
        //                                             &range.limit);
        rocksdb::Status status =
            rocksdb_DeleteFilesInRange(rocksdb_TransactionDB__GetBaseDB(rdb),
                                       cfh, range.start, range.limit);

        if (!status.ok()) {
          if (status.IsIncomplete()) {
            continue;
          } else if (status.IsShutdownInProgress()) {
            break;
          }
          rdb_handle_io_error(status, RDB_IO_ERROR_BG_THREAD);
        }

        rocksdb_rpc_log(14371, "run: rocksdb_TransactionDB__CompactRange");

        // ALTER
        // status = rdb->CompactRange(getCompactRangeOptions(), cfh,
        // &range.start,
        //                            &range.limit);
        status = rocksdb_TransactionDB__CompactRange(
            rdb, getCompactRangeOptions(), cfh, range.start, range.limit);

        if (!status.ok()) {
          if (status.IsIncomplete()) {
            continue;
          } else if (status.IsShutdownInProgress()) {
            break;
          }
          rdb_handle_io_error(status, RDB_IO_ERROR_BG_THREAD);
        }

        // ALTER
        // if (is_myrocks_index_empty(cfh.get(), is_reverse_cf, read_opts,
        //                            d.index_id)) {
        //   finished.insert(d);
        // }
        if (is_myrocks_index_empty(cfh, is_reverse_cf, read_opts, d.index_id)) {
          finished.insert(d);
        }
      }

      if (!finished.empty()) {
        dict_manager.finish_drop_indexes(finished);
      }
    }

    DBUG_EXECUTE_IF("rocksdb_drop_cf", {
      THD *thd = new THD();
      thd->thread_stack = reinterpret_cast<char *>(&(thd));
      thd->store_globals();

      const char act[] = "now wait_for ready_to_drop_cf";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

      thd->restore_globals();
      delete thd;
    });

    // Remove dropped column family
    // 1. Get all cf ids from ongoing_index_drop.
    // 2. Get all cf ids for cfs marked as dropped.
    // 3. If a cf id is in the list of ongoing_index_drop
    // , skip removing this cf. It will be removed later.
    // 4. If it is not, proceed to remove the cf.
    //
    // This should be under dict_manager lock

    {
      std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
      std::unordered_set<uint32> dropped_cf_ids;
      dict_manager.get_all_dropped_cfs(&dropped_cf_ids);

      rocksdb_rpc_log(14428, "run: get_ongoing_drop_indexes");

      if (!dropped_cf_ids.empty()) {
        std::unordered_set<GL_INDEX_ID> ongoing_drop_indices;
        dict_manager.get_ongoing_drop_indexes(&ongoing_drop_indices);

        std::unordered_set<uint32> ongoing_drop_cf_ids;
        for (const auto index : ongoing_drop_indices) {
          ongoing_drop_cf_ids.insert(index.cf_id);
        }

        for (const auto cf_id : dropped_cf_ids) {
          if (ongoing_drop_cf_ids.find(cf_id) == ongoing_drop_cf_ids.end()) {
            cf_manager.remove_dropped_cf(&dict_manager, rdb, cf_id);
          }
        }
      }
    }

    DBUG_EXECUTE_IF("rocksdb_drop_cf", {
      THD *thd = new THD();
      thd->thread_stack = reinterpret_cast<char *>(&(thd));
      thd->store_globals();

      const char act[] = "now signal drop_cf_done";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

      thd->restore_globals();
      delete thd;
    });
    RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
  }

  RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);
  rocksdb_rpc_log(14458, "run: end");
}

Rdb_tbl_def *ha_rocksdb::get_table_if_exists(const char *const tablename) {
  rocksdb_rpc_log(14462, "get_table_if_exists: start");

  std::string str;
  if (rdb_normalize_tablename(tablename, &str) != HA_EXIT_SUCCESS) {
    // We were not passed table name?
    DBUG_ASSERT(0);
    rocksdb_rpc_log(14468, "get_table_if_exists: end");

    return nullptr;
  }
  rocksdb_rpc_log(14472, "get_table_if_exists: end");

  return ddl_manager.find(str);
}

/*
  Overload func for delete table ---it deletes table meta data in data
  dictionary immediately and delete real data in background thread(async)

  @param tbl       IN      MyRocks table definition

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/
int ha_rocksdb::delete_table(Rdb_tbl_def *const tbl) {
  rocksdb_rpc_log(14488, "delete_table: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(tbl != nullptr);
  DBUG_ASSERT(m_tbl_def == nullptr || m_tbl_def == tbl);

  // ALTER
  // const std::unique_ptr<rocksdb::WriteBatch> wb = dict_manager.begin();
  // rocksdb::WriteBatch *const batch = wb.get();
  rocksdb_rpc_log(14498, "delete_table: dict_manager.begin");

  rocksdb::WriteBatch *batch = dict_manager.begin();

  DBUG_EXECUTE_IF("rocksdb_before_delete_table", {
    const char act[] =
        "now signal ready_to_mark_cf_dropped_before_delete_table wait_for "
        "mark_cf_dropped_done_before_delete_table";
    DBUG_ASSERT(!debug_sync_set_action(ha_thd(), STRING_WITH_LEN(act)));
  });

  {
    rocksdb_rpc_log(14513, "delete_table: dict_manager.add_drop_table");

    std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
    dict_manager.add_drop_table(tbl->m_key_descr_arr, tbl->m_key_count, batch);

    /*
      Remove the table entry in data dictionary (this will also remove it from
      the persistent data dictionary).
    */
    ddl_manager.remove(tbl, batch, true);

    int err = dict_manager.commit(batch);
    if (err) {
      rocksdb_rpc_log(14523, "delete_table: end");

      DBUG_RETURN(err);
    }
  }

  DBUG_EXECUTE_IF("rocksdb_after_delete_table", {
    const char act[] =
        "now signal ready_to_mark_cf_dropped_after_delete_table "
        "wait_for mark_cf_dropped_done_after_delete_table";
    DBUG_ASSERT(!debug_sync_set_action(ha_thd(), STRING_WITH_LEN(act)));
  });

  rdb_drop_idx_thread.signal();
  // avoid dangling pointer
  m_tbl_def = nullptr;
  rocksdb_rpc_log(14539, "delete_table: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
  Note: the following function is called when the table is not open. That is,
  this->table==nullptr, pk_key_descr==nullptr, etc.

  tablename points to line in form "./dbname/tablename".

  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (can be SE-specific)
*/

int ha_rocksdb::delete_table(const char *const tablename) {
  rocksdb_rpc_log(14556, "delete_table: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(tablename != nullptr);

  /* Find the table in the hash */
  rocksdb_rpc_log(14565, "delete_table: get_table_if_exists");

  Rdb_tbl_def *const tbl = get_table_if_exists(tablename);
  if (!tbl) {
    rocksdb_rpc_log(14569, "delete_table: end");

    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  }

  rocksdb_rpc_log(14574, "delete_table: end");

  DBUG_RETURN(delete_table(tbl));
}

int ha_rocksdb::remove_rows(Rdb_tbl_def *const tbl) {
  rocksdb_rpc_log(14578, "remove_rows: start");

  const rocksdb::WriteOptions wo =
      rdb_get_rocksdb_write_options(handler::ha_thd());

  // ALTER
  // rocksdb::ReadOptions opts;
  // opts.total_order_seek = true;

  rocksdb_rpc_log(14589, "remove_rows: rocksdb_ReadOptions__NewReadOptions");

  rocksdb::ReadOptions *opts = rocksdb_ReadOptions__NewReadOptions();
  rocksdb_ReadOptions__SetBoolProperty(opts, "total_order_seek", true);
  Rdb_transaction *const tx = get_or_create_tx(table->in_use);

  char key_buf[MAX_KEY_LENGTH];
  uint key_len;
  ulonglong bytes_written = 0;

  uchar lower_bound_buf[Rdb_key_def::INDEX_NUMBER_SIZE];
  uchar upper_bound_buf[Rdb_key_def::INDEX_NUMBER_SIZE];
  rocksdb::Slice lower_bound_slice;
  rocksdb::Slice upper_bound_slice;

  /*
    Remove all records in each index.
    (This is is not crash-safe, but it doesn't matter, because bulk row
    deletion will be handled on rocksdb side)
  */
  rocksdb_rpc_log(14607, "remove_rows: lower upper bound slice");

  for (uint i = 0; i < tbl->m_key_count; i++) {
    const Rdb_key_def &kd = *tbl->m_key_descr_arr[i];
    kd.get_infimum_key(reinterpret_cast<uchar *>(key_buf), &key_len);
    rocksdb::ColumnFamilyHandle *cf = kd.get_cf();
    const rocksdb::Slice table_key(key_buf, key_len);
    DBUG_ASSERT(key_len == Rdb_key_def::INDEX_NUMBER_SIZE);
    if (THDVAR(ha_thd(), enable_iterate_bounds)) {
      setup_iterator_bounds(kd, table_key, Rdb_key_def::INDEX_NUMBER_SIZE,
                            lower_bound_buf, upper_bound_buf,
                            &lower_bound_slice, &upper_bound_slice);

      rocksdb_rpc_log(14625, "remove_rows: rocksdb_ReadOptions__SetBound");

      // ALTER
      // opts.iterate_lower_bound = &lower_bound_slice;
      // opts.iterate_upper_bound = &upper_bound_slice;
      rocksdb_ReadOptions__SetBound(opts, lower_bound_slice, false, false);
      rocksdb_ReadOptions__SetBound(opts, upper_bound_slice, true, false);
    } else {
      // ALTER
      // opts.iterate_lower_bound = nullptr;
      // opts.iterate_upper_bound = nullptr;
      rocksdb_rpc_log(14634, "remove_rows: rocksdb_ReadOptions__SetBound");

      rocksdb_ReadOptions__SetBound(opts, lower_bound_slice, false, true);
      rocksdb_ReadOptions__SetBound(opts, upper_bound_slice, true, true);
    }
    rocksdb_rpc_log(14640, "remove_rows: rocksdb_TransactionDB__NewIterator");

    // ALTER
    // std::unique_ptr<rocksdb::Iterator> it(rdb->NewIterator(opts, cf));
    rocksdb::Iterator *it = rocksdb_TransactionDB__NewIterator(rdb, opts, cf);

    // ALTER
    // it->Seek(table_key);
    rocksdb_rpc_log(14646, "remove_rows: rocksdb_Iterator__Seek");

    rocksdb_Iterator__Seek(it, table_key);
    while (rocksdb_Iterator__Valid(it)) {
      rocksdb_rpc_log(14650, "remove_rows: rocksdb_Iterator__key");

      const rocksdb::Slice key = rocksdb_Iterator__key(it);
      if (!kd.covers_key(key)) {
        break;
      }

      rocksdb::Status s;
      rocksdb_rpc_log(14659,
                      "remove_rows: rocksdb_TransactionDB__SingleDelete");

      if (can_use_single_delete(i)) {
        s = rocksdb_TransactionDB__SingleDelete(rdb, wo, cf, key);
      } else {
        s = rocksdb_TransactionDB__Delete(rdb, wo, cf, key);
      }

      if (!s.ok()) {
        return tx->set_status_error(table->in_use, s, *m_pk_descr, m_tbl_def,
                                    m_table_handler);
      }
      bytes_written += key.size();
      rocksdb_rpc_log(14673, "remove_rows: rocksdb_Iterator__Next");

      rocksdb_Iterator__Next(it);
    }
  }

  rocksdb_rpc_log(14676, "remove_rows: update_bytes_written");
  tx->update_bytes_written(bytes_written);

  rocksdb_rpc_log(14679, "remove_rows: end");

  return HA_EXIT_SUCCESS;
}

/**
  @return
    HA_EXIT_SUCCESS  OK
    other            HA_ERR error code (cannot be SE-specific)
*/
int ha_rocksdb::rename_table(const char *const from, const char *const to) {
  rocksdb_rpc_log(14690, "rename_table: start");

  DBUG_ENTER_FUNC();

  std::string from_str;
  std::string to_str;
  std::string from_db;
  std::string to_db;
  int rc;

  rocksdb_rpc_log(14700, "rename_table: rdb_is_tablename_normalized");

  if (rdb_is_tablename_normalized(from)) {
    from_str = from;
  } else {
    rc = rdb_normalize_tablename(from, &from_str);
    if (rc != HA_EXIT_SUCCESS) {
      DBUG_RETURN(rc);
    }
  }

  rocksdb_rpc_log(14713, "rename_table: rdb_split_normalized_tablename");

  rc = rdb_split_normalized_tablename(from_str, &from_db);
  if (rc != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(14717, "rename_table: end");

    DBUG_RETURN(rc);
  }

  if (rdb_is_tablename_normalized(to)) {
    to_str = to;
  } else {
    rc = rdb_normalize_tablename(to, &to_str);
    if (rc != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(14727, "rename_table: end");

      DBUG_RETURN(rc);
    }
  }

  rocksdb_rpc_log(14731, "rename_table: rdb_split_normalized_tablename");

  rc = rdb_split_normalized_tablename(to_str, &to_db);
  if (rc != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(14733, "rename_table: end");

    DBUG_RETURN(rc);
  }

  // If the user changed the database part of the name then validate that the
  // 'to' database exists.
  if (from_db != to_db && !rdb_database_exists(to_db)) {
    // If we return a RocksDB specific error code here we get
    // "error: 206 - Unknown error 206".  InnoDB gets
    // "error -1 - Unknown error -1" so let's match them.
    rocksdb_rpc_log(14748, "rename_table: end");

    DBUG_RETURN(-1);
  }

  DBUG_EXECUTE_IF("gen_sql_table_name", to_str = to_str + "#sql-test";);

  // ALTER
  // const std::unique_ptr<rocksdb::WriteBatch> wb = dict_manager.begin();
  // rocksdb::WriteBatch *const batch = wb.get();
  rocksdb_rpc_log(14758, "rename_table: dict_manager.begin");

  rocksdb::WriteBatch *batch = dict_manager.begin();

  // rename table is under dict_manager lock, and the cfs used
  // by indices of this table cannot be dropped during the process.
  dict_manager.lock();

  if (ddl_manager.rename(from_str, to_str, batch)) {
    rc = HA_ERR_NO_SUCH_TABLE;
  } else {
    rc = dict_manager.commit(batch);
  }
  dict_manager.unlock();

  rocksdb_rpc_log(14773, "rename_table: end");

  DBUG_RETURN(rc);
}

/**
  check_if_incompatible_data() called if ALTER TABLE can't detect otherwise
  if new and old definition are compatible

  @details If there are no other explicit signs like changed number of
  fields this function will be called by compare_tables()
  (sql/sql_tables.cc) to decide should we rewrite whole table or only .frm
  file.

*/

bool ha_rocksdb::check_if_incompatible_data(HA_CREATE_INFO *const info,
                                            uint table_changes) {
  rocksdb_rpc_log(14789, "check_if_incompatible_data: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(info != nullptr);

  // this function is needed only for online alter-table
  rocksdb_rpc_log(14798, "check_if_incompatible_data: end");

  DBUG_RETURN(COMPATIBLE_DATA_NO);
}

/**
  @return
    HA_EXIT_SUCCESS  OK
*/
int ha_rocksdb::extra(enum ha_extra_function operation) {
  rocksdb_rpc_log(14806, "extra: begin");

  DBUG_ENTER_FUNC();

  switch (operation) {
    case HA_EXTRA_KEYREAD:
      m_keyread_only = true;
      break;
    case HA_EXTRA_NO_KEYREAD:
      m_keyread_only = false;
      break;
    case HA_EXTRA_FLUSH:
      /*
        If the table has blobs, then they are part of m_retrieved_record.
        This call invalidates them.
      */
      // ALTER
      // m_retrieved_record.Reset();
      rocksdb_PinnableSlice__Reset(m_retrieved_record);
      break;
    case HA_EXTRA_INSERT_WITH_UPDATE:
      // INSERT ON DUPLICATE KEY UPDATE
      if (rocksdb_enable_insert_with_update_caching) {
        m_insert_with_update = true;
      }
      break;
    case HA_EXTRA_NO_IGNORE_DUP_KEY:
      // PAIRED with HA_EXTRA_INSERT_WITH_UPDATE or HA_EXTRA_WRITE_CAN_REPLACE
      // that indicates the end of REPLACE / INSERT ON DUPLICATE KEY
      m_insert_with_update = false;
      break;

    default:
      break;
  }

  rocksdb_rpc_log(14845, "extra: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.
*/
ha_rows ha_rocksdb::records_in_range(uint inx, key_range *const min_key,
                                     key_range *const max_key) {
  rocksdb_rpc_log(14855, "records_in_range: start");

  DBUG_ENTER_FUNC();

  ha_rows ret = THDVAR(ha_thd(), records_in_range);
  if (ret) {
    DBUG_EXECUTE_IF(
        "rocksdb_mrr_debug2", if (inx != 0) { ret /= 100; });
    rocksdb_rpc_log(14863, "records_in_range: end");

    DBUG_RETURN(ret);
  }
  if (table->force_index) {
    const ha_rows force_rows = THDVAR(ha_thd(), force_index_records_in_range);
    if (force_rows) {
      rocksdb_rpc_log(14870, "records_in_range: end");

      DBUG_RETURN(force_rows);
    }
  }

  const Rdb_key_def &kd = *m_key_descr_arr[inx];

  auto disk_size = kd.m_stats.m_actual_disk_size;
  if (disk_size == 0) disk_size = kd.m_stats.m_data_size;
  auto rows = kd.m_stats.m_rows;
  if (rows == 0 || disk_size == 0) {
    rows = 1;
    disk_size = ROCKSDB_ASSUMED_KEY_VALUE_DISK_SIZE;
  }
  ulonglong total_size = 0;
  ulonglong total_row = 0;
  records_in_range_internal(inx, min_key, max_key, disk_size, rows, &total_size,
                            &total_row);
  ret = total_row;
  /*
    GetApproximateSizes() gives estimates so ret might exceed stats.records.
    MySQL then decides to use full index scan rather than range scan, which
    is not efficient for most cases.
    To prevent this, changing estimated records slightly smaller than
    stats.records.
  */
  if (ret >= stats.records) {
    ret = stats.records * 0.99;
  }

  if (rocksdb_debug_optimizer_n_rows > 0) {
    ret = rocksdb_debug_optimizer_n_rows;
  } else if (ret == 0) {
    ret = 1;
  }

  rocksdb_rpc_log(14908, "records_in_range: end");

  DBUG_RETURN(ret);
}

/*
  Given a starting key and an ending key, estimate the total size of rows that
  will exist between the two keys.
*/
ulonglong ha_rocksdb::records_size_in_range(uint inx, key_range *const min_key,
                                            key_range *const max_key) {
  rocksdb_rpc_log(14916, "records_size_in_range: start");

  DBUG_ENTER_FUNC();
  ulonglong total_size = 0;
  ulonglong total_row = 0;
  records_in_range_internal(inx, min_key, max_key,
                            ROCKSDB_ASSUMED_KEY_VALUE_DISK_SIZE, 1, &total_size,
                            &total_row);
  rocksdb_rpc_log(14926, "records_size_in_range: end");

  DBUG_RETURN(total_size);
}

void ha_rocksdb::records_in_range_internal(uint inx, key_range *const min_key,
                                           key_range *const max_key,
                                           int64 disk_size, int64 rows,
                                           ulonglong *total_size,
                                           ulonglong *row_count) {
  rocksdb_rpc_log(14936, "records_in_range_internal: start");

  DBUG_ENTER_FUNC();

  const Rdb_key_def &kd = *m_key_descr_arr[inx];

  uint size1 = 0;
  if (min_key) {
    size1 = kd.pack_index_tuple(table, m_pack_buffer, m_sk_packed_tuple,
                                min_key->key, min_key->keypart_map);
    if (min_key->flag == HA_READ_PREFIX_LAST_OR_PREV ||
        min_key->flag == HA_READ_PREFIX_LAST ||
        min_key->flag == HA_READ_AFTER_KEY) {
      kd.successor(m_sk_packed_tuple, size1);
    }
  } else {
    kd.get_infimum_key(m_sk_packed_tuple, &size1);
  }

  rocksdb_rpc_log(14954, "records_in_range_internal: maxkey");

  uint size2 = 0;
  if (max_key) {
    size2 = kd.pack_index_tuple(table, m_pack_buffer, m_sk_packed_tuple_old,
                                max_key->key, max_key->keypart_map);
    if (max_key->flag == HA_READ_PREFIX_LAST_OR_PREV ||
        max_key->flag == HA_READ_PREFIX_LAST ||
        max_key->flag == HA_READ_AFTER_KEY) {
      kd.successor(m_sk_packed_tuple_old, size2);
    }
  } else {
    kd.get_supremum_key(m_sk_packed_tuple_old, &size2);
  }

  rocksdb_rpc_log(14968, "records_in_range_internal: init slice");

  const rocksdb::Slice slice1((const char *)m_sk_packed_tuple, size1);
  const rocksdb::Slice slice2((const char *)m_sk_packed_tuple_old, size2);

  // It's possible to get slice1 == slice2 for a non-inclusive range with the
  // right bound being successor() of the left one, e.g. "t.key>10 AND t.key<11"
  if (slice1.compare(slice2) >= 0) {
    // It's not possible to get slice2 > slice1
    DBUG_ASSERT(slice1.compare(slice2) == 0);
    rocksdb_rpc_log(14980, "records_in_range_internal: end");

    DBUG_VOID_RETURN;
  }

  rocksdb::Range r(kd.m_is_reverse_cf ? slice2 : slice1,
                   kd.m_is_reverse_cf ? slice1 : slice2);

  uint64_t sz = 0;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
  // Getting statistics, including from Memtables
  uint8_t include_flags = rocksdb::DB::INCLUDE_FILES;

  rocksdb_rpc_log(
      14993,
      "records_in_range_internal: rocksdb_TransactionDB__GetApproximateSizes");

  // ALTER
  // rdb->GetApproximateSizes(kd.get_cf(), &r, 1, &sz, include_flags);
  rocksdb_TransactionDB__GetApproximateSizes(rdb, kd.get_cf(), r, &sz,
                                             include_flags);

  *row_count = rows * ((double)sz / (double)disk_size);
  *total_size = sz;
  uint64_t memTableCount;

  // ALTER
  // rdb->GetApproximateMemTableStats(kd.get_cf(), r, &memTableCount, &sz);
  rocksdb_TransactionDB__GetApproximateMemTableStats(rdb, kd.get_cf(), r,
                                                     &memTableCount, &sz);
  *row_count += memTableCount;
  *total_size += sz;
  rocksdb_rpc_log(15012, "records_in_range_internal: end");

  DBUG_VOID_RETURN;
}

void ha_rocksdb::update_create_info(HA_CREATE_INFO *const create_info) {
  rocksdb_rpc_log(15018, "update_create_info: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(create_info != nullptr);

  if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) {
    create_info->auto_increment_value = m_tbl_def->m_auto_incr_val;
  }

  rocksdb_rpc_log(15030, "update_create_info: end");

  DBUG_VOID_RETURN;
}

/**
  @brief
  Doing manual compaction on OPTIMIZE TABLE in RocksDB.
  Compaction itself is executed by background thread in RocksDB, but
  CompactRange() waits until compaction completes so this function
  may take a long time.
  Since RocksDB dataset is allocated per index id, OPTIMIZE TABLE
  triggers manual compaction for all indexes of the table.
  @details
  Compaction range is from the beginning of the index id to
  the first row of the next index id. When using reverse order
  column family, the first row of the next index id should be
  the last row of the previous index id.

  @return
    HA_ADMIN_OK      OK
    other            HA_ADMIN error code
*/
int ha_rocksdb::optimize(THD *const thd, HA_CHECK_OPT *const check_opt) {
  rocksdb_rpc_log(15053, "optimize: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(thd != nullptr);
  DBUG_ASSERT(check_opt != nullptr);

  for (uint i = 0; i < table->s->keys; i++) {
    uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2];
    auto range = get_range(i, buf);
    // ALTER
    // const rocksdb::Status s = rdb->CompactRange(getCompactRangeOptions(),
    //                                             m_key_descr_arr[i]->get_cf(),
    //                                             &range.start, &range.limit);
    rocksdb_rpc_log(15072, "optimize: rocksdb_TransactionDB__CompactRange");

    const rocksdb::Status s = rocksdb_TransactionDB__CompactRange(
        rdb, getCompactRangeOptions(), m_key_descr_arr[i]->get_cf(),
        range.start, range.limit);
    if (!s.ok()) {
      rocksdb_rpc_log(15073, "optimize: end");
      DBUG_RETURN(rdb_error_to_mysql(s));
    }
  }
  rocksdb_rpc_log(15078, "optimize: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

static void init_stats(
    const std::unordered_map<GL_INDEX_ID, std::shared_ptr<const Rdb_key_def>>
        &to_recalc,
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> *stats) {
  rocksdb_rpc_log(15085, "init_stats: start");

  for (const auto &it : to_recalc) {
    const GL_INDEX_ID index_id = it.first;
    auto &kd = it.second;

    (*stats).emplace(index_id, Rdb_index_stats(index_id));
    DBUG_ASSERT(kd->get_key_parts() > 0);
    (*stats)[index_id].m_distinct_keys_per_prefix.resize(kd->get_key_parts());
  }
  rocksdb_rpc_log(15095, "init_stats: end");
}

/**
  Calculate the following index stats for all indexes of a table:
  number of rows, file size, and cardinality. It adopts an index
  scan approach using rocksdb::Iterator. Sampling is used to
  accelerate the scan.
**/
static int calculate_cardinality_table_scan(
    const std::unordered_map<GL_INDEX_ID, std::shared_ptr<const Rdb_key_def>>
        &to_recalc,
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> *stats,
    table_cardinality_scan_type scan_type, uint64_t max_num_rows_scanned,
    std::atomic<THD::killed_state> *killed) {
  rocksdb_rpc_log(15110, "calculate_cardinality_table_scan: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(scan_type != SCAN_TYPE_NONE);
  init_stats(to_recalc, stats);

  // ALTER
  // auto read_opts = rocksdb::ReadOptions();
  // read_opts.fill_cache = false;
  // if (scan_type == SCAN_TYPE_MEMTABLE_ONLY) {
  //   read_opts.read_tier = rocksdb::ReadTier::kMemtableTier;
  // } else {
  //   read_opts.total_order_seek = true;
  // }
  rocksdb_rpc_log(15110,
                  "calculate_cardinality_table_scan: "
                  "myrocks_calculate_cardinality_table_scan__ReadOptions");

  rocksdb::ReadOptions *read_opts =
      myrocks_calculate_cardinality_table_scan__ReadOptions(
          scan_type == SCAN_TYPE_MEMTABLE_ONLY);

  Rdb_tbl_card_coll cardinality_collector(rocksdb_table_stats_sampling_pct);

  for (const auto &it_kd : to_recalc) {
    const GL_INDEX_ID index_id = it_kd.first;

    if (!ddl_manager.safe_find(index_id)) {
      // If index id is not in ddl manager, then it has been dropped.
      // Skip scanning index
      continue;
    }

    rocksdb_rpc_log(15144,
                    "calculate_cardinality_table_scan: "
                    "kd = it_kd.second");

    const std::shared_ptr<const Rdb_key_def> &kd = it_kd.second;
    DBUG_ASSERT(index_id == kd->get_gl_index_id());
    Rdb_index_stats &stat = (*stats)[kd->get_gl_index_id()];

    uchar r_buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2];
    auto r = myrocks_rpc::get_range(*kd, r_buf);
    uint64_t memtableCount;
    uint64_t memtableSize;

    rocksdb_rpc_log(15163,
                    "calculate_cardinality_table_scan: "
                    "rocksdb_TransactionDB__GetApproximateMemTableStats");
    // ALTER
    // rdb->GetApproximateMemTableStats(kd->get_cf(), r, &memtableCount,
    //                                  &memtableSize);
    rocksdb_TransactionDB__GetApproximateMemTableStats(
        rdb, kd->get_cf(), r, &memtableCount, &memtableSize);

    if (scan_type == SCAN_TYPE_MEMTABLE_ONLY &&
        memtableCount < (uint64_t)stat.m_rows / 10) {
      // skip tables that already have enough stats from SST files to reduce
      // overhead and avoid degradation of big tables stats by sampling from
      // relatively tiny (less than 10% of full data set) memtable dataset
      continue;
    }

    // Set memtable count to row count
    stat.m_rows = memtableCount;

    if (scan_type == SCAN_TYPE_FULL_TABLE) {
      // Set memtable size to file size
      stat.m_actual_disk_size = memtableSize;
    }

    // ALTER
    // std::unique_ptr<rocksdb::Iterator> it =
    // std::unique_ptr<rocksdb::Iterator>(
    //     rdb->NewIterator(read_opts, kd->get_cf()));
    rocksdb_rpc_log(
        15188,
        "calculate_cardinality_table_scan: rocksdb_TransactionDB__NewIterator");
    rocksdb::Iterator *it =
        rocksdb_TransactionDB__NewIterator(rdb, read_opts, kd->get_cf());

    rocksdb::Slice first_index_key((const char *)r_buf,
                                   Rdb_key_def::INDEX_NUMBER_SIZE);

    // Reset m_last_key for new index
    cardinality_collector.Reset();
    uint64_t rows_scanned = 0ul;

    // ALTER
    // for (it->Seek(first_index_key); is_valid_iterator(it.get()); it->Next())
    // {
    rocksdb_rpc_log(15200,
                    "calculate_cardinality_table_scan: rocksdb_Iterator__Seek");

    for (rocksdb_Iterator__Seek(it, first_index_key); is_valid_iterator(it);
         rocksdb_Iterator__Next(it)) {
      if (killed && *killed) {
        // NO_LINT_DEBUG
        sql_print_information(
            "Index stats calculation for index %s with id (%u,%u) is "
            "terminated",
            kd->get_name().c_str(), stat.m_gl_index_id.cf_id,
            stat.m_gl_index_id.index_id);
        rocksdb_rpc_log(15217, "calculate_cardinality_table_scan: end");

        DBUG_RETURN(HA_EXIT_FAILURE);
      }

      rocksdb_rpc_log(
          15223, "calculate_cardinality_table_scan: rocksdb_Iterator__key");
      // ALTER
      // const rocksdb::Slice key = it->key();
      const rocksdb::Slice key = rocksdb_Iterator__key(it);

      if ((scan_type == SCAN_TYPE_FULL_TABLE && max_num_rows_scanned > 0 &&
           rows_scanned >= max_num_rows_scanned) ||
          !kd->covers_key(key)) {
        break;  // end of this index
      }

      cardinality_collector.ProcessKey(key, kd.get(), &stat);
      rows_scanned++;
    }

    cardinality_collector.SetCardinality(&stat);
    cardinality_collector.AdjustStats(&stat);

    DBUG_EXECUTE_IF("rocksdb_calculate_stats", {
      if (kd->get_name() == "secondary_key") {
        THD *thd = new THD();
        thd->thread_stack = reinterpret_cast<char *>(&thd);
        thd->store_globals();

        const char act[] =
            "now signal ready_to_drop_index wait_for ready_to_save_index_stats";
        DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

        thd->restore_globals();
        delete thd;
      }
    });
  }

  rocksdb_rpc_log(15257, "calculate_cardinality_table_scan: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

static void reset_cardinality(
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> *stats) {
  rocksdb_rpc_log(15261, "reset_cardinality: start");

  for (auto &src : *stats) {
    Rdb_index_stats &stat = src.second;
    stat.reset_cardinality();
  }
  rocksdb_rpc_log(15268, "reset_cardinality: end");
}

static void merge_stats(
    const std::unordered_map<GL_INDEX_ID, std::shared_ptr<const Rdb_key_def>>
        &to_recalc,
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> *stats,
    const std::unordered_map<GL_INDEX_ID, Rdb_index_stats> &card_stats) {
  rocksdb_rpc_log(15275, "reset_cardinality: start");

  DBUG_ASSERT(stats->size() == card_stats.size());

  rocksdb_rpc_log(15275, "reset_cardinality: start");

  for (auto &src : *stats) {
    auto index_id = src.first;
    Rdb_index_stats &stat = src.second;
    auto it = card_stats.find(index_id);
    DBUG_ASSERT(it != card_stats.end());

    auto it_index = to_recalc.find(index_id);
    DBUG_ASSERT(it_index != to_recalc.end());
    stat.merge(it->second, true, it_index->second->max_storage_fmt_length());
  }
  rocksdb_rpc_log(15292, "reset_cardinality: end");
}

static void adjust_cardinality(
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> *stats,
    table_cardinality_scan_type scan_type, uint64_t max_num_rows_scanned) {
  rocksdb_rpc_log(15298, "adjust_cardinality: start");

  DBUG_ASSERT(scan_type == SCAN_TYPE_FULL_TABLE);
  DBUG_ASSERT(max_num_rows_scanned > 0);

  for (auto &src : *stats) {
    Rdb_index_stats &stat = src.second;
    if ((uint64_t)stat.m_rows > max_num_rows_scanned) {
      stat.adjust_cardinality(stat.m_rows / max_num_rows_scanned);
    }
#ifndef DBUG_OFF
    for (size_t i = 0; i < stat.m_distinct_keys_per_prefix.size(); i++) {
      DBUG_ASSERT(stat.m_distinct_keys_per_prefix[i] <= stat.m_rows);
    }
#endif
  }
  rocksdb_rpc_log(15315, "adjust_cardinality: end");
}

static int read_stats_from_ssts(
    const std::unordered_map<GL_INDEX_ID, std::shared_ptr<const Rdb_key_def>>
        &to_recalc,
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> *stats) {
  rocksdb_rpc_log(15320, "read_stats_from_ssts: start");

  DBUG_ENTER_FUNC();

  init_stats(to_recalc, stats);

  // find per column family key ranges which need to be queried
  std::unordered_map<rocksdb::ColumnFamilyHandle *, std::vector<rocksdb::Range>>
      ranges;
  std::vector<uchar> buf(to_recalc.size() * 2 * Rdb_key_def::INDEX_NUMBER_SIZE);

  uchar *bufp = buf.data();
  for (const auto &it : to_recalc) {
    auto &kd = it.second;
    ranges[kd->get_cf()].push_back(myrocks_rpc::get_range(*kd, bufp));
    bufp += 2 * Rdb_key_def::INDEX_NUMBER_SIZE;
  }

  rocksdb_rpc_log(15339, "read_stats_from_ssts: TablePropertiesCollection");

  // get RocksDB table properties for these ranges
  rocksdb::TablePropertiesCollection props;
  for (const auto &it : ranges) {
    const auto old_size MY_ATTRIBUTE((__unused__)) = props.size();

    rocksdb_rpc_log(15350,
                    "read_stats_from_ssts: "
                    "rocksdb_TransactionDB__GetPropertiesOfTablesInRange");

    // ALTER
    // const auto status = rdb->GetPropertiesOfTablesInRange(
    //     it.first, &it.second[0], it.second.size(), &props);
    const auto status = rocksdb_TransactionDB__GetPropertiesOfTablesInRange(
        rdb, it.first, it.second, &props);
    DBUG_ASSERT(props.size() >= old_size);
    if (!status.ok()) {
      rocksdb_rpc_log(15356, "read_stats_from_ssts: end");

      DBUG_RETURN(ha_rocksdb::rdb_error_to_mysql(
          status, "Could not access RocksDB properties"));
    }
  }

  int num_sst = 0;
  for (const auto &it : props) {
    std::vector<Rdb_index_stats> sst_stats;
    Rdb_tbl_prop_coll::read_stats_from_tbl_props(it.second, &sst_stats);
    /*
      sst_stats is a list of index statistics for indexes that have entries
      in the current SST file.
    */
    for (const auto &it1 : sst_stats) {
      /*
        Only update statistics for indexes that belong to this SQL table.

        The reason is: We are walking through all SST files that have
        entries from this table (and so can compute good statistics). For
        other SQL tables, it can be that we're only seeing a small fraction
        of table's entries (and so we can't update statistics based on that).
      */
      if (stats->find(it1.m_gl_index_id) == stats->end()) {
        continue;
      }

      auto it_index = to_recalc.find(it1.m_gl_index_id);
      DBUG_ASSERT(it_index != to_recalc.end());
      if (it_index == to_recalc.end()) {
        continue;
      }

      (*stats)[it1.m_gl_index_id].merge(
          it1, true, it_index->second->max_storage_fmt_length());
    }
    num_sst++;
  }

  rocksdb_rpc_log(15397, "read_stats_from_ssts: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

static int calculate_stats(
    const std::unordered_map<GL_INDEX_ID, std::shared_ptr<const Rdb_key_def>>
        &to_recalc,
    table_cardinality_scan_type scan_type,
    std::atomic<THD::killed_state> *killed) {
  rocksdb_rpc_log(15405, "calculate_stats: start");

  DBUG_ENTER_FUNC();

  std::unordered_map<GL_INDEX_ID, Rdb_index_stats> stats;
  int ret = read_stats_from_ssts(to_recalc, &stats);
  if (ret != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(15414, "calculate_stats: end");

    DBUG_RETURN(ret);
  }

  rocksdb_rpc_log(15419, "calculate_stats: scan_type");

  if (scan_type != SCAN_TYPE_NONE) {
    std::unordered_map<GL_INDEX_ID, Rdb_index_stats> card_stats;
    uint64_t max_num_rows_scanned = rocksdb_table_stats_max_num_rows_scanned;
    ret = calculate_cardinality_table_scan(to_recalc, &card_stats, scan_type,
                                           max_num_rows_scanned, killed);
    if (ret != HA_EXIT_SUCCESS) {
      DBUG_RETURN(ret);
    }

    if (scan_type == SCAN_TYPE_FULL_TABLE) {
      reset_cardinality(&stats);
    }

    merge_stats(to_recalc, &stats, card_stats);
    if (scan_type == SCAN_TYPE_FULL_TABLE && max_num_rows_scanned > 0) {
      adjust_cardinality(&stats, scan_type, max_num_rows_scanned);
    }
  }

  rocksdb_rpc_log(15442, "calculate_stats: ddl_manager.set_stats");

  // set and persist new stats
  ddl_manager.set_stats(stats);
  ddl_manager.persist_stats(true);

  rocksdb_rpc_log(15446, "calculate_stats: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

static int calculate_stats_for_table(
    const std::string &tbl_name, table_cardinality_scan_type scan_type,
    std::atomic<THD::killed_state> *killed = nullptr) {
  rocksdb_rpc_log(15454, "calculate_stats: start");

  DBUG_ENTER_FUNC();
  std::unordered_map<GL_INDEX_ID, std::shared_ptr<const Rdb_key_def>> to_recalc;
  std::vector<GL_INDEX_ID> indexes;
  ddl_manager.find_indexes(tbl_name, &indexes);

  for (const auto &index : indexes) {
    std::shared_ptr<const Rdb_key_def> keydef = ddl_manager.safe_find(index);

    if (keydef) {
      to_recalc.insert(std::make_pair(keydef->get_gl_index_id(), keydef));
    }
  }

  if (to_recalc.empty()) {
    rocksdb_rpc_log(15470, "calculate_stats: end");

    DBUG_RETURN(HA_EXIT_FAILURE);
  }

  DBUG_EXECUTE_IF("rocksdb_is_bg_thread_drop_table", {
    if (tbl_name == "test.t") {
      THD *thd = new THD();
      thd->thread_stack = reinterpret_cast<char *>(&thd);
      thd->store_globals();

      const char act[] = "now signal ready_to_drop_table";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

      thd->restore_globals();
      delete thd;
    }
  });
  rocksdb_rpc_log(15488, "calculate_stats: calculate_stats");

  int err = calculate_stats(to_recalc, scan_type, killed);
  if (err != HA_EXIT_SUCCESS) {
    DBUG_RETURN(err);
  }

  DBUG_EXECUTE_IF("rocksdb_is_bg_thread_drop_table", {
    if (tbl_name == "test.t") {
      THD *thd = new THD();
      thd->thread_stack = reinterpret_cast<char *>(&thd);
      thd->store_globals();

      const char act[] = "now wait_for ready_to_save_table_stats";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

      thd->restore_globals();
      delete thd;
    }
  });

  rocksdb_rpc_log(15509, "calculate_stats: SCAN_TYPE_FULL_TABLE");

  if (scan_type == SCAN_TYPE_FULL_TABLE) {
    // Save table stats including number of rows
    // and modified counter
    ddl_manager.set_table_stats(tbl_name);
  }

  rocksdb_rpc_log(15517, "calculate_stats: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/*
  @return
    HA_ADMIN_OK      OK
    other            HA_ADMIN error code
*/
int ha_rocksdb::analyze(THD *const thd, HA_CHECK_OPT *const check_opt) {
  rocksdb_rpc_log(15528, "analyze: start");

  DBUG_ENTER_FUNC();

  if (table) {
    table_cardinality_scan_type scan_type = rocksdb_table_stats_use_table_scan
                                                ? SCAN_TYPE_FULL_TABLE
                                                : SCAN_TYPE_MEMTABLE_ONLY;

    if (calculate_stats_for_table(m_tbl_def->full_tablename(), scan_type,
                                  &(thd->killed)) != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(15539, "analyze: end");

      DBUG_RETURN(HA_ADMIN_FAILED);
    }
  }

  // A call to ::info is needed to repopulate some SQL level structs. This is
  // necessary for online analyze because we cannot rely on another ::open
  // call to call info for us.
  if (info(HA_STATUS_CONST | HA_STATUS_VARIABLE) != HA_EXIT_SUCCESS) {
    rocksdb_rpc_log(15549, "analyze: end");

    DBUG_RETURN(HA_ADMIN_FAILED);
  }

  rocksdb_rpc_log(15554, "analyze: end");

  DBUG_RETURN(HA_ADMIN_OK);
}

int ha_rocksdb::adjust_handler_stats_sst_and_memtable() {
  rocksdb_rpc_log(15558, "adjust_handler_stats_sst_and_memtable: start");

  DBUG_ENTER_FUNC();

  /*
    If any stats are negative due to bad cached stats, re-run analyze table
    and re-retrieve the stats.
  */
  if (static_cast<longlong>(stats.data_file_length) < 0 ||
      static_cast<longlong>(stats.index_file_length) < 0 ||
      static_cast<longlong>(stats.records) < 0) {
    if (calculate_stats_for_table(m_tbl_def->full_tablename(),
                                  SCAN_TYPE_NONE)) {
      rocksdb_rpc_log(15573, "adjust_handler_stats_sst_and_memtable: end");

      DBUG_RETURN(HA_EXIT_FAILURE);
    }

    update_stats();
    rocksdb_rpc_log(15576, "adjust_handler_stats_sst_and_memtable: end");
  }

  // if number of records is hardcoded, we do not want to force computation
  // of memtable cardinalities
  if (stats.records == 0 || (rocksdb_force_compute_memtable_stats &&
                             rocksdb_debug_optimizer_n_rows == 0)) {
    // First, compute SST files stats
    uchar buf[Rdb_key_def::INDEX_NUMBER_SIZE * 2];
    auto r = get_range(pk_index(table, m_tbl_def), buf);
    uint64_t sz = 0;

    uint8_t include_flags = rocksdb::DB::INCLUDE_FILES;

    // recompute SST files stats only if records count is 0
    if (stats.records == 0) {
      // ALTER
      // rdb->GetApproximateSizes(m_pk_descr->get_cf(), &r, 1, &sz,
      // include_flags);
      rocksdb_rpc_log(15597,
                      "adjust_handler_stats_sst_and_memtable: "
                      "rocksdb_TransactionDB__GetApproximateSizes");

      rocksdb_TransactionDB__GetApproximateSizes(rdb, m_pk_descr->get_cf(), r,
                                                 &sz, include_flags);
      stats.records += sz / ROCKSDB_ASSUMED_KEY_VALUE_DISK_SIZE;
      stats.data_file_length += sz;
    }

    // Second, compute memtable stats. This call is expensive, so cache
    // values computed for some time.
    uint64_t cachetime = rocksdb_force_compute_memtable_stats_cachetime;
    uint64_t time = (cachetime == 0) ? 0 : my_micro_time();
    if (cachetime == 0 ||
        time > m_table_handler->m_mtcache_last_update + cachetime) {
      uint64_t memtableCount;
      uint64_t memtableSize;

      rocksdb_rpc_log(15627,
                      "adjust_handler_stats_sst_and_memtable: "
                      "rocksdb_TransactionDB__GetApproximateMemTableStats");

      // the stats below are calculated from skiplist wich is a probablistic
      // data structure, so the results vary between test runs
      // it also can return 0 for quite a large tables which means that
      // cardinality for memtable only indxes will be reported as 0

      // rdb->GetApproximateMemTableStats(m_pk_descr->get_cf(), r,
      // &memtableCount,
      //                                  &memtableSize);
      rocksdb_TransactionDB__GetApproximateMemTableStats(
          rdb, m_pk_descr->get_cf(), r, &memtableCount, &memtableSize);

      // Atomically update all of these fields at the same time
      if (cachetime > 0) {
        if (m_table_handler->m_mtcache_lock.fetch_add(
                1, std::memory_order_acquire) == 0) {
          m_table_handler->m_mtcache_count = memtableCount;
          m_table_handler->m_mtcache_size = memtableSize;
          m_table_handler->m_mtcache_last_update = time;
        }
        m_table_handler->m_mtcache_lock.fetch_sub(1, std::memory_order_release);
      }

      stats.records += memtableCount;
      stats.data_file_length += memtableSize;
    } else {
      // Cached data is still valid, so use it instead
      stats.records += m_table_handler->m_mtcache_count;
      stats.data_file_length += m_table_handler->m_mtcache_size;
    }
  }
  rocksdb_rpc_log(15651, "adjust_handler_stats_sst_and_memtable: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

void ha_rocksdb::get_auto_increment(ulonglong off, ulonglong inc,
                                    ulonglong nb_desired_values,
                                    ulonglong *const first_value,
                                    ulonglong *const nb_reserved_values) {
  /*
    MySQL has a somewhat complicated way of handling the auto-increment value.
    The first time get_auto_increment is called for a statement,
    nb_desired_values is the estimate for how many values will be needed.  The
    engine can then reserve some values, and those will be automatically used
    by MySQL, until a hard-coded value shows up in the insert statement, after
    which MySQL again calls this function to reset its starting value.
   *
    For simplicity we will just ignore nb_desired_values - we aren't going to
    reserve any extra values for a multi-insert statement.  Each row will
    simply acquire the next value as needed and we will always tell MySQL that
    we only reserved 1 value.  Since we are using an atomic value for
    m_auto_incr_val this should be safe - if we had to grab a mutex, doing
    an actual reserve of some values might be a better solution.
   */
  rocksdb_rpc_log(15675, "get_auto_increment: start");
  DEBUG_SYNC(ha_thd(), "rocksdb.autoinc_vars");

  if (off > inc) {
    off = 1;
  }

  Field *field;
  ulonglong new_val, max_val;
  field = table->key_info[table->s->next_number_index].key_part[0].field;
  max_val = rdb_get_int_col_max_value(field);

  // Local variable reference to simplify code below
  auto &auto_incr = m_tbl_def->m_auto_incr_val;

  if (inc == 1) {
    DBUG_ASSERT(off == 1);
    // Optimization for the standard case where we are always simply
    // incrementing from the last position

    // Use CAS operation in a loop to make sure automically get the next auto
    // increment value while ensuring that we don't wrap around to a negative
    // number.
    //
    // We set auto_incr to the min of max_val and new_val + 1. This means that
    // if we're at the maximum, we should be returning the same value for
    // multiple rows, resulting in duplicate key errors (as expected).
    //
    // If we return values greater than the max, the SQL layer will "truncate"
    // the value anyway, but it means that we store invalid values into
    // auto_incr that will be visible in SHOW CREATE TABLE.
    rocksdb_rpc_log(15703, "get_auto_increment: compare_exchange_weak");
    new_val = auto_incr;
    while (new_val != std::numeric_limits<ulonglong>::max()) {
      if (auto_incr.compare_exchange_weak(new_val,
                                          std::min(new_val + 1, max_val))) {
        break;
      }
    }
  } else {
    // The next value can be more complicated if either 'inc' or 'off' is not 1
    ulonglong last_val = auto_incr;

    if (last_val > max_val) {
      new_val = std::numeric_limits<ulonglong>::max();
    } else {
      // Loop until we can correctly update the atomic value
      do {
        DBUG_ASSERT(last_val > 0);
        // Calculate the next value in the auto increment series: offset
        // + N * increment where N is 0, 1, 2, ...
        //
        // For further information please visit:
        // http://dev.mysql.com/doc/refman/5.7/en/replication-options-master.html
        //
        // The following is confusing so here is an explanation:
        // To get the next number in the sequence above you subtract out the
        // offset, calculate the next sequence (N * increment) and then add the
        // offset back in.
        //
        // The additions are rearranged to avoid overflow.  The following is
        // equivalent to (last_val - 1 + inc - off) / inc. This uses the fact
        // that (a+b)/c = a/c + b/c + (a%c + b%c)/c. To show why:
        //
        // (a+b)/c
        // = (a - a%c + a%c + b - b%c + b%c) / c
        // = (a - a%c) / c + (b - b%c) / c + (a%c + b%c) / c
        // = a/c + b/c + (a%c + b%c) / c
        //
        // Now, substitute a = last_val - 1, b = inc - off, c = inc to get the
        // following statement.
        ulonglong n =
            (last_val - 1) / inc + ((last_val - 1) % inc + inc - off) / inc;

        // Check if n * inc + off will overflow. This can only happen if we have
        // an UNSIGNED BIGINT field.
        if (n > (std::numeric_limits<ulonglong>::max() - off) / inc) {
          DBUG_ASSERT(max_val == std::numeric_limits<ulonglong>::max());
          // The 'last_val' value is already equal to or larger than the largest
          // value in the sequence.  Continuing would wrap around (technically
          // the behavior would be undefined).  What should we do?
          // We could:
          //   1) set the new value to the last possible number in our sequence
          //      as described above.  The problem with this is that this
          //      number could be smaller than a value in an existing row.
          //   2) set the new value to the largest possible number.  This number
          //      may not be in our sequence, but it is guaranteed to be equal
          //      to or larger than any other value already inserted.
          //
          //  For now I'm going to take option 2.
          //
          //  Returning ULLONG_MAX from get_auto_increment will cause the SQL
          //  layer to fail with ER_AUTOINC_READ_FAILED. This means that due to
          //  the SE API for get_auto_increment, inserts will fail with
          //  ER_AUTOINC_READ_FAILED if the column is UNSIGNED BIGINT, but
          //  inserts will fail with ER_DUP_ENTRY for other types (or no failure
          //  if the column is in a non-unique SK).
          new_val = std::numeric_limits<ulonglong>::max();
          auto_incr = new_val;  // Store the largest value into auto_incr
          break;
        }

        new_val = n * inc + off;

        // Attempt to store the new value (plus 1 since m_auto_incr_val contains
        // the next available value) into the atomic value.  If the current
        // value no longer matches what we have in 'last_val' this will fail and
        // we will repeat the loop (`last_val` will automatically get updated
        // with the current value).
        //
        // See above explanation for inc == 1 for why we use std::min.
      } while (!auto_incr.compare_exchange_weak(
          last_val, std::min(new_val + 1, max_val)));
    }
  }
  rocksdb_rpc_log(15790, "get_auto_increment: end");

  *first_value = new_val;
  *nb_reserved_values = 1;
}

#ifndef DBUG_OFF

/* Debugger help function */
static char dbug_item_print_buf[512];

const char *dbug_print_item(Item *const item) {
  char *const buf = dbug_item_print_buf;
  String str(buf, sizeof(dbug_item_print_buf), &my_charset_bin);
  str.length(0);
  if (!item) return "(Item*)nullptr";
  item->print(&str, QT_ORDINARY);
  if (str.c_ptr() == buf) {
    return buf;
  } else {
    return "Couldn't fit into buffer";
  }
}

#endif /*DBUG_OFF*/

/**
  SQL layer calls this function to push an index condition.

  @details
    The condition is for index keyno (only one condition can be pushed at a
    time).
    The caller guarantees that condition refers only to index fields; besides
    that, fields must have

      $field->part_of_key.set_bit(keyno)

    which means that

       (handler->index_flags(keyno, $keypart, 0) & HA_KEYREAD_ONLY) == 1

    which means that field value can be restored from the index tuple.

  @return
    Part of condition we couldn't check (always nullptr).
*/

class Item *ha_rocksdb::idx_cond_push(uint keyno, class Item *const idx_cond) {
  rocksdb_rpc_log(15835, "idx_cond_push: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(keyno != MAX_KEY);
  DBUG_ASSERT(idx_cond != nullptr);

  pushed_idx_cond = idx_cond;
  pushed_idx_cond_keyno = keyno;
  in_range_check_pushed_down = TRUE;

  /* We will check the whole condition */
  rocksdb_rpc_log(15849, "idx_cond_push: end");

  DBUG_RETURN(nullptr);
}

/*
  @brief
  Check the index condition.

  @detail
  Check the index condition. (The caller has unpacked all needed index
  columns into table->record[0])

  @return
    ICP_NO_MATCH - Condition not satisfied (caller should continue
                   scanning)
    OUT_OF_RANGE - We've left the range we're scanning (caller should
                   stop scanning and return HA_ERR_END_OF_FILE)

    ICP_MATCH    - Condition is satisfied (caller should fetch the record
                   and return it)
*/

enum icp_result ha_rocksdb::check_index_cond() const {
  rocksdb_rpc_log(15849, "idx_cond_push: start");

  DBUG_ASSERT(pushed_idx_cond);
  DBUG_ASSERT(pushed_idx_cond_keyno != MAX_KEY);

  if (end_range && compare_key_icp(end_range) > 0) {
    /* caller should return HA_ERR_END_OF_FILE already */
    rocksdb_rpc_log(15880, "idx_cond_push: end");

    return ICP_OUT_OF_RANGE;
  }

  rocksdb_rpc_log(15885, "idx_cond_push: end");

  return pushed_idx_cond->val_int() ? ICP_MATCH : ICP_NO_MATCH;
}

/*
  Checks if inplace alter is supported for a given operation.
*/

my_core::enum_alter_inplace_result ha_rocksdb::check_if_supported_inplace_alter(
    TABLE *altered_table, my_core::Alter_inplace_info *const ha_alter_info) {
  rocksdb_rpc_log(15894, "check_if_supported_inplace_alter: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(ha_alter_info != nullptr);

  if (ha_alter_info->handler_flags &
      ~(my_core::Alter_inplace_info::DROP_INDEX |
        my_core::Alter_inplace_info::DROP_UNIQUE_INDEX |
        my_core::Alter_inplace_info::ADD_INDEX |
        my_core::Alter_inplace_info::ADD_UNIQUE_INDEX |
        my_core::Alter_inplace_info::CHANGE_CREATE_OPTION |
        (rocksdb_alter_column_default_inplace
             ? my_core::Alter_inplace_info::ALTER_COLUMN_DEFAULT
             : 0))) {
    rocksdb_rpc_log(15911, "check_if_supported_inplace_alter: end");

    DBUG_RETURN(my_core::HA_ALTER_INPLACE_NOT_SUPPORTED);
  }

  /* We don't support unique keys on table w/ no primary keys */
  if ((ha_alter_info->handler_flags &
       my_core::Alter_inplace_info::ADD_UNIQUE_INDEX) &&
      has_hidden_pk(altered_table)) {
    rocksdb_rpc_log(15920, "check_if_supported_inplace_alter: end");

    DBUG_RETURN(my_core::HA_ALTER_INPLACE_NOT_SUPPORTED);
  }

  /* We only support changing auto_increment for table options. */
  if ((ha_alter_info->handler_flags &
       my_core::Alter_inplace_info::CHANGE_CREATE_OPTION) &&
      !(ha_alter_info->create_info->used_fields & HA_CREATE_USED_AUTO)) {
    rocksdb_rpc_log(15930, "check_if_supported_inplace_alter: end");

    DBUG_RETURN(my_core::HA_ALTER_INPLACE_NOT_SUPPORTED);
  }

  rocksdb_rpc_log(15934, "check_if_supported_inplace_alter: end");

  DBUG_RETURN(my_core::HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE);
}

/**
  Allows the storage engine to update internal structures with concurrent
  writes blocked. If check_if_supported_inplace_alter() returns
  HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE or
  HA_ALTER_INPLACE_SHARED_AFTER_PREPARE, this function is called with
  exclusive lock otherwise the same level of locking as for
  inplace_alter_table() will be used.

  @note Storage engines are responsible for reporting any errors by
  calling my_error()/print_error()

  @note If this function reports error, commit_inplace_alter_table()
  will be called with commit= false.

  @note For partitioning, failing to prepare one partition, means that
  commit_inplace_alter_table() will be called to roll back changes for
  all partitions. This means that commit_inplace_alter_table() might be
  called without prepare_inplace_alter_table() having been called first
  for a given partition.

  @param    altered_table     TABLE object for new version of table.
  @param    ha_alter_info     Structure describing changes to be done
                              by ALTER TABLE and holding data used
                              during in-place alter.

  @retval   true              Error
  @retval   false             Success
*/
bool ha_rocksdb::prepare_inplace_alter_table(
    TABLE *const altered_table,
    my_core::Alter_inplace_info *const ha_alter_info) {
  rocksdb_rpc_log(15968, "prepare_inplace_alter_table: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(altered_table != nullptr);
  DBUG_ASSERT(ha_alter_info != nullptr);

  Rdb_tbl_def *new_tdef = nullptr;
  std::shared_ptr<Rdb_key_def> *old_key_descr = nullptr;
  std::shared_ptr<Rdb_key_def> *new_key_descr = nullptr;
  uint old_n_keys = m_tbl_def->m_key_count;
  uint new_n_keys = altered_table->s->keys;
  std::unordered_set<std::shared_ptr<Rdb_key_def>> added_indexes;
  std::unordered_set<GL_INDEX_ID> dropped_index_ids;
  uint n_dropped_keys = 0;
  uint n_added_keys = 0;
  ulonglong max_auto_incr = 0;

  if (ha_alter_info->handler_flags &
      (my_core::Alter_inplace_info::DROP_INDEX |
       my_core::Alter_inplace_info::DROP_UNIQUE_INDEX |
       my_core::Alter_inplace_info::ADD_INDEX |
       my_core::Alter_inplace_info::ADD_UNIQUE_INDEX)) {
    if (has_hidden_pk(altered_table)) {
      new_n_keys += 1;
    }

    rocksdb_rpc_log(15995, "prepare_inplace_alter_table: old_table = table");

    const TABLE *const old_table = table;
    old_key_descr = m_tbl_def->m_key_descr_arr;
    new_key_descr = new std::shared_ptr<Rdb_key_def>[new_n_keys];

    new_tdef = new Rdb_tbl_def(m_tbl_def->full_tablename());
    new_tdef->m_key_descr_arr = new_key_descr;
    new_tdef->m_key_count = new_n_keys;
    new_tdef->m_auto_incr_val =
        m_tbl_def->m_auto_incr_val.load(std::memory_order_relaxed);
    new_tdef->m_hidden_pk_val =
        m_tbl_def->m_hidden_pk_val.load(std::memory_order_relaxed);

    rocksdb_rpc_log(16011, "prepare_inplace_alter_table: create_key_defs");

    if (create_key_defs(altered_table, new_tdef, table, m_tbl_def)) {
      /* Delete the new key descriptors */
      delete[] new_key_descr;

      /*
        Explicitly mark as nullptr so we don't accidentally remove entries
        from data dictionary on cleanup (or cause double delete[]).
        */
      new_tdef->m_key_descr_arr = nullptr;
      delete new_tdef;

      my_error(ER_KEY_CREATE_DURING_ALTER, MYF(0));
      rocksdb_rpc_log(16025, "prepare_inplace_alter_table: end");

      DBUG_RETURN(HA_EXIT_FAILURE);
    }

    uint i;
    uint j;

    /* Determine which(if any) key definition(s) need to be dropped */
    for (i = 0; i < ha_alter_info->index_drop_count; i++) {
      const KEY *const dropped_key = ha_alter_info->index_drop_buffer[i];
      for (j = 0; j < old_n_keys; j++) {
        const KEY *const old_key =
            &old_table->key_info[old_key_descr[j]->get_keyno()];

        if (!compare_keys(old_key, dropped_key)) {
          dropped_index_ids.insert(old_key_descr[j]->get_gl_index_id());
          break;
        }
      }
    }

    /* Determine which(if any) key definitions(s) need to be added */
    int identical_indexes_found = 0;
    for (i = 0; i < ha_alter_info->index_add_count; i++) {
      const KEY *const added_key =
          &ha_alter_info->key_info_buffer[ha_alter_info->index_add_buffer[i]];
      for (j = 0; j < new_n_keys; j++) {
        const KEY *const new_key =
            &altered_table->key_info[new_key_descr[j]->get_keyno()];
        if (!compare_keys(new_key, added_key)) {
          /*
            Check for cases where an 'identical' index is being dropped and
            re-added in a single ALTER statement.  Turn this into a no-op as the
            index has not changed.

            E.G. Unique index -> non-unique index requires no change

            Note that cases where the index name remains the same but the
            key-parts are changed is already handled in create_inplace_key_defs.
            In these cases the index needs to be rebuilt.
            */
          if (dropped_index_ids.count(new_key_descr[j]->get_gl_index_id())) {
            dropped_index_ids.erase(new_key_descr[j]->get_gl_index_id());
            identical_indexes_found++;
          } else {
            added_indexes.insert(new_key_descr[j]);
          }

          break;
        }
      }
    }

    n_dropped_keys = ha_alter_info->index_drop_count - identical_indexes_found;
    n_added_keys = ha_alter_info->index_add_count - identical_indexes_found;
    DBUG_ASSERT(dropped_index_ids.size() == n_dropped_keys);
    DBUG_ASSERT(added_indexes.size() == n_added_keys);
    DBUG_ASSERT(new_n_keys == (old_n_keys - n_dropped_keys + n_added_keys));
  }
  if (ha_alter_info->handler_flags &
      my_core::Alter_inplace_info::CHANGE_CREATE_OPTION) {
    if (!new_tdef) {
      new_tdef = m_tbl_def;
    }
    if (table->found_next_number_field) {
      max_auto_incr = load_auto_incr_value_from_index();
    }
  }

  ha_alter_info->handler_ctx = new Rdb_inplace_alter_ctx(
      new_tdef, old_key_descr, new_key_descr, old_n_keys, new_n_keys,
      added_indexes, dropped_index_ids, n_added_keys, n_dropped_keys,
      max_auto_incr);

  rocksdb_rpc_log(16099, "prepare_inplace_alter_table: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/**
  Alter the table structure in-place with operations specified using
  HA_ALTER_FLAGS and Alter_inplace_info. The level of concurrency allowed
  during this operation depends on the return value from
  check_if_supported_inplace_alter().

  @note Storage engines are responsible for reporting any errors by
  calling my_error()/print_error()

  @note If this function reports error, commit_inplace_alter_table()
  will be called with commit= false.

  @param    altered_table     TABLE object for new version of table.
  @param    ha_alter_info     Structure describing changes to be done
                              by ALTER TABLE and holding data used
                              during in-place alter.

  @retval   true              Error
  @retval   false             Success
*/
bool ha_rocksdb::inplace_alter_table(
    TABLE *const altered_table,
    my_core::Alter_inplace_info *const ha_alter_info) {
  rocksdb_rpc_log(16128, "inplace_alter_table: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(altered_table != nullptr);
  DBUG_ASSERT(ha_alter_info != nullptr);
  DBUG_ASSERT(ha_alter_info->handler_ctx != nullptr);

  Rdb_inplace_alter_ctx *const ctx =
      static_cast<Rdb_inplace_alter_ctx *>(ha_alter_info->handler_ctx);

  if (ha_alter_info->handler_flags &
      (my_core::Alter_inplace_info::ADD_INDEX |
       my_core::Alter_inplace_info::ADD_UNIQUE_INDEX)) {
    /*
      Buffers need to be set up again to account for new, possibly longer
      secondary keys.
    */
    free_key_buffers();

    DBUG_ASSERT(ctx != nullptr);

    /*
      If adding unique index, allocate special buffers for duplicate checking.
    */
    int err;

    rocksdb_rpc_log(16155, "inplace_alter_table: alloc_key_buffers");

    if ((err = alloc_key_buffers(
             altered_table, ctx->m_new_tdef,
             ha_alter_info->handler_flags &
                 my_core::Alter_inplace_info::ADD_UNIQUE_INDEX))) {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      DBUG_RETURN(err);
    }

    /* Populate all new secondary keys by scanning the primary key. */

    rocksdb_rpc_log(16167, "inplace_alter_table: inplace_populate_sk");

    if ((err = inplace_populate_sk(altered_table, ctx->m_added_indexes))) {
      my_error(ER_SK_POPULATE_DURING_ALTER, MYF(0));
      DBUG_RETURN(HA_EXIT_FAILURE);
    }
  }

  DBUG_EXECUTE_IF("myrocks_simulate_index_create_rollback", {
    dbug_create_err_inplace_alter();
    DBUG_RETURN(HA_EXIT_FAILURE);
  };);

  rocksdb_rpc_log(16180, "inplace_alter_table: end");

  DBUG_RETURN(HA_EXIT_SUCCESS);
}

/**
 Scan the Primary Key index entries and populate the new secondary keys.
*/
int ha_rocksdb::inplace_populate_sk(
    TABLE *const new_table_arg,
    const std::unordered_set<std::shared_ptr<Rdb_key_def>> &indexes) {
  rocksdb_rpc_log(16190, "inplace_populate_sk: start");

  DBUG_ENTER_FUNC();
  int res = HA_EXIT_SUCCESS;

  // ALTER
  // const std::unique_ptr<rocksdb::WriteBatch> wb = dict_manager.begin();
  // rocksdb::WriteBatch *const batch = wb.get();
  rocksdb_rpc_log(16199, "inplace_populate_sk: dict_manager.begin");

  rocksdb::WriteBatch *batch = dict_manager.begin();

  DBUG_EXECUTE_IF("rocksdb_inplace_populate_sk", {
    const char act[] =
        "now signal ready_to_mark_cf_dropped_in_populate_sk "
        "wait_for mark_cf_dropped_done_in_populate_sk";
    DBUG_ASSERT(!debug_sync_set_action(ha_thd(), STRING_WITH_LEN(act)));
  });

  {
    std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
    for (const auto &kd : indexes) {
      // ALTER
      // const std::string cf_name = kd->get_cf()->GetName();
      rocksdb_rpc_log(
          16215, "inplace_populate_sk: rocksdb_ColumnFamilyHandle__GetName");

      const std::string cf_name =
          rocksdb_ColumnFamilyHandle__GetName(kd->get_cf());

      // ALTER
      // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
      //     cf_manager.get_cf(cf_name);
      rocksdb_rpc_log(16225, "inplace_populate_sk: cf_manager.get_cf");

      rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(cf_name);

      if (!cfh || cfh != kd->get_shared_cf()) {
        // The CF has been dropped, i.e., cf_manager.remove_dropped_cf() has
        // been called.
        DBUG_RETURN(HA_EXIT_FAILURE);
      }

      // ALTER
      // uint32 cf_id = cfh->GetID();
      uint32 cf_id = rocksdb_ColumnFamilyHandle__GetID(cfh);
      if (dict_manager.get_dropped_cf(cf_id)) {
        DBUG_RETURN(HA_EXIT_FAILURE);
      }
    }

    /* Update the data dictionary */
    std::unordered_set<GL_INDEX_ID> create_index_ids;
    for (const auto &index : indexes) {
      create_index_ids.insert(index->get_gl_index_id());
    }
    dict_manager.add_create_index(create_index_ids, batch);
    res = dict_manager.commit(batch);
    if (res != HA_EXIT_SUCCESS) {
      rocksdb_rpc_log(16225, "inplace_populate_sk: end");
      return res;
    }

    /*
      Add uncommitted key definitons to ddl_manager.  We need to do this
      so that the property collector can find this keydef when it needs to
      update stats.  The property collector looks for the keydef in the
      data dictionary, but it won't be there yet since this key definition
      is still in the creation process.
    */
    rocksdb_rpc_log(16260, "inplace_populate_sk: add_uncommitted_keydefs");
    ddl_manager.add_uncommitted_keydefs(indexes);
  }

  const bool hidden_pk_exists = has_hidden_pk(table);

  rocksdb_rpc_log(16267, "inplace_populate_sk: get_or_create_tx");

  Rdb_transaction *tx = get_or_create_tx(table->in_use);

  /*
    There is one specific scenario where m_sst_info may not be nullptr. This
    happens if the handler we're using happens to be the handler where the PK
    bulk load was done on. The sequence of events that lead to this is as
    follows (T1 is PK bulk load, T2 is SK alter table):

    T1: Execute last INSERT statement
    T1: Return TABLE and handler object back to Table_cache_manager
    T1: Close connection
    T2: Execute ALTER statement
    T2: Take same TABLE/handler from Table_cache_manager
    T2: Call closefrm which will call finalize_bulk_load on every other open
        table/handler *except* the one it's on.
    T2: Acquire stale snapshot of PK
    T1: Call finalize_bulk_load

    This is rare because usually, closefrm will call the destructor (and thus
    finalize_bulk_load) on the handler where PK bulk load is done. However, if
    the thread ids of the bulk load thread and the alter thread differ by a
    multiple of table_cache_instances (8 by default), then they hash to the
    same bucket in Table_cache_manager and the alter thread will not not call
    the destructor on the handler it is holding. Thus, its m_sst_info will not
    be nullptr.

    At this point, it is safe to refresh the snapshot because we know all other
    open handlers have been closed at this point, and the one we're on is the
    only one left.
  */
  if (m_sst_info) {
    if ((res = finalize_bulk_load())) {
      DBUG_RETURN(res);
    }
    rocksdb_rpc_log(16302, "inplace_populate_sk: tx->commit");
    tx->commit();
  }

  const ulonglong rdb_merge_buf_size = THDVAR(ha_thd(), merge_buf_size);
  const ulonglong rdb_merge_combine_read_size =
      THDVAR(ha_thd(), merge_combine_read_size);
  const ulonglong rdb_merge_tmp_file_removal_delay =
      THDVAR(ha_thd(), merge_tmp_file_removal_delay_ms);

  for (const auto &index : indexes) {
    bool is_unique_index =
        new_table_arg->key_info[index->get_keyno()].flags & HA_NOSAME;

    Rdb_index_merge rdb_merge(tx->get_rocksdb_tmpdir(), rdb_merge_buf_size,
                              rdb_merge_combine_read_size,
                              rdb_merge_tmp_file_removal_delay,
                              index->get_cf());

    if ((res = rdb_merge.init())) {
      DBUG_RETURN(res);
    }

    /*
      Note: We pass in the currently existing table + tbl_def object here,
      as the pk index position may have changed in the case of hidden primary
      keys.
    */
    const uint pk = pk_index(table, m_tbl_def);
    res = ha_index_init(pk, true);
    if (res) DBUG_RETURN(res);

    /* Scan each record in the primary key in order */
    for (res = index_first(table->record[0]); res == 0;
         res = index_next(table->record[0])) {
      longlong hidden_pk_id = 0;
      if (hidden_pk_exists &&
          (res = read_hidden_pk_id_from_rowkey(&hidden_pk_id))) {
        // NO_LINT_DEBUG
        sql_print_error("Error retrieving hidden pk id.");
        ha_index_end();
        DBUG_RETURN(res);
      }

      /* Create new secondary index entry */
      const int new_packed_size = index->pack_record(
          new_table_arg, m_pack_buffer, table->record[0], m_sk_packed_tuple,
          &m_sk_tails, should_store_row_debug_checksums(), hidden_pk_id, 0,
          nullptr, m_ttl_bytes);

      rocksdb_rpc_log(16352, "inplace_populate_sk: index->pack_record");

      const rocksdb::Slice key = rocksdb::Slice(
          reinterpret_cast<const char *>(m_sk_packed_tuple), new_packed_size);
      const rocksdb::Slice val =
          rocksdb::Slice(reinterpret_cast<const char *>(m_sk_tails.ptr()),
                         m_sk_tails.get_current_pos());

      /*
        Add record to offset tree in preparation for writing out to
        disk in sorted chunks.
      */
      if ((res = rdb_merge.add(key, val))) {
        ha_index_end();
        DBUG_RETURN(res);
      }
    }

    if (res != HA_ERR_END_OF_FILE) {
      // NO_LINT_DEBUG
      sql_print_error("Error retrieving index entry from primary key.");
      ha_index_end();

      rocksdb_rpc_log(16376, "inplace_populate_sk: end");

      DBUG_RETURN(res);
    }

    ha_index_end();

    /*
      Perform an n-way merge of n sorted buffers on disk, then writes all
      results to RocksDB via SSTFileWriter API.
    */
    rocksdb::Slice merge_key;
    rocksdb::Slice merge_val;

    struct unique_sk_buf_info sk_info;
    sk_info.dup_sk_buf = m_dup_sk_packed_tuple;
    sk_info.dup_sk_buf_old = m_dup_sk_packed_tuple_old;

    while ((res = rdb_merge.next(&merge_key, &merge_val)) == 0) {
      /* Perform uniqueness check if needed */
      if (is_unique_index) {
        if (check_duplicate_sk(new_table_arg, *index, &merge_key, &sk_info)) {
          /*
            Duplicate entry found when trying to create unique secondary key.
            We need to unpack the record into new_table_arg->record[0] as it
            is used inside print_keydup_error so that the error message shows
            the duplicate record.
          */
          if (index->unpack_record(
                  new_table_arg, new_table_arg->record[0], &merge_key,
                  &merge_val, m_converter->get_verify_row_debug_checksums())) {
            /* Should never reach here */
            DBUG_ASSERT(0);
          }

          print_keydup_error(new_table_arg,
                             &new_table_arg->key_info[index->get_keyno()],
                             MYF(0), ha_thd());
          rocksdb_rpc_log(16414, "inplace_populate_sk: end");

          DBUG_RETURN(ER_DUP_ENTRY);
        }
      }

      /*
        Insert key and slice to SST via SSTFileWriter API.
      */
      rocksdb_rpc_log(16423, "inplace_populate_sk: bulk_load_key");

      if ((res = bulk_load_key(tx, *index, merge_key, merge_val, false))) {
        break;
      }
    }

    /*
      Here, res == -1 means that we are finished, while > 0 means an error
      occurred.
    */
    if (res > 0) {
      // NO_LINT_DEBUG
      sql_print_error("Error while bulk loading keys in external merge sort.");
      rocksdb_rpc_log(16435, "inplace_populate_sk: end");

      DBUG_RETURN(res);
    }

    bool is_critical_error;
    res = tx->finish_bulk_load(&is_critical_error);
    rocksdb_rpc_log(16442, "inplace_populate_sk: finish_bulk_load");

    if (res && is_critical_error) {
      // NO_LINT_DEBUG
      sql_print_error("Error finishing bulk load.");

      rocksdb_rpc_log(16450, "inplace_populate_sk: end");

      DBUG_RETURN(res);
    }
  }

  /*
    Explicitly tell jemalloc to clean up any unused dirty pages at this point.
    See https://reviews.facebook.net/D63723 for more details.
  */
  purge_all_jemalloc_arenas();

  DBUG_EXECUTE_IF("crash_during_online_index_creation", DBUG_SUICIDE(););
  rocksdb_rpc_log(16463, "inplace_populate_sk: end");

  DBUG_RETURN(res);
}

/**
  Commit or rollback the changes made during prepare_inplace_alter_table()
  and inplace_alter_table() inside the storage engine.
  Note that in case of rollback the allowed level of concurrency during
  this operation will be the same as for inplace_alter_table() and thus
  might be higher than during prepare_inplace_alter_table(). (For example,
  concurrent writes were blocked during prepare, but might not be during
  rollback).

  @note Storage engines are responsible for reporting any errors by
  calling my_error()/print_error()

  @note If this function with commit= true reports error, it will be called
  again with commit= false.

  @note In case of partitioning, this function might be called for rollback
  without prepare_inplace_alter_table() having been called first.
  Also partitioned tables sets ha_alter_info->group_commit_ctx to a NULL
  terminated array of the partitions handlers and if all of them are
  committed as one, then group_commit_ctx should be set to NULL to indicate
  to the partitioning handler that all partitions handlers are committed.
  @see prepare_inplace_alter_table().

  @param    altered_table     TABLE object for new version of table.
  @param    ha_alter_info     Structure describing changes to be done
                              by ALTER TABLE and holding data used
                              during in-place alter.
  @param    commit            True => Commit, False => Rollback.

  @retval   true              Error
  @retval   false             Success
*/
bool ha_rocksdb::commit_inplace_alter_table(
    my_core::TABLE *const altered_table,
    my_core::Alter_inplace_info *const ha_alter_info, bool commit) {
  rocksdb_rpc_log(16503, "commit_inplace_alter_table: start");

  DBUG_ENTER_FUNC();

  DBUG_ASSERT(altered_table != nullptr);
  DBUG_ASSERT(ha_alter_info != nullptr);

  Rdb_inplace_alter_ctx *const ctx0 =
      static_cast<Rdb_inplace_alter_ctx *>(ha_alter_info->handler_ctx);

  DEBUG_SYNC(ha_thd(), "rocksdb.commit_in_place_alter_table");

  /*
    IMPORTANT: When rollback is requested, mysql will abort with
    an assertion failure. That means every failed commit during inplace alter
    table will result in a fatal error on the server. Indexes ongoing creation
    will be detected when the server restarts, and dropped.

    For partitioned tables, a rollback call to this function (commit == false)
    is done for each partition.  A successful commit call only executes once
    for all partitions.
  */
  if (!commit) {
    /* If ctx has not been created yet, nothing to do here */
    if (!ctx0) {
      rocksdb_rpc_log(16528, "commit_inplace_alter_table: end");

      DBUG_RETURN(HA_EXIT_SUCCESS);
    }

    /*
      Cannot call destructor for Rdb_tbl_def directly because we don't want to
      erase the mappings inside the ddl_manager, as the old_key_descr is still
      using them.
    */
    if (ctx0->m_new_key_descr) {
      /* Delete the new key descriptors */
      for (uint i = 0; i < ctx0->m_new_tdef->m_key_count; i++) {
        ctx0->m_new_key_descr[i] = nullptr;
      }

      delete[] ctx0->m_new_key_descr;
      ctx0->m_new_key_descr = nullptr;
      ctx0->m_new_tdef->m_key_descr_arr = nullptr;

      delete ctx0->m_new_tdef;
    }

    {
      std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
      /* Remove uncommitted key definitons from ddl_manager */
      ddl_manager.remove_uncommitted_keydefs(ctx0->m_added_indexes);

      std::unordered_set<GL_INDEX_ID> all_gl_index_ids;
      dict_manager.get_ongoing_create_indexes(&all_gl_index_ids);

      std::unordered_set<GL_INDEX_ID> gl_index_ids;
      for (auto index : ctx0->m_added_indexes) {
        auto gl_index_id = index->get_gl_index_id();
        if (all_gl_index_ids.find(gl_index_id) != all_gl_index_ids.end()) {
          gl_index_ids.insert(gl_index_id);
        }
      }

      if (!gl_index_ids.empty()) {
        /* Rollback any partially created indexes of this table */
        rocksdb_rpc_log(
            16569,
            "commit_inplace_alter_table: rollback_ongoing_index_creation");

        dict_manager.rollback_ongoing_index_creation(gl_index_ids);
      }
    }

    rocksdb_rpc_log(16576, "commit_inplace_alter_table: end");

    DBUG_RETURN(HA_EXIT_SUCCESS);
  }

  DBUG_ASSERT(ctx0);

  /*
    For partitioned tables, we need to commit all changes to all tables at
    once, unlike in the other inplace alter API methods.
  */
  inplace_alter_handler_ctx **ctx_array;
  inplace_alter_handler_ctx *ctx_single[2];

  if (ha_alter_info->group_commit_ctx) {
    DBUG_EXECUTE_IF("crash_during_index_creation_partition", DBUG_SUICIDE(););
    ctx_array = ha_alter_info->group_commit_ctx;
  } else {
    ctx_single[0] = ctx0;
    ctx_single[1] = nullptr;
    ctx_array = ctx_single;
  }

  DBUG_ASSERT(ctx0 == ctx_array[0]);
  ha_alter_info->group_commit_ctx = nullptr;

  if (ha_alter_info->handler_flags &
      (my_core::Alter_inplace_info::DROP_INDEX |
       my_core::Alter_inplace_info::DROP_UNIQUE_INDEX |
       my_core::Alter_inplace_info::ADD_INDEX |
       my_core::Alter_inplace_info::ADD_UNIQUE_INDEX)) {
    rocksdb_rpc_log(16613, "commit_inplace_alter_table: dict_manager.begin()");

    // ALTER
    // const std::unique_ptr<rocksdb::WriteBatch> wb = dict_manager.begin();
    // rocksdb::WriteBatch *const batch = wb.get();
    rocksdb::WriteBatch *batch = dict_manager.begin();
    std::unordered_set<GL_INDEX_ID> create_index_ids;

    m_tbl_def = ctx0->m_new_tdef;
    m_key_descr_arr = m_tbl_def->m_key_descr_arr;
    m_pk_descr = m_key_descr_arr[pk_index(altered_table, m_tbl_def)];

    DBUG_EXECUTE_IF("rocksdb_commit_alter_table", {
      const char act[] =
          "now signal ready_to_mark_cf_dropped_before_commit_alter_table "
          "wait_for mark_cf_dropped_done_before_commit_alter_table";
      DBUG_ASSERT(!debug_sync_set_action(ha_thd(), STRING_WITH_LEN(act)));
    });

    {
      std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
      for (inplace_alter_handler_ctx **pctx = ctx_array; *pctx; pctx++) {
        Rdb_inplace_alter_ctx *const ctx =
            static_cast<Rdb_inplace_alter_ctx *>(*pctx);

        rocksdb_rpc_log(16631, "commit_inplace_alter_table: add_drop_index");
        /* Mark indexes to be dropped */
        dict_manager.add_drop_index(ctx->m_dropped_index_ids, batch);

        for (const auto &index : ctx->m_added_indexes) {
          create_index_ids.insert(index->get_gl_index_id());
        }

        rocksdb_rpc_log(16639, "commit_inplace_alter_table: put_and_write");
        if (ddl_manager.put_and_write(ctx->m_new_tdef, batch)) {
          /*
            Failed to write new entry into data dictionary, this should never
            happen.
          */
          DBUG_ASSERT(0);
        }

        /*
          Remove uncommitted key definitons from ddl_manager, as they are now
          committed into the data dictionary.
        */
        rocksdb_rpc_log(
            16652, "commit_inplace_alter_table: remove_uncommitted_keydefs");

        ddl_manager.remove_uncommitted_keydefs(ctx->m_added_indexes);
      }

      rocksdb_rpc_log(16658, "commit_inplace_alter_table: commit");

      if (dict_manager.commit(batch)) {
        /*
          Should never reach here. We assume MyRocks will abort if commit
          fails.
        */
        DBUG_ASSERT(0);
      }

      rocksdb_rpc_log(16668,
                      "commit_inplace_alter_table: finish_indexes_operation");
      /* Mark ongoing create indexes as finished/remove from data dictionary */
      dict_manager.finish_indexes_operation(
          create_index_ids, Rdb_key_def::DDL_CREATE_INDEX_ONGOING);
    }

    DBUG_EXECUTE_IF("rocksdb_delete_index", {
      const char act[] =
          "now signal ready_to_mark_cf_dropped_after_commit_alter_table "
          "wait_for mark_cf_dropped_done_after_commit_alter_table";
      DBUG_ASSERT(!debug_sync_set_action(ha_thd(), STRING_WITH_LEN(act)));
    });

    rdb_drop_idx_thread.signal();

    if (rocksdb_table_stats_use_table_scan && !ctx0->m_added_indexes.empty()) {
      // If new indexes are created, add the table to the recalc queue
      // to calculate stats for new indexes
      rdb_is_thread.add_index_stats_request(m_tbl_def->full_tablename());
    }
  }

  rocksdb_rpc_log(16691, "commit_inplace_alter_table: handler_flags");

  if (ha_alter_info->handler_flags &
      (my_core::Alter_inplace_info::CHANGE_CREATE_OPTION)) {
    // ALTER
    // const std::unique_ptr<rocksdb::WriteBatch> wb = dict_manager.begin();
    // rocksdb::WriteBatch *const batch = wb.get();
    rocksdb::WriteBatch *batch = dict_manager.begin();
    std::unordered_set<GL_INDEX_ID> create_index_ids;

    ulonglong auto_incr_val = ha_alter_info->create_info->auto_increment_value;

    for (inplace_alter_handler_ctx **pctx = ctx_array; *pctx; pctx++) {
      Rdb_inplace_alter_ctx *const ctx =
          static_cast<Rdb_inplace_alter_ctx *>(*pctx);
      auto_incr_val = std::max(auto_incr_val, ctx->m_max_auto_incr);

      rocksdb_rpc_log(16712, "commit_inplace_alter_table: put_auto_incr_val");

      dict_manager.put_auto_incr_val(
          batch, ctx->m_new_tdef->get_autoincr_gl_index_id(), auto_incr_val,
          true /* overwrite */);
      ctx->m_new_tdef->m_auto_incr_val = auto_incr_val;
    }

    if (dict_manager.commit(batch)) {
      DBUG_ASSERT(0);
    }
  }
  rocksdb_rpc_log(16724, "commit_inplace_alter_table: end");
  DBUG_RETURN(HA_EXIT_SUCCESS);
}

#define SHOW_FNAME(name) rocksdb_show_##name

#define DEF_SHOW_FUNC(name, key)                                           \
  static int SHOW_FNAME(name)(MYSQL_THD thd, SHOW_VAR * var, char *buff) { \
    rocksdb_status_counters.name =                                         \
        rocksdb_stats->getTickerCount(rocksdb::key);                       \
    var->type = SHOW_LONGLONG;                                             \
    var->value = reinterpret_cast<char *>(&rocksdb_status_counters.name);  \
    return HA_EXIT_SUCCESS;                                                \
  }

#define DEF_STATUS_VAR(name) \
  { "rocksdb_" #name, (char *)&SHOW_FNAME(name), SHOW_FUNC }

#define DEF_STATUS_VAR_PTR(name, ptr, option) \
  { "rocksdb_" name, (char *)ptr, option }

#define DEF_STATUS_VAR_FUNC(name, ptr, option) \
  { name, reinterpret_cast<char *>(ptr), option }

struct rocksdb_status_counters_t {
  uint64_t block_cache_miss;
  uint64_t block_cache_hit;
  uint64_t block_cache_add;
  uint64_t block_cache_add_failures;
  uint64_t block_cache_index_miss;
  uint64_t block_cache_index_hit;
  uint64_t block_cache_index_add;
  uint64_t block_cache_index_bytes_insert;
  uint64_t block_cache_index_bytes_evict;
  uint64_t block_cache_filter_miss;
  uint64_t block_cache_filter_hit;
  uint64_t block_cache_filter_add;
  uint64_t block_cache_filter_bytes_insert;
  uint64_t block_cache_filter_bytes_evict;
  uint64_t block_cache_bytes_read;
  uint64_t block_cache_bytes_write;
  uint64_t block_cache_data_bytes_insert;
  uint64_t block_cache_data_miss;
  uint64_t block_cache_data_hit;
  uint64_t block_cache_data_add;
  uint64_t bloom_filter_useful;
  uint64_t bloom_filter_full_positive;
  uint64_t bloom_filter_full_true_positive;
  uint64_t memtable_hit;
  uint64_t memtable_miss;
  uint64_t get_hit_l0;
  uint64_t get_hit_l1;
  uint64_t get_hit_l2_and_up;
  uint64_t compaction_key_drop_new;
  uint64_t compaction_key_drop_obsolete;
  uint64_t compaction_key_drop_user;
  uint64_t number_keys_written;
  uint64_t number_keys_read;
  uint64_t number_keys_updated;
  uint64_t bytes_written;
  uint64_t bytes_read;
  uint64_t number_db_seek;
  uint64_t number_db_seek_found;
  uint64_t number_db_next;
  uint64_t number_db_next_found;
  uint64_t number_db_prev;
  uint64_t number_db_prev_found;
  uint64_t iter_bytes_read;
  uint64_t no_file_closes;
  uint64_t no_file_opens;
  uint64_t no_file_errors;
  uint64_t stall_micros;
  uint64_t num_iterators;
  uint64_t number_multiget_get;
  uint64_t number_multiget_keys_read;
  uint64_t number_multiget_bytes_read;
  uint64_t number_deletes_filtered;
  uint64_t number_merge_failures;
  uint64_t bloom_filter_prefix_checked;
  uint64_t bloom_filter_prefix_useful;
  uint64_t number_reseeks_iteration;
  uint64_t getupdatessince_calls;
  uint64_t block_cachecompressed_miss;
  uint64_t block_cachecompressed_hit;
  uint64_t wal_synced;
  uint64_t wal_bytes;
  uint64_t write_self;
  uint64_t write_other;
  uint64_t write_timedout;
  uint64_t write_wal;
  uint64_t flush_write_bytes;
  uint64_t compact_read_bytes;
  uint64_t compact_write_bytes;
  uint64_t number_superversion_acquires;
  uint64_t number_superversion_releases;
  uint64_t number_superversion_cleanups;
  uint64_t number_block_not_compressed;
};

static rocksdb_status_counters_t rocksdb_status_counters;

DEF_SHOW_FUNC(block_cache_miss, BLOCK_CACHE_MISS)
DEF_SHOW_FUNC(block_cache_hit, BLOCK_CACHE_HIT)
DEF_SHOW_FUNC(block_cache_add, BLOCK_CACHE_ADD)
DEF_SHOW_FUNC(block_cache_add_failures, BLOCK_CACHE_ADD_FAILURES)
DEF_SHOW_FUNC(block_cache_index_miss, BLOCK_CACHE_INDEX_MISS)
DEF_SHOW_FUNC(block_cache_index_hit, BLOCK_CACHE_INDEX_HIT)
DEF_SHOW_FUNC(block_cache_index_add, BLOCK_CACHE_INDEX_ADD)
DEF_SHOW_FUNC(block_cache_index_bytes_insert, BLOCK_CACHE_INDEX_BYTES_INSERT)
DEF_SHOW_FUNC(block_cache_index_bytes_evict, BLOCK_CACHE_INDEX_BYTES_EVICT)
DEF_SHOW_FUNC(block_cache_filter_miss, BLOCK_CACHE_FILTER_MISS)
DEF_SHOW_FUNC(block_cache_filter_hit, BLOCK_CACHE_FILTER_HIT)
DEF_SHOW_FUNC(block_cache_filter_add, BLOCK_CACHE_FILTER_ADD)
DEF_SHOW_FUNC(block_cache_filter_bytes_insert, BLOCK_CACHE_FILTER_BYTES_INSERT)
DEF_SHOW_FUNC(block_cache_filter_bytes_evict, BLOCK_CACHE_FILTER_BYTES_EVICT)
DEF_SHOW_FUNC(block_cache_bytes_read, BLOCK_CACHE_BYTES_READ)
DEF_SHOW_FUNC(block_cache_bytes_write, BLOCK_CACHE_BYTES_WRITE)
DEF_SHOW_FUNC(block_cache_data_bytes_insert, BLOCK_CACHE_DATA_BYTES_INSERT)
DEF_SHOW_FUNC(block_cache_data_miss, BLOCK_CACHE_DATA_MISS)
DEF_SHOW_FUNC(block_cache_data_hit, BLOCK_CACHE_DATA_HIT)
DEF_SHOW_FUNC(block_cache_data_add, BLOCK_CACHE_DATA_ADD)
DEF_SHOW_FUNC(bloom_filter_useful, BLOOM_FILTER_USEFUL)
DEF_SHOW_FUNC(bloom_filter_full_positive, BLOOM_FILTER_FULL_POSITIVE)
DEF_SHOW_FUNC(bloom_filter_full_true_positive, BLOOM_FILTER_FULL_TRUE_POSITIVE)
DEF_SHOW_FUNC(memtable_hit, MEMTABLE_HIT)
DEF_SHOW_FUNC(memtable_miss, MEMTABLE_MISS)
DEF_SHOW_FUNC(get_hit_l0, GET_HIT_L0)
DEF_SHOW_FUNC(get_hit_l1, GET_HIT_L1)
DEF_SHOW_FUNC(get_hit_l2_and_up, GET_HIT_L2_AND_UP)
DEF_SHOW_FUNC(compaction_key_drop_new, COMPACTION_KEY_DROP_NEWER_ENTRY)
DEF_SHOW_FUNC(compaction_key_drop_obsolete, COMPACTION_KEY_DROP_OBSOLETE)
DEF_SHOW_FUNC(compaction_key_drop_user, COMPACTION_KEY_DROP_USER)
DEF_SHOW_FUNC(number_keys_written, NUMBER_KEYS_WRITTEN)
DEF_SHOW_FUNC(number_keys_read, NUMBER_KEYS_READ)
DEF_SHOW_FUNC(number_keys_updated, NUMBER_KEYS_UPDATED)
DEF_SHOW_FUNC(bytes_written, BYTES_WRITTEN)
DEF_SHOW_FUNC(bytes_read, BYTES_READ)
DEF_SHOW_FUNC(number_db_seek, NUMBER_DB_SEEK)
DEF_SHOW_FUNC(number_db_seek_found, NUMBER_DB_SEEK_FOUND)
DEF_SHOW_FUNC(number_db_next, NUMBER_DB_NEXT)
DEF_SHOW_FUNC(number_db_next_found, NUMBER_DB_NEXT_FOUND)
DEF_SHOW_FUNC(number_db_prev, NUMBER_DB_PREV)
DEF_SHOW_FUNC(number_db_prev_found, NUMBER_DB_PREV_FOUND)
DEF_SHOW_FUNC(iter_bytes_read, ITER_BYTES_READ)
DEF_SHOW_FUNC(no_file_closes, NO_FILE_CLOSES)
DEF_SHOW_FUNC(no_file_opens, NO_FILE_OPENS)
DEF_SHOW_FUNC(no_file_errors, NO_FILE_ERRORS)
DEF_SHOW_FUNC(stall_micros, STALL_MICROS)
DEF_SHOW_FUNC(num_iterators, NO_ITERATORS)
DEF_SHOW_FUNC(number_multiget_get, NUMBER_MULTIGET_CALLS)
DEF_SHOW_FUNC(number_multiget_keys_read, NUMBER_MULTIGET_KEYS_READ)
DEF_SHOW_FUNC(number_multiget_bytes_read, NUMBER_MULTIGET_BYTES_READ)
DEF_SHOW_FUNC(number_deletes_filtered, NUMBER_FILTERED_DELETES)
DEF_SHOW_FUNC(number_merge_failures, NUMBER_MERGE_FAILURES)
DEF_SHOW_FUNC(bloom_filter_prefix_checked, BLOOM_FILTER_PREFIX_CHECKED)
DEF_SHOW_FUNC(bloom_filter_prefix_useful, BLOOM_FILTER_PREFIX_USEFUL)
DEF_SHOW_FUNC(number_reseeks_iteration, NUMBER_OF_RESEEKS_IN_ITERATION)
DEF_SHOW_FUNC(getupdatessince_calls, GET_UPDATES_SINCE_CALLS)
DEF_SHOW_FUNC(block_cachecompressed_miss, BLOCK_CACHE_COMPRESSED_MISS)
DEF_SHOW_FUNC(block_cachecompressed_hit, BLOCK_CACHE_COMPRESSED_HIT)
DEF_SHOW_FUNC(wal_synced, WAL_FILE_SYNCED)
DEF_SHOW_FUNC(wal_bytes, WAL_FILE_BYTES)
DEF_SHOW_FUNC(write_self, WRITE_DONE_BY_SELF)
DEF_SHOW_FUNC(write_other, WRITE_DONE_BY_OTHER)
DEF_SHOW_FUNC(write_timedout, WRITE_TIMEDOUT)
DEF_SHOW_FUNC(write_wal, WRITE_WITH_WAL)
DEF_SHOW_FUNC(flush_write_bytes, FLUSH_WRITE_BYTES)
DEF_SHOW_FUNC(compact_read_bytes, COMPACT_READ_BYTES)
DEF_SHOW_FUNC(compact_write_bytes, COMPACT_WRITE_BYTES)
DEF_SHOW_FUNC(number_superversion_acquires, NUMBER_SUPERVERSION_ACQUIRES)
DEF_SHOW_FUNC(number_superversion_releases, NUMBER_SUPERVERSION_RELEASES)
DEF_SHOW_FUNC(number_superversion_cleanups, NUMBER_SUPERVERSION_CLEANUPS)
DEF_SHOW_FUNC(number_block_not_compressed, NUMBER_BLOCK_NOT_COMPRESSED)

static void myrocks_update_status() {
  export_stats.rows_deleted = global_stats.rows[ROWS_DELETED];
  export_stats.rows_inserted = global_stats.rows[ROWS_INSERTED];
  export_stats.rows_read = global_stats.rows[ROWS_READ];
  export_stats.rows_updated = global_stats.rows[ROWS_UPDATED];
  export_stats.rows_deleted_blind = global_stats.rows[ROWS_DELETED_BLIND];
  export_stats.rows_expired = global_stats.rows[ROWS_EXPIRED];
  export_stats.rows_filtered = global_stats.rows[ROWS_FILTERED];

  export_stats.system_rows_deleted = global_stats.system_rows[ROWS_DELETED];
  export_stats.system_rows_inserted = global_stats.system_rows[ROWS_INSERTED];
  export_stats.system_rows_read = global_stats.system_rows[ROWS_READ];
  export_stats.system_rows_updated = global_stats.system_rows[ROWS_UPDATED];

  export_stats.queries_point = global_stats.queries[QUERIES_POINT];
  export_stats.queries_range = global_stats.queries[QUERIES_RANGE];

  export_stats.table_index_stats_success =
      global_stats.table_index_stats_result[TABLE_INDEX_STATS_SUCCESS];
  export_stats.table_index_stats_failure =
      global_stats.table_index_stats_result[TABLE_INDEX_STATS_FAILURE];
  export_stats.table_index_stats_req_queue_length =
      rdb_is_thread.get_request_queue_size();

  export_stats.covered_secondary_key_lookups =
      global_stats.covered_secondary_key_lookups;
}

static void myrocks_update_memory_status() {
  rocksdb_rpc_log(16923, "myrocks_update_memory_status: start");

  std::vector<rocksdb::DB *> dbs;
  std::unordered_set<const rocksdb::Cache *> cache_set;
  dbs.push_back(rdb);
  std::map<rocksdb::MemoryUtil::UsageType, uint64_t> temp_usage_by_type;

  // ALTER
  // rocksdb::MemoryUtil::GetApproximateMemoryUsageByType(dbs, cache_set,
  //                                                      &temp_usage_by_type);
  rocksdb_MemoryUtil_GetApproximateMemoryUsageByType(dbs, cache_set,
                                                     temp_usage_by_type);
  memory_stats.memtable_total =
      temp_usage_by_type[rocksdb::MemoryUtil::kMemTableTotal];
  memory_stats.memtable_unflushed =
      temp_usage_by_type[rocksdb::MemoryUtil::kMemTableUnFlushed];
  rocksdb_rpc_log(16939, "myrocks_update_memory_status: end");
}

static SHOW_VAR myrocks_status_variables[] = {
    DEF_STATUS_VAR_FUNC("rows_deleted", &export_stats.rows_deleted,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("rows_inserted", &export_stats.rows_inserted,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("rows_read", &export_stats.rows_read, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("rows_updated", &export_stats.rows_updated,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("rows_deleted_blind", &export_stats.rows_deleted_blind,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("rows_expired", &export_stats.rows_expired,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("rows_filtered", &export_stats.rows_filtered,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("system_rows_deleted",
                        &export_stats.system_rows_deleted, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("system_rows_inserted",
                        &export_stats.system_rows_inserted, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("system_rows_read", &export_stats.system_rows_read,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("system_rows_updated",
                        &export_stats.system_rows_updated, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("memtable_total", &memory_stats.memtable_total,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("memtable_unflushed", &memory_stats.memtable_unflushed,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("queries_point", &export_stats.queries_point,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("queries_range", &export_stats.queries_range,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("table_index_stats_success",
                        &export_stats.table_index_stats_success, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("table_index_stats_failure",
                        &export_stats.table_index_stats_failure, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("table_index_stats_req_queue_length",
                        &export_stats.table_index_stats_req_queue_length,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("covered_secondary_key_lookups",
                        &export_stats.covered_secondary_key_lookups,
                        SHOW_LONGLONG),

    {NullS, NullS, SHOW_LONG}};

static void show_myrocks_vars(THD *thd, SHOW_VAR *var, char *buff) {
  rocksdb_rpc_log(16989, "show_myrocks_vars: start");

  myrocks_update_status();
  myrocks_update_memory_status();
  var->type = SHOW_ARRAY;
  var->value = reinterpret_cast<char *>(&myrocks_status_variables);
  rocksdb_rpc_log(16994, "show_myrocks_vars: end");
}

static ulonglong io_stall_prop_value(
    const std::map<std::string, std::string> &props, const std::string &key) {
  rocksdb_rpc_log(17000, "io_stall_prop_value: start");

  std::map<std::string, std::string>::const_iterator iter =
      props.find("io_stalls." + key);
  if (iter != props.end()) {
    rocksdb_rpc_log(17004, "io_stall_prop_value: end");

    return std::stoull(iter->second);
  } else {
    DBUG_PRINT("warning",
               ("RocksDB GetMapPropery hasn't returned key=%s", key.c_str()));
    DBUG_ASSERT(0);
    rocksdb_rpc_log(17011, "io_stall_prop_value: end");

    return 0;
  }
}

static void update_rocksdb_stall_status() {
  rocksdb_rpc_log(17016, "update_rocksdb_stall_status: start");

  st_io_stall_stats local_io_stall_stats;
  for (const auto &cf_name : cf_manager.get_cf_names()) {
    rocksdb_rpc_log(17025, "update_rocksdb_stall_status: cf_manager.get_cf");

    // ALTER
    // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
    //     cf_manager.get_cf(cf_name);
    rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(cf_name);
    if (!cfh) {
      continue;
    }

    // Retrieve information from valid CF handle object. It is safe
    // even if the CF is removed from cf_manager at this point.
    std::map<std::string, std::string> props;

    rocksdb_rpc_log(
        17040,
        "update_rocksdb_stall_status: rocksdb_TransactionDB__GetMapProperty");

    // ALTER
    // if (!rdb->GetMapProperty(cfh.get(), "rocksdb.cfstats", &props)) {
    //   continue;
    // }
    if (!rocksdb_TransactionDB__GetMapProperty(rdb, cfh, "rocksdb.cfstats",
                                               &props)) {
      continue;
    }

    local_io_stall_stats.level0_slowdown +=
        io_stall_prop_value(props, "level0_slowdown");
    local_io_stall_stats.level0_slowdown_with_compaction +=
        io_stall_prop_value(props, "level0_slowdown_with_compaction");
    local_io_stall_stats.level0_numfiles +=
        io_stall_prop_value(props, "level0_numfiles");
    local_io_stall_stats.level0_numfiles_with_compaction +=
        io_stall_prop_value(props, "level0_numfiles_with_compaction");
    local_io_stall_stats.stop_for_pending_compaction_bytes +=
        io_stall_prop_value(props, "stop_for_pending_compaction_bytes");
    local_io_stall_stats.slowdown_for_pending_compaction_bytes +=
        io_stall_prop_value(props, "slowdown_for_pending_compaction_bytes");
    local_io_stall_stats.memtable_compaction +=
        io_stall_prop_value(props, "memtable_compaction");
    local_io_stall_stats.memtable_slowdown +=
        io_stall_prop_value(props, "memtable_slowdown");
    local_io_stall_stats.total_stop += io_stall_prop_value(props, "total_stop");
    local_io_stall_stats.total_slowdown +=
        io_stall_prop_value(props, "total_slowdown");
  }
  io_stall_stats = local_io_stall_stats;
  rocksdb_rpc_log(17070, "update_rocksdb_stall_status: end");
}

static SHOW_VAR rocksdb_stall_status_variables[] = {
    DEF_STATUS_VAR_FUNC("l0_file_count_limit_slowdowns",
                        &io_stall_stats.level0_slowdown, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("locked_l0_file_count_limit_slowdowns",
                        &io_stall_stats.level0_slowdown_with_compaction,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("l0_file_count_limit_stops",
                        &io_stall_stats.level0_numfiles, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("locked_l0_file_count_limit_stops",
                        &io_stall_stats.level0_numfiles_with_compaction,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("pending_compaction_limit_stops",
                        &io_stall_stats.stop_for_pending_compaction_bytes,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("pending_compaction_limit_slowdowns",
                        &io_stall_stats.slowdown_for_pending_compaction_bytes,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("memtable_limit_stops",
                        &io_stall_stats.memtable_compaction, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("memtable_limit_slowdowns",
                        &io_stall_stats.memtable_slowdown, SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("total_stops", &io_stall_stats.total_stop,
                        SHOW_LONGLONG),
    DEF_STATUS_VAR_FUNC("total_slowdowns", &io_stall_stats.total_slowdown,
                        SHOW_LONGLONG),
    // end of the array marker
    {NullS, NullS, SHOW_LONG}};

static void show_rocksdb_stall_vars(THD *thd, SHOW_VAR *var, char *buff) {
  rocksdb_rpc_log(17100, "show_rocksdb_stall_vars: start");

  update_rocksdb_stall_status();
  var->type = SHOW_ARRAY;
  var->value = reinterpret_cast<char *>(&rocksdb_stall_status_variables);
  rocksdb_rpc_log(17106, "show_rocksdb_stall_vars: end");
}

static SHOW_VAR rocksdb_status_vars[] = {
    DEF_STATUS_VAR(block_cache_miss),
    DEF_STATUS_VAR(block_cache_hit),
    DEF_STATUS_VAR(block_cache_add),
    DEF_STATUS_VAR(block_cache_add_failures),
    DEF_STATUS_VAR(block_cache_index_miss),
    DEF_STATUS_VAR(block_cache_index_hit),
    DEF_STATUS_VAR(block_cache_index_add),
    DEF_STATUS_VAR(block_cache_index_bytes_insert),
    DEF_STATUS_VAR(block_cache_index_bytes_evict),
    DEF_STATUS_VAR(block_cache_filter_miss),
    DEF_STATUS_VAR(block_cache_filter_hit),
    DEF_STATUS_VAR(block_cache_filter_add),
    DEF_STATUS_VAR(block_cache_filter_bytes_insert),
    DEF_STATUS_VAR(block_cache_filter_bytes_evict),
    DEF_STATUS_VAR(block_cache_bytes_read),
    DEF_STATUS_VAR(block_cache_bytes_write),
    DEF_STATUS_VAR(block_cache_data_bytes_insert),
    DEF_STATUS_VAR(block_cache_data_miss),
    DEF_STATUS_VAR(block_cache_data_hit),
    DEF_STATUS_VAR(block_cache_data_add),
    DEF_STATUS_VAR(bloom_filter_useful),
    DEF_STATUS_VAR(bloom_filter_full_positive),
    DEF_STATUS_VAR(bloom_filter_full_true_positive),
    DEF_STATUS_VAR(memtable_hit),
    DEF_STATUS_VAR(memtable_miss),
    DEF_STATUS_VAR(get_hit_l0),
    DEF_STATUS_VAR(get_hit_l1),
    DEF_STATUS_VAR(get_hit_l2_and_up),
    DEF_STATUS_VAR(compaction_key_drop_new),
    DEF_STATUS_VAR(compaction_key_drop_obsolete),
    DEF_STATUS_VAR(compaction_key_drop_user),
    DEF_STATUS_VAR(number_keys_written),
    DEF_STATUS_VAR(number_keys_read),
    DEF_STATUS_VAR(number_keys_updated),
    DEF_STATUS_VAR(bytes_written),
    DEF_STATUS_VAR(bytes_read),
    DEF_STATUS_VAR(number_db_seek),
    DEF_STATUS_VAR(number_db_seek_found),
    DEF_STATUS_VAR(number_db_next),
    DEF_STATUS_VAR(number_db_next_found),
    DEF_STATUS_VAR(number_db_prev),
    DEF_STATUS_VAR(number_db_prev_found),
    DEF_STATUS_VAR(iter_bytes_read),
    DEF_STATUS_VAR(no_file_closes),
    DEF_STATUS_VAR(no_file_opens),
    DEF_STATUS_VAR(no_file_errors),
    DEF_STATUS_VAR(stall_micros),
    DEF_STATUS_VAR(num_iterators),
    DEF_STATUS_VAR(number_multiget_get),
    DEF_STATUS_VAR(number_multiget_keys_read),
    DEF_STATUS_VAR(number_multiget_bytes_read),
    DEF_STATUS_VAR(number_deletes_filtered),
    DEF_STATUS_VAR(number_merge_failures),
    DEF_STATUS_VAR(bloom_filter_prefix_checked),
    DEF_STATUS_VAR(bloom_filter_prefix_useful),
    DEF_STATUS_VAR(number_reseeks_iteration),
    DEF_STATUS_VAR(getupdatessince_calls),
    DEF_STATUS_VAR(block_cachecompressed_miss),
    DEF_STATUS_VAR(block_cachecompressed_hit),
    DEF_STATUS_VAR(wal_synced),
    DEF_STATUS_VAR(wal_bytes),
    DEF_STATUS_VAR(write_self),
    DEF_STATUS_VAR(write_other),
    DEF_STATUS_VAR(write_timedout),
    DEF_STATUS_VAR(write_wal),
    DEF_STATUS_VAR(flush_write_bytes),
    DEF_STATUS_VAR(compact_read_bytes),
    DEF_STATUS_VAR(compact_write_bytes),
    DEF_STATUS_VAR(number_superversion_acquires),
    DEF_STATUS_VAR(number_superversion_releases),
    DEF_STATUS_VAR(number_superversion_cleanups),
    DEF_STATUS_VAR(number_block_not_compressed),
    DEF_STATUS_VAR_PTR("row_lock_deadlocks", &rocksdb_row_lock_deadlocks,
                       SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("row_lock_wait_timeouts",
                       &rocksdb_row_lock_wait_timeouts, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("snapshot_conflict_errors",
                       &rocksdb_snapshot_conflict_errors, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("wal_group_syncs", &rocksdb_wal_group_syncs,
                       SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("manual_compactions_processed",
                       &rocksdb_manual_compactions_processed, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("manual_compactions_cancelled",
                       &rocksdb_manual_compactions_cancelled, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("manual_compactions_running",
                       &rocksdb_manual_compactions_running, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("manual_compactions_pending",
                       &rocksdb_manual_compactions_pending, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("number_sst_entry_put", &rocksdb_num_sst_entry_put,
                       SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("number_sst_entry_delete", &rocksdb_num_sst_entry_delete,
                       SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("number_sst_entry_singledelete",
                       &rocksdb_num_sst_entry_singledelete, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("number_sst_entry_merge", &rocksdb_num_sst_entry_merge,
                       SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("number_sst_entry_other", &rocksdb_num_sst_entry_other,
                       SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("additional_compaction_triggers",
                       &rocksdb_additional_compaction_triggers, SHOW_LONGLONG),
#ifndef DBUG_OFF
    DEF_STATUS_VAR_PTR("num_get_for_update_calls",
                       &rocksdb_num_get_for_update_calls, SHOW_LONGLONG),
#endif
    DEF_STATUS_VAR_PTR("select_bypass_executed",
                       &rocksdb_select_bypass_executed, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("select_bypass_rejected",
                       &rocksdb_select_bypass_rejected, SHOW_LONGLONG),
    DEF_STATUS_VAR_PTR("select_bypass_failed", &rocksdb_select_bypass_failed,
                       SHOW_LONGLONG),
    // the variables generated by SHOW_FUNC are sorted only by prefix (first
    // arg in the tuple below), so make sure it is unique to make sorting
    // deterministic as quick sort is not stable
    {"rocksdb", reinterpret_cast<char *>(&show_myrocks_vars), SHOW_FUNC},
    {"rocksdb_stall", reinterpret_cast<char *>(&show_rocksdb_stall_vars),
     SHOW_FUNC},
    {NullS, NullS, SHOW_LONG}};

/*
  Background thread's main logic
*/

void Rdb_background_thread::run() {
  rocksdb_rpc_log(17232, "Rdb_background_thread::run: start");

  // How many seconds to wait till flushing the WAL next time.
  const int WAKE_UP_INTERVAL = 1;

  timespec ts_next_sync;
  clock_gettime(CLOCK_REALTIME, &ts_next_sync);
  ts_next_sync.tv_sec += WAKE_UP_INTERVAL;

  for (;;) {
    // Wait until the next timeout or until we receive a signal to stop the
    // thread. Request to stop the thread should only be triggered when the
    // storage engine is being unloaded.
    RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
    const auto ret MY_ATTRIBUTE((__unused__)) =
        mysql_cond_timedwait(&m_signal_cond, &m_signal_mutex, &ts_next_sync);

    // Check that we receive only the expected error codes.
    DBUG_ASSERT(ret == 0 || ret == ETIMEDOUT);
    const THD::killed_state local_killed = m_killed;
    const bool local_save_stats = m_save_stats;
    reset();
    RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);

    if (local_killed) {
      // If we're here then that's because condition variable was signaled by
      // another thread and we're shutting down. Break out the loop to make
      // sure that shutdown thread can proceed.
      break;
    }

    // This path should be taken only when the timer expired.
    DBUG_ASSERT(ret == ETIMEDOUT);

    if (local_save_stats) {
      ddl_manager.persist_stats();
    }

    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Flush the WAL. Sync it for both background and never modes to copy
    // InnoDB's behavior. For mode never, the wal file isn't even written,
    // whereas background writes to the wal file, but issues the syncs in a
    // background thread.
    if (rdb && (rocksdb_flush_log_at_trx_commit != FLUSH_LOG_SYNC) &&
        !rocksdb_DBOptions__GetBoolOptions(rocksdb_db_options,
                                           "allow_mmap_writes")) {
      rocksdb_rpc_log(
          17284, "Rdb_background_thread::run: rocksdb_TransactionDB__FlushWAL");

      // ALTER
      // const rocksdb::Status s = rdb->FlushWAL(true);
      const rocksdb::Status s = rocksdb_TransactionDB__FlushWAL(rdb, true);
      if (!s.ok()) {
        rdb_handle_io_error(s, RDB_IO_ERROR_BG_THREAD);
      }
    }

    // Recalculate statistics for indexes only if
    // rocksdb_table_stats_use_table_scan is disabled.
    //  Otherwise, Rdb_index_stats_thread will do the work
    if (!rocksdb_table_stats_use_table_scan && rocksdb_stats_recalc_rate) {
      std::vector<std::string> to_recalc;
      if (rdb_tables_to_recalc.empty()) {
        struct Rdb_index_collector : public Rdb_tables_scanner {
          int add_table(Rdb_tbl_def *tdef) override {
            rdb_tables_to_recalc.push_back(tdef->full_tablename());
            return HA_EXIT_SUCCESS;
          }
        } collector;
        rocksdb_rpc_log(
            17304, "Rdb_background_thread::run: ddl_manager.scan_for_tables");

        ddl_manager.scan_for_tables(&collector);
      }

      while (to_recalc.size() < rocksdb_stats_recalc_rate &&
             !rdb_tables_to_recalc.empty()) {
        to_recalc.push_back(rdb_tables_to_recalc.back());
        rdb_tables_to_recalc.pop_back();
      }

      for (const auto &tbl_name : to_recalc) {
        calculate_stats_for_table(tbl_name, SCAN_TYPE_NONE);
      }
    }

    // Set the next timestamp for mysql_cond_timedwait() (which ends up calling
    // pthread_cond_timedwait()) to wait on.
    ts_next_sync.tv_sec = ts.tv_sec + WAKE_UP_INTERVAL;
  }

  // save remaining stats which might've left unsaved
  rocksdb_rpc_log(17320, "Rdb_background_thread::run: end");

  ddl_manager.persist_stats();
}

void Rdb_index_stats_thread::run() {
  rocksdb_rpc_log(17332, "Rdb_index_stats_thread::run: begin");
  const int WAKE_UP_INTERVAL = 1;
#ifdef TARGET_OS_LINUX
  RDB_MUTEX_LOCK_CHECK(m_is_mutex);
  m_tid_set = true;
  m_tid = syscall(SYS_gettid);
  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
#endif

  renice(rocksdb_table_stats_background_thread_nice_value);
  for (;;) {
    RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
    if (m_killed) {
      RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);
      break;
    }

    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    // Wait for 24 hours if the table scan based index calculation
    // is off. When the switch is turned on and any request is added
    // to the recalc queue, this thread will be signaled.
    ts.tv_sec +=
        (rocksdb_table_stats_use_table_scan) ? WAKE_UP_INTERVAL : 24 * 60 * 60;

    const auto ret MY_ATTRIBUTE((__unused__)) =
        mysql_cond_timedwait(&m_signal_cond, &m_signal_mutex, &ts);

    if (m_killed) {
      RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);
      break;
    }

    // Make sure, no program error is returned
    DBUG_ASSERT(ret == 0 || ret == ETIMEDOUT);
    RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);

    for (;;) {
      if (!rocksdb_table_stats_use_table_scan) {
        // Clear the recalc queue
        clear_all_index_stats_requests();
        break;
      }

      rocksdb_rpc_log(17377,
                      "Rdb_index_stats_thread::run: get_index_stats_request");
      std::string tbl_name;
      if (!get_index_stats_request(&tbl_name)) {
        // No request in the recalc queue
        break;
      }

      Rdb_table_stats tbl_stats;
      if (ddl_manager.find_table_stats(tbl_name, &tbl_stats) !=
          HA_EXIT_SUCCESS) {
        // The table has been dropped. Skip this table.
        continue;
      }

      clock_gettime(CLOCK_REALTIME, &ts);
      if (difftime(ts.tv_sec, tbl_stats.m_last_recalc) <
          RDB_MIN_RECALC_INTERVAL) {
        /* Stats were (re)calculated not long ago. To avoid
        too frequent stats updates we put back the table on
        the recalc queue and do nothing. */

        add_index_stats_request(tbl_name);
        break;
      }

      DBUG_EXECUTE_IF("rocksdb_is_bg_thread", {
        if (tbl_name == "test.t") {
          THD *thd = new THD();
          thd->thread_stack = reinterpret_cast<char *>(&thd);
          thd->store_globals();

          const char act[] = "now wait_for ready_to_calculate_index_stats";
          DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

          thd->restore_globals();
          delete thd;
        }
      });

      int err =
          calculate_stats_for_table(tbl_name, SCAN_TYPE_FULL_TABLE, &m_killed);

      if (err != HA_EXIT_SUCCESS) {
        global_stats.table_index_stats_result[TABLE_INDEX_STATS_FAILURE].inc();
        break;
      }

      global_stats.table_index_stats_result[TABLE_INDEX_STATS_SUCCESS].inc();

      DBUG_EXECUTE_IF("rocksdb_is_bg_thread", {
        if (tbl_name == "test.t") {
          THD *thd = new THD();
          thd->thread_stack = reinterpret_cast<char *>(&thd);
          thd->store_globals();

          const char act[] = "now signal index_stats_calculation_done";
          DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));

          thd->restore_globals();
          delete thd;
        }
      });
    }
  }

  RDB_MUTEX_LOCK_CHECK(m_is_mutex);
  m_tid_set = false;
  m_tid = 0;
  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
  rocksdb_rpc_log(17477, "Rdb_index_stats_thread::run: end");
}

bool Rdb_index_stats_thread::get_index_stats_request(std::string *tbl_name) {
  rocksdb_rpc_log(17377, "get_index_stats_request: start");

  RDB_MUTEX_LOCK_CHECK(m_is_mutex);
  if (m_requests.empty()) {
    RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
    return false;
  }

  *tbl_name = m_requests[0];
  m_requests.pop_front();

  auto count = m_tbl_names.erase(*tbl_name);
  if (count != 1) {
    DBUG_ASSERT(0);
  }

  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
  rocksdb_rpc_log(17468, "get_index_stats_request: end");

  return true;
}

void Rdb_index_stats_thread::add_index_stats_request(
    const std::string &tbl_name) {
  rocksdb_rpc_log(17478, "add_index_stats_request: start");
  RDB_MUTEX_LOCK_CHECK(m_is_mutex);

  /* Quit if already in the queue */
  auto ret = m_tbl_names.insert(tbl_name);
  if (!ret.second) {
    RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
    rocksdb_rpc_log(17485, "add_index_stats_request: end");
    return;
  }

  m_requests.push_back(*ret.first);
  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
  signal();
  rocksdb_rpc_log(17489, "add_index_stats_request: end");
}

void Rdb_index_stats_thread::clear_all_index_stats_requests() {
  rocksdb_rpc_log(17493, "clear_all_index_stats_requests: start");
  RDB_MUTEX_LOCK_CHECK(m_is_mutex);
  m_requests.clear();
  m_tbl_names.clear();
  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
  rocksdb_rpc_log(17498, "clear_all_index_stats_requests: end");
}

int Rdb_index_stats_thread::renice(int nice_val) {
  rocksdb_rpc_log(17502, "renice: start");

  RDB_MUTEX_LOCK_CHECK(m_is_mutex);
  if (!m_tid_set) {
    RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
    return HA_EXIT_FAILURE;
  }

#ifdef TARGET_OS_LINUX
  int ret = setpriority(PRIO_PROCESS, m_tid, nice_val);
  if (ret != 0) {
    // NO_LINT_DEBUG
    sql_print_error("Set index stats thread priority failed due to %s",
                    strerror(errno));
    RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
    rocksdb_rpc_log(17519, "renice: end");

    return HA_EXIT_FAILURE;
  }
#endif

  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);
  rocksdb_rpc_log(17526, "renice: end");

  return HA_EXIT_SUCCESS;
}

size_t Rdb_index_stats_thread::get_request_queue_size() {
  rocksdb_rpc_log(17530, "get_request_queue_size: start");

  size_t len = 0;
  RDB_MUTEX_LOCK_CHECK(m_is_mutex);
  len = m_requests.size();
  RDB_MUTEX_UNLOCK_CHECK(m_is_mutex);

  rocksdb_rpc_log(17539, "get_request_queue_size: end");
  return len;
}

/*
  A background thread to handle manual compactions,
  except for dropping indexes/tables. Every second, it checks
  pending manual compactions, and it calls CompactRange if there is.
*/
void Rdb_manual_compaction_thread::run() {
  rocksdb_rpc_log(17547, "Rdb_manual_compaction_thread::run: start");

  RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
  for (;;) {
    if (m_killed) {
      break;
    }
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;

    const auto ret MY_ATTRIBUTE((__unused__)) =
        mysql_cond_timedwait(&m_signal_cond, &m_signal_mutex, &ts);
    if (m_killed) {
      break;
    }
    // make sure, no program error is returned
    DBUG_ASSERT(ret == 0 || ret == ETIMEDOUT);
    RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);

    RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
    // Grab the first PENDING state item and proceed, if not empty.
    if (m_requests.empty()) {
      RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
      RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
      continue;
    }
    rocksdb_rpc_log(17574, "Rdb_manual_compaction_thread::run: get request");

    auto it = m_requests.begin();
    auto pending_it = m_requests.end();
    // Remove all items with client_done. client_done means
    // caller no longer uses the mcr object so it is safe to erase.
    // Pick first PENDING state item
    it = m_requests.begin();
    while (it != m_requests.end()) {
      if (it->second.client_done) {
        m_requests.erase(it++);
      } else if (it->second.state == Manual_compaction_request::PENDING &&
                 pending_it == m_requests.end()) {
        // found
        pending_it = it;
        it++;
      } else {
        it++;
      }
    }
    if (pending_it == m_requests.end()) {
      RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
      RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
      continue;
    }

    Manual_compaction_request &mcr = pending_it->second;
    DBUG_ASSERT(mcr.cf);
    DBUG_ASSERT(mcr.state == Manual_compaction_request::PENDING);
    mcr.state = Manual_compaction_request::RUNNING;
    rocksdb_manual_compactions_running++;
    rocksdb_manual_compactions_pending--;
    RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);

    rocksdb_rpc_log(
        17609, "Rdb_manual_compaction_thread::run: manual compaction started");

    DBUG_ASSERT(mcr.state == Manual_compaction_request::RUNNING);
    // NO_LINT_DEBUG
    // ALTER
    // sql_print_information("Manual Compaction id %d cf %s started.",
    // mcr.mc_id,
    //                       mcr.cf->GetName().c_str());
    sql_print_information("Manual Compaction id %d cf %s started.", mcr.mc_id,
                          rocksdb_ColumnFamilyHandle__GetName(mcr.cf).c_str());
    if (rocksdb_debug_manual_compaction_delay > 0) {
      my_sleep(rocksdb_debug_manual_compaction_delay * 1000000);
    }

    DBUG_EXECUTE_IF("rocksdb_manual_compaction", {
      THD *thd = new THD();
      thd->thread_stack = reinterpret_cast<char *>(&(thd));
      thd->store_globals();
      const char act[] =
          "now signal ready_to_mark_cf_dropped_in_manual_compaction wait_for "
          "mark_cf_dropped_done_in_manual_compaction";
      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
      thd->restore_globals();
      delete thd;
    });

    rocksdb_rpc_log(17646,
                    "Rdb_manual_compaction_thread::run: "
                    "rocksdb_TransactionDB__CompactRange");

    // CompactRange may take a very long time. On clean shutdown,
    // it is cancelled by CancelAllBackgroundWork, then status is
    // set to shutdownInProgress.

    // ALTER
    // const rocksdb::Status s =
    //     rdb->CompactRange(mcr.option, mcr.cf.get(), mcr.start, mcr.limit);
    const rocksdb::Status s = rocksdb_TransactionDB__CompactRange(
        rdb, mcr.option, mcr.cf, *mcr.start, *mcr.limit);

    rocksdb_manual_compactions_running--;
    if (s.ok()) {
      rocksdb_manual_compactions_processed++;
      // NO_LINT_DEBUG

      // ALTER
      // sql_print_information("Manual Compaction id %d cf %s ended.",
      // mcr.mc_id,
      //                       mcr.cf->GetName().c_str());
      sql_print_information(
          "Manual Compaction id %d cf %s ended.", mcr.mc_id,
          rocksdb_ColumnFamilyHandle__GetName(mcr.cf).c_str());
      set_state(mcr, Manual_compaction_request::SUCCESS);
    } else {
      // ALTER
      // if (!cf_manager.get_cf(mcr.cf->GetID())) {
      if (!cf_manager.get_cf(rocksdb_ColumnFamilyHandle__GetID(mcr.cf))) {
        // NO_LINT_DEBUG
        // ALTER
        // sql_print_information("cf %s has been dropped",
        //                       mcr.cf->GetName().c_str());
        sql_print_information(
            "cf %s has been dropped",
            rocksdb_ColumnFamilyHandle__GetName(mcr.cf).c_str());

        set_state(mcr, Manual_compaction_request::SUCCESS);
      } else if (s.IsIncomplete()) {
        // NO_LINT_DEBUG

        // ALTER
        // sql_print_information(
        //     "Manual Compaction id %d cf %s cancelled. (%d:%d, %s)",
        //     mcr.mc_id, mcr.cf->GetName().c_str(), s.code(), s.subcode(),
        //     s.getState());
        sql_print_information(
            "Manual Compaction id %d cf %s cancelled. (%d:%d, %s)", mcr.mc_id,
            rocksdb_ColumnFamilyHandle__GetName(mcr.cf).c_str(), s.code(),
            s.subcode(), s.getState());

        // Cancelled
        set_state(mcr, Manual_compaction_request::CANCEL);
        rocksdb_manual_compactions_cancelled++;
      } else {
        // NO_LINT_DEBUG

        // ALTER
        // sql_print_information(
        //     "Manual Compaction id %d cf %s aborted. (%d:%d, %s)", mcr.mc_id,
        //     mcr.cf->GetName().c_str(), s.code(), s.subcode(), s.getState());
        sql_print_information(
            "Manual Compaction id %d cf %s aborted. (%d:%d, %s)", mcr.mc_id,
            rocksdb_ColumnFamilyHandle__GetName(mcr.cf).c_str(), s.code(),
            s.subcode(), s.getState());

        set_state(mcr, Manual_compaction_request::FAILURE);
        if (!s.IsShutdownInProgress()) {
          rdb_handle_io_error(s, RDB_IO_ERROR_BG_THREAD);
        } else {
          DBUG_ASSERT(m_requests.size() == 1);
        }
      }
    }
    RDB_MUTEX_LOCK_CHECK(m_signal_mutex);
  }
  clear_all_manual_compaction_requests();
  DBUG_ASSERT(m_requests.empty());
  RDB_MUTEX_UNLOCK_CHECK(m_signal_mutex);
  rocksdb_rpc_log(17716, "Rdb_manual_compaction_thread::run: end");
}

void Rdb_manual_compaction_thread::clear_all_manual_compaction_requests() {
  rocksdb_rpc_log(17720, "clear_all_manual_compaction_requests: begin");
  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  DBUG_ASSERT(rocksdb_manual_compactions_pending == 0);
  m_requests.clear();
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
  rocksdb_rpc_log(17725, "clear_all_manual_compaction_requests: end");
}

void Rdb_manual_compaction_thread::
    cancel_all_pending_manual_compaction_requests() {
  rocksdb_rpc_log(17731,
                  "cancel_all_pending_manual_compaction_requests: start");

  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  auto it = m_requests.begin();
  while (it != m_requests.end()) {
    Manual_compaction_request &mcr = it->second;
    if (mcr.state == Manual_compaction_request::PENDING) {
      mcr.state = Manual_compaction_request::CANCEL;
      rocksdb_manual_compactions_cancelled++;
      rocksdb_manual_compactions_pending--;
    }
    it++;
  }
  DBUG_ASSERT(rocksdb_manual_compactions_pending == 0);
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
  rocksdb_rpc_log(17746, "cancel_all_pending_manual_compaction_requests: end");
}

/**
 *  Requesting to cancel a Manual Compaction job with mc_id.
 *  Only PENDING or RUNNING states need cancellation.
 *  This function may take a while if state is RUNNING.
 *  Returning true if hitting timeout and state is RUNNING.
 */
bool Rdb_manual_compaction_thread::cancel_manual_compaction_request(
    const int mc_id, const int timeout_100ms) {
  rocksdb_rpc_log(17757, "cancel_manual_compaction_request: start");

  Manual_compaction_request::mc_state state =
      Manual_compaction_request::PENDING;

  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  auto it = m_requests.find(mc_id);
  if (it != m_requests.end()) {
    Manual_compaction_request &mcr = it->second;
    if (mcr.state == Manual_compaction_request::PENDING) {
      mcr.state = Manual_compaction_request::CANCEL;
      rocksdb_manual_compactions_cancelled++;
      rocksdb_manual_compactions_pending--;
      RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
      rocksdb_rpc_log(17771, "cancel_manual_compaction_request: end");
      return false;
    } else if (mcr.state == Manual_compaction_request::RUNNING) {
      // explicitly requesting to cancel compaction (cancellation happens in
      // background may take time)

      // TODO: ALTER
      // mcr.option.canceled->store(true, std::memory_order_release);
      state = mcr.state;
    }
  }
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);

  // Waiting to get manual compaction to get cancelled.
  // Even if returning timeouts to clients, manual compaction
  // is still running so further compactions can remain
  // in pending state until the compaction completes cancellation.
  uint64_t retry = timeout_100ms;
  while (retry > 0 && state == Manual_compaction_request::RUNNING) {
    my_sleep(100000);
    retry--;
    state = manual_compaction_state(mc_id);
  }

  rocksdb_rpc_log(17797, "cancel_manual_compaction_request: end");

  return retry <= 0 && state == Manual_compaction_request::RUNNING;
}

/**
 * This function is for clients to request for Manual Compaction.
 * This function adds mcr (Manual Compaction Request) in a queue
 * as PENDING state then returns. Worker Thread then later picks it up
 * and processes compaction.
 * Clients should call set_client_done() when the clients are done with
 * the status of the requests.
 */

// ALTER
int Rdb_manual_compaction_thread::request_manual_compaction(
    rocksdb::ColumnFamilyHandle *cf, rocksdb::Slice *start,
    rocksdb::Slice *limit, const uint manual_compaction_threads,
    const rocksdb::BottommostLevelCompaction bottommost_level_compaction) {
  rocksdb_rpc_log(17815, "request_manual_compaction: start");

  int mc_id = -1;
  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  if (m_requests.size() >= rocksdb_max_manual_compactions) {
    RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
    return mc_id;
  }
  Manual_compaction_request mcr;
  mc_id = mcr.mc_id = ++m_latest_mc_id;
  mcr.state = Manual_compaction_request::PENDING;
  mcr.cf = cf;
  mcr.start = start;
  mcr.limit = limit;
  mcr.option = getCompactRangeOptions(manual_compaction_threads,
                                      bottommost_level_compaction);
  mcr.canceled =
      std::shared_ptr<std::atomic<bool>>(new std::atomic<bool>(false));
  mcr.option.canceled = mcr.canceled.get();
  mcr.client_done = false;

  rocksdb_rpc_log(17836, "request_manual_compaction: set mcr");

  rocksdb_manual_compactions_pending++;
  m_requests.insert(std::make_pair(mcr.mc_id, mcr));
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
  rocksdb_rpc_log(17842, "request_manual_compaction: end");

  return mc_id;
}

Rdb_manual_compaction_thread::Manual_compaction_request::mc_state
Rdb_manual_compaction_thread::manual_compaction_state(const int mc_id) {
  rocksdb_rpc_log(17849, "manual_compaction_state: start");

  Manual_compaction_request::mc_state state =
      Manual_compaction_request::SUCCESS;
  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  auto it = m_requests.find(mc_id);
  if (it != m_requests.end()) {
    Manual_compaction_request &mcr = it->second;
    state = mcr.state;
  }
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
  rocksdb_rpc_log(17860, "manual_compaction_state: end");

  return state;
}

void Rdb_manual_compaction_thread::set_state(
    Manual_compaction_request &mcr,
    const Manual_compaction_request::mc_state new_state) {
  rocksdb_rpc_log(17866, "set_state: start");

  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  mcr.state = new_state;
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
  rocksdb_rpc_log(17871, "set_state: end");
}

bool Rdb_manual_compaction_thread::set_client_done(const int mc_id) {
  rocksdb_rpc_log(17875, "set_client_done: start");
  bool rc = false;
  RDB_MUTEX_LOCK_CHECK(m_mc_mutex);
  auto it = m_requests.find(mc_id);
  if (it != m_requests.end()) {
    Manual_compaction_request &mcr = it->second;
    mcr.client_done = true;
    rc = true;
  }
  RDB_MUTEX_UNLOCK_CHECK(m_mc_mutex);
  rocksdb_rpc_log(17885, "set_client_done: end");
  return rc;
}

/**
 * Locking read + Not Found + Read Committed occurs if we accessed
 * a row by Seek, tried to lock it, failed, released and reacquired the
 * snapshot (because of READ COMMITTED mode) and the row was deleted by
 * someone else in the meantime.
 * If so, we either just skipping the row, or re-creating a snapshot
 * and seek again. In both cases, Read Committed constraint is not broken.
 */
bool ha_rocksdb::should_skip_invalidated_record(const int rc) {
  rocksdb_rpc_log(17898, "should_skip_invalidated_record: start");

  if ((m_lock_rows != RDB_LOCK_NONE && rc == HA_ERR_KEY_NOT_FOUND &&
       my_core::thd_tx_isolation(ha_thd()) == ISO_READ_COMMITTED)) {
    rocksdb_rpc_log(17902, "should_skip_invalidated_record: start");

    return true;
  }
  rocksdb_rpc_log(17908, "should_skip_invalidated_record: start");

  return false;
}
/**
 * Indicating snapshot needs to be re-created and retrying seek again,
 * instead of returning errors or empty set. This is normally applicable
 * when hitting kBusy when locking the first row of the transaction,
 * with Repeatable Read isolation level.
 */
bool ha_rocksdb::should_recreate_snapshot(const int rc,
                                          const bool is_new_snapshot) {
  rocksdb_rpc_log(17918, "should_recreate_snapshot: start");

  if (should_skip_invalidated_record(rc) ||
      (rc == HA_ERR_ROCKSDB_STATUS_BUSY && is_new_snapshot)) {
    rocksdb_rpc_log(17924, "should_recreate_snapshot: end");

    return true;
  }
  rocksdb_rpc_log(17928, "should_recreate_snapshot: end");

  return false;
}

/**
 * If calling put/delete/singledelete without locking the row,
 * it is necessary to pass assume_tracked=false to RocksDB TX API.
 * Read Free Replication and Blind Deletes are the cases when
 * using TX API and skipping row locking.
 */
bool ha_rocksdb::can_assume_tracked(THD *thd) {
  rocksdb_rpc_log(17938, "can_assume_tracked: start");

  if (use_read_free_rpl() || (THDVAR(thd, blind_delete_primary_key))) {
    rocksdb_rpc_log(17942, "can_assume_tracked: end");

    return false;
  }
  rocksdb_rpc_log(17946, "can_assume_tracked: end");

  return true;
}

bool ha_rocksdb::check_bloom_and_set_bounds(
    THD *thd, const Rdb_key_def &kd, const rocksdb::Slice &eq_cond,
    const bool use_all_keys, size_t bound_len, uchar *const lower_bound,
    uchar *const upper_bound, rocksdb::Slice *lower_bound_slice,
    rocksdb::Slice *upper_bound_slice) {
  rocksdb_rpc_log(17955, "check_bloom_and_set_bounds: start");

  bool can_use_bloom = can_use_bloom_filter(thd, kd, eq_cond, use_all_keys);
  if (!can_use_bloom && (THDVAR(thd, enable_iterate_bounds))) {
    setup_iterator_bounds(kd, eq_cond, bound_len, lower_bound, upper_bound,
                          lower_bound_slice, upper_bound_slice);
  }
  rocksdb_rpc_log(17962, "check_bloom_and_set_bounds: end");

  return can_use_bloom;
}

/**
  Deciding if it is possible to use bloom filter or not.

  @detail
   Even if bloom filter exists, it is not always possible
   to use bloom filter. If using bloom filter when you shouldn't,
   false negative may happen -- fewer rows than expected may be returned.
   It is users' responsibility to use bloom filter correctly.

   If bloom filter does not exist, return value does not matter because
   RocksDB does not use bloom filter internally.

  @param kd
  @param eq_cond      Equal condition part of the key. This always includes
                      system index id (4 bytes).
  @param use_all_keys True if all key parts are set with equal conditions.
                      This is aware of extended keys.
*/
bool ha_rocksdb::can_use_bloom_filter(THD *thd, const Rdb_key_def &kd,
                                      const rocksdb::Slice &eq_cond,
                                      const bool use_all_keys) {
  rocksdb_rpc_log(17988, "can_use_bloom_filter: start");

  bool can_use = false;

  if (THDVAR(thd, skip_bloom_filter_on_read)) {
    return can_use;
  }

  rocksdb_rpc_log(17996, "can_use_bloom_filter: kd.get_extractor");
  const rocksdb::SliceTransform *prefix_extractor = kd.get_extractor();
  if (prefix_extractor) {
    /*
      This is an optimized use case for CappedPrefixTransform.
      If eq_cond length >= prefix extractor length and if
      all keys are used for equal lookup, it is
      always possible to use bloom filter.

      Prefix bloom filter can't be used on descending scan with
      prefix lookup (i.e. WHERE id1=1 ORDER BY id2 DESC), because of
      RocksDB's limitation. On ascending (or not sorting) scan,
      keys longer than the capped prefix length will be truncated down
      to the capped length and the resulting key is added to the bloom filter.

      Keys shorter than the capped prefix length will be added to
      the bloom filter. When keys are looked up, key conditionals
      longer than the capped length can be used; key conditionals
      shorter require all parts of the key to be available
      for the short key match.
    */

    // ALTER
    // if ((use_all_keys && prefix_extractor->InRange(eq_cond)) ||
    //     prefix_extractor->SameResultWhenAppended(eq_cond)) {
    //   can_use = true;
    // } else {
    //   can_use = false;
    // }
    rocksdb_rpc_log(18025,
                    "can_use_bloom_filter: rocksdb_SliceTransform__InRange "
                    "rocksdb_SliceTransform__SameResultWhenAppended");

    if ((use_all_keys &&
         rocksdb_SliceTransform__InRange(prefix_extractor, eq_cond)) ||
        rocksdb_SliceTransform__SameResultWhenAppended(prefix_extractor,
                                                       eq_cond)) {
      can_use = true;
    } else {
      can_use = false;
    }
  } else {
    /*
      if prefix extractor is not defined, all key parts have to be
      used by eq_cond.
    */
    if (use_all_keys) {
      can_use = true;
    } else {
      can_use = false;
    }
  }

  rocksdb_rpc_log(18050, "can_use_bloom_filter: end");
  return can_use;
}

/* For modules that need access to the global data structures */
rocksdb::TransactionDB *rdb_get_rocksdb_db() { return rdb; }

Rdb_cf_manager &rdb_get_cf_manager() { return cf_manager; }

// ALTER
// const rocksdb::BlockBasedTableOptions &rdb_get_table_options() {
//   return *rocksdb_tbl_options;
// }

rocksdb::BlockBasedTableOptions *rdb_get_table_options() {
  return rocksdb_tbl_options;
}

bool rdb_is_table_scan_index_stats_calculation_enabled() {
  return rocksdb_table_stats_use_table_scan;
}
bool rdb_is_ttl_enabled() { return rocksdb_enable_ttl; }
bool rdb_is_ttl_read_filtering_enabled() {
  return rocksdb_enable_ttl_read_filtering;
}
#ifndef DBUG_OFF
int rdb_dbug_set_ttl_rec_ts() { return rocksdb_debug_ttl_rec_ts; }
int rdb_dbug_set_ttl_snapshot_ts() { return rocksdb_debug_ttl_snapshot_ts; }
int rdb_dbug_set_ttl_read_filter_ts() {
  return rocksdb_debug_ttl_read_filter_ts;
}
bool rdb_dbug_set_ttl_ignore_pk() { return rocksdb_debug_ttl_ignore_pk; }
#endif

void rdb_update_global_stats(const operation_type &type, uint count,
                             bool is_system_table) {
  rocksdb_rpc_log(18086, "rdb_update_global_stats: start");
  DBUG_ASSERT(type < ROWS_MAX);

  if (count == 0) {
    rocksdb_rpc_log(18090, "rdb_update_global_stats: end");
    return;
  }

  if (is_system_table) {
    global_stats.system_rows[type].add(count);
  } else {
    global_stats.rows[type].add(count);
  }
  rocksdb_rpc_log(18099, "rdb_update_global_stats: end");
}

int rdb_get_table_perf_counters(const char *const tablename,
                                Rdb_perf_counters *const counters) {
  rocksdb_rpc_log(18103, "rdb_get_table_perf_counters: start");

  DBUG_ASSERT(tablename != nullptr);

  Rdb_table_handler *table_handler;
  table_handler = rdb_open_tables.get_table_handler(tablename);
  if (table_handler == nullptr) {
    rocksdb_rpc_log(18110, "rdb_get_table_perf_counters: end");

    return HA_ERR_ROCKSDB_INVALID_TABLE;
  }

  counters->load(table_handler->m_table_perf_context);

  rdb_open_tables.release_table_handler(table_handler);
  rocksdb_rpc_log(18118, "rdb_get_table_perf_counters: end");

  return HA_EXIT_SUCCESS;
}

const char *get_rdb_io_error_string(const RDB_IO_ERROR_TYPE err_type) {
  // If this assertion fails then this means that a member has been either added
  // to or removed from RDB_IO_ERROR_TYPE enum and this function needs to be
  // changed to return the appropriate value.
  static_assert(RDB_IO_ERROR_LAST == 4, "Please handle all the error types.");

  switch (err_type) {
    case RDB_IO_ERROR_TYPE::RDB_IO_ERROR_TX_COMMIT:
      return "RDB_IO_ERROR_TX_COMMIT";
    case RDB_IO_ERROR_TYPE::RDB_IO_ERROR_DICT_COMMIT:
      return "RDB_IO_ERROR_DICT_COMMIT";
    case RDB_IO_ERROR_TYPE::RDB_IO_ERROR_BG_THREAD:
      return "RDB_IO_ERROR_BG_THREAD";
    case RDB_IO_ERROR_TYPE::RDB_IO_ERROR_GENERAL:
      return "RDB_IO_ERROR_GENERAL";
    default:
      DBUG_ASSERT(false);
      return "(unknown)";
  }
}

// In case of core dump generation we want this function NOT to be optimized
// so that we can capture as much data as possible to debug the root cause
// more efficiently.
void rdb_handle_io_error(const rocksdb::Status status,
                         const RDB_IO_ERROR_TYPE err_type) {
  if (status.IsIOError()) {
    /* skip dumping core if write failed and we are allowed to do so */
    if (skip_core_dump_on_error) {
      opt_core_file = false;
    }

    switch (err_type) {
      case RDB_IO_ERROR_TX_COMMIT:
      case RDB_IO_ERROR_DICT_COMMIT: {
        rdb_log_status_error(status, "failed to write to WAL");
        /* NO_LINT_DEBUG */
        sql_print_error("MyRocks: aborting on WAL write error.");
        abort();
        break;
      }
      case RDB_IO_ERROR_BG_THREAD: {
        rdb_log_status_error(status, "BG thread failed to write to RocksDB");
        /* NO_LINT_DEBUG */
        sql_print_error("MyRocks: aborting on BG write error.");
        abort();
        break;
      }
      case RDB_IO_ERROR_GENERAL: {
        rdb_log_status_error(status, "failed on I/O");
        /* NO_LINT_DEBUG */
        sql_print_error("MyRocks: aborting on I/O error.");
        abort();
        break;
      }
      default:
        DBUG_ASSERT(0);
        break;
    }
  } else if (status.IsCorruption()) {
    rdb_log_status_error(status, "data corruption detected!");
    rdb_persist_corruption_marker();
    /* NO_LINT_DEBUG */
    sql_print_error("MyRocks: aborting because of data corruption.");
    abort();
  } else if (!status.ok()) {
    switch (err_type) {
      case RDB_IO_ERROR_TX_COMMIT:
      case RDB_IO_ERROR_DICT_COMMIT: {
        rdb_log_status_error(status, "Failed to write to WAL (non kIOError)");
        /* NO_LINT_DEBUG */
        sql_print_error("MyRocks: aborting on WAL write error.");
        abort();
        break;
      }
      default:
        rdb_log_status_error(status, "Failed to read/write in RocksDB");
        break;
    }
  }
}

Rdb_dict_manager *rdb_get_dict_manager(void) { return &dict_manager; }

Rdb_ddl_manager *rdb_get_ddl_manager(void) { return &ddl_manager; }

Rdb_binlog_manager *rdb_get_binlog_manager(void) { return &binlog_manager; }

void rocksdb_set_compaction_options(
    my_core::THD *const thd MY_ATTRIBUTE((__unused__)),
    my_core::st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr, const void *const save) {
  rocksdb_rpc_log(18215, "rocksdb_set_compaction_options: start");

  if (var_ptr && save) {
    *(uint64_t *)var_ptr = *(const uint64_t *)save;
  }
  const Rdb_compact_params params = {
      (uint64_t)rocksdb_compaction_sequential_deletes,
      (uint64_t)rocksdb_compaction_sequential_deletes_window,
      (uint64_t)rocksdb_compaction_sequential_deletes_file_size};
  if (properties_collector_factory) {
    properties_collector_factory->SetCompactionParams(params);
  }
  rocksdb_rpc_log(18227, "rocksdb_set_compaction_options: end");
}

void rocksdb_set_table_stats_sampling_pct(
    my_core::THD *const thd MY_ATTRIBUTE((__unused__)),
    my_core::st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  rocksdb_rpc_log(18234, "rocksdb_set_table_stats_sampling_pct: start");

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const uint32_t new_val = *static_cast<const uint32_t *>(save);

  if (new_val != rocksdb_table_stats_sampling_pct) {
    rocksdb_table_stats_sampling_pct = new_val;

    if (properties_collector_factory) {
      properties_collector_factory->SetTableStatsSamplingPct(
          rocksdb_table_stats_sampling_pct);
    }
  }

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(18251, "rocksdb_set_table_stats_sampling_pct: end");
}

void rocksdb_update_table_stats_use_table_scan(
    THD *const /* thd */, struct st_mysql_sys_var *const /* var */,
    void *const var_ptr, const void *const save) {
  rocksdb_rpc_log(18256, "rocksdb_update_table_stats_use_table_scan: start");

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);
  bool old_val = *static_cast<const my_bool *>(var_ptr);
  bool new_val = *static_cast<const my_bool *>(save);

  if (old_val == new_val) {
    RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
    rocksdb_rpc_log(18264, "rocksdb_update_table_stats_use_table_scan: end");

    return;
  }

  if (new_val) {
    struct Rdb_table_collector : public Rdb_tables_scanner {
      int add_table(Rdb_tbl_def *tdef) override {
        DBUG_ASSERT(tdef->m_key_count > 0);
        tdef->m_tbl_stats.set(tdef->m_key_count > 0
                                  ? tdef->m_key_descr_arr[0]->m_stats.m_rows
                                  : 0,
                              0, 0);

        return HA_EXIT_SUCCESS;
      }
    } collector;

    rocksdb_rpc_log(
        18283, "rocksdb_update_table_stats_use_table_scan: scan_for_tables");

    ddl_manager.scan_for_tables(&collector);

    // We do not add all tables to the index stats recalculation queue
    // to avoid index stats calculation workload spike.
  } else {
    rocksdb_rpc_log(18291,
                    "rocksdb_update_table_stats_use_table_scan: "
                    "clear_all_index_stats_requests");

    rdb_is_thread.clear_all_index_stats_requests();
  }

  *static_cast<my_bool *>(var_ptr) = *static_cast<const my_bool *>(save);
  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(18299, "rocksdb_update_table_stats_use_table_scan: end");
}

int rocksdb_index_stats_thread_renice(THD *const /* thd */,
                                      struct st_mysql_sys_var *const /* var */,
                                      void *const save,
                                      struct st_mysql_value *const value) {
  rocksdb_rpc_log(182306, "rocksdb_index_stats_thread_renice: start");

  long long nice_val;
  /* value is NULL */
  if (value->val_int(value, &nice_val)) {
    return HA_EXIT_FAILURE;
  }

  if (rdb_is_thread.renice(nice_val) != HA_EXIT_SUCCESS) {
    return HA_EXIT_FAILURE;
  }

  *static_cast<int32_t *>(save) = static_cast<int32_t>(nice_val);
  rocksdb_rpc_log(182306, "rocksdb_index_stats_thread_renice: end");

  return HA_EXIT_SUCCESS;
}

/*
  This function allows setting the rate limiter's bytes per second value
  but only if the rate limiter is turned on which has to be done at startup.
  If the rate is already 0 (turned off) or we are changing it to 0 (trying
  to turn it off) this function will push a warning to the client and do
  nothing.
  This is similar to the code in innodb_doublewrite_update (found in
  storage/innobase/handler/ha_innodb.cc).
*/
void rocksdb_set_rate_limiter_bytes_per_sec(
    my_core::THD *const thd,
    my_core::st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  rocksdb_rpc_log(182337, "rocksdb_set_rate_limiter_bytes_per_sec: start");

  const uint64_t new_val = *static_cast<const uint64_t *>(save);
  if (new_val == 0 || rocksdb_rate_limiter_bytes_per_sec == 0) {
    /*
      If a rate_limiter was not enabled at startup we can't change it nor
      can we disable it if one was created at startup
    */
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, ER_WRONG_ARGUMENTS,
                        "RocksDB: rocksdb_rate_limiter_bytes_per_sec cannot "
                        "be dynamically changed to or from 0.  Do a clean "
                        "shutdown if you want to change it from or to 0.");
  } else if (new_val != rocksdb_rate_limiter_bytes_per_sec) {
    /* Apply the new value to the rate limiter and store it locally */
    DBUG_ASSERT(rocksdb_rate_limiter != nullptr);
    rocksdb_rate_limiter_bytes_per_sec = new_val;

    rocksdb_rpc_log(182354,
                    "rocksdb_set_rate_limiter_bytes_per_sec: "
                    "rocksdb_RateLimiter__SetBytesPerSecond");

    // ALTER
    // rocksdb_rate_limiter->SetBytesPerSecond(new_val);
    rocksdb_RateLimiter__SetBytesPerSecond(rocksdb_rate_limiter, new_val);
  }
  rocksdb_rpc_log(182362,
                  "rocksdb_set_rate_limiter_bytes_per_sec: "
                  "end");
}

void rocksdb_set_sst_mgr_rate_bytes_per_sec(
    my_core::THD *const thd,
    my_core::st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  rocksdb_rpc_log(182362, "rocksdb_set_sst_mgr_rate_bytes_per_sec: begin");
  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const uint64_t new_val = *static_cast<const uint64_t *>(save);

  if (new_val != rocksdb_sst_mgr_rate_bytes_per_sec) {
    rocksdb_sst_mgr_rate_bytes_per_sec = new_val;

    // ALTER
    // rocksdb_db_options->sst_file_manager->SetDeleteRateBytesPerSecond(
    //     rocksdb_sst_mgr_rate_bytes_per_sec);
    rocksdb_DBOptions__SetDeleteRateBytesPerSecond(
        rocksdb_db_options, rocksdb_sst_mgr_rate_bytes_per_sec);
  }

  rocksdb_rpc_log(182362, "rocksdb_set_sst_mgr_rate_bytes_per_sec: end");
  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

void rocksdb_set_delayed_write_rate(THD *thd, struct st_mysql_sys_var *var,
                                    void *var_ptr, const void *save) {
  rocksdb_rpc_log(182392, "rocksdb_set_delayed_write_rate: start");

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);
  const uint64_t new_val = *static_cast<const uint64_t *>(save);
  if (rocksdb_delayed_write_rate != new_val) {
    rocksdb_delayed_write_rate = new_val;

    // ALTER
    // rocksdb::Status s =
    //     rdb->SetDBOptions({{"delayed_write_rate", std::to_string(new_val)}});
    rocksdb_rpc_log(
        18402,
        "rocksdb_set_delayed_write_rate: rocksdb_TransactionDB__SetDBOptions");

    rocksdb::Status s = rocksdb_TransactionDB__SetDBOptions(
        rdb, {{"delayed_write_rate", std::to_string(new_val)}});

    if (!s.ok()) {
      /* NO_LINT_DEBUG */
      sql_print_warning(
          "MyRocks: failed to update delayed_write_rate. "
          "status code = %d, status = %s",
          s.code(), s.ToString().c_str());
    }
  }
  rocksdb_rpc_log(18418, "rocksdb_set_delayed_write_rate: end");

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

void rocksdb_set_max_latest_deadlocks(THD *thd, struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save) {
  rocksdb_rpc_log(18425, "rocksdb_set_max_latest_deadlocks: start");

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);
  const uint32_t new_val = *static_cast<const uint32_t *>(save);
  if (rocksdb_max_latest_deadlocks != new_val) {
    rocksdb_max_latest_deadlocks = new_val;

    rocksdb_rpc_log(18435,
                    "rocksdb_set_max_latest_deadlocks: "
                    "rocksdb_TransactionDB__SetDeadlockInfoBufferSize");

    // ALTER
    // rdb->SetDeadlockInfoBufferSize(rocksdb_max_latest_deadlocks);
    rocksdb_TransactionDB__SetDeadlockInfoBufferSize(
        rdb, rocksdb_max_latest_deadlocks);
  }
  rocksdb_rpc_log(18444, "rocksdb_set_max_latest_deadlocks: end");

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
}

void rdb_set_collation_exception_list(const char *const exception_list) {
  rocksdb_rpc_log(18446, "rdb_set_collation_exception_list: start");

  DBUG_ASSERT(rdb_collation_exceptions != nullptr);

  if (!rdb_collation_exceptions->set_patterns(exception_list)) {
    my_core::warn_about_bad_patterns(rdb_collation_exceptions,
                                     "strict_collation_exceptions");
  }
  rocksdb_rpc_log(18454, "rdb_set_collation_exception_list: end");
}

void rocksdb_set_collation_exception_list(THD *const thd,
                                          struct st_mysql_sys_var *const var,
                                          void *const var_ptr,
                                          const void *const save) {
  rocksdb_rpc_log(18454, "rocksdb_set_collation_exception_list: start");

  const char *const val = *static_cast<const char *const *>(save);

  rdb_set_collation_exception_list(val == nullptr ? "" : val);

  *static_cast<const char **>(var_ptr) = val;
  rocksdb_rpc_log(18470, "rocksdb_set_collation_exception_list: end");
}

int mysql_value_to_bool(struct st_mysql_value *value, my_bool *return_value) {
  rocksdb_rpc_log(18474, "mysql_value_to_bool: start");

  int new_value_type = value->value_type(value);
  if (new_value_type == MYSQL_VALUE_TYPE_STRING) {
    char buf[16];
    int len = sizeof(buf);
    const char *str = value->val_str(value, buf, &len);
    if (str && (my_strcasecmp(system_charset_info, "true", str) == 0 ||
                my_strcasecmp(system_charset_info, "on", str) == 0)) {
      *return_value = TRUE;
    } else if (str && (my_strcasecmp(system_charset_info, "false", str) == 0 ||
                       my_strcasecmp(system_charset_info, "off", str) == 0)) {
      *return_value = FALSE;
    } else {
      rocksdb_rpc_log(18488, "mysql_value_to_bool: start");

      return 1;
    }
  } else if (new_value_type == MYSQL_VALUE_TYPE_INT) {
    long long intbuf;
    value->val_int(value, &intbuf);
    if (intbuf > 1) return 1;
    *return_value = intbuf > 0 ? TRUE : FALSE;
  } else {
    rocksdb_rpc_log(18496, "mysql_value_to_bool: end");

    return 1;
  }

  rocksdb_rpc_log(18501, "mysql_value_to_bool: end");

  return 0;
}

static int check_rocksdb_skip_locks_if_skip_unique_check(
    THD *const thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var, void *const save,
    struct st_mysql_value *const value) {
  rocksdb_rpc_log(18510,
                  "check_rocksdb_skip_locks_if_skip_unique_check: start");

  my_bool new_value;
  if (mysql_value_to_bool(value, &new_value) != 0) {
    rocksdb_rpc_log(18514,
                    "check_rocksdb_skip_locks_if_skip_unique_check: end");

    return HA_EXIT_FAILURE;
  }

  if (new_value && opt_mts_dependency_replication) {
    my_error(ER_CANT_SKIP_LOCK_WHEN_DEPENDENCY_REPLICATION, MYF(0));
    rocksdb_rpc_log(18524,
                    "check_rocksdb_skip_locks_if_skip_unique_check: end");
    return HA_EXIT_FAILURE;
  }

  *static_cast<bool *>(save) = new_value;
  rocksdb_rpc_log(18529, "check_rocksdb_skip_locks_if_skip_unique_check: end");
  return HA_EXIT_SUCCESS;
}

int rocksdb_check_bulk_load(
    THD *const thd, struct st_mysql_sys_var *var MY_ATTRIBUTE((__unused__)),
    void *save, struct st_mysql_value *value) {
  rocksdb_rpc_log(18536, "rocksdb_check_bulk_load: start");

  my_bool new_value;
  if (mysql_value_to_bool(value, &new_value) != 0) {
    rocksdb_rpc_log(18541, "rocksdb_check_bulk_load: end");

    return 1;
  }

  rocksdb_rpc_log(18547, "rocksdb_check_bulk_load: get_tx_from_thd");

  Rdb_transaction *&tx = get_tx_from_thd(thd);
  if (tx != nullptr) {
    bool is_critical_error;
    const int rc = tx->finish_bulk_load(&is_critical_error);
    if (rc != 0 && is_critical_error) {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Error %d finalizing last SST file while "
          "setting bulk loading variable",
          rc);
      THDVAR(thd, bulk_load) = 0;
      return 1;
    }
  }

  rocksdb_rpc_log(18563, "rocksdb_check_bulk_load: end");

  *static_cast<bool *>(save) = new_value;
  return 0;
}

int rocksdb_check_bulk_load_allow_unsorted(
    THD *const thd, struct st_mysql_sys_var *var MY_ATTRIBUTE((__unused__)),
    void *save, struct st_mysql_value *value) {
  rocksdb_rpc_log(18572, "rocksdb_check_bulk_load_allow_unsorted: start");

  my_bool new_value;
  if (mysql_value_to_bool(value, &new_value) != 0) {
    rocksdb_rpc_log(18577, "rocksdb_check_bulk_load_allow_unsorted: end");

    return 1;
  }

  if (THDVAR(thd, bulk_load)) {
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), "SET",
             "Cannot change this setting while bulk load is enabled");
    rocksdb_rpc_log(18585, "rocksdb_check_bulk_load_allow_unsorted: end");

    return 1;
  }

  rocksdb_rpc_log(18589, "rocksdb_check_bulk_load_allow_unsorted: end");

  *static_cast<bool *>(save) = new_value;
  return 0;
}

static void rocksdb_set_max_background_jobs(THD *thd,
                                            struct st_mysql_sys_var *const var,
                                            void *const var_ptr,
                                            const void *const save) {
  rocksdb_rpc_log(18598, "rocksdb_set_max_background_jobs: start");

  DBUG_ASSERT(save != nullptr);
  DBUG_ASSERT(rocksdb_db_options != nullptr);
  DBUG_ASSERT(rocksdb_db_options->env != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const int new_val = *static_cast<const int *>(save);

  rocksdb_rpc_log(
      18609,
      "rocksdb_set_max_background_jobs: rocksdb_DBOptions__GetIntOptions");

  // ALTER
  // if (rocksdb_db_options->max_background_jobs != new_val) {
  if (rocksdb_DBOptions__GetIntOptions(rocksdb_db_options,
                                       "max_background_jobs") != new_val) {
    rocksdb_db_options->max_background_jobs = new_val;
    rocksdb_DBOptions__SetIntOptions(rocksdb_db_options, "max_background_jobs",
                                     new_val);
    // ALTER
    // rocksdb::Status s =
    //     rdb->SetDBOptions({{"max_background_jobs",
    //     std::to_string(new_val)}});
    rocksdb::Status s = rocksdb_TransactionDB__SetDBOptions(
        rdb, {{"max_background_jobs", std::to_string(new_val)}});

    if (!s.ok()) {
      /* NO_LINT_DEBUG */
      sql_print_warning(
          "MyRocks: failed to update max_background_jobs. "
          "Status code = %d, status = %s.",
          s.code(), s.ToString().c_str());
    }
  }
  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(18633, "rocksdb_set_max_background_jobs: end");
}

static void rocksdb_set_max_background_compactions(
    THD *thd, struct st_mysql_sys_var *const var, void *const var_ptr,
    const void *const save) {
  rocksdb_rpc_log(18640, "rocksdb_set_max_background_compactions: start");

  DBUG_ASSERT(save != nullptr);
  DBUG_ASSERT(rocksdb_db_options != nullptr);
  DBUG_ASSERT(rocksdb_db_options->env != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const int new_val = *static_cast<const int *>(save);

  // ALTER
  if (rocksdb_DBOptions__GetIntOptions(
          rocksdb_db_options, "max_background_compactions") != new_val) {
    // ALTER
    // rocksdb_db_options->max_background_compactions = new_val;
    rocksdb_DBOptions__SetIntOptions(rocksdb_db_options,
                                     "max_background_compactions", new_val);

    // ALTER
    // rocksdb::Status s = rdb->SetDBOptions(
    //     {{"max_background_compactions", std::to_string(new_val)}});
    rocksdb::Status s = rocksdb_TransactionDB__SetDBOptions(
        rdb, {{"max_background_compactions", std::to_string(new_val)}});

    if (!s.ok()) {
      /* NO_LINT_DEBUG */
      sql_print_warning(
          "MyRocks: failed to update max_background_compactions. "
          "Status code = %d, status = %s.",
          s.code(), s.ToString().c_str());
    }
  }

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(18674, "rocksdb_set_max_background_compactions: end");
}

/**
   rocksdb_set_max_bottom_pri_background_compactions_internal() changes
   the number of rocksdb background threads.
   Creating new threads may take up to a few seconds, so instead of
   calling the function at sys_var::update path where global mutex is held,
   doing at sys_var::check path so that other queries are not blocked.
   Same optimization is done for rocksdb_block_cache_size too.
*/
static int rocksdb_validate_max_bottom_pri_background_compactions(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *var_ptr, struct st_mysql_value *value) {
  rocksdb_rpc_log(
      18690, "rocksdb_validate_max_bottom_pri_background_compactions: start");

  DBUG_ASSERT(value != nullptr);

  long long new_value;

  /* value is NULL */
  if (value->val_int(value, &new_value)) {
    rocksdb_rpc_log(
        18700, "rocksdb_validate_max_bottom_pri_background_compactions: end");

    return HA_EXIT_FAILURE;
  }
  if (new_value < 0 ||
      new_value > ROCKSDB_MAX_BOTTOM_PRI_BACKGROUND_COMPACTIONS) {
    rocksdb_rpc_log(
        18707, "rocksdb_validate_max_bottom_pri_background_compactions: end");
    return HA_EXIT_FAILURE;
  }
  RDB_MUTEX_LOCK_CHECK(rdb_bottom_pri_background_compactions_resize_mutex);
  if (rocksdb_max_bottom_pri_background_compactions != new_value) {
    if (new_value == 0) {
      my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0), "SET",
               "max_bottom_pri_background_compactions can't be changed to 0 "
               "online.");
      RDB_MUTEX_UNLOCK_CHECK(
          rdb_bottom_pri_background_compactions_resize_mutex);
      rocksdb_rpc_log(
          18719, "rocksdb_validate_max_bottom_pri_background_compactions: end");
      return HA_EXIT_FAILURE;
    }
    rocksdb_set_max_bottom_pri_background_compactions_internal(new_value);
  }
  *static_cast<int64_t *>(var_ptr) = static_cast<int64_t>(new_value);
  RDB_MUTEX_UNLOCK_CHECK(rdb_bottom_pri_background_compactions_resize_mutex);

  rocksdb_rpc_log(
      18728, "rocksdb_validate_max_bottom_pri_background_compactions: end");
  return HA_EXIT_SUCCESS;
}

static void rocksdb_set_bytes_per_sync(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  rocksdb_rpc_log(18737, "rocksdb_set_bytes_per_sync: start");
  DBUG_ASSERT(save != nullptr);
  DBUG_ASSERT(rocksdb_db_options != nullptr);
  DBUG_ASSERT(rocksdb_db_options->env != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const ulonglong new_val = *static_cast<const ulonglong *>(save);

  // ALTER
  // if (rocksdb_db_options->bytes_per_sync != new_val) {
  //   rocksdb_db_options->bytes_per_sync = new_val;
  if (rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
                                          "bytes_per_sync") != new_val) {
    rocksdb_DBOptions__SetUInt64Options(rocksdb_db_options, "bytes_per_sync",
                                        new_val);
    rocksdb::Status s = rocksdb_TransactionDB__SetDBOptions(
        rdb, {{"bytes_per_sync", std::to_string(new_val)}});

    if (!s.ok()) {
      /* NO_LINT_DEBUG */
      sql_print_warning(
          "MyRocks: failed to update max_background_jobs. "
          "Status code = %d, status = %s.",
          s.code(), s.ToString().c_str());
    }
  }

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(18765, "rocksdb_set_bytes_per_sync: end");
}

static void rocksdb_set_wal_bytes_per_sync(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *const var_ptr MY_ATTRIBUTE((__unused__)), const void *const save) {
  rocksdb_rpc_log(18772, "rocksdb_set_wal_bytes_per_sync: start");

  DBUG_ASSERT(save != nullptr);
  DBUG_ASSERT(rocksdb_db_options != nullptr);
  DBUG_ASSERT(rocksdb_db_options->env != nullptr);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  const ulonglong new_val = *static_cast<const ulonglong *>(save);

  // ALTER
  // if (rocksdb_db_options->wal_bytes_per_sync != new_val) {
  //   rocksdb_db_options->wal_bytes_per_sync = new_val;

  if (rocksdb_DBOptions__GetUInt64Options(rocksdb_db_options,
                                          "wal_bytes_per_sync") != new_val) {
    rocksdb_DBOptions__SetUInt64Options(rocksdb_db_options,
                                        "wal_bytes_per_sync", new_val);
    rocksdb::Status s = rocksdb_TransactionDB__SetDBOptions(
        rdb, {{"wal_bytes_per_sync", std::to_string(new_val)}});

    if (!s.ok()) {
      /* NO_LINT_DEBUG */
      sql_print_warning(
          "MyRocks: failed to update max_background_jobs. "
          "Status code = %d, status = %s.",
          s.code(), s.ToString().c_str());
    }
  }

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(18803, "rocksdb_set_wal_bytes_per_sync: end");
}

/*
  Validating and updating block cache size via sys_var::check path.
  SetCapacity may take seconds when reducing block cache, and
  sys_var::update holds LOCK_global_system_variables mutex, so
  updating block cache size is done at check path instead.
*/
static int rocksdb_validate_set_block_cache_size(
    THD *thd MY_ATTRIBUTE((__unused__)),
    struct st_mysql_sys_var *const var MY_ATTRIBUTE((__unused__)),
    void *var_ptr, struct st_mysql_value *value) {
  rocksdb_rpc_log(18818, "rocksdb_validate_set_block_cache_size: start");

  DBUG_ASSERT(value != nullptr);

  long long new_value;

  /* value is NULL */
  if (value->val_int(value, &new_value)) {
    return HA_EXIT_FAILURE;
  }

  if (new_value < RDB_MIN_BLOCK_CACHE_SIZE ||
      (uint64_t)new_value > (uint64_t)LLONG_MAX) {
    return HA_EXIT_FAILURE;
  }

  RDB_MUTEX_LOCK_CHECK(rdb_block_cache_resize_mutex);

  // ALTER
  // const rocksdb::BlockBasedTableOptions &table_options =
  //     rdb_get_table_options();

  rocksdb_rpc_log(
      18842, "rocksdb_validate_set_block_cache_size: rdb_get_table_options");

  rocksdb::BlockBasedTableOptions *table_options = rdb_get_table_options();

  // ALTER
  // if (rocksdb_block_cache_size != new_value && table_options.block_cache) {
  //   table_options.block_cache->SetCapacity(new_value);
  // }
  if (rocksdb_block_cache_size != new_value) {
    rocksdb_BlockBasedTableOptions__SetCapacity(table_options, new_value);
  }
  *static_cast<int64_t *>(var_ptr) = static_cast<int64_t>(new_value);
  RDB_MUTEX_UNLOCK_CHECK(rdb_block_cache_resize_mutex);
  rocksdb_rpc_log(18855, "rocksdb_validate_set_block_cache_size: end");

  return HA_EXIT_SUCCESS;
}

static int rocksdb_validate_update_cf_options(
    THD * /* unused */, struct st_mysql_sys_var * /*unused*/, void *save,
    struct st_mysql_value *value) {
  rocksdb_rpc_log(18862, "rocksdb_validate_update_cf_options: start");

  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;
  int length;
  length = sizeof(buff);
  str = value->val_str(value, buff, &length);

  *(const char **)save = nullptr;
  if (str == nullptr) {
    rocksdb_rpc_log(18872, "rocksdb_validate_update_cf_options: end");

    return HA_EXIT_SUCCESS;
  }

  Rdb_cf_options::Name_to_config_t option_map;

  // Basic sanity checking and parsing the options into a map. If this fails
  // then there's no point to proceed.
  if (!Rdb_cf_options::parse_cf_options(str, &option_map)) {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), "rocksdb_update_cf_options", str);
    rocksdb_rpc_log(18883, "rocksdb_validate_update_cf_options: end");

    return HA_EXIT_FAILURE;
  }

  // Loop through option_map and create missing column families
  for (Rdb_cf_options::Name_to_config_t::iterator it = option_map.begin();
       it != option_map.end(); ++it) {
    // If the CF is removed at this point, i.e., cf_manager.drop_cf() has
    // been called, it is OK to create a new CF.

    const auto &cf_name = it->first;
    {
      std::lock_guard<Rdb_dict_manager> dm_lock(dict_manager);
      auto cfh = cf_manager.get_or_create_cf(rdb, cf_name);

      if (!cfh) {
        rocksdb_rpc_log(18900, "rocksdb_validate_update_cf_options: end");

        return HA_EXIT_FAILURE;
      }

      if (cf_manager.create_cf_flags_if_needed(
              &dict_manager,
              /*ALTER cfh->GetID()*/ rocksdb_ColumnFamilyHandle__GetID(cfh),
              cf_name)) {
        rocksdb_rpc_log(18907, "rocksdb_validate_update_cf_options: end");

        return HA_EXIT_FAILURE;
      }
    }
  }

  // In some cases, str can point to buff in the stack.
  // This can cause invalid memory access after validation is finished.
  // To avoid this kind case, let's alway duplicate the str.
  *(const char **)save = my_strdup(str, MYF(0));

  rocksdb_rpc_log(18919, "rocksdb_validate_update_cf_options: end");

  return HA_EXIT_SUCCESS;
}

static void rocksdb_set_update_cf_options(
    THD *const /* unused */, struct st_mysql_sys_var *const /* unused */,
    void *const var_ptr, const void *const save) {
  rocksdb_rpc_log(18928, "rocksdb_set_update_cf_options: start");

  const char *const val = *static_cast<const char *const *>(save);

  RDB_MUTEX_LOCK_CHECK(rdb_sysvars_mutex);

  if (!val) {
    *reinterpret_cast<char **>(var_ptr) = nullptr;
    RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
    return;
  }

  DBUG_ASSERT(val != nullptr);

  // Reset the pointers regardless of how much success we had with updating
  // the CF options. This will results in consistent behavior and avoids
  // dealing with cases when only a subset of CF-s was successfully updated.
  *reinterpret_cast<const char **>(var_ptr) = val;

  // Do the real work of applying the changes.
  Rdb_cf_options::Name_to_config_t option_map;

  // This should never fail, because of rocksdb_validate_update_cf_options
  if (!Rdb_cf_options::parse_cf_options(val, &option_map)) {
    RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
    return;
  }

  // For each CF we have, see if we need to update any settings.
  for (const auto &cf_name : cf_manager.get_cf_names()) {
    DBUG_ASSERT(!cf_name.empty());

    // ALTER
    // std::shared_ptr<rocksdb::ColumnFamilyHandle> cfh =
    //     cf_manager.get_cf(cf_name);

    rocksdb_rpc_log(18964, "rocksdb_set_update_cf_options: cf_manager.get_c");

    rocksdb::ColumnFamilyHandle *cfh = cf_manager.get_cf(cf_name);

    if (!cfh) {
      // NO_LINT_DEBUG
      sql_print_information(
          "Skip updating options for cf %s because the cf has been dropped.",
          cf_name.c_str());
      continue;
    }

    const auto it = option_map.find(cf_name);
    std::string per_cf_options = (it != option_map.end()) ? it->second : "";

    if (!per_cf_options.empty()) {
      Rdb_cf_options::Name_to_config_t opt_map;
      rocksdb::Status s = rocksdb::StringToMap(per_cf_options, &opt_map);

      if (s != rocksdb::Status::OK()) {
        // NO_LINT_DEBUG
        sql_print_warning(
            "MyRocks: failed to convert the options for column "
            "family '%s' to a map. %s",
            cf_name.c_str(), s.ToString().c_str());
      } else {
        DBUG_ASSERT(rdb != nullptr);

        // Finally we can apply the options.
        // If cf_manager.drop_cf() has been called at this point, SetOptions()
        // will still succeed. The options data will only be cleared when
        // the CF handle object is destroyed.

        // ALTER
        // s = rdb->SetOptions(cfh.get(), opt_map);
        s = rocksdb_TransactionDB__SetOptions(rdb, cfh, opt_map);

        if (s != rocksdb::Status::OK()) {
          // NO_LINT_DEBUG
          sql_print_warning(
              "MyRocks: failed to apply the options for column "
              "family '%s'. %s",
              cf_name.c_str(), s.ToString().c_str());
        } else {
          // NO_LINT_DEBUG
          sql_print_information(
              "MyRocks: options for column family '%s' "
              "have been successfully updated.",
              cf_name.c_str());

          // Make sure that data is internally consistent as well and update
          // the CF options. This is necessary also to make sure that the CF
          // options will be correctly reflected in the relevant table:
          // ROCKSDB_CF_OPTIONS in INFORMATION_SCHEMA.

          // ALTER
          // rocksdb::ColumnFamilyOptions cf_options =
          // rdb->GetOptions(cfh.get()); std::string updated_options;

          // s = rocksdb::GetStringFromColumnFamilyOptions(&updated_options,
          //                                               cf_options);
          rocksdb_rpc_log(19027,
                          "rocksdb_set_update_cf_options: "
                          "rocksdb_GetStringFromColumnFamilyOptions");

          std::string updated_options;
          s = rocksdb_GetStringFromColumnFamilyOptions(rdb, cfh,
                                                       &updated_options);

          DBUG_ASSERT(s == rocksdb::Status::OK());
          DBUG_ASSERT(!updated_options.empty());

          cf_manager.update_options_map(cf_name, updated_options);
        }
      }
    }
  }

  // Our caller (`plugin_var_memalloc_global_update`) will call `my_free` to
  // free up resources used before.

  RDB_MUTEX_UNLOCK_CHECK(rdb_sysvars_mutex);
  rocksdb_rpc_log(19047, "rocksdb_set_update_cf_options: end");
}

void rdb_queue_save_stats_request() {
  rocksdb_rpc_log(19051, "request_save_stats: start");

  rdb_bg_thread.request_save_stats();
}

void ha_rocksdb::rpl_before_delete_rows() {
  rocksdb_rpc_log(19056, "rpl_before_delete_rows: start");

  DBUG_ENTER_FUNC();

  m_in_rpl_delete_rows = true;

  rocksdb_rpc_log(19062, "rpl_before_delete_rows: end");

  DBUG_VOID_RETURN;
}

void ha_rocksdb::rpl_after_delete_rows() {
  DBUG_ENTER_FUNC();

  m_in_rpl_delete_rows = false;

  DBUG_VOID_RETURN;
}

void ha_rocksdb::rpl_before_update_rows() {
  DBUG_ENTER_FUNC();

  m_in_rpl_update_rows = true;

  DBUG_VOID_RETURN;
}

void ha_rocksdb::rpl_after_update_rows() {
  DBUG_ENTER_FUNC();

  m_in_rpl_update_rows = false;

  DBUG_VOID_RETURN;
}

bool ha_rocksdb::is_read_free_rpl_table() const {
  return table->s && m_tbl_def->m_is_read_free_rpl_table;
}

/**
  @brief
  Read Free Replication can be used or not. Returning true means
  Read Free Replication can be used.
*/
bool ha_rocksdb::use_read_free_rpl() const {
  rocksdb_rpc_log(19100, "use_read_free_rpl: start");

  DBUG_ENTER_FUNC();

  if (!ha_thd()->rli_slave || table->triggers || !is_read_free_rpl_table()) {
    DBUG_RETURN(false);
  }

  switch (rocksdb_read_free_rpl) {
    case read_free_rpl_type::OFF:
      DBUG_RETURN(false);
    case read_free_rpl_type::PK_ONLY:
      DBUG_RETURN(!has_hidden_pk(table) && table->s->keys == 1);
    case read_free_rpl_type::PK_SK:
      DBUG_RETURN(!has_hidden_pk(table));
  }

  DBUG_ASSERT(false);
  DBUG_RETURN(false);
}

/**
  @brief
  Whether the table or last access partition has TTL column
  Only used in replication error checking
*/
bool ha_rocksdb::last_part_has_ttl_column() const {
  rocksdb_rpc_log(19127, "last_part_has_ttl_column: start");

  return m_tbl_def->has_ttl_col();
}

double ha_rocksdb::read_time(uint index, uint ranges, ha_rows rows) {
  rocksdb_rpc_log(19133, "read_time: start");

  DBUG_ENTER_FUNC();

  if (index != table->s->primary_key) {
    /* Non covering index range scan */
    rocksdb_rpc_log(19141, "read_time: end");

    DBUG_RETURN(handler::read_time(index, ranges, rows));
  }
  rocksdb_rpc_log(19145, "read_time: end");

  DBUG_RETURN((rows / 20.0) + 1);
}

void ha_rocksdb::print_error(int error, myf errflag) {
  if (error == HA_ERR_ROCKSDB_STATUS_BUSY) {
    error = HA_ERR_LOCK_DEADLOCK;
  }
  handler::print_error(error, errflag);
}

std::string rdb_corruption_marker_file_name() {
  std::string ret(rocksdb_datadir);
  ret.append("/ROCKSDB_CORRUPTED");
  return ret;
}

static void rocksdb_select_bypass_rejected_query_history_size_update(
    THD *const /* unused */, struct st_mysql_sys_var *const /* unused */,
    void *const var_ptr, const void *const save) {
  rocksdb_rpc_log(
      19165, "rocksdb_select_bypass_rejected_query_history_size_update: start");

  DBUG_ASSERT(rdb != nullptr);

  uint32_t val = *static_cast<uint32_t *>(var_ptr) =
      *static_cast<const uint32_t *>(save);

  const std::lock_guard<std::mutex> lock(
      myrocks_rpc::rejected_bypass_query_lock);
  if (myrocks_rpc::rejected_bypass_queries.size() > val) {
    myrocks_rpc::rejected_bypass_queries.resize(val);
  }
  rocksdb_rpc_log(
      19176, "rocksdb_select_bypass_rejected_query_history_size_update: end");
}

select_bypass_policy_type get_select_bypass_policy() {
  return static_cast<select_bypass_policy_type>(rocksdb_select_bypass_policy);
}

bool should_fail_unsupported_select_bypass() {
  return rocksdb_select_bypass_fail_unsupported;
}

bool should_log_rejected_select_bypass() {
  return rocksdb_select_bypass_log_rejected;
}

bool should_log_failed_select_bypass() {
  return rocksdb_select_bypass_log_failed;
}

bool should_allow_filters_select_bypass() {
  return rocksdb_select_bypass_allow_filters;
}

uint32_t get_select_bypass_rejected_query_history_size() {
  return rocksdb_select_bypass_rejected_query_history_size;
}

uint32_t get_select_bypass_debug_row_delay() {
  return rocksdb_select_bypass_debug_row_delay;
}

unsigned long long  // NOLINT(runtime/int)
get_select_bypass_multiget_min() {
  return rocksdb_select_bypass_multiget_min;
}

// ALTER
// const rocksdb::ReadOptions &rdb_tx_acquire_snapshot(Rdb_transaction *tx) {
//   tx->acquire_snapshot(true);
//   return tx->m_read_opts;
// }
rocksdb::ReadOptions *rdb_tx_acquire_snapshot(Rdb_transaction *tx) {
  rocksdb_rpc_log(19220, "rdb_tx_acquire_snapshot: start");

  tx->acquire_snapshot(true);
  rocksdb_rpc_log(19224, "rdb_tx_acquire_snapshot: end");

  return tx->m_read_opts;
}

rocksdb::Iterator *rdb_tx_get_iterator(
    Rdb_transaction *tx, rocksdb::ColumnFamilyHandle *const column_family,
    bool skip_bloom_filter, bool fill_cache,
    const rocksdb::Slice &lower_bound_slice,
    const rocksdb::Slice &upper_bound_slice, bool read_current,
    bool create_snapshot) {
  rocksdb_rpc_log(19234, "rdb_tx_get_iterator: start");

  return tx->get_iterator(column_family, skip_bloom_filter, fill_cache,
                          lower_bound_slice, upper_bound_slice, read_current,
                          create_snapshot);
}

bool rdb_tx_started(Rdb_transaction *tx) { return tx->is_tx_started(); }

rocksdb::Status rdb_tx_get(Rdb_transaction *tx,
                           rocksdb::ColumnFamilyHandle *const column_family,
                           const rocksdb::Slice &key,
                           rocksdb::PinnableSlice *&value) {
  rocksdb_rpc_log(19248, "rdb_tx_get: start");

  return tx->get(column_family, key, value);
}

void rdb_tx_multi_get(Rdb_transaction *tx,
                      rocksdb::ColumnFamilyHandle *const column_family,
                      const size_t num_keys, const rocksdb::Slice *keys,
                      rocksdb::PinnableSlice **values,
                      rocksdb::Status *statuses, const bool sorted_input) {
  rocksdb_rpc_log(19257, "rdb_tx_multi_get: start");
  tx->multi_get(column_family, num_keys, keys, values, statuses, sorted_input);
}

/****************************************************************************
 * Multi-Range-Read implementation based on RocksDB's MultiGet() call
 ***************************************************************************/

/*
  Check if MultiGet-MRR can be used to scan given list of ranges.

  @param  seq            List of ranges to scan
  @param  bufsz   INOUT  IN:  Size of the buffer available for use
                        OUT:  How much buffer space will be required
  @param  flags   INOUT  Properties of the scan to be done

  @return
     HA_POS_ERROR - The scan cannot be done at all
     Other value  - Number of expected output rows
*/

ha_rows ha_rocksdb::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                                void *seq_init_param,
                                                uint n_ranges, uint *bufsz,
                                                uint *flags,
                                                Cost_estimate *cost) {
  rocksdb_rpc_log(19285, "multi_range_read_info_const: start");

  ha_rows res;
  THD *thd = table->in_use;

  // We allow MultiGet-MRR only with these settings:
  //   optimizer_switch='mrr=on,mrr_cost_based=off'
  // mrr_cost_based is not supported
  bool mrr_enabled =
      thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MRR) &&
      !thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MRR_COST_BASED);
  uint def_bufsz = *bufsz;

  rocksdb_rpc_log(19298,
                  "multi_range_read_info_const: multi_range_read_info_const");

  res = handler::multi_range_read_info_const(keyno, seq, seq_init_param,
                                             n_ranges, &def_bufsz, flags, cost);

  if (res == HA_POS_ERROR) return res;  // Not possible to do the scan

  // Use the default MRR implementation if @@optimizer_switch value tells us
  // to, or if the query needs to do a locking read.
  if (!mrr_enabled || m_lock_rows != RDB_LOCK_NONE) return res;

  // How many buffer required to store all requried keys
  uint calculated_buf = mrr_get_length_per_rec() * res * 10 + 1;
  // How many buffer required to store maximum number of keys per MRR
  ssize_t elements_limit = THDVAR(thd, mrr_batch_size);
  uint mrr_batch_size_buff =
      mrr_get_length_per_rec() * elements_limit * 1.1 + 1;
  // The final bufsz value should be minimum among these three values:
  // 1. The passed in bufsz: contains maximum available buff size --- by
  // default, its value is specify by session variable read_rnd_buff_size,
  // 2. calculated_buf, specify buffer required to store all required keys
  // 3. mrr_batch_size_buffer, specify the maximun number of keys to fetch
  // during each MRR
  uint mrr_bufsz =
      std::min(std::min(*bufsz, calculated_buf), mrr_batch_size_buff);

  if (keyno == table->s->primary_key) {
    // We need all ranges to be single-point lookups using full PK values.
    // (Range scans, like "pk BETWEEN 10 and 20" or restrictions on PK prefix
    //  cannot be used)
    bool all_eq_ranges = true;
    KEY_MULTI_RANGE range;
    range_seq_t seq_it;
    seq_it = seq->init(seq_init_param, n_ranges, *flags);
    while (!seq->next(seq_it, &range)) {
      if (!(range.range_flag & UNIQUE_RANGE)) {
        all_eq_ranges = false;
        break;
      }
      if (table->in_use->killed) return HA_POS_ERROR;
    }

    if (all_eq_ranges) {
      // Indicate that we will use MultiGet MRR
      *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
      *flags |= HA_MRR_SUPPORT_SORTED;
      *bufsz = mrr_bufsz;
    }
  } else {
    // For scans on secondary keys, we use MultiGet when we read the PK values.
    // We only need PK values when the scan is non-index-only.
    if (!(*flags & HA_MRR_INDEX_ONLY)) {
      *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
      *flags |= HA_MRR_SUPPORT_SORTED;
      *flags |= HA_MRR_CONVERT_REF_TO_RANGE;
      *bufsz = mrr_bufsz;
    }
  }

  rocksdb_rpc_log(19356, "multi_range_read_info_const: end");

  return res;
}

ha_rows ha_rocksdb::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                          uint *bufsz, uint *flags,
                                          Cost_estimate *cost) {
  rocksdb_rpc_log(19362, "multi_range_read_info: start");
  ha_rows res;
  THD *thd = table->in_use;
  bool mrr_enabled =
      thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MRR) &&
      !thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MRR_COST_BASED);

  res =
      handler::multi_range_read_info(keyno, n_ranges, keys, bufsz, flags, cost);
  if (res || m_lock_rows != RDB_LOCK_NONE || !mrr_enabled) return res;

  if (keyno == table->s->primary_key && (*flags & HA_MRR_FULL_EXTENDED_KEYS)) {
    *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
    *flags |= HA_MRR_CONVERT_REF_TO_RANGE;
    *flags |= HA_MRR_SUPPORT_SORTED;
  }

  if (keyno != table->s->primary_key && !(*flags & HA_MRR_INDEX_ONLY)) {
    *flags &= ~HA_MRR_USE_DEFAULT_IMPL;
    *flags |= HA_MRR_CONVERT_REF_TO_RANGE;
    *flags &= ~HA_MRR_SUPPORT_SORTED;  // Non-sorted mode
  }

  rocksdb_rpc_log(19388, "multi_range_read_info: end");
  return 0;  // "0" means ok, despite the ha_rows return type.
}

//
// Source of Rowids for the MRR scan
//
class Mrr_rowid_source {
 public:
  // Get the next rowid, in the on-disk mem-comparable form. Also, get the
  // "range pointer" associated with the rowid (it is returned in *range_ptr).
  virtual int get_next_rowid(uchar *buf, int *size, char **range_ptr) = 0;
  virtual bool eof() = 0;
  virtual ~Mrr_rowid_source() {}
};

//
// Rowid source that produces rowids by enumerating a sequence of ranges
//
class Mrr_pk_scan_rowid_source : public Mrr_rowid_source {
  bool mrr_ranges_eof;  // true means we've got eof when enumerating the ranges.
  ha_rocksdb *self;

 public:
  Mrr_pk_scan_rowid_source(ha_rocksdb *self_arg, void *seq_init_param,
                           uint n_ranges, uint mode)
      : mrr_ranges_eof(false), self(self_arg) {
    self->mrr_iter = self->mrr_funcs.init(seq_init_param, n_ranges, mode);
  }

  int get_next_rowid(uchar *buf, int *size, char **range_ptr) override {
    if (mrr_ranges_eof) return HA_ERR_END_OF_FILE;  //  At eof already

    KEY_MULTI_RANGE range;
    if ((mrr_ranges_eof = self->mrr_funcs.next(self->mrr_iter, &range)))
      return HA_ERR_END_OF_FILE;  //  Got eof now

    key_part_map all_parts_map =
        (key_part_map(1) << self->m_pk_descr->get_key_parts()) - 1;
    DBUG_ASSERT(range.start_key.keypart_map == all_parts_map);
    DBUG_ASSERT(range.end_key.keypart_map == all_parts_map);
    DBUG_ASSERT(range.start_key.flag == HA_READ_KEY_EXACT);
    DBUG_ASSERT(range.end_key.flag == HA_READ_AFTER_KEY);

    *range_ptr = range.ptr;
    *size = self->m_pk_descr->pack_index_tuple(self->table, self->m_pack_buffer,
                                               buf, range.start_key.key,
                                               all_parts_map);
    return 0;
  }

  virtual bool eof() override { return mrr_ranges_eof; }
};

//
// Rowid source that produces rowids by doing an index-only scan on a
// secondary index and returning rowids from the index records
//
class Mrr_sec_key_rowid_source : public Mrr_rowid_source {
  ha_rocksdb *self;
  int err;

 public:
  Mrr_sec_key_rowid_source(ha_rocksdb *self_arg) : self(self_arg), err(0) {}

  int init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges, uint mode) {
    rocksdb_rpc_log(19453, "Mrr_sec_key_rowid_source.init: start");

    self->m_keyread_only = true;
    self->mrr_enabled_keyread = true;
    rocksdb_rpc_log(19459, "Mrr_sec_key_rowid_source.init: end");

    return self->handler::multi_range_read_init(seq, seq_init_param, n_ranges,
                                                mode, nullptr);
  }

  int get_next_rowid(uchar *buf, int *size, char **range_ptr) override {
    rocksdb_rpc_log(19464, "get_next_rowid: start");

    if (err) return err;

    while (!(err = self->handler::multi_range_read_next(range_ptr))) {
      if (self->mrr_funcs.skip_index_tuple &&
          self->mrr_funcs.skip_index_tuple(self->mrr_iter, *range_ptr)) {
        // BKA's variant of "Index Condition Pushdown" check failed
        continue;
      }

      if (self->mrr_funcs.skip_record &&
          self->mrr_funcs.skip_record(self->mrr_iter, *range_ptr,
                                      (uchar *)self->m_last_rowkey.ptr())) {
        continue;
      }

      memcpy(buf, self->m_last_rowkey.ptr(), self->m_last_rowkey.length());
      *size = self->m_last_rowkey.length();
      break;
    }
    rocksdb_rpc_log(19485, "get_next_rowid: END");

    return err;
  }
  virtual bool eof() override { return err != 0; }
};

// Initialize an MRR scan
int ha_rocksdb::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                      uint n_ranges, uint mode,
                                      HANDLER_BUFFER *buf) {
  rocksdb_rpc_log(19497, "multi_range_read_init: start");

  m_need_build_decoder = true;

  int res;

  if (!current_thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MRR) ||
      (mode & HA_MRR_USE_DEFAULT_IMPL) ||
      (buf->buffer_end - buf->buffer < mrr_get_length_per_rec()) ||
      (THDVAR(current_thd, mrr_batch_size) == 0)) {
    mrr_uses_default_impl = true;
    res = handler::multi_range_read_init(seq, seq_init_param, n_ranges, mode,
                                         buf);
    rocksdb_rpc_log(19512, "multi_range_read_init: end");

    return res;
  }

  // Ok, using a non-default MRR implementation, MultiGet-MRR

  mrr_uses_default_impl = false;
  mrr_n_elements = 0;  // nothing to cleanup, yet.
  mrr_enabled_keyread = false;
  mrr_rowid_reader = nullptr;

  mrr_funcs = *seq;
  mrr_buf = *buf;

  bool is_mrr_assoc = !MY_TEST(mode & HA_MRR_NO_ASSOCIATION);
  if (is_mrr_assoc)
    status_var_increment(
        table->in_use->status_var.ha_multi_range_read_init_count);

  if (active_index == table->s->primary_key) {
    // ICP is not supported for PK, so we don't expect that BKA's variant
    // of ICP would be used:
    DBUG_ASSERT(!mrr_funcs.skip_index_tuple);
    mrr_used_cpk = true;
    mrr_rowid_reader =
        new Mrr_pk_scan_rowid_source(this, seq_init_param, n_ranges, mode);
  } else {
    mrr_used_cpk = false;
    auto reader = new Mrr_sec_key_rowid_source(this);
    reader->init(seq, seq_init_param, n_ranges, mode);
    mrr_rowid_reader = reader;
  }

  res = mrr_fill_buffer();

  // note: here, we must NOT return HA_ERR_END_OF_FILE even if we know there
  // are no matches. We should return 0 here and return HA_ERR_END_OF_FILE
  // from the first multi_range_read_next() call.
  if (res == HA_ERR_END_OF_FILE) res = 0;

  rocksdb_rpc_log(19550, "multi_range_read_init: end");

  return res;
}

// Return the amount of buffer space that MRR scan requires for each record
// returned
uint ha_rocksdb::mrr_get_length_per_rec() {
  return sizeof(rocksdb::Slice) + sizeof(rocksdb::Status) +
         sizeof(rocksdb::PinnableSlice) +
         sizeof(char *) +  // this for KEY_MULTI_RANGE::ptr
         m_pk_descr->max_storage_fmt_length();
}

template <typename T>
void align_ptr(char **p) {
  if (((size_t)p) % alignof(T)) {
    *p += alignof(T) - ((size_t)p) % alignof(T);
  }
}

/*
  We've got a buffer in mrr_buf, and in order to call RocksDB's MultiGet, we
  need to use this space to construct several arrays of the same size N:

    rocksdb::Slice[N]         - lookup keys
    rocksdb::Status[N]        - return statuses
    rocksdb::PinnableSlice[N] - return rows (*)
    char*[N]                  - "ptr" value of KEY_MULTI_RANGE. This tells the
                                SQL layer which lookup key the returned record
                                matches with (**)
    {PK lookup value}[N]      - The rowid (Primary Key) to lookup. The
                                corresponding rocksdb::Slice object points to
                                this key.

  (*) The memory for rows is allocated somewhere inside RocksDB, there's no
      way to make it use the user-supplied buffer.
  (**) The engine could specify HA_MRR_NO_ASSOCIATION which would mean "we
      cannot tell which key the returned records match" we don't do this.

  The PK lookup value is in mem-comparable encoding. It may have variable
  length (this is the case when table's PRIMARY KEY has VARCHAR() columns).
  Currently, we optimize for fixed-size primary keys and consume
  m_pk_descr->max_storage_fmt_length() bytes for each lookup value. One can
  develop a solution for variable-length PKs but this is not a priority.

  Note that the buffer may be much larger than necessary. For range scans,
  @@rnd_buffer_size=256K is passed, even if there will be only a few lookup
  values.
*/

int ha_rocksdb::mrr_fill_buffer() {
  rocksdb_rpc_log(19602, "mrr_fill_buffer: start");

  mrr_free_rows();
  mrr_read_index = 0;

  // This should agree with the code in mrr_get_length_per_rec():
  ssize_t element_size = sizeof(rocksdb::Slice) + sizeof(rocksdb::Status) +
                         sizeof(rocksdb::PinnableSlice) +
                         sizeof(char *) +  // this for KEY_MULTI_RANGE::ptr
                         m_pk_descr->max_storage_fmt_length();

  // The buffer has space for this many elements:
  ssize_t n_elements = (mrr_buf.buffer_end - mrr_buf.buffer) / element_size;

  THD *thd = table->in_use;
  ssize_t elements_limit = THDVAR(thd, mrr_batch_size);
  n_elements = std::min(n_elements, elements_limit);

  if (n_elements < 1) {
    // We shouldn't get here as multi_range_read_init() has logic to fall back
    // to the default MRR implementation in this case.
    DBUG_ASSERT(0);
    rocksdb_rpc_log(19626, "mrr_fill_buffer: end");

    return HA_ERR_INTERNAL_ERROR;
  }

  char *buf = (char *)mrr_buf.buffer;

  align_ptr<rocksdb::Slice>(&buf);
  mrr_keys = (rocksdb::Slice *)buf;
  buf += sizeof(rocksdb::Slice) * n_elements;

  align_ptr<rocksdb::Status>(&buf);
  mrr_statuses = (rocksdb::Status *)buf;
  buf += sizeof(rocksdb::Status) * n_elements;

  // ALTER
  // align_ptr<rocksdb::PinnableSlice>(&buf);
  // mrr_values = (rocksdb::PinnableSlice *)buf;
  // buf += sizeof(rocksdb::PinnableSlice) * n_elements;
  align_ptr<rocksdb::PinnableSlice *>(&buf);
  mrr_values = (rocksdb::PinnableSlice **)buf;
  buf += sizeof(rocksdb::PinnableSlice *) * n_elements;

  align_ptr<char *>(&buf);
  mrr_range_ptrs = (char **)buf;
  buf += sizeof(char *) * n_elements;

  if (buf + m_pk_descr->max_storage_fmt_length() >=
      (char *)mrr_buf.buffer_end) {
    // a VERY unlikely scenario:  we were given a really small buffer,
    // (probably for just one rowid), and also we had to use some bytes for
    // alignment. As a result, there's no buffer space left to hold even one
    // rowid. Return an error immediately to avoid looping.
    DBUG_ASSERT(0);
    rocksdb_rpc_log(19658, "mrr_fill_buffer: end");

    return HA_ERR_INTERNAL_ERROR;  // error
  }

  ssize_t elem = 0;

  mrr_n_elements = elem;
  int key_size;
  char *range_ptr;
  int err;
  while (!(err = mrr_rowid_reader->get_next_rowid((uchar *)buf, &key_size,
                                                  &range_ptr))) {
    DEBUG_SYNC(table->in_use, "rocksdb.mrr_fill_buffer.loop");
    if (table->in_use->killed) return HA_ERR_QUERY_INTERRUPTED;

    new (&mrr_keys[elem]) rocksdb::Slice(buf, key_size);
    new (&mrr_statuses[elem]) rocksdb::Status;
    // ALTER
    // new (&mrr_values[elem]) rocksdb::PinnableSlice;
    new (&mrr_values[elem]) rocksdb::PinnableSlice *;
    mrr_range_ptrs[elem] = range_ptr;
    buf += key_size;

    elem++;
    mrr_n_elements = elem;

    if ((elem == n_elements) || (buf + m_pk_descr->max_storage_fmt_length() >=
                                 (char *)mrr_buf.buffer_end)) {
      // No more buffer space
      break;
    }
  }

  if (err && err != HA_ERR_END_OF_FILE) return err;

  if (mrr_n_elements == 0) return HA_ERR_END_OF_FILE;  // nothing to scan

  Rdb_transaction *const tx = get_or_create_tx(table->in_use);

  if (active_index == table->s->primary_key)
    stats.rows_requested += mrr_n_elements;

  tx->multi_get(m_pk_descr->get_cf(), mrr_n_elements, mrr_keys, mrr_values,
                mrr_statuses, active_index == table->s->primary_key);
  rocksdb_rpc_log(19705, "mrr_fill_buffer: end");

  return 0;
}

void ha_rocksdb::mrr_free() {
  rocksdb_rpc_log(19709, "mrr_free: start");

  // Free everything
  if (mrr_enabled_keyread) {
    m_keyread_only = false;
    mrr_enabled_keyread = false;
  }
  mrr_free_rows();
  delete mrr_rowid_reader;
  mrr_rowid_reader = nullptr;
  rocksdb_rpc_log(19719, "mrr_free: end");
}

void ha_rocksdb::mrr_free_rows() {
  rocksdb_rpc_log(19723, "mrr_free_rows: start");

  for (ssize_t i = 0; i < mrr_n_elements; i++) {
    // TODO: ALTER
    // mrr_values[i].~PinnableSlice();
    mrr_statuses[i].~Status();
    // no need to free mrr_keys
  }

  // There could be rows that MultiGet has returned but MyRocks hasn't
  // returned to the SQL layer (typically due to LIMIT clause)
  // Count them in in "rows_read" anyway. (This is only necessary when using
  // clustered PK. When using a secondary key, the index-only part of the scan
  // that collects the rowids has caused all counters to be incremented)
  if (mrr_used_cpk && mrr_n_elements) {
    stats.rows_read += mrr_n_elements - mrr_read_index;
  }

  mrr_n_elements = 0;
  // We can't rely on the data from HANDLER_BUFFER once the scan is over, so:
  mrr_values = nullptr;
}

int ha_rocksdb::multi_range_read_next(char **range_info) {
  check_build_decoder();

  if (mrr_uses_default_impl) {
    rocksdb_rpc_log(19751, "mrr_free_rows: end");

    return handler::multi_range_read_next(range_info);
  }

  Rdb_transaction *&tx = get_tx_from_thd(table->in_use);
  int rc;

  while (1) {
    while (1) {
      if (table->in_use->killed) return HA_ERR_QUERY_INTERRUPTED;

      if (mrr_read_index >= mrr_n_elements) {
        if (mrr_rowid_reader->eof() || !mrr_n_elements) {
          table->status = STATUS_NOT_FOUND;  // not sure if this is necessary?
          mrr_free_rows();
          rocksdb_rpc_log(19768, "mrr_free_rows: end");

          return HA_ERR_END_OF_FILE;
        }

        if ((rc = mrr_fill_buffer())) {
          if (rc == HA_ERR_END_OF_FILE) table->status = STATUS_NOT_FOUND;
          rocksdb_rpc_log(19774, "mrr_free_rows: end");

          return rc;
        }
      }
      // If we found a status that has a row, leave the loop
      if (mrr_statuses[mrr_read_index].ok()) break;

      // Skip the NotFound errors, return any other error to the SQL layer
      if (!mrr_statuses[mrr_read_index].IsNotFound())
        return rdb_error_to_mysql(mrr_statuses[mrr_read_index]);

      mrr_read_index++;
    }
    size_t cur_key = mrr_read_index++;

    const rocksdb::Slice &rowkey = mrr_keys[cur_key];

    if (mrr_funcs.skip_record &&
        mrr_funcs.skip_record(mrr_iter, mrr_range_ptrs[cur_key],
                              (uchar *)rowkey.data())) {
      rc = HA_ERR_END_OF_FILE;
      continue;
    }

    m_last_rowkey.copy((const char *)rowkey.data(), rowkey.size(),
                       &my_charset_bin);

    *range_info = mrr_range_ptrs[cur_key];

    // ALTER
    // m_retrieved_record.Reset();
    rocksdb_rpc_log(19806, "mrr_free_rows: rocksdb_PinnableSlice__Reset");

    rocksdb_PinnableSlice__Reset(m_retrieved_record);

    rocksdb_rpc_log(19809, "mrr_free_rows: rocksdb_PinnableSlice__PinSlice");

    // ALTER
    // m_retrieved_record.PinSlice(mrr_values[cur_key], &mrr_values[cur_key]);
    rocksdb_PinnableSlice__PinSlice(m_retrieved_record, mrr_values[cur_key],
                                    mrr_values[cur_key]);

    /* If we found the record, but it's expired, pretend we didn't find it.  */

    // ALTER
    // if (m_pk_descr->has_ttl() &&
    //     should_hide_ttl_rec(*m_pk_descr, m_retrieved_record,
    //                         tx->m_snapshot_timestamp)) {
    rocksdb_rpc_log(19822, "mrr_free_rows: rocksdb_PinnableSlice__PinSlice");

    if (m_pk_descr->has_ttl() &&
        should_hide_ttl_rec(*m_pk_descr,
                            rocksdb_PinnableSlice__Slice(m_retrieved_record),
                            tx->m_snapshot_timestamp)) {
      continue;
    }

    rc = convert_record_from_storage_format(&rowkey, table->record[0]);

    // When using a secondary index, the scan on secondary index increments the
    // count
    if (active_index == table->s->primary_key) {
      stats.rows_read++;
      update_row_stats(ROWS_READ);
    }
    break;
  }
  table->status = rc ? STATUS_NOT_FOUND : 0;
  rocksdb_rpc_log(19844, "mrr_free_rows: end");

  return rc;
}

}  // namespace myrocks_rpc

/*
  Register the storage engine plugin outside of myrocks namespace
  so that mysql_declare_plugin does not get confused when it does
  its name generation.
*/
rpc_logger l_27(19856, "rocksdb_rpc_storage_engine");

struct st_mysql_storage_engine rocksdb_rpc_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

mysql_declare_plugin(rocksdb_rpc_se){
    MYSQL_STORAGE_ENGINE_PLUGIN,             /* Plugin Type */
    &rocksdb_rpc_storage_engine,             /* Plugin Descriptor */
    "ROCKSDB_RPC",                           /* Plugin Name */
    "BobBai",                                /* Plugin Author */
    "RocksDB storage engine in rpc verison", /* Plugin Description */
    PLUGIN_LICENSE_GPL,                      /* Plugin Licence */
    myrocks_rpc::rocksdb_init_func,          /* Plugin Entry Point */
    myrocks_rpc::rocksdb_done_func,          /* Plugin Deinitializer */
    0x0001,                                  /* version number (0.1) */
    myrocks_rpc::rocksdb_status_vars,        /* status variables */
    myrocks_rpc::rocksdb_system_variables,   /* system variables */
    nullptr,                                 /* config options */
    0,                                       /* flags */
},
    myrocks_rpc::rdb_rpc_i_s_cfstats, myrocks_rpc::rdb_rpc_i_s_dbstats,
    myrocks_rpc::rdb_rpc_i_s_perf_context,
    myrocks_rpc::rdb_rpc_i_s_perf_context_global,
    myrocks_rpc::rdb_rpc_i_s_cfoptions, myrocks_rpc::rdb_rpc_i_s_compact_stats,
    myrocks_rpc::rdb_rpc_i_s_global_info, myrocks_rpc::rdb_rpc_i_s_ddl,
    myrocks_rpc::rdb_rpc_i_s_sst_props, myrocks_rpc::rdb_rpc_i_s_index_file_map,
    myrocks_rpc::rdb_rpc_i_s_lock_info, myrocks_rpc::rdb_rpc_i_s_trx_info,
    myrocks_rpc::rdb_rpc_i_s_deadlock_info,
    myrocks_rpc::rdb_rpc_i_s_bypass_rejected_query_history
    mysql_declare_plugin_end;