#include <gtest/gtest.h>

#include "common/net/EventLoop.h"
#include "common/net/Network.h"
#include "common/utils/FdWrapper.h"

namespace hf3fs::net::test {
namespace {

TEST(TestEventLoop, Normal) {
  auto eventLoop = EventLoop::create();
  ASSERT_TRUE(eventLoop->start());
  eventLoop->stopAndJoin();
}

TEST(TestEventLoop, AddObject) {
  auto eventLoop = EventLoop::create();
  ASSERT_TRUE(eventLoop->start());

  class PipeReader : public EventLoop::EventHandler {
   public:
    PipeReader(int fd)
        : fd_(fd) {}

    int fd() const final { return fd_; }
    void handleEvents(uint32_t epollEvents) final {
      ASSERT_TRUE(epollEvents & EPOLLIN);
      uint32_t val;
      ASSERT_EQ(::read(fd_, &val, sizeof(val)), sizeof(val));
      val_ = val;
    }

    auto &val() const { return val_; }

   private:
    FdWrapper fd_;
    std::atomic<uint32_t> val_{0};
  };

  int p[2];
  ASSERT_EQ(::pipe(p), 0);
  auto pipeReader = std::make_shared<PipeReader>(p[0]);
  ASSERT_TRUE(eventLoop->add(pipeReader, EPOLLIN | EPOLLET));
  ASSERT_EQ(pipeReader->val().load(), 0);

  uint32_t val = rand();
  ASSERT_EQ(::write(p[1], &val, sizeof(val)), sizeof(val));
  while (pipeReader->val().load() == 0) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_EQ(pipeReader->val().load(), val);

  ASSERT_TRUE(eventLoop->remove(pipeReader.get()));
  std::this_thread::sleep_for(10ms);
}

}  // namespace
}  // namespace hf3fs::net::test
