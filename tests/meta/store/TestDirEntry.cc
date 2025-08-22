#include <cstdlib>
#include <fmt/core.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <vector>

#include "common/kv/IKVEngine.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Coroutine.h"
#include "common/utils/String.h"
#include "common/utils/UtcTime.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
using namespace hf3fs::kv;

class TestDirEntry : public ::testing::Test {
 protected:
  MemKVEngine engine_;
};

const Acl acl(Uid(0), Gid(0), Permission(0777));

TEST_F(TestDirEntry, LoadStore) {
  srand(time(nullptr));

  folly::coro::blockingWait([&]() -> CoTask<void> {
    static constexpr size_t kDirEntries = 1000;
    std::vector<DirEntry> entries;
    for (size_t i = 0; i < kDirEntries; i++) {
      auto entry = MetaTestHelper::randomDirEntry();
      auto storeTxn = engine_.createReadWriteTransaction();
      auto storeResult = co_await entry.store(*storeTxn);
      CO_ASSERT_TRUE(storeResult.hasValue());
      auto commitResult = co_await storeTxn->commit();
      CO_ASSERT_OK(commitResult);

      entries.push_back(std::move(entry));
    }

    auto loadTxn = engine_.createReadonlyTransaction();
    for (auto &entry : entries) {
      auto loadResult =
          (co_await DirEntry::snapshotLoad(*loadTxn, entry.parent, entry.name)).then(checkMetaFound<DirEntry>);
      CO_ASSERT_OK(loadResult);
      CO_ASSERT_EQ(entry, *loadResult);
    }

    for (auto &entry : entries) {
      auto removeTxn = engine_.createReadWriteTransaction();
      auto removeResult = co_await entry.remove(*removeTxn);
      CO_ASSERT_TRUE(removeResult.hasValue());
      auto commitResult = co_await removeTxn->commit();
      CO_ASSERT_TRUE(commitResult.hasValue());
    }

    for (auto &entry : entries) {
      DirEntry other(entry.parent, entry.name);
      auto loadTxn = engine_.createReadonlyTransaction();
      auto loadResult =
          (co_await DirEntry::snapshotLoad(*loadTxn, entry.parent, entry.name)).then(checkMetaFound<DirEntry>);
      CO_ASSERT_ERROR(loadResult, MetaCode::kNotFound);
    }
  }());
}

TEST_F(TestDirEntry, List) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    std::vector<DirEntry> entries;
    InodeId parent = MetaTestHelper::randomInodeId();
    for (size_t i = 0; i < 100; i++) {
      String name = fmt::format("file-{}", i);
      InodeId id = MetaTestHelper::randomInodeId();
      DirEntry entry = DirEntry::newFile(parent, name, id);

      auto storeTxn = engine_.createReadWriteTransaction();
      auto storeResult = co_await entry.store(*storeTxn);
      CO_ASSERT_TRUE(storeResult.hasValue());

      auto commitResult = co_await storeTxn->commit();
      CO_ASSERT_OK(commitResult);
    }

    auto loadTxn = engine_.createReadonlyTransaction();
    auto listResult = co_await DirEntryList::snapshotLoad(*loadTxn, parent, {}, 10, false);
    CO_ASSERT_TRUE(listResult.hasValue());
    auto list = listResult.value();
    CO_ASSERT_EQ(list.entries.size(), 10);
    CO_ASSERT_TRUE(list.inodes.empty());
    CO_ASSERT_TRUE(list.more);

    // load last entries
    listResult = co_await DirEntryList::snapshotLoad(*loadTxn, parent, list.entries.at(9).name, 90, false);
    CO_ASSERT_TRUE(listResult.hasValue());
    list = listResult.value();
    CO_ASSERT_EQ(list.entries.size(), 90);
    CO_ASSERT_TRUE(list.inodes.empty());
    CO_ASSERT_FALSE(list.more);
  }());
}

TEST_F(TestDirEntry, Conflict) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    for (int i = 0; i < 1000; i++) {
      // txn1 update entry, txn2 include entry into it's read conflict set (by load or addReadConflict) and set a random
      // key, txn1 commit first, txn2 should fail
      DirEntry entry(MetaTestHelper::randomInodeId(), std::to_string(i));
      entry.id = MetaTestHelper::randomInodeId();
      auto txn1 = engine_.createReadWriteTransaction();
      auto txn2 = engine_.createReadWriteTransaction();

      auto store = co_await entry.store(*txn1);
      CO_ASSERT_OK(store);

      if (rand() % 2) {
        CO_ASSERT_OK(co_await DirEntry::load(*txn2, entry.parent, entry.name));
      } else {
        auto addConflict = co_await entry.addIntoReadConflict(*txn2);
        CO_ASSERT_OK(addConflict);
      }
      auto set = co_await txn2->set(std::to_string(rand()), std::to_string(rand()));
      CO_ASSERT_OK(set);

      auto txn1Result = co_await txn1->commit();
      CO_ASSERT_OK(txn1Result);
      auto txn2Result = co_await txn2->commit();
      CO_ASSERT_TRUE(txn2Result.hasError());
      CO_ASSERT_EQ(txn2Result.error().code(), TransactionCode::kConflict);
    }
  }());
}

}  // namespace hf3fs::meta::server
