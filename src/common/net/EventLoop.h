#pragma once

#include <folly/concurrency/UnboundedQueue.h>
#include <list>
#include <memory>
#include <sys/epoll.h>
#include <thread>

#include "common/utils/FdWrapper.h"
#include "common/utils/Result.h"
#include "common/utils/TypeTraits.h"

namespace hf3fs::net {

// EventLoop provides a main loop that notifies EventHandler callback objects when I/O is ready on a file descriptor.
class EventLoop : public hf3fs::enable_shared_from_this<EventLoop> {
  struct HandlerWrapper;

 protected:
  EventLoop() = default;

 public:
  ~EventLoop() { stopAndJoin(); }

  // start and stop.
  Result<Void> start(const std::string &threadName = "EventLoop");
  Result<Void> wakeUp();
  void stopAndJoin();

  class EventHandler {
   public:
    virtual ~EventHandler() = default;
    virtual int fd() const = 0;
    virtual void handleEvents(uint32_t epollEvents) = 0;

   protected:
    friend class EventLoop;
    std::weak_ptr<EventLoop> eventLoop_;
    std::list<HandlerWrapper>::iterator it_;
  };

  // add a event handler with interest events into event loop.
  Result<Void> add(const std::shared_ptr<EventHandler> &handler, uint32_t interestEvents);

  // remove a event handler from event loop.
  Result<Void> remove(EventHandler *handler);

 private:
  struct HandlerWrapper {
    std::weak_ptr<EventHandler> handler;
  };

  void loop();

 private:
  FdWrapper epfd_;
  FdWrapper eventfd_;

  std::atomic<bool> stop_{false};
  std::jthread thread_;

  std::mutex mutex_;
  std::list<HandlerWrapper> wrapperList_;

  // wake up the event loop to do deletion if the size of delete queue greater than this threshold.
  constexpr static size_t kDeleteQueueWakeUpLoopThreshold = 128u;
  // deletion of the wrapper object is done in the loop thread.
  folly::UMPSCQueue<std::list<HandlerWrapper>::iterator, true> deleteQueue_;
};

class EventLoopPool {
 public:
  EventLoopPool(size_t numThreads);

  // start and stop.
  Result<Void> start(const std::string &threadName);
  void stopAndJoin();

  // add a event handler with interest events into event loop.
  Result<Void> add(const std::shared_ptr<EventLoop::EventHandler> &handler, uint32_t interestEvents);

 private:
  std::vector<std::shared_ptr<EventLoop>> eventLoops_;
};

}  // namespace hf3fs::net
