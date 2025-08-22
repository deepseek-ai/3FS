#include "ThreadPoolGroup.h"

#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/logging/xlog.h>

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"

namespace hf3fs::net {

CoTask<void> ThreadPoolGroup::collectStatsPeriodically() {
  XLOGF(DBG3, "Collecting stats of thread pool group '{}'", fullname_);
  while (true) {
    XLOGF(DBG3, "Scheduled a stats collection task for thread pool group '{}'", fullname_);
    auto result = co_await folly::coro::co_awaitTry(folly::coro::sleep(2_s));
    if (result.hasException()) {
      break;
    }
    procThreadPoolReporter_.report();
    ioThreadPoolReporter_.report();
    bgThreadPoolReporter_.report();
    connThreadPoolReporter_.report();
  }
  XLOGF(WARN, "Stop to collect stats of thread pool group '{}'", fullname_);
  co_return;
}

void ThreadPoolGroup::stopAndJoin() {
  XLOGF(WARN, "Stopping thread pool group '{}'", fullname_);
  // stop scheduled executor
  bool stopping = stopping_.exchange(true);
  if (stopping) {
    return;
  }
  cancel_.requestCancellation();
  if (future_.valid()) {
    future_.wait();
  }
  // join thread pools
  XLOGF(INFO, "thread pool group {} join procThreadPool", fullname_);
  procThreadPool_.join();
  XLOGF(INFO, "thread pool group {} join ioThreadPool", fullname_);
  ioThreadPool_.join();
  XLOGF(INFO, "thread pool group {} join bgThreadPool", fullname_);
  bgThreadPool_.join();
  XLOGF(INFO, "thread pool group {} join connThreadPool", fullname_);
  connThreadPool_.join();
  XLOGF(WARN, "All thread pools of '{}' stopped", fullname_);
}

}  // namespace hf3fs::net
