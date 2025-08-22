#include <fcntl.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "common/utils/Coroutine.h"
#include "common/utils/Path.h"
#include "common/utils/StatusCode.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/PathResolve.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestSymlink : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestSymlink, KVTypes);

TYPED_TEST(TestSymlink, RealPath) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    FAULT_INJECTION_SET(10, 5);

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "/a/b", p755, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "/a/c", p755, true}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "symlink1", "file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "symlink2", "/file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/symlink1", "../file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/symlink2", "/file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/symlink3", "../../file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/b/symlink1", "../../file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/b/symlink2", "../.././file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/b/symlink3", "../c/./../../../file"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/b/symlink4", "symlink3"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/c/symlink1", "/a/b/symlink4"}));

    for (auto path : {"/symlink1",
                      "symlink1",
                      "symlink2",
                      "/a/symlink1",
                      "/a/symlink2",
                      "/a/symlink3",
                      "/a/b/symlink1",
                      "/a/b/symlink2",
                      "/a/b/symlink3",
                      "/a/c/symlink1"}) {
      auto result = co_await meta.getRealPath({SUPER_USER, path, false});
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->path, "/file") << path << " -> " << result->path;
    }

    auto b = co_await meta.open({SUPER_USER, "/a/b", {}, O_RDONLY | O_DIRECTORY});
    CO_ASSERT_OK(b);
    for (auto path : {"symlink1",
                      "symlink2",
                      "./symlink3",
                      "../b/symlink3",
                      "../symlink1",
                      "../symlink2",
                      "../symlink3",
                      "../../a/symlink3",
                      "../../../a/b/symlink3"}) {
      auto result = co_await meta.getRealPath({SUPER_USER, PathAt(b->stat.id, path), false});
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->path, "/file") << path << " -> " << result->path;
    }

    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/c/symlink2", "../b/../b"}));
    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/a/c/symlink3", "../b/../.."}));
    auto c = co_await meta.open({SUPER_USER, "/a/c", {}, O_RDONLY | O_DIRECTORY});
    CO_ASSERT_OK(c);

    for (auto [path, rpath] : {std::make_pair("symlink2", "../b"),
                               std::make_pair("symlink3", "../.."),
                               {"symlink2/symlink1", "/file"},
                               {"symlink2/../b", "../b"}}) {
      auto result = co_await meta.getRealPath({SUPER_USER, PathAt(c->stat.id, path), false});
      CO_ASSERT_OK(result);
      CO_ASSERT_EQ(result->path, rpath) << path << " -> " << result->path << " != " << rpath;
    }
  }());
}

}  // namespace hf3fs::meta::server