#include "common/net/WriteItem.h"

#include <atomic>
#include <utility>

#include "common/monitor/Recorder.h"
#include "common/net/Transport.h"
#include "common/net/Waiter.h"

namespace hf3fs::net {

monitor::CountRecorder toIOVecCPUTime{"common_net_to_io_vec_cpu_time"};
monitor::CountRecorder advanceCPUTime{"common_net_advance_cpu_time"};

void WriteList::clear() {
  while (head_ != nullptr) {
    if (head_->isReq()) {
      Waiter::instance().sendFail(head_->uuid);
    }
    WriteItemPtr old{head_};
    head_ = head_->next.load(std::memory_order_acquire);
  }
  tail_ = nullptr;
}

WriteList WriteList::extractForRetry() {
  WriteItem *newHead = nullptr;
  WriteItem *newTail = nullptr;

  while (head_ != nullptr) {
    auto next = head_->next.load(std::memory_order_acquire);
    if (head_->isReq() && head_->retryTimes++ < head_->maxRetryTimes) {
      Waiter::instance().setTransport(head_->uuid, nullptr);
      if (newTail == nullptr) {
        newHead = newTail = head_;
      } else {
        newTail->next.store(head_, std::memory_order_release);
        newTail = head_;
      }
    } else {
      if (head_->isReq()) {
        Waiter::instance().sendFail(head_->uuid);
      }
      WriteItemPtr old{head_};
    }
    head_ = next;
  }

  tail_ = nullptr;
  if (newTail) {
    newTail->next.store(nullptr, std::memory_order_release);
  }
  return WriteList{newHead, newTail};
}

void WriteList::setTransport(std::shared_ptr<Transport> tr) {
  WriteItem newHead;
  tail_ = &newHead;

  while (head_ != nullptr) {
    auto next = head_->next.load(std::memory_order_acquire);
    if (!head_->isReq() || Waiter::instance().setTransport(head_->uuid, tr)) {
      // keep it.
      tail_->next.store(head_, std::memory_order_release);
      tail_ = head_;
    } else {
      // remove it.
      WriteItemPtr old(head_);
    }
    head_ = next;
  }

  head_ = newHead.next.load(std::memory_order_acquire);
  tail_->next.store(nullptr, std::memory_order_release);
  if (tail_ == &newHead) {
    tail_ = nullptr;
  }
}

uint32_t WriteListWithProgress::toIOVec(struct iovec *iovec, uint32_t len, size_t &size) {
  auto guard = folly::makeGuard([startTime = std::chrono::steady_clock::now()] {
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    toIOVecCPUTime.addSample(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  });

  uint32_t n = 0;
  uint32_t offset = firstOffset_;
  size = 0;
  for (auto p = head_; p != nullptr && n < len; p = p->next.load(std::memory_order_acquire)) {
    auto &iov = iovec[n++];
    iov.iov_base = p->buf->buff() + offset;
    size += iov.iov_len = p->buf->length() - offset;
    offset = 0;
  }
  return n;
}

void WriteListWithProgress::advance(size_t written) {
  auto guard = folly::makeGuard([startTime = std::chrono::steady_clock::now()] {
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    advanceCPUTime.addSample(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  });

  written += firstOffset_;
  while (head_ != nullptr && written > 0) {
    auto currentSize = head_->buf->length();
    if (written >= currentSize) {
      written -= currentSize;
      WriteItemPtr old{head_};
      head_ = head_->next.load(std::memory_order_acquire);
    } else {
      firstOffset_ = written;
      return;
    }
  }
  firstOffset_ = 0;
}

constexpr uintptr_t kUnconnectedState = ~0UL;

void MPSCWriteList::add(WriteList list) {
  list.tail_->next.store(reinterpret_cast<WriteItem *>(kUnconnectedState), std::memory_order_release);
  auto prevHead = head_.exchange(std::exchange(list.head_, nullptr), std::memory_order_acq_rel);
  std::exchange(list.tail_, nullptr)->next.store(prevHead, std::memory_order_release);
}

WriteList MPSCWriteList::takeOut() {
  auto item = head_.exchange(nullptr, std::memory_order_acq_rel);
  WriteItem *tail = item;
  WriteItem *prev = nullptr;

  // reverse the list.
  while (item) {
    WriteItem *next;
    [&] {
      for (;;) {
        next = item->next.load(std::memory_order_acquire);
        if (reinterpret_cast<uintptr_t>(next) != kUnconnectedState) return;
        for (int n = 1; n < 1024; n <<= 1) {
          for (int i = 0; i < n; ++i) {
            folly::asm_volatile_pause();
          }
          next = item->next.load(std::memory_order_acquire);
          if (reinterpret_cast<uintptr_t>(next) != kUnconnectedState) return;
        }
        std::this_thread::yield();
      }
    }();

    item->next.store(prev, std::memory_order_release);
    prev = item;
    item = next;
  }
  return WriteList{prev, tail};
}

}  // namespace hf3fs::net
