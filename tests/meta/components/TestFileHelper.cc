#include <chrono>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Math.h>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest-param-test.h>
#include <gtest/gtest.h>
#include <limits>
#include <map>
#include <vector>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "gtest/gtest.h"
#include "meta/components/GcManager.h"
#include "meta/components/SessionManager.h"
#include "meta/store/Inode.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestFileHelper : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestFileHelper, KVTypes);

TYPED_TEST(TestFileHelper, sync) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &fileHelper = cluster.meta().getFileHelper();
    auto &storage = cluster.meta().getStorageClient();

    for (size_t i = 0; i < 100; i++) {
      auto result = co_await meta.create({SUPER_USER, fmt::format("file-{}", i), std::nullopt, O_RDONLY, p644});
      CO_ASSERT_OK(result);

      auto inode = Inode(result->stat);
      auto offset = folly::Random::rand64(100ULL << 30);
      auto length = folly::Random::rand64(16ULL << 20);
      co_await randomWrite(meta, storage, inode, offset, length);
      size_t total = offset + length;

      FAULT_INJECTION_SET(10, 10);
      while (true) {
        auto stat = co_await meta.stat({SUPER_USER, inode.id, AtFlags()});
        CO_ASSERT_OK(stat);
        auto result = co_await fileHelper.queryLength({}, stat->stat, nullptr);
        if (!result.hasError()) {
          CO_ASSERT_EQ(*result, total);
          break;
        }
      }
    }
  }());
}
TYPED_TEST(TestFileHelper, remove) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &fileHelper = cluster.meta().getFileHelper();
    auto &storage = cluster.meta().getStorageClient();
    auto mgmtd = cluster.mgmtdClient();

    struct File {
      Inode inode;
      size_t chunks = 0;
      size_t length = 0;
    };
    std::map<size_t, File> files;
    for (size_t i = 0; i < 10; i++) {
      auto result = co_await meta.create({SUPER_USER, fmt::format("file-{}", i), std::nullopt, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      File file{result->stat};
      file.inode.asFile().length = folly::Random::rand64(100ULL << 30);
      file.inode.asFile().truncateVer = folly::Random::rand64();
      std::set<size_t> chunks;
      for (size_t i = 0; i < 10; i++) {
        auto offset = folly::Random::rand64(100ULL << 30);
        auto length = folly::Random::rand64(16ULL << 20);
        co_await randomWrite(meta, storage, file.inode, offset, length);
        size_t firstChunk = offset / file.inode.asFile().layout.chunkSize.u64();
        size_t lastChunk = folly::divCeil(offset + length, file.inode.asFile().layout.chunkSize.u64());
        for (size_t chunk = firstChunk; chunk < lastChunk; chunk++) {
          if (chunks.contains(chunk)) {
            continue;
          }
          chunks.insert(chunk);
          file.chunks++;
        }
        file.length = std::max(file.length, offset + length);
      }
      files[i] = file;
    }

    size_t total = 0;
    for (auto &[i, file] : files) {
      total += file.chunks;
    }
    auto query = co_await queryTotalChunks(storage, *mgmtd);
    CO_ASSERT_OK(query);
    CO_ASSERT_EQ(*query, total);

    for (size_t i = 0; i < 10; i++) {
      if (folly::Random::oneIn(2)) {
        continue;
      }
      while (true) {
        FAULT_INJECTION_SET(20, 10);
        files[i].inode.asFile().dynStripe = 0;
        auto result = co_await fileHelper.remove({}, files[i].inode, {}, folly::Random::rand32(8, 16));
        if (!result.hasError()) {
          break;
        }
      }
      total -= files[i].chunks;
      auto query = co_await queryTotalChunks(storage, *mgmtd);
      CO_ASSERT_OK(query);
      CO_ASSERT_EQ(*query, total);
    }
  }());
}
}  // namespace hf3fs::meta::server