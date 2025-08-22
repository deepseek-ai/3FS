#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <folly/Random.h>
#include <folly/Synchronized.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <folly/futures/Barrier.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "core/user/UserStore.h"
#include "fbs/meta/Schema.h"
#include "fbs/storage/Common.h"
#include "gtest/gtest.h"
#include "meta/components/SessionManager.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestAuth : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestAuth, KVTypes);

TYPED_TEST(TestAuth, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().set_authenticate(true);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    flat::Uid rootId(0), aliceId(1), bobId(2), charlieId(3);
    UserAttr root, alic, bob, charlie;
    READ_WRITE_TRANSACTION_OK({
      core::UserStore userStore;
      root = (co_await userStore.addUser(*txn, rootId, "root", {}, true)).value();
      alic = (co_await userStore.addUser(*txn, aliceId, "alic", {flat::Gid(2), flat::Gid(1001)}, true)).value();
      bob = (co_await userStore.addUser(*txn, bobId, "bob", {flat::Gid(1001)}, false)).value();
      charlie = (co_await userStore.addUser(*txn, charlieId, "charlie", {}, false)).value();
    });

    std::vector<std::pair<UserInfo, UserInfo>> succ{
        // root & sudoer can auth with any uid that exists
        {{rootId, root.gid, root.token}, {rootId, root.gid, root.groups, root.token}},
        {{aliceId, alic.gid, root.token}, {aliceId, alic.gid, alic.groups, root.token}},
        {{bobId, bob.gid, root.token}, {bobId, bob.gid, bob.groups, root.token}},
        {{bobId, charlie.gid, root.token}, {bobId, charlie.gid, bob.groups, root.token}},
        {{rootId, root.gid, alic.token}, {rootId, root.gid, root.groups, alic.token}},
        {{aliceId, alic.gid, alic.token}, {aliceId, alic.gid, alic.groups, alic.token}},
        {{bobId, bob.gid, alic.token}, {bobId, bob.gid, bob.groups, alic.token}},
        {{bobId, charlie.gid, alic.token}, {bobId, charlie.gid, bob.groups, alic.token}},
        // other user can only auth with self uid and gid
        {{bobId, bob.gid, bob.token}, {bobId, bob.gid, bob.groups, bob.token}},
        {{charlieId, charlie.gid, charlie.token}, {charlieId, charlie.gid, charlie.groups, charlie.token}},
    };
    for (auto [req, expected] : succ) {
      auto result = co_await meta.authenticate(AuthReq(req));
      CO_ASSERT_OK(result);
      const auto &actual = result->user;
      CO_ASSERT_EQ(actual, expected) << fmt::format("{} vs {}; ", actual, expected)
                                     << fmt::format("{} vs {}",
                                                    fmt::join(actual.groups.begin(), actual.groups.end(), ","),
                                                    fmt::join(expected.groups.begin(), expected.groups.end(), ","));
    }

    std::vector<UserInfo> fail{// don't allow unknown uid
                               {flat::Uid(999), flat::Gid(0), root.token},
                               {flat::Uid(1024), flat::Gid(0), root.token},
                               // other user can only auth with self uid and gid
                               {bobId, charlie.gid, bob.token},
                               {charlieId, bob.gid, bob.token},
                               {charlieId, bob.gid, charlie.token},
                               {bobId, charlie.gid, charlie.token},
                               // invalid token
                               {bobId, bob.gid, ""}};
    for (const auto &req : fail) {
      fmt::print("{}\n", req);
      auto result = co_await meta.authenticate(AuthReq(req));
      CO_ASSERT_ERROR(result, StatusCode::kAuthenticationFail);
    }
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
