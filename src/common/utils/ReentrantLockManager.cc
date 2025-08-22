#include "common/utils/ReentrantLockManager.h"

#include <chrono>
#include <utility>

#include "common/monitor/Recorder.h"

namespace hf3fs {
namespace {
monitor::LatencyRecorder readLockLatency("common_read_lock_latency");
monitor::LatencyRecorder writeLockLatency("common_write_lock_latency");
monitor::LatencyRecorder readLockWaitingLatency("common_read_lock_waiting_latency");
monitor::LatencyRecorder writeLockWaitingLatency("common_write_lock_waiting_latency");
monitor::CountRecorder wakeUpWaitingReadLockTimes("common_wakeup_waiting_read_lock");
monitor::CountRecorder wakeUpWaitingWriteLockTimes("common_wakeup_waiting_write_lock");
}  // namespace

void ReentrantLockManager::init(uint32_t shardsNum) {
  shardsNum_ = shardsNum;
  shards_ = std::make_unique<Shard[]>(shardsNum);
}

ReentrantLockManager::LockGuard::LockGuard(ReentrantLockManager *locker, uint64_t id, LockType type)
    : locker_(locker),
      id_(id),
      type_(type) {}

ReentrantLockManager::LockGuard::LockGuard(LockGuard &&o)
    : locker_(o.locker_),
      id_(o.id_),
      type_(std::exchange(o.type_, LockType::NONE)) {}

void ReentrantLockManager::LockGuard::unlock() {
  switch (type_) {
    case LockType::NONE:
      break;
    case LockType::READ:
      locker_->readUnlock(id_);
      type_ = LockType::NONE;
      break;
    case LockType::WRITE:
      locker_->writeUnlock(id_);
      type_ = LockType::NONE;
      break;
  }
}

ReentrantLockManager::LockGuard &ReentrantLockManager::LockGuard::operator=(LockGuard &&o) {
  if (std::addressof(o) != this) {
    unlock();
    locker_ = o.locker_;
    id_ = o.id_;
    type_ = std::exchange(o.type_, LockType::NONE);
  }
  return *this;
}

ReentrantLockManager::LockGuard ReentrantLockManager::readLock(uint64_t id) {
  auto guard = folly::makeGuard(
      [now = std::chrono::steady_clock::now()] { readLockLatency.addSample(std::chrono::steady_clock::now() - now); });

  auto &shard = getShard(id);
  auto lock = std::unique_lock(shard.mutex);
  auto &status = shard.map[id];

  if (!status.readyToReadLock()) {
    auto start = std::chrono::steady_clock::now();
    ++status.waitingReadLock;
    shard.readCond.wait(lock, [&] { return shard.map[id].readyToReadLock(); });
    --status.waitingReadLock;
    auto elapsed = std::chrono::steady_clock::now() - start;
    readLockWaitingLatency.addSample(elapsed);
  }
  status.readLock();
  return LockGuard(this, id, LockType::READ);
}

ReentrantLockManager::LockGuard ReentrantLockManager::tryReadLock(uint64_t id) {
  auto &shard = getShard(id);
  auto lock = std::unique_lock(shard.mutex);
  auto &status = shard.map[id];

  if (status.readyToReadLock()) {
    status.readLock();
    return LockGuard(this, id, LockType::READ);
  }
  return LockGuard(this, id, LockType::NONE);
}

void ReentrantLockManager::readUnlock(uint64_t id) {
  auto &shard = getShard(id);
  auto lock = std::unique_lock(shard.mutex);
  auto &status = shard.map[id];

  status.readUnlock();
  assert(status.currentLock >= 0);
  if (status.readyToErase()) {
    shard.map.erase(id);
  } else if (status.wakeUpWaitingWriteLock()) {
    shard.writeCond.notify_all();
    wakeUpWaitingWriteLockTimes.addSample(1);
  }
}

ReentrantLockManager::LockGuard ReentrantLockManager::writeLock(uint64_t id) {
  auto guard = folly::makeGuard(
      [now = std::chrono::steady_clock::now()] { writeLockLatency.addSample(std::chrono::steady_clock::now() - now); });

  auto &shard = getShard(id);
  auto lock = std::unique_lock(shard.mutex);
  auto &status = shard.map[id];

  if (!status.readyToWriteLock()) {
    auto start = std::chrono::steady_clock::now();
    ++status.waitingWriteLock;
    shard.writeCond.wait(lock, [&] { return shard.map[id].readyToWriteLock(); });
    --status.waitingWriteLock;
    auto elapsed = std::chrono::steady_clock::now() - start;
    writeLockWaitingLatency.addSample(elapsed);
  }
  status.writeLock();
  return LockGuard(this, id, LockType::WRITE);
}

ReentrantLockManager::LockGuard ReentrantLockManager::tryWriteLock(uint64_t id) {
  auto &shard = getShard(id);
  auto lock = std::unique_lock(shard.mutex);
  auto &status = shard.map[id];

  if (status.readyToWriteLock()) {
    status.writeLock();
    return LockGuard(this, id, LockType::WRITE);
  }
  return LockGuard(this, id, LockType::NONE);
}

void ReentrantLockManager::writeUnlock(uint64_t id) {
  auto &shard = getShard(id);
  auto lock = std::unique_lock(shard.mutex);
  auto &status = shard.map[id];

  status.writeUnlock();
  assert(status.currentLock == 0);
  if (status.readyToErase()) {
    shard.map.erase(id);
  } else if (status.wakeUpWaitingWriteLock()) {
    shard.writeCond.notify_all();
    wakeUpWaitingWriteLockTimes.addSample(1);
  } else if (status.wakeUpWaitingReadLock()) {
    shard.readCond.notify_all();
    wakeUpWaitingReadLockTimes.addSample(1);
  }
}

}  // namespace hf3fs
