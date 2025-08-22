#include "SimpleSemaphore.h"

#include <folly/logging/xlog.h>

namespace hf3fs {
struct SimpleSemaphore::Waiter {
  Waiter *next = nullptr;
  Waiter *prev = nullptr;
  std::condition_variable cv;
  bool done = false;

  static Waiter *add(Waiter *head, Waiter *waiter) {
    if (!head) {
      waiter->next = waiter;
      waiter->prev = waiter;
      return waiter;
    }
    auto *p = head->prev;
    waiter->next = head;
    waiter->prev = p;
    p->next = waiter;
    head->prev = waiter;
    return head;
  }

  static Waiter *remove(Waiter *head, Waiter *waiter) {
    if (waiter->next == nullptr) {
      return head;
    }
    if (waiter->next == waiter) {
      // list only contains waiter, new head is nullptr after remove
      head = nullptr;
    } else {
      // remove waiter from list
      auto *p = waiter->prev;
      auto *n = waiter->next;
      p->next = n;
      n->prev = p;
      if (head == waiter) {
        head = n;
      }
    }
    // reset next and prev to distinguish dangling waiter
    waiter->next = nullptr;
    waiter->prev = nullptr;
    return head;
  }
};

SimpleSemaphore::SimpleSemaphore([[maybe_unused]] Options opts) {}

bool SimpleSemaphore::post() {
  auto lock = std::unique_lock(mu_);
  if (auto *node = head; node) {
    head = Waiter::remove(head, node);
    node->done = true;
    node->cv.notify_one();
  } else {
    ++count_;
  }
  return true;
}

bool SimpleSemaphore::fastWait() {
  if (!head && count_ > 0) {
    --count_;
    return true;
  }
  return false;
}

void SimpleSemaphore::wait() {
  Waiter waiter;
  auto lock = std::unique_lock(mu_);
  if (!fastWait()) {
    head = Waiter::add(head, &waiter);
    waiter.cv.wait(lock, [&waiter] { return waiter.done; });
    head = Waiter::remove(head, &waiter);
  }
}

bool SimpleSemaphore::try_wait() {
  auto lock = std::unique_lock(mu_);
  return fastWait();
}

bool SimpleSemaphore::try_wait_for(std::chrono::milliseconds time) {
  Waiter waiter;
  auto lock = std::unique_lock(mu_);
  if (fastWait()) {
    return true;
  }
  head = Waiter::add(head, &waiter);
  waiter.cv.wait_for(lock, time, [&waiter] { return waiter.done; });
  head = Waiter::remove(head, &waiter);
  return waiter.done;
}
}  // namespace hf3fs
