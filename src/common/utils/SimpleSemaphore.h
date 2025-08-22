#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace hf3fs {
class SimpleSemaphore {
 public:
  struct Options {};

  SimpleSemaphore(Options opts);

  bool post();
  void wait();
  bool try_wait();
  bool try_wait_for(std::chrono::milliseconds time);

 private:
  bool fastWait();

  std::mutex mu_;
  int64_t count_{0};

  struct Waiter;
  Waiter *head = nullptr;
};
}  // namespace hf3fs
