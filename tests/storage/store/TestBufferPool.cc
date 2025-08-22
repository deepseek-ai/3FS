#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>

#include "common/serde/Serde.h"
#include "common/utils/Reflection.h"
#include "fbs/storage/Common.h"
#include "fbs/storage/Service.h"
#include "storage/aio/AioReadWorker.h"
#include "storage/service/BufferPool.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::storage::test {
namespace {

TEST(TestBufferPool, Normal) {
  CPUExecutorGroup executor(8, "");

  BufferPool::Config bufferPoolConfig;
  bufferPoolConfig.set_rdmabuf_count(256);
  bufferPoolConfig.set_big_rdmabuf_count(4);
  AioReadWorker::Config aioReadWorkerConfig;
  aioReadWorkerConfig.set_num_threads(8);
  for (auto i = 0; i < 8; ++i) {
    BufferPool pool(bufferPoolConfig);
    ASSERT_OK(pool.init(executor));

    AioReadWorker worker(aioReadWorkerConfig);
    ASSERT_OK(worker.start({}, pool.iovecs()));
    ASSERT_OK(worker.stopAndJoin());

    pool.clear(executor);
  }
}

TEST(TestBufferPool, BigBuffer) {
  CPUExecutorGroup executor(8, "");

  BufferPool::Config bufferPoolConfig;
  bufferPoolConfig.set_rdmabuf_count(256);
  bufferPoolConfig.set_rdmabuf_size(4_MB);
  bufferPoolConfig.set_big_rdmabuf_count(4);
  bufferPoolConfig.set_big_rdmabuf_size(64_MB);

  BufferPool pool(bufferPoolConfig);
  ASSERT_OK(pool.init(executor));

  auto guard = pool.get();
  auto result = folly::coro::blockingWait(guard.allocate(64_MB));
  ASSERT_OK(result);
  ASSERT_EQ(result->size(), 64_MB);

  pool.clear(executor);
}

}  // namespace
}  // namespace hf3fs::storage::test
