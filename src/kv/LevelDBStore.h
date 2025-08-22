#pragma once

#include <folly/ThreadLocal.h>
#include <leveldb/cache.h>
#include <leveldb/db.h>
#include <leveldb/write_batch.h>

#include "LevelDBLogger.h"
#include "kv/KVStore.h"

namespace hf3fs::kv {

class LevelDBStore : public KVStore {
 public:
  LevelDBStore(const Config &config)
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
  class LevelDBBatchOperations : public BatchOperations {
   public:
    LevelDBBatchOperations(LevelDBStore &db)
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
    LevelDBStore &db_;
    leveldb::WriteBatch writeBatch_;
  };
  BatchOptionsPtr createBatchOps() override;

  // iterator.
  class LevelDBIterator : public Iterator {
   public:
    LevelDBIterator(std::unique_ptr<leveldb::Iterator> it)
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
    std::unique_ptr<leveldb::Iterator> iterator_;
  };
  IteratorPtr createIterator() override;

  // create a LevelDB instance.
  static std::unique_ptr<LevelDBStore> create(const Config &config, const Options &options);

 protected:
  // initialize leveldb.
  Result<Void> init(const Options &optionsIn);

 private:
  const Config &config_;
  std::unique_ptr<leveldb::DB> db_;
  std::unique_ptr<leveldb::Cache> cache_;
  std::unique_ptr<LevelDBLogger> logger_;
};

}  // namespace hf3fs::kv
