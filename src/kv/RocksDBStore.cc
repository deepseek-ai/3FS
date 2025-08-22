#include "kv/RocksDBStore.h"

#include <folly/logging/xlog.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>
#include <rocksdb/status.h>
#include <rocksdb/write_batch.h>
#include <variant>

#include "common/utils/ObjectPool.h"
#include "common/utils/Size.h"

namespace hf3fs::kv {
namespace {

inline rocksdb_internal::Slice slice(std::string_view v) { return rocksdb_internal::Slice{v.data(), v.length()}; }
inline std::string_view view(rocksdb_internal::Slice s) { return std::string_view{s.data(), s.size()}; }

using RocksDBBatchOperationsPool = ObjectPool<RocksDBStore::RocksDBBatchOperations>;
using RocksDBIteratorPool = ObjectPool<RocksDBStore::RocksDBIterator>;

std::shared_ptr<rocksdb_internal::Cache> sharedBlockCache(size_t capacity) {
  static std::shared_ptr<rocksdb_internal::Cache> cache{rocksdb_internal::NewLRUCache(capacity)};
  return cache;
}

constexpr size_t rocksdbPrefixLen = 12;

}  // namespace

// get value corresponding to key.
Result<std::string> RocksDBStore::get(std::string_view key) {
  rocksdb_internal::ReadOptions options;
  std::string value;
  auto status = db_->Get(options, slice(key), &value);
  if (UNLIKELY(!status.ok())) {
    if (status.IsNotFound()) {
      return makeError(StatusCode::kKVStoreNotFound);
    }
    auto msg = fmt::format("RocksDB get error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreGetError, std::move(msg));
  }
  return Result<std::string>(std::move(value));
}

Result<Void> RocksDBStore::iterateKeysWithPrefix(std::string_view prefix,
                                                 uint32_t limit,
                                                 IterateFunc func,
                                                 std::optional<std::string> *nextValidKey /* = nullptr */) {
  rocksdb_internal::ReadOptions options;
  options.readahead_size = config_.rocksdb_readahead_size();
  options.prefix_same_as_start = config_.rocksdb_enable_prefix_transform() && prefix.size() >= rocksdbPrefixLen;
  std::unique_ptr<rocksdb_internal::Iterator> iterator{db_->NewIterator(options)};
  iterator->Seek(slice(prefix));
  bool prefixBreak = false;
  for (uint32_t i = 0; iterator->Valid() && i < limit; ++i, iterator->Next()) {
    auto key = iterator->key();
    if (key.starts_with(slice(prefix))) {
      RETURN_ON_ERROR(func(view(key), view(iterator->value())));
    } else {
      prefixBreak = true;
      break;
    }
  }
  if (nextValidKey && !prefixBreak && iterator->Valid() && iterator->key().starts_with(slice(prefix))) {
    *nextValidKey = view(iterator->key());
  }
  if (UNLIKELY(!iterator->status().ok())) {
    auto msg = fmt::format("RocksDB iterate error: {}", iterator->status().ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreGetError, std::move(msg));
  }
  return Void{};
}

// put a key-value pair.
Result<Void> RocksDBStore::put(std::string_view key, std::string_view value, bool sync /* = false */) {
  rocksdb_internal::WriteOptions options;
  options.sync = config_.sync_when_write() || sync;
  auto status = db_->Put(options, slice(key), slice(value));
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("RocksDB put error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreSetError, std::move(msg));
  }
  return Void{};
}

// remove a key-value pair.
Result<Void> RocksDBStore::remove(std::string_view key) {
  rocksdb_internal::WriteOptions options;
  options.sync = config_.sync_when_write();
  auto status = db_->Delete(options, slice(key));
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("RocksDB delete error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreSetError, std::move(msg));
  }
  return Void{};
}

// batch operations.
void RocksDBStore::RocksDBBatchOperations::put(std::string_view key, std::string_view value) {
  writeBatch_.Put(slice(key), slice(value));
}

void RocksDBStore::RocksDBBatchOperations::remove(std::string_view key) { writeBatch_.Delete(slice(key)); }

Result<Void> RocksDBStore::RocksDBBatchOperations::commit() {
  rocksdb_internal::WriteOptions options;
  options.sync = db_.config_.sync_when_write();
  auto status = db_.db_->Write(options, &writeBatch_);
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("RocksDB write error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreSetError, std::move(msg));
  }
  return Void{};
}

void RocksDBStore::RocksDBBatchOperations::destroy() { RocksDBBatchOperationsPool::Ptr{this}; }

KVStore::BatchOptionsPtr RocksDBStore::createBatchOps() {
  return BatchOptionsPtr{RocksDBBatchOperationsPool::get(*this).release()};
}

void RocksDBStore::RocksDBIterator::seek(std::string_view key) { iterator_->Seek(slice(key)); }
void RocksDBStore::RocksDBIterator::seekToFirst() { iterator_->SeekToFirst(); }
void RocksDBStore::RocksDBIterator::seekToLast() { iterator_->SeekToLast(); }
void RocksDBStore::RocksDBIterator::next() { iterator_->Next(); }
Result<Void> RocksDBStore::RocksDBIterator::status() const {
  if (UNLIKELY(!iterator_->status().ok())) {
    auto msg = fmt::format("RocksDB iterate error: {}", iterator_->status().ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreGetError, std::move(msg));
  }
  return Void{};
}
bool RocksDBStore::RocksDBIterator::valid() const { return iterator_->Valid(); }
std::string_view RocksDBStore::RocksDBIterator::key() const { return view(iterator_->key()); }
std::string_view RocksDBStore::RocksDBIterator::value() const { return view(iterator_->value()); }
void RocksDBStore::RocksDBIterator::destroy() { RocksDBIteratorPool::Ptr{this}; }
KVStore::IteratorPtr RocksDBStore::createIterator() {
  rocksdb_internal::ReadOptions options;
  options.readahead_size = config_.rocksdb_readahead_size();
  std::unique_ptr<rocksdb_internal::Iterator> it(db_->NewIterator(options));
  if (UNLIKELY(it == nullptr)) {
    return nullptr;
  }
  return IteratorPtr{RocksDBIteratorPool::get(std::move(it)).release()};
}

// initialize rocksdb.
Result<Void> RocksDBStore::init(const Options &optionsIn) {
  rocksdb_internal::Options options;
  rocksdb_internal::BlockBasedTableOptions table_options;

  options.create_if_missing = optionsIn.createIfMissing;
  options.max_manifest_file_size = config_.rocksdb_max_manifest_file_size();
  options.stats_dump_period_sec =
      std::chrono::duration_cast<std::chrono::seconds>(config_.rocksdb_stats_dump_period()).count();
  options.enable_pipelined_write = config_.rocksdb_enable_pipelined_write();
  options.unordered_write = config_.rocksdb_unordered_write();
  options.avoid_flush_during_recovery = config_.rocksdb_avoid_flush_during_recovery();
  options.avoid_flush_during_shutdown = config_.rocksdb_avoid_flush_during_shutdown();
  options.avoid_unnecessary_blocking_io = config_.rocksdb_avoid_unnecessary_blocking_io();
  options.lowest_used_cache_tier = config_.rocksdb_lowest_used_cache_tier();
  options.write_buffer_size = config_.rocksdb_write_buffer_size();
  options.compression = config_.rocksdb_compression();
  options.level0_file_num_compaction_trigger = config_.rocksdb_level0_file_num_compaction_trigger();
  if (config_.rocksdb_enable_prefix_transform()) {
    options.prefix_extractor.reset(rocksdb_internal::NewFixedPrefixTransform(rocksdbPrefixLen));
  }
  if (config_.rocksdb_enable_bloom_filter()) {
    table_options.filter_policy.reset(
        rocksdb_internal::NewBloomFilterPolicy(config_.rocksdb_bloom_filter_bits_per_key()));
  }
  options.num_levels = config_.rocksdb_num_levels();
  options.target_file_size_base = config_.rocksdb_target_file_size_base();
  options.target_file_size_multiplier = config_.rocksdb_target_file_size_multiplier();
  if (config_.rocksdb_shared_block_cache()) {
    table_options.block_cache = sharedBlockCache(config_.rocksdb_block_cache_size());
  } else {
    table_options.block_cache = rocksdb_internal::NewLRUCache(config_.rocksdb_block_cache_size());
  }
  table_options.cache_index_and_filter_blocks = true;
  table_options.index_type = rocksdb_internal::BlockBasedTableOptions::IndexType::kTwoLevelIndexSearch;
  table_options.block_size = config_.rocksdb_block_size();
  table_options.prepopulate_block_cache = config_.rocksdb_prepopulate_block_cache();

  options.table_factory.reset(rocksdb_internal::NewBlockBasedTableFactory(table_options));
  options.IncreaseParallelism(config_.rocksdb_threads_num());
  options.wal_recovery_mode = config_.rocksdb_wal_recovery_mode();
  options.keep_log_file_num = config_.rocksdb_keep_log_file_num();

  rocksdb_internal::DB *db = nullptr;
  auto status = rocksdb_internal::DB::Open(options, optionsIn.path.string(), &db);
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("RocksDB init error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreOpenFailed, std::move(msg));
  }
  db_.reset(db);
  return Void{};
}

// create a rocksdb chunk meta store object.
std::unique_ptr<RocksDBStore> RocksDBStore::create(const Config &config, const Options &options) {
  auto store = std::make_unique<RocksDBStore>(config);
  auto result = store->init(options);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "create rocksdb at {} failed: {}, config: {}", options, result.error(), config.toString());
    return nullptr;
  }
  return store;
}

}  // namespace hf3fs::kv
