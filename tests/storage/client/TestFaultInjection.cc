#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/logging/xlog.h>

#include "benchmarks/storage_bench/StorageBench.h"
#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/net/Client.h"
#include "tests/GtestHelpers.h"
#include "tests/lib/UnitTestFabric.h"

namespace hf3fs::storage::client {
namespace {

using namespace hf3fs::test;

class TestFaultInjection : public ::testing::Test {
 protected:
  void SetUp() override {
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
    GTEST_SKIP() << "Skipping all tests since thread sanitizer enabled";
#endif
#endif

    // init ib device
    net::IBDevice::Config ibConfig;
    auto ibResult = net::IBManager::start(ibConfig);
    ASSERT_OK(ibResult);
  }

  void TearDown() override {}

 protected:
  test::SystemSetupConfig setupConfig_ = {
      128_KB /*chunkSize*/,
      3 /*numChains*/,
      2 /*numReplicas*/,
      3 /*numStorageNodes*/,
      {folly::fs::temp_directory_path()} /*dataPaths*/,
  };
};

TEST_F(TestFaultInjection, ServerError) {
  benchmark::StorageBench::Options benchOptions{
      32 /*numChunks*/,
      64_KB /*readSize*/,
      64_KB /*writeSize*/,
      16 /*batchSize*/,
      3 /*numReadSecs*/,
      3 /*numWriteSecs*/,
      300000 /*clientTimeoutMS*/,
      64 /*numCoroutines*/,
      32 /*numTestThreads*/,
      0 /*randSeed*/,
      0xFFFF /*chunkIdPrefix*/,
      false /*benchmarkNetwork*/,
      false /*benchmarkStorage*/,
      false /*ignoreIOError*/,
      true /*injectRandomServerError*/,
      false /*injectRandomClientError*/,
      true /*retryPermanentError*/,
      true /*verifyReadData*/,
      true /*verifyReadChecksum*/,
  };

  benchmark::StorageBench bench(setupConfig_, benchOptions);
  ASSERT_TRUE(bench.setup());
  bench.generateChunkIds();

  ASSERT_TRUE(bench.run());
  ASSERT_EQ(StatusCode::kOK, bench.truncate());
  ASSERT_EQ(StatusCode::kOK, bench.cleanup());
  bench.teardown();
}

TEST_F(TestFaultInjection, ClientError) {
  benchmark::StorageBench::Options benchOptions{
      32 /*numChunks*/,
      64_KB /*readSize*/,
      64_KB /*writeSize*/,
      16 /*batchSize*/,
      3 /*numReadSecs*/,
      3 /*numWriteSecs*/,
      300000 /*clientTimeoutMS*/,
      64 /*numCoroutines*/,
      32 /*numTestThreads*/,
      0 /*randSeed*/,
      0xFFFF /*chunkIdPrefix*/,
      false /*benchmarkNetwork*/,
      false /*benchmarkStorage*/,
      false /*ignoreIOError*/,
      false /*injectRandomServerError*/,
      true /*injectRandomClientError*/,
      true /*retryPermanentError*/,
      true /*verifyReadData*/,
      true /*verifyReadChecksum*/,
  };

  benchmark::StorageBench bench(setupConfig_, benchOptions);
  ASSERT_TRUE(bench.setup());
  bench.generateChunkIds();

  ASSERT_TRUE(bench.run());
  ASSERT_EQ(StatusCode::kOK, bench.truncate());
  ASSERT_EQ(StatusCode::kOK, bench.cleanup());
  bench.teardown();
}

}  // namespace
}  // namespace hf3fs::storage::client
