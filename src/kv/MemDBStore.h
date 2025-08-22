#pragma once

#include <map>
#include <mutex>

#include "kv/KVStore.h"

namespace hf3fs::kv {

class MemDBStore : public KVStore {
 public:
  MemDBStore(const Config &config)
      : config_(config) {}

  // get value corresponding to key.
  Result<std::string> get(std::string_view key) final {
    auto lock = std::unique_lock(mutex_);
    auto it = map_.find(std::string{key});
    if (it == map_.end()) {
      return makeError(StatusCode::kKVStoreNotFound);
    }
    return it->second;
  }

  // get the first key which is greater than input key.
  Result<Void> iterateKeysWithPrefix(std::string_view prefix,
                                     uint32_t limit,
                                     IterateFunc func,
                                     std::optional<std::string> *nextValidKey = nullptr) final {
    auto lock = std::unique_lock(mutex_);
    auto it = map_.lower_bound(std::string{prefix});
    for (auto i = 0u; i < limit && it != map_.end(); ++i, ++it) {
      if (it->first.starts_with(prefix)) {
        RETURN_ON_ERROR(func(it->first, it->second));
      } else {
        break;
      }
    }
    if (nextValidKey && it != map_.end() && it->first.starts_with(prefix)) {
      *nextValidKey = it->first;
    }
    return Void{};
  }

  // put a key-value pair.
  Result<Void> put(std::string_view key, std::string_view value, bool) final {
    auto lock = std::unique_lock(mutex_);
    map_[std::string{key}] = value;
    return Void{};
  }

  // remove a key-value pair.
  Result<Void> remove(std::string_view key) final {
    auto lock = std::unique_lock(mutex_);
    map_.erase(std::string{key});
    return Void{};
  }

  // batch operations.
  class MemBatchOperations : public BatchOperations {
   public:
    MemBatchOperations(MemDBStore &db)
        : db_(db) {}
    // put a key-value pair.
    void put(std::string_view key, std::string_view value) override {
      writeBatch_.emplace_back(std::string{key}, std::string{value});
    }
    // remove a key.
    void remove(std::string_view key) override { writeBatch_.emplace_back(std::string{key}, std::nullopt); }
    // clear a batch operations.
    void clear() override { writeBatch_.clear(); }
    // commit a batch of operations.
    Result<Void> commit() override {
      auto lock = std::unique_lock(db_.mutex_);
      for (auto &pair : writeBatch_) {
        if (pair.second.has_value()) {
          db_.map_[std::string{pair.first}] = pair.second.value();
        } else {
          db_.map_.erase(std::string{pair.first});
        }
      }
      return Void{};
    }
    // destroy self.
    void destroy() override { delete this; }

   private:
    MemDBStore &db_;
    std::vector<std::pair<std::string, std::optional<std::string>>> writeBatch_;
  };
  BatchOptionsPtr createBatchOps() override {
    return BatchOptionsPtr{std::make_unique<MemBatchOperations>(*this).release()};
  }

  // iterator.
  class MemIterator : public Iterator {
   public:
    MemIterator(MemDBStore &db)
        : lock_(db.mutex_),
          map_(db.map_),
          iterator_(map_.end()) {}
    void seek(std::string_view key) override { iterator_ = map_.lower_bound(std::string{key}); }
    void seekToFirst() override { iterator_ = map_.begin(); }
    void seekToLast() override { iterator_ = --map_.end(); }
    void next() override { ++iterator_; }
    Result<Void> status() const override { return Void{}; }
    bool valid() const override { return iterator_ != map_.end(); }
    std::string_view key() const override { return iterator_->first; }
    std::string_view value() const override { return iterator_->second; }
    void destroy() override { delete this; }

   private:
    std::unique_lock<std::mutex> lock_;
    std::map<std::string, std::string> &map_;
    std::map<std::string, std::string>::iterator iterator_;
  };
  IteratorPtr createIterator() override { return IteratorPtr{std::make_unique<MemIterator>(*this).release()}; }

 private:
  [[maybe_unused]] const Config &config_;
  std::mutex mutex_;
  std::map<std::string, std::string> map_;
};

}  // namespace hf3fs::kv
