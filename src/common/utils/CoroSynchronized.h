#pragma once

#include <folly/experimental/coro/SharedMutex.h>

#include "common/utils/Coroutine.h"

namespace hf3fs {
namespace detail {
template <typename Guard, typename T>
class CoLockGuard {
 public:
  CoLockGuard(Guard &&guard, T &obj)
      : guard_(std::move(guard)),
        obj_(obj) {}

  T *operator->() { return &obj_; }

  T &operator*() { return obj_; }

 private:
  Guard guard_;
  T &obj_;
};

template <typename T>
using CoExclusiveLockGuard = CoLockGuard<std::unique_lock<folly::coro::SharedMutex>, T>;

template <typename T>
using CoSharedLockGuard = CoLockGuard<folly::coro::SharedLock<folly::coro::SharedMutex>, const T>;
}  // namespace detail

template <typename T>
class CoroSynchronized {
 public:
  using SharedLockPtr = detail::CoSharedLockGuard<T>;
  using LockPtr = detail::CoExclusiveLockGuard<T>;

  template <typename... Args>
  explicit CoroSynchronized(Args &&...args)
      : obj_(std::forward<Args>(args)...) {}

  CoTask<SharedLockPtr> coSharedLock() {
    auto lock = co_await sharedMu_.co_scoped_lock_shared();
    co_return SharedLockPtr(std::move(lock), obj_);
  }

  CoTask<LockPtr> coLock() {
    auto lock = co_await sharedMu_.co_scoped_lock();
    co_return LockPtr(std::move(lock), obj_);
  }

 private:
  folly::coro::SharedMutex sharedMu_;
  T obj_;
};
}  // namespace hf3fs
