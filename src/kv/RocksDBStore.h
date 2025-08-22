#pragma once

#include <folly/ThreadLocal.h>
#include <rocksdb/db.h>
#include <rocksdb/write_batch.h>

#include "kv/KVStore.h"

namespace hf3fs::kv {

class RocksDBStore : public KVStore {
 public:
  RocksDBStore(const Config &config)
      : config_(config) {}

  // get value corresponding to key.
  Result<std::string> get(std::string_view key) final;

  // get the first key which is greater than input key.
  Result<Void> iterateKeysWithPrefix(std::string_view prefix,
                                     uint32_t limit,
                                     IterateFunc func,
                                     std::optional<std::string> *nextValidKey = nullptr) final;

  // put a key-value pair.
  Result<Void> put(std::string_view key, std::string_view value, bool sync = false) final;

  // remove a key-value pair.
  Result<Void> remove(std::string_view key) final;

  // batch operations.
  class RocksDBBatchOperations : public BatchOperations {
   public:
    RocksDBBatchOperations(RocksDBStore &db)
        : db_(db) {}
    // put a key-value pair.
    void put(std::string_view key, std::string_view value) override;
    // remove a key.
    void remove(std::string_view key) override;
    // clear a batch operations.
    void clear() override { writeBatch_.Clear(); }
    // commit a batch of operations.
    Result<Void> commit() override;
    // destroy self.
    void destroy() override;

   private:
    RocksDBStore &db_;
    rocksdb_internal::WriteBatch writeBatch_;
  };
  BatchOptionsPtr createBatchOps() override;

  // iterator.
  class RocksDBIterator : public Iterator {
   public:
    RocksDBIterator(std::unique_ptr<rocksdb_internal::Iterator> it)
        : iterator_(std::move(it)) {}
    void seek(std::string_view key) override;
    void seekToFirst() override;
    void seekToLast() override;
    void next() override;
    Result<Void> status() const override;
    bool valid() const override;
    std::string_view key() const override;
    std::string_view value() const override;
    void destroy() override;

   private:
    std::unique_ptr<rocksdb_internal::Iterator> iterator_;
  };
  IteratorPtr createIterator() override;

  // create a RocksDB instance.
  static std::unique_ptr<RocksDBStore> create(const Config &config, const Options &options);

 protected:
  // initialize rocksdb.
  Result<Void> init(const Options &optionsIn);

 private:
  const Config &config_;
  std::unique_ptr<rocksdb_internal::DB> db_;
};

}  // namespace hf3fs::kv
