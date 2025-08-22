#include <folly/net/NetworkSocket.h>
#include <gtest/gtest.h>

#include "common/net/IOWorker.h"
#include "common/net/Network.h"
#include "common/net/Processor.h"
#include "common/net/Transport.h"

namespace hf3fs::net {

TEST(TestIOWorker, Normal) {
  CPUExecutorGroup procExecutor(2, "");
  CPUExecutorGroup ioExecutor(2, "");
  folly::IOThreadPoolExecutor connExecutor(2);
  serde::Services serdeServices;
  Processor::Config processorConfig{};
  Processor processor(serdeServices, procExecutor, processorConfig);

  IOWorker::Config ioWorkerConfig{};
  IOWorker ioWorker(processor, ioExecutor, connExecutor, ioWorkerConfig);

  ASSERT_TRUE(ioWorker.start("Test"));
}

}  // namespace hf3fs::net
