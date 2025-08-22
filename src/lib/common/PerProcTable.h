#pragma once

#include <folly/concurrency/AtomicSharedPtr.h>
#include <memory>
#include <ranges>
#include <shared_mutex>
#include <vector>

#include "common/utils/Result.h"
#include "common/utils/RobinHood.h"

namespace hf3fs::lib {
template <typename T, uint32_t KeyError, uint32_t OverflowError>
class PerProcTable {
 public:
  using Item = T;
  using ItemPtr = std::shared_ptr<Item>;

  PerProcTable() = delete;
  PerProcTable(int p, int pp, size_t cap)
      : pid_(p),
        ppid_(pp),
        table_(cap) {
    free_.reserve(cap);
  }
  PerProcTable(const PerProcTable &rhs, int p, int pp, size_t cap)
      : pid_(p),
        ppid_(pp) {
    if (ppid_ == rhs.pid_) {
      {
        std::shared_lock lock(rhs.mtx_);
        std::vector<AtomicItemPtr> t(std::max(rhs.table_.size(), cap));
        nextAvail_ = rhs.nextAvail_;
        for (int i = 0; i < nextAvail_; ++i) {
          t[i] = rhs.table_[i].load();
        }
        swap(t, table_);
        free_ = rhs.free_;
      }
      if (table_.size() < cap) {  // we don't shrink the table or items may drop out
        free_.reserve(cap);
      }
    } else {  // rhs is not our parent proc, so do not copy its fd table
      std::vector<AtomicItemPtr> t(cap);
      swap(t, table_);
    }
  }
  int pid() const { return pid_; }
  int ppid() const { return ppid_; }
  hf3fs::Result<ItemPtr> at(int idx, bool check = true) {
    if (idx < 0) {
      XLOGF(DBG, "bad index {}", idx);
      return makeError(KeyError,
                       fmt::format("invalid index {} to lookup in table for proc {} parent {}", idx, pid_, ppid_));
    } else {
      {
        //        std::shared_lock lock(mtx_);
        if (table_.size() < (size_t)idx) {
          XLOGF(DBG, "invalid index {}", idx);
          return makeError(KeyError, fmt::format("invalid index {} in table for proc {} parent {}", idx, pid_, ppid_));
        } else {
          auto p = table_[idx].load();
          if (check && !p) {
            XLOGF(DBG, "no item at index {}", idx);
            return makeError(KeyError,
                             fmt::format("index {} not found in table for proc {} parent {}", idx, pid_, ppid_));
          }

          return p;
        }
      }
    }
  }
  void setAt(int idx, const ItemPtr &v) {
    std::unique_lock lock(mtx_);
    auto p = table_[idx].load();
    XLOGF(DBG,
          "before set at idx {} table idx {} next avail {} free size {}",
          idx,
          (void *)p.get(),
          nextAvail_,
          free_.size());
    if (!p) {  // add to specific place
      if (idx >= nextAvail_) {
        for (auto i = nextAvail_; i < idx; ++i) {
          free_.push_back(i);
        }
        std::ranges::make_heap(free_, std::greater<int>());
        nextAvail_ = idx + 1;
      } else {
        if (free_.size() == 1) {
          assert(free_[0] == idx);
          free_.clear();
        } else {
          *std::ranges::find(free_, idx) = free_.back();
          free_.pop_back();
          std::ranges::make_heap(free_, std::greater<int>());
        }
      }
    }
    table_[idx] = v;
  }
  hf3fs::Result<int> add(const ItemPtr &v) {
    std::unique_lock lock(mtx_);
    auto useFree = !free_.empty();
    if (!useFree && (size_t)nextAvail_ >= table_.size()) {
      return makeError(OverflowError,
                       fmt::format("no free slot in table for proc {} parent {} table size {} next avail {}",
                                   pid_,
                                   ppid_,
                                   table_.size(),
                                   nextAvail_));
    }
    auto idx = useFree ? free_.front() : nextAvail_++;
    if (useFree) {
      std::ranges::pop_heap(free_, std::greater<int>());
      free_.pop_back();
    }
    auto p = table_[idx].load();

    XLOGF(DBG,
          "idx {} use free {} next avail {} free size {} table idx {}",
          idx,
          useFree,
          nextAvail_,
          free_.size(),
          (void *)p.get());
    assert(!p);
    table_[idx] = v;
    return idx;
  }
  void resetAt(int idx) {
    std::unique_lock lock(mtx_);
    table_[idx] = ItemPtr{};
    free_.push_back(idx);
    std::ranges::push_heap(free_, std::greater<int>());
    XLOGF(DBG, "reset idx {} next avail {} free size {} first free {}", idx, nextAvail_, free_.size(), free_.front());
  }
  size_t size() const {
    std::shared_lock lock(mtx_);
    return (size_t)nextAvail_ - free_.size();
  }
  std::vector<int> allUsed() const {
    std::vector<int> used;
    used.reserve(table_.size());

    std::shared_lock lock(mtx_);
    for (int i = 1; i < (int)table_.size(); ++i) {
      if (table_[i].load()) {
        used.push_back(i);
      }
    }

    return used;
  }
  std::unique_lock<std::shared_mutex> lock() const { return std::unique_lock<std::shared_mutex>(mtx_); }

 private:
  int pid_;
  int ppid_;
  mutable std::shared_mutex mtx_;
  using AtomicItemPtr = folly::atomic_shared_ptr<Item>;
  std::vector<AtomicItemPtr> table_;
  int nextAvail_ = 0;
  std::vector<int> free_;
};

template <typename T, uint32_t KeyError, uint32_t OverflowError>
class AllProcMap {
 public:
  using Table = PerProcTable<T, KeyError, OverflowError>;
  using TablePtr = std::shared_ptr<Table>;
  using ItemPtr = typename Table::ItemPtr;

  TablePtr procTable(int pid, int ppid, size_t cap) {
    TablePtr parentTable;
    {
      std::shared_lock lock(mtx_);
      auto it = map_.find(pid);
      if (it != map_.end()) {
        if (ppid == it->second->ppid()) {
          return it->second;
        }  // else, same pid but diff proc, old proc must have died
      } else {
        auto it = map_.find(ppid);
        if (it != map_.end()) {
          parentTable = it->second;
        }
      }
    }

    TablePtr newTable;
    if (parentTable) {
      newTable = std::make_shared<Table>(*parentTable, pid, ppid, cap);
    } else {
      newTable = std::make_shared<Table>(pid, ppid, cap);
    }

    std::unique_lock lock(mtx_);
    return map_[pid] = std::move(newTable);
  }

  void removeProc(int pid) {
    std::unique_lock lock(mtx_);
    map_.erase(pid);
  }

  std::vector<std::pair<int, int>> allProcs() const {
    std::vector<std::pair<int, int>> procs;
    std::shared_lock lock(mtx_);
    procs.reserve(map_.size());
    for (auto &&p : map_) {
      if (p.first != 0) {
        procs.emplace_back(std::make_pair(p.first, p.second->ppid()));
      }
    }

    return procs;
  }

 private:
  mutable std::shared_mutex mtx_;
  robin_hood::unordered_map<int, TablePtr> map_;
};
}  // namespace hf3fs::lib
