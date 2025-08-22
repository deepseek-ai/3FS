#pragma once

#include <condition_variable>
#include <folly/Hash.h>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include "common/utils/RobinHood.h"

namespace hf3fs {

class ReentrantLockManager {
  enum class LockType { NONE, READ, WRITE };

 public:
  void init(uint32_t shardsNum = 4099u);

  class LockGuard {
   public:
    LockGuard() = default;
    LockGuard(ReentrantLockManager *locker, uint64_t id, LockType type);
    LockGuard(const LockGuard &) = delete;
    LockGuard(LockGuard &&o);
    ~LockGuard() { unlock(); }
    void unlock();
    bool locked() const { return type_ != LockType::NONE; }
    bool readLocked() const { return type_ == LockType::READ; }
    bool writeLocked() const { return type_ == LockType::WRITE; }
    LockGuard &operator=(LockGuard &&o);

   private:
    ReentrantLockManager *locker_ = nullptr;
    uint64_t id_ = 0;
    LockType type_ = LockType::NONE;
  };

  LockGuard readLock(uint64_t id);
  LockGuard tryReadLock(uint64_t id);
  void readUnlock(uint64_t id);
  LockGuard writeLock(uint64_t id);
  LockGuard tryWriteLock(uint64_t id);
  void writeUnlock(uint64_t id);

 private:
  struct Status {
    int currentLock = 0;  // positive for read locked, negative for write locked.
    int waitingReadLock = 0;
    int waitingWriteLock = 0;

    bool readyToErase() const { return currentLock == 0 && waitingReadLock == 0 && waitingWriteLock == 0; }
    bool readyToReadLock() const { return currentLock >= 0 && waitingWriteLock == 0; }
    void readLock() { ++currentLock; }
    void readUnlock() { --currentLock; }
    bool wakeUpWaitingReadLock() { return currentLock >= 0 && waitingReadLock > 0; }
    bool readyToWriteLock() const { return currentLock == 0; }
    void writeLock() { --currentLock; }
    void writeUnlock() { ++currentLock; }
    bool wakeUpWaitingWriteLock() { return currentLock == 0 && waitingWriteLock > 0; }
  };
  struct Shard {
    std::mutex mutex;
    std::condition_variable readCond;
    std::condition_variable writeCond;
    robin_hood::unordered_map<uint64_t, Status> map;
  };
  Shard &getShard(uint64_t id) { return shards_[folly::hash::twang_32from64(id) % shardsNum_]; }

 private:
  uint32_t shardsNum_;
  std::unique_ptr<Shard[]> shards_;
};

}  // namespace hf3fs
