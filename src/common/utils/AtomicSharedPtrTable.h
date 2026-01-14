#pragma once

#include <folly/concurrency/AtomicSharedPtr.h>
#include <mutex>
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

    auto idx = nextAvail.load();
    if (idx < cap) {
      ++nextAvail;
      return idx;
    } else {
      return std::nullopt;
    }
  }
  void dealloc(int idx) {
    if (idx < 0 || idx >= cap) {
      return;
    }

    std::lock_guard lock(mutex);
    if (idx == nextAvail - 1) {
      while (free.find(--nextAvail) != free.end()) {
        // move back next avail as much as possible
        ;
      }
    } else {
      free.insert(idx);
    }
  }

  int cap;
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
