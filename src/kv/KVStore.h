#pragma once

#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <string>
#include <string_view>

#include "common/serde/Serde.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Duration.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/Size.h"

namespace hf3fs::kv {

class KVStore {
 public:
  enum class Type { LevelDB, RocksDB, MemDB };
  class Config : public ConfigBase<Config> {
    CONFIG_ITEM(type, Type::LevelDB);
    CONFIG_ITEM(create_if_missing, false);
    CONFIG_HOT_UPDATED_ITEM(sync_when_write, true);

    // for leveldb.
    CONFIG_ITEM(leveldb_sst_file_size, 16_MB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(leveldb_write_buffer_size, 16_MB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(leveldb_block_cache_size, 8_GB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(leveldb_shared_block_cache, true);
    CONFIG_HOT_UPDATED_ITEM(leveldb_iterator_fill_cache, true);
    CONFIG_ITEM(integrate_leveldb_log, false);

    // for rocksdb
    CONFIG_ITEM(rocksdb_max_manifest_file_size, 64_MB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(rocksdb_stats_dump_period, 2_min);
    CONFIG_ITEM(rocksdb_enable_pipelined_write, false);
    CONFIG_ITEM(rocksdb_unordered_write, false);
    CONFIG_ITEM(rocksdb_avoid_flush_during_recovery, false);
    CONFIG_ITEM(rocksdb_avoid_flush_during_shutdown, false);
    CONFIG_ITEM(rocksdb_avoid_unnecessary_blocking_io, false);
    CONFIG_ITEM(rocksdb_lowest_used_cache_tier, rocksdb_internal::CacheTier::kNonVolatileBlockTier);
    CONFIG_ITEM(rocksdb_write_buffer_size, 16_MB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(rocksdb_compression, rocksdb_internal::CompressionType::kNoCompression);
    CONFIG_ITEM(rocksdb_level0_file_num_compaction_trigger, 4, ConfigCheckers::checkGE<int, 2>);
    CONFIG_ITEM(rocksdb_enable_prefix_transform, true);
    CONFIG_ITEM(rocksdb_enable_bloom_filter, true);
    CONFIG_ITEM(rocksdb_bloom_filter_bits_per_key, 10);
    CONFIG_ITEM(rocksdb_num_levels, 7, ConfigCheckers::checkGE<int, 3>);
    CONFIG_ITEM(rocksdb_target_file_size_base, 64_MB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(rocksdb_target_file_size_multiplier, 1, ConfigCheckers::checkPositive);
    CONFIG_ITEM(rocksdb_block_cache_size, 8_GB, ConfigCheckers::checkGE<size_t, 4_MB>);
    CONFIG_ITEM(rocksdb_shared_block_cache, true);
    CONFIG_ITEM(rocksdb_block_size, 4_KB, ConfigCheckers::checkGE<size_t, 4_KB>);
    CONFIG_ITEM(rocksdb_prepopulate_block_cache,
                rocksdb_internal::BlockBasedTableOptions::PrepopulateBlockCache::kDisable);
    CONFIG_ITEM(rocksdb_threads_num, 8u, ConfigCheckers::checkPositive);
    CONFIG_ITEM(rocksdb_wal_recovery_mode, rocksdb_internal::WALRecoveryMode::kTolerateCorruptedTailRecords);
    CONFIG_ITEM(rocksdb_keep_log_file_num, 10u);
    CONFIG_HOT_UPDATED_ITEM(rocksdb_readahead_size, 2_MB);
  };

  struct Options {
    SERDE_STRUCT_FIELD(type, Type::RocksDB);
    SERDE_STRUCT_FIELD(path, Path{});
    SERDE_STRUCT_FIELD(createIfMissing, false);
  };

  virtual ~KVStore() = default;

  // get value corresponding to key.
  virtual Result<std::string> get(std::string_view key) = 0;

  // get the first key which is greater than input key.
  using IterateFunc = std::function<Result<Void>(std::string_view, std::string_view)>;
  virtual Result<Void> iterateKeysWithPrefix(std::string_view prefix,
                                             uint32_t limit,
                                             IterateFunc func,
                                             std::optional<std::string> *nextValidKey = nullptr) = 0;

  // put a key-value pair.
  virtual Result<Void> put(std::string_view key, std::string_view value, bool sync = false) = 0;

  // remove a key-value pair.
  virtual Result<Void> remove(std::string_view key) = 0;

  // batch operations.
  class BatchOperations {
   public:
    virtual ~BatchOperations() = default;
    // put a key-value pair.
    virtual void put(std::string_view key, std::string_view value) = 0;
    // remove a key.
    virtual void remove(std::string_view key) = 0;
    // clear a batch operations.
    virtual void clear() = 0;
    // commit a batch of operations.
    virtual Result<Void> commit() = 0;
    // destroy self.
    virtual void destroy() = 0;
    // deleter for std::unique_ptr.
    struct Deleter {
      void operator()(BatchOperations *b) { b->destroy(); }
    };
  };
  using BatchOptionsPtr = std::unique_ptr<BatchOperations, BatchOperations::Deleter>;
  virtual BatchOptionsPtr createBatchOps() = 0;

  // iterator.
  class Iterator {
   public:
    virtual ~Iterator() = default;
    virtual void seek(std::string_view key) = 0;
    virtual void seekToFirst() = 0;
    virtual void seekToLast() = 0;
    virtual void next() = 0;
    virtual Result<Void> status() const = 0;
    virtual bool valid() const = 0;
    virtual std::string_view key() const = 0;
    virtual std::string_view value() const = 0;
    virtual void destroy() = 0;
    struct Deleter {
      void operator()(Iterator *it) { it->destroy(); }
    };
  };
  using IteratorPtr = std::unique_ptr<Iterator, Iterator::Deleter>;
  virtual IteratorPtr createIterator() = 0;

  // create a KVStore instance.
  static std::unique_ptr<KVStore> create(const Config &config, const Options &options);
};

}  // namespace hf3fs::kv
