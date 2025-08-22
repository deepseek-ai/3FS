#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <gtest/gtest.h>
#include <thread>

#include "common/utils/Coroutine.h"
#include "common/utils/Semaphore.h"
#include "common/utils/SemaphoreGuard.h"
#include "common/utils/Size.h"
#include "memory/common/GlobalMemoryAllocator.h"

namespace hf3fs::test {
namespace {

TEST(TestMemoryAllocator, Basic) {
  size_t totalSize = 512_MB;
  size_t blockSize = 16_KB;
  std::vector<void *> memBlocks;

  for (size_t allocatedSize = 0; allocatedSize < totalSize; allocatedSize += blockSize) {
    auto mem = memory::allocate(blockSize);
    memBlocks.push_back(mem);
  }

  char logbuf[512] = {0};
  memory::logstatus(logbuf, sizeof(logbuf));
  XLOGF(INFO, "Memory allocator log: {}", logbuf);

  for (auto mem : memBlocks) {
    memory::deallocate(mem);
  }

  memory::logstatus(logbuf, sizeof(logbuf));
  XLOGF(INFO, "Memory allocator log: {}", logbuf);
}

TEST(TestMemoryAllocator, AlignedAlloc) {
  static size_t kPageSize = sysconf(_SC_PAGESIZE);
  size_t totalSize = 512_MB;
  size_t blockSize = 100_KB;
  std::vector<void *> memBlocks;

  for (size_t allocatedSize = 0; allocatedSize < totalSize; allocatedSize += blockSize) {
    auto mem = memory::memalign(kPageSize, blockSize);
    memBlocks.push_back(mem);
  }

  char logbuf[512] = {0};
  memory::logstatus(logbuf, sizeof(logbuf));
  XLOGF(INFO, "Memory allocator log: {}", logbuf);

  for (auto mem : memBlocks) {
    memory::deallocate(mem);
  }

  memory::logstatus(logbuf, sizeof(logbuf));
  XLOGF(INFO, "Memory allocator log: {}", logbuf);
}

/* Set the following environment variables before run the profiling test
    export JE_MALLOC_CONF="prof:true,prof_active:false"
    export MEMORY_ALLOCATOR_LIB_PATH=build/src/memory/jemalloc/libjemalloc_wrapper.so
 */
TEST(TestMemoryAllocator, Profiling) {
  size_t totalSize = 512_MB;
  size_t blockSize = 16_KB;
  std::vector<void *> memBlocks;

  auto enabled = memory::profiling(true /*active*/, "prof");

  // skip this test if profiling is not supported
  if (!enabled) return;

  for (size_t allocatedSize = 0; allocatedSize < totalSize; allocatedSize += blockSize) {
    auto mem = memory::allocate(blockSize);
    memBlocks.push_back(mem);
  }

  // re-enable to dump profiling
  ASSERT_TRUE(memory::profiling(true /*active*/, nullptr));

  // disable and dump profiling
  ASSERT_TRUE(memory::profiling(false /*active*/, "prof"));

  // enable profiling
  enabled = memory::profiling(true /*active*/, "prof");

  for (auto mem : memBlocks) {
    memory::deallocate(mem);
  }

  // disable and dump profiling
  ASSERT_TRUE(memory::profiling(false /*active*/, nullptr));
}

}  // namespace
}  // namespace hf3fs::test
