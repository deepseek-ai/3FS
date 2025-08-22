#pragma once

#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/experimental/coro/Mutex.h>
#include <folly/hash/Hash.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include "common/utils/Coroutine.h"
#include "common/utils/FairSharedMutex.h"
#include "common/utils/RobinHood.h"

namespace hf3fs {

template <typename Hash>
inline std::string hashKeyToString(const Hash &) noexcept {
  return "";
}

template <typename Hash, typename T, typename... Ts>
std::string hashKeyToString(const Hash &h, const T &t, const Ts &...ts) {
  if (sizeof...(ts) == 0) return fmt::format("{}#{:#x}", t, h);
  return fmt::format("{}/{}", t, hashKeyToString(h, ts...));
}

template <class Mutex = std::mutex>
class LockManager {
 public:
  LockManager(uint32_t numBuckets = 4099u)
      : numBuckets_(numBuckets),
        mutexes_(std::make_unique<Mutex[]>(numBuckets)),
        owners_(numBuckets) {
    for (auto &owner : owners_) owner = std::make_shared<std::string>("(unknown)");
  }

  auto lock(auto &&...args) { return std::unique_lock{mutexes_[idx(args...)]}; }
  auto try_lock(auto &&...args) { return std::unique_lock{mutexes_[idx(args...)], std::try_to_lock}; }

  auto lock_shared(auto &&...args) { return std::shared_lock{mutexes_[idx(args...)]}; }
  auto try_lock_shared(auto &&...args) { return std::shared_lock{mutexes_[idx(args...)], std::try_to_lock}; }

  auto co_scoped_lock(auto &&...args) { return mutexes_[idx(args...)].co_scoped_lock(); }

  CoTask<std::unique_lock<folly::coro::Mutex>> co_scoped_lock_log_owner(auto &&...args) {
    auto index = idx(args...);
    auto &mutex = mutexes_[index];
    auto &owner = owners_[index];

    std::unique_lock<folly::coro::Mutex> lock{mutex, std::try_to_lock};

    if (!lock) {
      XLOGF(DBG3, "Key {} is waiting on mutex #{} ownerd by {}", hashKeyToString(index, args...), index, *owner.load());
      auto waitLock = co_await mutex.co_scoped_lock();
      lock.swap(waitLock);
      XLOGF(DBG3, "Key {} just got a lock on mutex #{}", hashKeyToString(index, args...), index);
    }

    if (XLOG_IS_ON(DBG3)) owner = std::make_shared<std::string>(hashKeyToString(index, args...));

    co_return lock;
  }

  auto lock_at(uint64_t index) { return std::unique_lock{mutexes_[index]}; }
  auto try_lock_at(uint64_t index) { return std::unique_lock{mutexes_[index], std::try_to_lock}; }

  auto lock_shared_at(uint64_t index) { return std::shared_lock{mutexes_[index]}; }
  auto try_lock_shared_at(uint64_t index) { return std::shared_lock{mutexes_[index], std::try_to_lock}; }

  auto co_scoped_lock_at(uint64_t index) { return mutexes_[index].co_scoped_lock(); }

  uint64_t idx(auto &&...args) const {
    if constexpr (sizeof...(args) == 0) {
      return 0;
    } else {
      return folly::hash::hash_combine(args...) % numBuckets_;
    }
  }

  uint32_t numBuckets() const { return numBuckets_; }

 private:
  uint32_t numBuckets_{};
  std::unique_ptr<Mutex[]> mutexes_;
  std::vector<folly::atomic_shared_ptr<std::string>> owners_;
};

using UniqueLockManager = LockManager<std::mutex>;
using SharedLockManager = LockManager<FairSharedMutex>;
using CoUniqueLockManager = LockManager<folly::coro::Mutex>;

}  // namespace hf3fs
