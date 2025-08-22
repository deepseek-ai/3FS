#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <folly/experimental/coro/Baton.h>
#include <mutex>
#include <vector>

#include "common/net/Network.h"
#include "common/net/RDMAControl.h"
#include "common/net/Transport.h"
#include "common/serde/MessagePacket.h"
#include "common/utils/Duration.h"
#include "common/utils/RobinHood.h"
#include "common/utils/Shards.h"
#include "common/utils/Status.h"

namespace hf3fs::net {

// Wake up the waiting caller.
class Waiter {
 public:
  // is singleton.
  static Waiter &instance();
  ~Waiter();

  struct Item {
    folly::coro::Baton baton;
    IOBufPtr buf;
    serde::MessagePacket<> packet;
    Status status = Status::OK;
    TransportPtr transport;
    RDMATransmissionLimiterPtr limiter;
    RelativeTime timestamp = RelativeTime::now();
  };

  size_t bind(Item &item) {
    thread_local size_t start = 0, end = 0;
    if (UNLIKELY(start >= end)) {
      start = uuid_idx_.fetch_add(kShardsSize);
      end = start + kShardsSize;
    }

    auto uuid = start++;
    shards_.withLock([&](Map &map) { map.emplace(uuid, item); }, uuid);
    return uuid;
  }

  bool setTransport(size_t uuid, TransportPtr transport) {
    return shards_.withLock(
        [&](Map &map) {
          auto it = map.find(uuid);
          if (it == map.end()) {
            return false;
          }
          it->second.transport = std::move(transport);
          return true;
        },
        uuid);
  }

  std::optional<Duration> setTransmissionLimiterPtr(size_t uuid,
                                                    const RDMATransmissionLimiterPtr &limiter,
                                                    RelativeTime startTime) {
    return shards_.withLock(
        [&](Map &map) -> std::optional<Duration> {
          auto it = map.find(uuid);
          if (it == map.end() || it->second.limiter) {
            return std::nullopt;
          }
          it->second.limiter = limiter;
          return startTime - std::exchange(it->second.timestamp, RelativeTime::now());
        },
        uuid);
  }

  void post(const serde::MessagePacket<> &packet, IOBufPtr buff) {
    auto item = find(packet.uuid);
    if (item) {
      item->buf = std::move(buff);
      item->packet = packet;
      if (item->limiter) {
        item->limiter->signal(RelativeTime::now() - item->timestamp);
      }
      item->baton.post();
    }
  }

  static void error(Item *item, Status status) {
    if (item) {
      item->status = status;
      if (item->limiter) {
        item->limiter->signal(RelativeTime::now() - item->timestamp);
      }
      item->baton.post();
    }
  }

  bool contains(size_t uuid) {
    return shards_.withLock([&](Map &map) { return map.contains(uuid); }, uuid);
  }

  void timeout(size_t uuid) { error(find(uuid), Status(RPCCode::kTimeout)); }
  void sendFail(size_t uuid) { error(find(uuid), Status(RPCCode::kSendFailed)); }

  void clearPendingRequestsOnTransportFailure(Transport *tr) {
    shards_.iterate([tr](Map &map) {
      for (auto it = map.begin(); it != map.end();) {
        Item *item = &it->second;
        if (item->transport.get() == tr) {
          error(item, Status(RPCCode::kTimeout));
          it = map.erase(it);
        } else {
          ++it;
        }
      }
    });
  }

  void schedule(uint64_t uuid, std::chrono::milliseconds timeout);

 private:
  Waiter();

  Item *find(size_t uuid) {
    return shards_.withLock(
        [&](Map &map) -> Item * {
          auto it = map.find(uuid);
          if (it == map.end()) {
            return nullptr;
          }
          auto item = &it->second;
          map.erase(it);
          return item;
        },
        uuid);
  }

  void run();

 private:
  std::atomic<size_t> uuid_idx_{0};

  constexpr static auto kShardsBit = 8u;
  constexpr static auto kShardsSize = (1u << kShardsBit);
  using Map = robin_hood::unordered_map<size_t, Item &>;
  Shards<Map, kShardsSize> shards_;

  // for timer.
  struct Task {
    uint64_t uuid;
    int64_t runTime;
    bool operator<(const Task &o) const { return runTime > o.runTime; }
  };

  class TaskShard {
   public:
    bool schedule(uint64_t uuid, int64_t runTime);
    void exchangeTasks(std::vector<Task> &out);

   private:
    std::mutex mutex_;
    int64_t nearestRunTime_ = std::numeric_limits<int64_t>::max();
    std::vector<Task> tasks_;
  };

  std::atomic<bool> stop_ = false;
  constexpr static auto kTaskShardNum = 13u;
  std::array<TaskShard, kTaskShardNum> taskShards_;

  std::mutex mutex_;
  std::condition_variable cond_;
  int64_t nearestRunTime_ = std::numeric_limits<int64_t>::max();
  std::vector<Task> reserved_;

  std::jthread thread_;
};

}  // namespace hf3fs::net
