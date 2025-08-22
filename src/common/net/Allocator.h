#pragma once

#include <folly/Likely.h>
#include <memory>
#include <utility>

#include "common/utils/ObjectPool.h"

namespace hf3fs::net {

// A memory allocator with TLS cache support.
template <size_t DefaultSize = 1_KB, size_t NumTLSCachedItem = 1024, size_t NumGlobalCachedItem = 64 * 1024>
class Allocator {
 public:
  static constexpr auto kDefaultSize = DefaultSize;

  static std::uint8_t *allocate(size_t size) {
    if (LIKELY(size <= DefaultSize)) {
      return Pool::get().release()->data();
    }
    return new uint8_t[size];
  }

  static void deallocate(uint8_t *buf, size_t size) {
    if (LIKELY(size <= DefaultSize)) {
      auto autoRelease = typename Pool::Ptr{reinterpret_cast<Memory *>(buf)};
      return;
    }
    delete[] buf;
  }

 private:
  using Memory = std::array<uint8_t, DefaultSize>;
  using Pool = ObjectPool<Memory, NumTLSCachedItem, NumGlobalCachedItem>;
};

template <size_t DefaultSize = 1_KB>
class SystemAllocator {
 public:
  static constexpr auto kDefaultSize = DefaultSize;
  static std::uint8_t *allocate(size_t size) { return new uint8_t[size]; }
  static void deallocate(uint8_t *buf, size_t) { delete[] buf; }
};

}  // namespace hf3fs::net
