#include "common/net/TransportPool.h"

#include <folly/Random.h>
#include <folly/logging/xlog.h>

#include "common/monitor/Recorder.h"

namespace hf3fs::net {
namespace {

std::atomic<uint32_t> acceptedCount{};
std::atomic<uint32_t> connectedCount{};
monitor::ValueRecorder acceptedCountRecorder{"common.net.accepted_count"};
monitor::ValueRecorder connectedCountRecorder{"common.net.connected_count"};

}  // namespace

TransportPtr TransportSet::acquire(uint32_t idx) {
  ensureSize(idx);
  auto it = idxToTransport_[idx];
  return UNLIKELY(it == transports_.end()) ? nullptr : it->first;
}

void TransportSet::add(TransportPtr tr, uint32_t idx) {
  ensureSize(idx);
  idxToTransport_[idx] = transports_.insert_or_assign(std::move(tr), idx).first;
}

bool TransportSet::remove(TransportPtr tr) {
  auto it = transports_.find(tr);
  if (UNLIKELY(it == transports_.end())) {
    return false;
  }
  erase(it);
  return true;
}

uint32_t TransportSet::dropAll() {
  uint32_t removed{};
  for (auto it = transports_.begin(); it != transports_.end();) {
    it = erase(it);
    ++removed;
  }
  return removed;
}

uint32_t TransportSet::checkAll(Duration expiredTime) {
  uint32_t removed{};
  for (auto it = transports_.begin(); it != transports_.end();) {
    if (it->first->expired(expiredTime)) {
      it = erase(it);
      ++removed;
    } else {
      auto result = it->first->check();
      if (UNLIKELY(result.hasError())) {
        it = erase(it);
        ++removed;
      } else {
        ++it;
      }
    }
  }
  return removed;
}

void TransportSet::ensureSize(uint32_t idx) {
  auto size = idxToTransport_.size();
  if (LIKELY(idx < size)) {
    return;
  }
  while (size <= idx) {
    size *= 2;
  }
  idxToTransport_.resize(size, transports_.end());
}

TransportSet::TransportMap::iterator TransportSet::erase(TransportMap::iterator it) {
  ensureSize(it->second);
  if (idxToTransport_[it->second] == it) {
    idxToTransport_[it->second] = transports_.end();
  }
  return transports_.erase(it);
}

TransportPool::~TransportPool() { dropConnections(true, true); }

void TransportPool::add(TransportPtr tr) {
  auto addr = tr->serverAddr();
  shards_.withLock(
      [&](Map &map) {
        auto &set = map[addr];
        set.add(std::move(tr), 0);
        if (addr.ip == 0) {
          acceptedCountRecorder.set(++acceptedCount);
        } else {
          connectedCountRecorder.set(++connectedCount);
        }
      },
      addr);
}

void TransportPool::remove(TransportPtr tr) {
  auto addr = tr->serverAddr();
  shards_.withLock(
      [&](Map &map) {
        auto &set = map[addr];
        bool succ = set.remove(std::move(tr));
        if (succ) {
          if (addr.ip == 0) {
            acceptedCountRecorder.set(--acceptedCount);
          } else {
            connectedCountRecorder.set(--connectedCount);
          }
        }
      },
      addr);
}

std::pair<TransportPtr, bool> TransportPool::get(Address addr, IOWorker &io_worker) {
  uint32_t idx = folly::Random::rand32() % config_.max_connections();

  // 1. try to find transport in thread local cache.
  auto &cached = (*tlsCache_)[TransportCacheKey{addr, idx}];
  auto transport = cached.lock();
  if (LIKELY(transport != nullptr && !transport->invalidated())) {
    return std::make_pair(std::move(transport), false);
  } else {
    transport = nullptr;
  }

  return shards_.withLock(
      [&](Map &map) {
        // 2. try to find transport in pool.
        auto &set = map[addr];
        transport = set.acquire(idx);
        if (LIKELY(transport != nullptr)) {
          cached = transport;
          return std::make_pair(std::move(transport), false);
        }

        // 3. create a new transport. still protected by mutex.
        transport = Transport::create(addr, io_worker);
        set.add(transport, idx);
        acceptedCountRecorder.set(++acceptedCount);
        cached = transport;
        return std::make_pair(std::move(transport), true);
      },
      addr);
}

void TransportPool::dropConnections(bool dropAll /* = true */, bool dropIncome /* = false */) {
  if (dropAll) {
    uint32_t removeAccepted{};
    uint32_t removeConnected{};
    shards_.iterate([&](Map &map) {
      for (auto it = map.begin(); it != map.end();) {
        if (it->first.ip == 0 && !dropIncome) {
          ++it;  // skip incoming connections.
        } else {
          if (it->first.ip == 0) {
            removeAccepted += it->second.transports().size();
          } else {
            removeConnected += it->second.transports().size();
          }
          it = map.erase(it);
        }
      }
    });
    acceptedCountRecorder.set(acceptedCount -= removeAccepted);
    connectedCountRecorder.set(connectedCount -= removeConnected);
  }
}

void TransportPool::dropConnections(Address addr) {
  shards_.withLock(
      [&](Map &map) {
        auto it = map.find(addr);
        if (it == map.end()) {
          return;
        }
        auto removed = it->second.dropAll();
        if (addr.ip == 0) {
          acceptedCountRecorder.set(acceptedCount -= removed);
        } else {
          connectedCountRecorder.set(connectedCount -= removed);
        }
      },
      addr);
}

void TransportPool::checkConnections(Address addr, Duration expiredTime) {
  shards_.withLock(
      [&](Map &map) {
        auto it = map.find(addr);
        if (it == map.end()) {
          return;
        }
        auto removed = it->second.checkAll(expiredTime);
        if (addr.ip == 0) {
          acceptedCountRecorder.set(acceptedCount -= removed);
        } else {
          connectedCountRecorder.set(connectedCount -= removed);
        }
      },
      addr);
}

}  // namespace hf3fs::net
