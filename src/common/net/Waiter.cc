#include "common/net/Waiter.h"

namespace hf3fs::net {

static int64_t nowMS() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

Waiter &Waiter::instance() {
  static Waiter w;
  return w;
}

Waiter::Waiter() {
  thread_ = std::jthread(&Waiter::run, this);
  folly::setThreadName(thread_.get_id(), "Timer");
}

Waiter::~Waiter() {
  stop_.store(true, std::memory_order_relaxed);
  {
    std::unique_lock lock(mutex_);
    nearestRunTime_ = 0;
  }
  cond_.notify_one();
  thread_ = std::jthread{};
}

bool Waiter::TaskShard::schedule(uint64_t uuid, int64_t runTime) {
  std::unique_lock lock(mutex_);
  tasks_.emplace_back(Task{uuid, runTime});
  if (runTime < nearestRunTime_) {
    nearestRunTime_ = runTime;
    return true;
  }
  return false;
}

void Waiter::TaskShard::exchangeTasks(std::vector<Task> &out) {
  std::unique_lock lock(mutex_);
  if (!tasks_.empty()) {
    out.swap(tasks_);
    nearestRunTime_ = std::numeric_limits<int64_t>::max();
  }
}

void Waiter::schedule(uint64_t uuid, std::chrono::milliseconds timeout) {
  const int64_t runTime = nowMS() + timeout.count();
  auto idx = std::hash<std::thread::id>{}(std::this_thread::get_id()) % taskShards_.size();
  auto earlier = taskShards_[idx].schedule(uuid, runTime);
  if (earlier) {
    std::unique_lock lock(mutex_);
    if (runTime < nearestRunTime_) {
      nearestRunTime_ = runTime;
      lock.unlock();
      cond_.notify_one();
    }
  }
}

void Waiter::run() {
  std::vector<Task> tasks;
  tasks.reserve(4096);

  while (!stop_.load(std::memory_order_relaxed)) {
    {
      std::unique_lock lock(mutex_);
      nearestRunTime_ = std::numeric_limits<int64_t>::max();
    }

    for (auto &shard : taskShards_) {
      reserved_.clear();
      if (reserved_.capacity() > 1_MB) {
        reserved_.shrink_to_fit();
      }
      shard.exchangeTasks(reserved_);

      for (auto &task : reserved_) {
        if (Waiter::instance().contains(task.uuid)) {
          tasks.push_back(task);
          std::push_heap(tasks.begin(), tasks.end());
        }
      }
    }

    bool pullAgain = false;
    while (!tasks.empty()) {
      Task task1 = tasks.front();
      if (task1.runTime > nowMS()) {
        break;
      }

      {
        std::unique_lock lock(mutex_);
        if (task1.runTime > nearestRunTime_) {
          pullAgain = true;
          break;
        }
      }

      std::pop_heap(tasks.begin(), tasks.end());
      tasks.pop_back();
      Waiter::instance().timeout(task1.uuid);
    }
    if (pullAgain) {
      continue;
    }

    int64_t nextRunTime = std::numeric_limits<int64_t>::max();
    if (!tasks.empty()) {
      nextRunTime = tasks.front().runTime;
    }

    {
      std::unique_lock lock(mutex_);
      if (nextRunTime > nearestRunTime_) {
        continue;
      } else {
        nearestRunTime_ = nextRunTime;
      }

      if (nextRunTime == std::numeric_limits<int64_t>::max()) {
        cond_.wait(lock);
      } else {
        auto now = nowMS();
        if (nextRunTime > now) {
          cond_.wait_for(lock, std::chrono::milliseconds(nextRunTime - now));
        }
      }
    }
  }
}

}  // namespace hf3fs::net
