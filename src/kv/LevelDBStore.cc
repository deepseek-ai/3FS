#include "kv/LevelDBStore.h"

#include <folly/logging/xlog.h>
#include <utility>
#include <variant>

#include "common/utils/ObjectPool.h"
#include "common/utils/Size.h"
#include "leveldb/cache.h"
#include "leveldb/iterator.h"
#include "leveldb/options.h"
#include "leveldb/slice.h"
#include "leveldb/status.h"
#include "leveldb/write_batch.h"

namespace hf3fs::kv {
namespace {

inline leveldb::Slice slice(std::string_view v) { return leveldb::Slice{v.data(), v.length()}; }
inline std::string_view view(leveldb::Slice s) { return std::string_view{s.data(), s.size()}; }

using LevelDBBatchOperationsPool = ObjectPool<LevelDBStore::LevelDBBatchOperations>;
using LevelDBIteratorPool = ObjectPool<LevelDBStore::LevelDBIterator>;

leveldb::Cache *sharedBlockCache(size_t capacity) {
  static std::unique_ptr<leveldb::Cache> cache{leveldb::NewLRUCache(capacity)};
  return cache.get();
}

}  // namespace

// get value corresponding to key.
Result<std::string> LevelDBStore::get(std::string_view key) {
  leveldb::ReadOptions options;
  std::string value;
  auto status = db_->Get(options, slice(key), &value);
  if (UNLIKELY(!status.ok())) {
    if (status.IsNotFound()) {
      return makeError(StatusCode::kKVStoreNotFound);
    }
    auto msg = fmt::format("LevelDB get error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreGetError, std::move(msg));
  }
  return Result<std::string>(std::move(value));
}

Result<Void> LevelDBStore::iterateKeysWithPrefix(std::string_view prefix,
                                                 uint32_t limit,
                                                 IterateFunc func,
                                                 std::optional<std::string> *nextValidKey /* = nullptr */) {
  leveldb::ReadOptions options;
  options.fill_cache = config_.leveldb_iterator_fill_cache();
  std::unique_ptr<leveldb::Iterator> iterator{db_->NewIterator(options)};
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
    auto msg = fmt::format("LevelDB iterate error: {}", iterator->status().ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreGetError, std::move(msg));
  }
  return Void{};
}

// put a key-value pair.
Result<Void> LevelDBStore::put(std::string_view key, std::string_view value, bool sync /* = false */) {
  leveldb::WriteOptions options;
  options.sync = config_.sync_when_write() || sync;
  auto status = db_->Put(options, slice(key), slice(value));
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("LevelDB put error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreSetError, std::move(msg));
  }
  return Void{};
}

// remove a key-value pair.
Result<Void> LevelDBStore::remove(std::string_view key) {
  leveldb::WriteOptions options;
  options.sync = config_.sync_when_write();
  auto status = db_->Delete(options, slice(key));
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("LevelDB delete error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreSetError, std::move(msg));
  }
  return Void{};
}

// batch operations.
void LevelDBStore::LevelDBBatchOperations::put(std::string_view key, std::string_view value) {
  writeBatch_.Put(slice(key), slice(value));
}

void LevelDBStore::LevelDBBatchOperations::remove(std::string_view key) { writeBatch_.Delete(slice(key)); }

Result<Void> LevelDBStore::LevelDBBatchOperations::commit() {
  if (writeBatch_.ApproximateSize() == 0) {
    return Void{};
  }
  leveldb::WriteOptions options;
  options.sync = db_.config_.sync_when_write();
  auto status = db_.db_->Write(options, &writeBatch_);
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("LevelDB write error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreSetError, std::move(msg));
  }
  return Void{};
}

void LevelDBStore::LevelDBBatchOperations::destroy() { LevelDBBatchOperationsPool::Ptr{this}; }

KVStore::BatchOptionsPtr LevelDBStore::createBatchOps() {
  return BatchOptionsPtr{LevelDBBatchOperationsPool::get(*this).release()};
}

void LevelDBStore::LevelDBIterator::seek(std::string_view key) { iterator_->Seek(slice(key)); }
void LevelDBStore::LevelDBIterator::seekToFirst() { iterator_->SeekToFirst(); }
void LevelDBStore::LevelDBIterator::seekToLast() { iterator_->SeekToLast(); }
void LevelDBStore::LevelDBIterator::next() { iterator_->Next(); }
Result<Void> LevelDBStore::LevelDBIterator::status() const {
  if (UNLIKELY(!iterator_->status().ok())) {
    auto msg = fmt::format("LevelDB iterate error: {}", iterator_->status().ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreGetError, std::move(msg));
  }
  return Void{};
}
bool LevelDBStore::LevelDBIterator::valid() const { return iterator_->Valid(); }
std::string_view LevelDBStore::LevelDBIterator::key() const { return view(iterator_->key()); }
std::string_view LevelDBStore::LevelDBIterator::value() const { return view(iterator_->value()); }
void LevelDBStore::LevelDBIterator::destroy() { LevelDBIteratorPool::Ptr{this}; }
KVStore::IteratorPtr LevelDBStore::createIterator() {
  leveldb::ReadOptions options;
  options.fill_cache = config_.leveldb_iterator_fill_cache();
  std::unique_ptr<leveldb::Iterator> it(db_->NewIterator(options));
  if (UNLIKELY(it == nullptr)) {
    return nullptr;
  }
  return IteratorPtr{LevelDBIteratorPool::get(std::move(it)).release()};
}

// initialize leveldb.
Result<Void> LevelDBStore::init(const Options &optionsIn) {
  leveldb::Options options;
  options.create_if_missing = optionsIn.createIfMissing;
  options.max_file_size = config_.leveldb_sst_file_size();
  options.write_buffer_size = config_.leveldb_write_buffer_size();
  if (config_.leveldb_shared_block_cache()) {
    options.block_cache = sharedBlockCache(config_.leveldb_block_cache_size());
  } else {
    cache_.reset(options.block_cache = leveldb::NewLRUCache(config_.leveldb_block_cache_size()));
  }
  if (config_.integrate_leveldb_log()) {
    logger_ = std::make_unique<LevelDBLogger>();
    options.info_log = logger_.get();
  }
  leveldb::DB *db = nullptr;
  auto status = leveldb::DB::Open(options, optionsIn.path.string(), &db);
  if (UNLIKELY(!status.ok())) {
    auto msg = fmt::format("LevelDB init error: {}", status.ToString());
    XLOG(ERR, msg);
    return makeError(StatusCode::kKVStoreOpenFailed, std::move(msg));
  }
  db_.reset(db);
  return Void{};
}

// create a leveldb chunk meta store object.
std::unique_ptr<LevelDBStore> LevelDBStore::create(const Config &config, const Options &options) {
  auto store = std::make_unique<LevelDBStore>(config);
  auto result = store->init(options);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "create leveldb at {} failed: {}, config: {}", options, result.error(), config.toString());
    return nullptr;
  }
  return store;
}

}  // namespace hf3fs::kv
