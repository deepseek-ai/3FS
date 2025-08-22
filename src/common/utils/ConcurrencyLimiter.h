#pragma once

#include <folly/experimental/coro/Baton.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <queue>
#include <unordered_map>
#include <utility>

#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Shards.h"

namespace hf3fs {

class ConcurrencyLimiterConfig : public ConfigBase<ConcurrencyLimiterConfig> {
  CONFIG_HOT_UPDATED_ITEM(max_concurrency, 1u, ConfigCheckers::checkPositive);
};

template <class Key>
class ConcurrencyLimiter {
 private:
  struct State {
    uint32_t concurrency{};
    std::queue<std::reference_wrapper<folly::coro::Baton>> queue{};
  };
  using Map = std::unordered_map<Key, State>;
  using It = typename Map::iterator;
  const ConcurrencyLimiterConfig &config_;
  Shards<Map, 16> shards_;

 public:
  ConcurrencyLimiter(const ConcurrencyLimiterConfig &config)
      : config_(config) {}

  struct Guard {
    Guard(ConcurrencyLimiter &limiter, std::size_t pos, It it)
        : limiter_(&limiter),
          pos_(pos),
          it_(it) {}
    Guard(const Guard &) = delete;
    Guard(Guard &&o)
        : limiter_(std::exchange(o.limiter_, nullptr)),
          pos_(o.pos_),
          it_(o.it_) {}
    ~Guard() { limiter_ == nullptr ? void() : limiter_->unlock(pos_, it_); }

   private:
    ConcurrencyLimiter *limiter_;
    std::size_t pos_;
    It it_;
  };

  CoTask<Guard> lock(const Key &key) {
    folly::coro::Baton baton;
    auto pos = shards_.position(key);
    auto [it, succ] = shards_.withLockAt(
        [&](Map &map) {
          auto [it, succ] = map.emplace(key, State{});
          auto &state = it->second;
          if (state.concurrency < config_.max_concurrency()) {
            ++state.concurrency;
            return std::make_pair(it, true);
          }
          state.queue.push(std::ref(baton));
          return std::make_pair(it, false);
        },
        pos);
    if (!succ) {
      co_await baton;
    }
    co_return Guard{*this, pos, it};
  }

 private:
  void unlock(size_t pos, It it) {
    shards_.withLockAt(
        [&](Map &map) {
          auto &state = it->second;
          assert(state.concurrency > 0);
          if (!state.queue.empty()) {
            if (state.concurrency <= config_.max_concurrency()) {
              // wake up the next waiting baton.
              auto next = std::move(state.queue.front());
              state.queue.pop();
              next.get().post();
            } else {
              // give up.
              --state.concurrency;
            }
          } else if (--state.concurrency == 0) {
            // remove current state.
            map.erase(it);
          }
        },
        pos);
  }
};

}  // namespace hf3fs
