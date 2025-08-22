#include <algorithm>
#include <fcntl.h>
#include <folly/Random.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/futures/ManualTimekeeper.h>
#include <gtest/gtest.h>
#include <optional>
#include <vector>

#include "common/utils/FaultInjection.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "meta/store/MetaStore.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
template <typename KV>
class TestUtimes : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestUtimes, KVTypes);

TYPED_TEST(TestUtimes, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p777}));

    auto result = co_await meta.stat({SUPER_USER, "file", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(result);
    auto &inode = result->stat;

    auto test =
        [&](const flat::UserInfo &user, PathAt path, UtcTime atime, UtcTime mtime, uint64_t error = 0) -> CoTask<void> {
      auto gran = cluster.config().mock_meta().time_granularity();
      atime = (atime == SETATTR_TIME_NOW) ? SETATTR_TIME_NOW : atime.castGranularity(gran);
      mtime = (atime == SETATTR_TIME_NOW) ? SETATTR_TIME_NOW : mtime.castGranularity(gran);
      auto before = UtcClock::now().castGranularity(gran);
      auto result = co_await meta.setAttr(SetAttrReq::utimes(user, path, AtFlags(0), atime, mtime));
      if (error) {
        CO_ASSERT_ERROR(result, error);
        co_return;
      } else {
        CO_ASSERT_OK(result);
      }

      auto sresult = co_await meta.stat({SUPER_USER, "file", AtFlags(AT_SYMLINK_FOLLOW)});
      CO_ASSERT_OK(sresult);
      auto &inode = sresult->stat;
      if (atime != SETATTR_TIME_NOW) {
        CO_ASSERT_EQ(inode.atime, atime) << fmt::format("{} {}", inode.atime, atime);
      } else {
        CO_ASSERT_GE(inode.atime, before);
      }
      if (mtime != SETATTR_TIME_NOW) {
        CO_ASSERT_EQ(inode.mtime, mtime) << fmt::format("{} {}", inode.mtime, mtime);
      } else {
        CO_ASSERT_GE(inode.mtime, before);
      }
    };

    co_await test(SUPER_USER, "file", UtcClock::now(), UtcClock::now());
    co_await test(SUPER_USER, "file", SETATTR_TIME_NOW, SETATTR_TIME_NOW);
    co_await test(SUPER_USER, inode.id, UtcClock::now(), UtcClock::now());
    co_await test(SUPER_USER, inode.id, SETATTR_TIME_NOW, SETATTR_TIME_NOW);

    flat::UserInfo ua(Uid(1), Gid(1), String());
    co_await test(ua, "file", SETATTR_TIME_NOW, SETATTR_TIME_NOW);
    co_await test(ua, "file", UtcClock::now(), UtcClock::now(), MetaCode::kNoPermission);
    co_await test(ua, inode.id, SETATTR_TIME_NOW, SETATTR_TIME_NOW);
    co_await test(ua, inode.id, UtcClock::now(), UtcClock::now(), MetaCode::kNoPermission);

    CO_ASSERT_OK(
        co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, "file", AtFlags(0), {}, {}, std::optional(p700))));

    co_await test(ua, "file", SETATTR_TIME_NOW, SETATTR_TIME_NOW, MetaCode::kNoPermission);
    co_await test(ua, "file", UtcClock::now(), UtcClock::now(), MetaCode::kNoPermission);
    co_await test(ua, inode.id, SETATTR_TIME_NOW, SETATTR_TIME_NOW, MetaCode::kNoPermission);
    co_await test(ua, inode.id, UtcClock::now(), UtcClock::now(), MetaCode::kNoPermission);
  }());
}

TYPED_TEST(TestUtimes, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();

    // create file, directory and symlink
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p777}));

    auto result = co_await meta.stat({SUPER_USER, "file", AtFlags(AT_SYMLINK_FOLLOW)});
    CO_ASSERT_OK(result);
    auto fileId = result->stat.id;

    CHECK_CONFLICT_SET(
        [&](auto &txn) -> CoTask<void> {
          auto req = SetAttrReq::utimes(SUPER_USER,
                                        "file",
                                        AtFlags(0),
                                        UtcTime::fromMicroseconds(folly::Random::rand32()),
                                        UtcTime::fromMicroseconds(folly::Random::rand32()));
          CO_ASSERT_OK(co_await store.setAttr(req)->run(txn));
        },
        (std::vector<String>{
            MetaTestHelper::getInodeKey(fileId),  // inode
        }),
        (std::vector<String>{
            MetaTestHelper::getInodeKey(fileId),  // inode
        }),
        true);
  }());
}
}  // namespace hf3fs::meta::server
