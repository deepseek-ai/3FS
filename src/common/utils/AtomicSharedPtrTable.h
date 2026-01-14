#pragma once

#include <folly/concurrency/AtomicSharedPtr.h>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace hf3fs {
struct AvailSlots {
  AvailSlots(int c)
      : cap(c) {}
  std::optional<int> alloc() {
    std::lock_guard lock(mutex);
    if (!free.empty()) {
      auto idx = *free.begin();
      free.erase(free.begin());
      return idx;
    }

    if (nextAvail < cap) {
      return nextAvail++;
    }
    return std::nullopt;
  }

  void dealloc(int idx) {
    std::lock_guard lock(mutex);
    if (idx < 0 || idx >= nextAvail) {
      return;
    }

    if (idx == nextAvail - 1) {
      do {
        --nextAvail;
      } while (nextAvail > 0 && free.erase(nextAvail - 1));
    } else {
      free.insert(idx);
    }
  }

  const int cap;
  mutable std::mutex mutex;
  int nextAvail{0};
  std::set<int> free;
};

template <typename T>
struct AtomicSharedPtrTable {
  AtomicSharedPtrTable(int cap)
      : slots(cap),
        table(cap) {}

  std::optional<int> alloc() { return slots.alloc(); }
  void dealloc(int idx) { slots.dealloc(idx); }
  void remove(int idx) {
    if (idx < 0 || idx >= (int)table.size()) {
      return;
    }

    auto &ap = table[idx];
    if (!ap.load()) {
      return;
    }

    ap.store(std::shared_ptr<T>());
    dealloc(idx);
  }

  AvailSlots slots;
  std::vector<folly::atomic_shared_ptr<T>> table;
};
};  // namespace hf3fs
