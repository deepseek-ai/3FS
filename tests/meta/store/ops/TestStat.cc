#include <fcntl.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Task.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {

template <typename KV>
class TestStat : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestStat, KVTypes);

TYPED_TEST(TestStat, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    // create a
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "a/b/c", p755, true}));

    auto result = co_await meta.stat({SUPER_USER, "a", AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(result);
    auto a = result->stat;
    CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, "b", AtFlags(AT_SYMLINK_NOFOLLOW)}), MetaCode::kNotFound);
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, PathAt(a.id, "b"), AtFlags(AT_SYMLINK_NOFOLLOW)}));

    result = co_await meta.stat({SUPER_USER, a.id, AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->stat, std::optional(a));

    // create file
    auto cresult = co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p755});
    CO_ASSERT_OK(cresult);
    auto file = cresult->stat;

    CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, PathAt(file.id, "."), AtFlags(AT_SYMLINK_NOFOLLOW)}),
                    MetaCode::kNotDirectory);

    result = co_await meta.stat({SUPER_USER, file.id, AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(result->stat, std::optional(file));
  }());
}

TYPED_TEST(TestStat, FollowSymlink) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    // create a/b/c
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "a/b/c", p755, true}));
    // symlink a/d -> a/b
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "a/d", "/a/b"}));
    // a/d is symlink but not the last symlink
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, "a/d/c", AtFlags(AT_SYMLINK_NOFOLLOW)}));
  }());
}

TYPED_TEST(TestStat, FaultInjection) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    std::vector<std::string> paths;
    for (auto i = 0; i < 10; i++) {
      std::string path = std::to_string(i);
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      paths.push_back(path);
    }

    paths.push_back("not_exists");
    for (auto i = 0; i < 100; i++) {
      FAULT_INJECTION_SET(10, 3);  // 10%, 3 faults
      auto path = paths[folly::Random::rand32(paths.size())];
      auto result = co_await meta.stat({SUPER_USER, path, AtFlags(AT_SYMLINK_FOLLOW)});
      if (path == "not_exists") {
        CO_ASSERT_ERROR(result, MetaCode::kNotFound);
      } else {
        CO_ASSERT_OK(result);
      }
    }
  }());
}

}  // namespace
}  // namespace hf3fs::meta::server
