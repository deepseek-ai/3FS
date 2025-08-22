#pragma once

#include <array>
#include <folly/hash/Hash.h>
#include <mutex>

#include "RobinHoodUtils.h"

namespace hf3fs {

template <class T, std::size_t N>
class Shards {
 public:
  auto position(auto &&...args) { return folly::hash::hash_combine_generic(RobinHoodHasher{}, args...) % N; }

  auto withLockAt(auto &&f, std::size_t pos) {
    auto lock = std::unique_lock(locks_[pos]);
    return f(array_[pos]);
  }

  auto withLock(auto &&f, auto &&...args) { return withLockAt(f, position(args...)); }

  void iterate(auto &&f) {
    for (std::size_t idx = 0; idx < N; ++idx) {
      withLockAt(f, idx);
    }
  }

 private:
  std::array<std::mutex, N> locks_;
  std::array<T, N> array_;
};

}  // namespace hf3fs
