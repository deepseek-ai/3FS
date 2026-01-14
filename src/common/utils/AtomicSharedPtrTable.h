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

    auto current = nextAvail.load(std::memory_order_relaxed);
    if (current < cap) {
      nextAvail.fetch_add(1, std::memory_order_release);
      return current;
    }
    return std::nullopt;
  }

  void dealloc(int idx) {
    std::lock_guard lock(mutex);
    auto current = nextAvail.load(std::memory_order_relaxed);
    if (idx < 0 || idx >= current) {
      return;
    }

    if (idx == current - 1) {
      do {
        current = nextAvail.fetch_sub(1, std::memory_order_release) - 1;
      } while (current > 0 && free.erase(current - 1));
    } else {
      free.insert(idx);
    }
  }

  const int cap;
  mutable std::mutex mutex;
  std::atomic<int> nextAvail{0};
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
