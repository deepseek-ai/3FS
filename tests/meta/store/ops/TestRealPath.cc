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
#include "fbs/meta/Common.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/PathResolve.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestRealPath : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestRealPath, KVTypes);

TYPED_TEST(TestRealPath, basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, InodeId::root(), true}))->path, "/");

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "/a/b/c", p777, true}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "/a/b/c/file", {}, O_RDONLY, p777}));
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a", true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a/", true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a/.", true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a/..", true}))->path, "/");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a/../../..", true}))->path, "/");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a/b", true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "/a/b/c", true}))->path, "/a/b/c");

    auto b = (co_await meta.stat({SUPER_USER, "/a/b", AtFlags()}))->stat.id;
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "."), false}))->path, ".");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "."), true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "c"), true}))->path, "/a/b/c");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "c/.."), false}))->path, ".");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "c/.."), true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, ".."), true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "c/file"), false}))->path, "c/file");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(b, "c/file"), true}))->path, "/a/b/c/file");

    auto file = (co_await meta.stat({SUPER_USER, "/a/b/c/file", AtFlags()}))->stat.id;
    CO_ASSERT_ERROR((co_await meta.getRealPath({SUPER_USER, file, true})), MetaCode::kNotDirectory);

    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "/symlink-1", "/a/b"}));
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/.", false}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/.", true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/c", true}))->path, "/a/b/c");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/c/..", false}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/c/..", true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/..", true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/c/file", false}))->path, "/a/b/c/file");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "symlink-1/c/file", true}))->path, "/a/b/c/file");

    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, "a/symlink-2", "b"}));
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/.", false}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/..", false}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/.", true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/..", true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/c", true}))->path, "/a/b/c");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/c/..", false}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/c/..", true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/..", true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/c/file", false}))->path, "/a/b/c/file");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, "a/symlink-2/c/file", true}))->path, "/a/b/c/file");

    auto a = (co_await meta.stat({SUPER_USER, "/a", AtFlags()}))->stat.id;
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/."), false}))->path, "b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/.."), false}))->path, ".");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/."), true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/c"), true}))->path, "/a/b/c");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/.."), true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/c/.."), false}))->path, "b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/c/.."), true}))->path, "/a/b");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/.."), true}))->path, "/a");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/c/file"), false}))->path, "b/c/file");
    CO_ASSERT_EQ((co_await meta.getRealPath({SUPER_USER, PathAt(a, "symlink-2/c/file"), true}))->path, "/a/b/c/file");
  }());
}

}  // namespace hf3fs::meta::server