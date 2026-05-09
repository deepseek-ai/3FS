#include <atomic>
#include <chrono>
#include <cstdlib>
#include <double-conversion/utils.h>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <variant>
#include <vector>

#include "common/kv/IKVEngine.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "meta/event/Scan.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

using InodeMap = folly::ConcurrentHashMap<InodeId, Inode>;
using DirEntryMap = folly::ConcurrentHashMap<InodeId, DirEntry>;

template <typename KV>
class TestScan : public MetaTestBase<KV> {
 protected:
  template <typename Map>
  void create(Map &map) {
    folly::CPUThreadPoolExecutor exec(8);
    std::vector<folly::SemiFuture<Void>> tasks;
    for (size_t i = 0; i < 8; i++) {
      auto task = folly::coro::co_invoke([&]() -> CoTask<void> {
        for (size_t i = 0; i < (1 << 10); i++) {
          READ_WRITE_TRANSACTION_OK({
            if constexpr (std::is_same_v<Map, InodeMap>) {
              auto &inodes = map;
              for (size_t j = 0; j < folly::Random::rand32(8, 12); j++) {
                auto inode = MetaTestHelper::randomInode();
                CO_ASSERT_OK(co_await inode.store(*txn));
                CO_ASSERT_TRUE(inodes.insert(inode.id, inode).second);
              }
            } else {
              auto &entries = map;
              for (size_t j = 0; j < 10; j++) {
                auto entry = MetaTestHelper::randomDirEntry();
                CO_ASSERT_OK(co_await entry.store(*txn));
                CO_ASSERT_TRUE(entries.insert(entry.id, entry).second);
              }
            }
          });
        }
        co_return;
      });
      tasks.push_back(std::move(task).scheduleOn(&exec).start());
    }
    folly::coro::collectAllRange(std::move(tasks)).semi().wait();
  }
};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestScan, KVTypes);

TYPED_TEST(TestScan, Inode) {
  folly::ConcurrentHashMap<InodeId, Inode> allInodes;

  // create some inodes
  auto beginCreate = std::chrono::steady_clock::now();
  this->create(allInodes);

  auto beginScan = std::chrono::steady_clock::now();
  MetaScan scan(MetaScan::Options(), this->kvEngine());

  std::atomic_uint64_t scanned{0};
  folly::ConcurrentHashMap<InodeId, Void> scannedInodes;
  while (true) {
    auto inodes = scan.getInodes();
    if (inodes.empty()) {
      break;
    }
    for (auto &inode : inodes) {
      scanned.fetch_add(1);
      ASSERT_EQ(inode, allInodes[inode.id]);
      ASSERT_TRUE(scannedInodes.insert(inode.id, Void{}).second);
    }
  }

  ASSERT_EQ(allInodes.size(), scanned.load());
  ASSERT_EQ(scanned.load(), scannedInodes.size());

  auto now = std::chrono::steady_clock::now();
  fmt::print("create {}ms, scan {}ms, total {}\n",
             std::chrono::duration_cast<std::chrono::milliseconds>(beginScan - beginCreate).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(now - beginScan).count(),
             scanned.load());
}

TYPED_TEST(TestScan, DirEntry) {
  folly::ConcurrentHashMap<InodeId, DirEntry> allEntries;

  // create some inodes
  auto beginCreate = std::chrono::steady_clock::now();
  this->create(allEntries);

  auto beginScan = std::chrono::steady_clock::now();
  MetaScan scan(MetaScan::Options(), this->kvEngine());

  std::atomic_uint64_t scanned{0};
  folly::ConcurrentHashMap<InodeId, DirEntry> scannedEntries;
  while (true) {
    auto entries = scan.getDirEntries();
    if (entries.empty()) {
      break;
    }
    for (auto &entry : entries) {
      scanned.fetch_add(1);
      ASSERT_EQ(entry, allEntries[entry.id]);
      ASSERT_TRUE(scannedEntries.insert(entry.id, entry).second);
    }
  }

  ASSERT_EQ(allEntries.size(), scanned.load());
  ASSERT_EQ(scanned.load(), scannedEntries.size());

  auto now = std::chrono::steady_clock::now();
  fmt::print("create {}ms, scan {}ms, total {}\n",
             std::chrono::duration_cast<std::chrono::milliseconds>(beginScan - beginCreate).count(),
             std::chrono::duration_cast<std::chrono::milliseconds>(now - beginScan).count(),
             scanned.load());
}

TYPED_TEST(TestScan, Exit) {
  folly::ConcurrentHashMap<InodeId, DirEntry> allEntries;
  this->create(allEntries);

  // start scan but exit without consume all entries.
  MetaScan scan(MetaScan::Options(), this->kvEngine());
  auto entries = scan.getDirEntries();
  ASSERT_FALSE(entries.empty());
}

}  // namespace hf3fs::meta::server
