#pragma once

#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <limits>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>

#include "common/utils/Coroutine.h"
#include "common/utils/Shards.h"

namespace hf3fs {

template <std::size_t N = 256>
class CoLockManager {
  struct State {
    std::string tag;
    folly::coro::Baton &baton;
  };
  using Queue = std::queue<State>;
  using Map = std::unordered_map<std::string, Queue>;
  using It = typename Map::iterator;

 public:
  struct Guard {
    static constexpr auto kInvalidPos = std::numeric_limits<std::size_t>::max();
    Guard(CoLockManager &manager, folly::coro::Baton &baton, std::size_t pos, It it, std::string currentTag)
        : manager_(manager),
          baton_(baton),
          pos_(pos),
          it_(it),
          currentTag_(std::move(currentTag)) {}
    Guard(const Guard &) = delete;
    Guard(Guard &&o)
        : manager_(o.manager_),
          baton_(o.baton_),
          pos_(std::exchange(o.pos_, kInvalidPos)),
          it_(o.it_),
          currentTag_(std::move(o.currentTag_)) {}
    ~Guard() {
      if (pos_ != kInvalidPos) {
        if (!locked()) {
          folly::coro::blockingWait(lock());
        }
        manager_.unlock(baton_, pos_, it_);
      }
    }

    // check locked or not.
    bool locked() const { return baton_.ready(); }

    // get current locked tag.
    auto &currentTag() const { return currentTag_; }

    // lock.
    [[nodiscard]] CoTask<void> lock() { co_await baton_; }

   private:
    CoLockManager &manager_;
    folly::coro::Baton &baton_;
    std::size_t pos_;
    It it_;
    std::string currentTag_;
  };

  auto lock(folly::coro::Baton &baton, const std::string &key, const std::string &tag = "") {
    auto pos = shards_.position(key);
    auto [it, succ, currentTag] = shards_.withLockAt(
        [&](Map &map) {
          auto [it, succ] = map.emplace(key, Queue{});
          it->second.push({tag, baton});
          return std::make_tuple(it, succ, it->second.front().tag);
        },
        pos);
    if (succ) {
      baton.post();
    }
    return Guard{*this, baton, pos, it, std::move(currentTag)};
  }

  auto tryLock(folly::coro::Baton &baton, const std::string &key, const std::string &tag = "") {
    auto pos = shards_.position(key);
    auto [it, succ, currentTag] = shards_.withLockAt(
        [&](Map &map) {
          auto [it, succ] = map.emplace(key, Queue{});
          if (succ) {
            it->second.push({tag, baton});
          }
          return std::make_tuple(it, succ, succ ? std::string{} : it->second.front().tag);
        },
        pos);
    if (succ) {
      baton.post();
    }
    return Guard{*this, baton, succ ? pos : Guard::kInvalidPos, it, std::move(currentTag)};
  }

 protected:
  void unlock(folly::coro::Baton &baton, size_t pos, It it) {
    shards_.withLockAt(
        [&](Map &map) {
          auto &queue = it->second;
          if (UNLIKELY(queue.empty() || &queue.front().baton != &baton)) {
            assert(false);
          }
          queue.pop();
          if (!queue.empty()) {
            queue.front().baton.post();
          } else {
            map.erase(it);
          }
        },
        pos);
  }

 private:
  Shards<Map, N> shards_;
};

}  // namespace hf3fs
