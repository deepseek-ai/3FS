#include <array>
#include <cstdint>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/mem/MemKV.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Coroutine.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/String.h"
#include "fdb/FDB.h"
#include "gtest/gtest.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {
namespace {
template <typename KV>
class TestHardLink : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestHardLink, KVTypes);

TYPED_TEST(TestHardLink, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    FAULT_INJECTION_SET(10, 5);

    auto createResult = co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p644});
    CO_ASSERT_OK(createResult);
    auto &file = createResult->stat;

    auto mkdirResult = co_await meta.mkdirs({SUPER_USER, "directory", p755, true});
    CO_ASSERT_OK(mkdirResult);
    auto &dir = mkdirResult->stat;

    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, PathAt("symlink-file"), "file"}));
    auto symlinkFileResult = co_await meta.stat({SUPER_USER, PathAt("symlink-file"), AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(symlinkFileResult);
    auto &symlinkFile = symlinkFileResult->stat;

    CO_ASSERT_OK(co_await meta.symlink({SUPER_USER, PathAt("symlink-dir"), "directory"}));
    auto symlinkDirResult = co_await meta.stat({SUPER_USER, PathAt("symlink-dir"), AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(symlinkDirResult);
    auto &symlinkDir = symlinkDirResult->stat;

    using TestArg = std::tuple<std::string /* oldpath*/,
                               std::string /* newpath*/,
                               bool /* follow */,
                               std::optional<Status> /* error */,
                               InodeId /* target */,
                               uint16_t /* nlink*/>;
    for (auto [oldPath, newPath, follow, error, targetId, nlink] :
         {TestArg("file", "file-hardlink1", false, {}, file.id, 2),
          TestArg("file", "file-hardlink2", false, {}, file.id, 3),
          TestArg("directory", "directory-hardlink", false, MetaCode::kIsDirectory, dir.id, 1),
          TestArg("symlink-file", "symlink-file-hardlink-follow", true, {}, file.id, 4),
          TestArg("symlink-file", "symlink-file-hardlink-no-follow", false, {}, symlinkFile.id, 2),
          TestArg("symlink-dir", "symlink-dir-hardlink-follow", true, MetaCode::kIsDirectory, dir.id, 1),
          TestArg("symlink-dir", "symlink-dir-hardlink-no-follow", false, {}, symlinkDir.id, 2)}) {
      fmt::print("hardlink {} -> {}, follow {}\n", oldPath, newPath, follow);
      auto result = co_await meta.hardLink(
          {SUPER_USER, oldPath, newPath, follow ? AtFlags(AT_SYMLINK_FOLLOW) : AtFlags(AT_SYMLINK_NOFOLLOW)});
      if (error) {
        CO_ASSERT_ERROR(result, error->code());
        continue;
      }
      CO_ASSERT_OK(result);
      auto &inode = result->stat;
      CO_ASSERT_EQ(inode.id, targetId);
      CO_ASSERT_EQ(inode.nlink, nlink);
    }

    FAULT_INJECTION_SET(0, 0);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file", AtFlags(), false}));
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file-hardlink1", AtFlags(), false}));
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "symlink-file-hardlink-follow", AtFlags(), false}));
    auto result = co_await meta.stat({SUPER_USER, "file-hardlink2", AtFlags(AT_SYMLINK_NOFOLLOW)});
    CO_ASSERT_OK(result);
    auto &stat = result->stat;
    CO_ASSERT_EQ(stat.nlink, 1);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file-hardlink2", AtFlags(), false}));
    CO_ASSERT_ERROR(co_await meta.stat({SUPER_USER, "file-hardlink2", AtFlags(AT_SYMLINK_NOFOLLOW)}),
                    MetaCode::kNotFound);
  }());
}
}  // namespace
}  // namespace hf3fs::meta::server