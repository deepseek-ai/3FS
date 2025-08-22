#pragma once

#include <folly/Likely.h>
#include <memory>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

#include "common/utils/Size.h"

namespace hf3fs {

template <class T, size_t NumTLSCachedItem = 64, size_t NumGlobalCachedItem = 64 * 64>
class ObjectPool {
  using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;
  using Batch = std::vector<std::unique_ptr<Storage>>;

  struct Deleter {
    constexpr Deleter() = default;
    void operator()(T *item) {
      item->~T();
      tls().put(std::unique_ptr<Storage>(reinterpret_cast<Storage *>(item)));
    }
  };

 public:
  using Ptr = std::unique_ptr<T, Deleter>;
  static Ptr get() { return Ptr(new (tls().get().release()) T); }

  template <class... Args>
  static Ptr get(Args &&...args) {
    return Ptr(new (tls().get().release()) T(std::forward<Args>(args)...));
  }

 protected:
  class TLS {
   public:
    TLS(ObjectPool &parent)
        : parent_(parent) {}

    // get a object from thread local cache or global cache.
    std::unique_ptr<Storage> get() {
      // pop from second batch.
      if (!second_.empty()) {
        auto item = std::move(second_.back());
        second_.pop_back();
        return item;
      }

      // allocate a batch when local and global cache is empty.
      if (first_.empty() && !parent_.getBatch(first_)) {
        first_.resize(kThreadLocalMaxNum);
        for (auto &item : first_) {
          item.reset(new Storage);  // DO NOT REPLACE IT WITH std::make_unique<Storage>.
                                    // std::make_unique_for_overwrite is not available currently.
        }
      }

      // pop from the first batch.
      auto item = std::move(first_.back());
      first_.pop_back();
      return item;
    }

    // put a object into thread local cache or global cache.
    void put(std::unique_ptr<Storage> obj) {
      // push to the first batch.
      if (first_.size() < kThreadLocalMaxNum) {
        first_.push_back(std::move(obj));
        return;
      }

      // push to the second batch.
      second_.push_back(std::move(obj));
      if (UNLIKELY(second_.size() >= kThreadLocalMaxNum)) {
        // push to the global cache.
        parent_.putBatch(std::move(second_));
        second_.clear();
        second_.reserve(kThreadLocalMaxNum);
      }
    }

   private:
    ObjectPool &parent_;
    Batch first_;
    Batch second_;
  };

  static auto &tls() {
    static ObjectPool instance;
    thread_local TLS tls{instance};
    return tls;
  }

  bool getBatch(Batch &batch) {
    auto lock = std::unique_lock(mutex_);
    if (global_.empty()) {
      return false;
    }

    // pop a batch from global cache.
    batch = std::move(global_.back());
    global_.pop_back();
    return true;
  }

  void putBatch(Batch batch) {
    auto lock = std::unique_lock(mutex_);
    if (global_.size() < kGlobalMaxBatchNum) {
      global_.push_back(std::move(batch));
    }
  }

 private:
  static constexpr auto kThreadLocalMaxNum = std::max(1ul, NumTLSCachedItem);
  static constexpr auto kGlobalMaxBatchNum = std::max(1ul, NumGlobalCachedItem / NumTLSCachedItem);

  std::mutex mutex_;
  std::vector<Batch> global_;
};

}  // namespace hf3fs
