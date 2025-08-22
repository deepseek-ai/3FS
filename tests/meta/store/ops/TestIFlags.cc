#include <chrono>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <linux/fs.h>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/String.h"
#include "common/utils/UtcTime.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "gtest/gtest.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Utils.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestIFlags : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestIFlags, KVTypes);

TYPED_TEST(TestIFlags, FS_HUGE_FILE_FL) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(true);
    config.mock_meta().gc().set_gc_directory_delay(0_s);
    config.mock_meta().gc().set_gc_file_delay(0_s);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    auto ua = flat::UserInfo{flat::Uid(1), flat::Gid(1), String()};
    auto ub = flat::UserInfo{flat::Uid(2), flat::Gid(2), String()};

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir", p777, true}));
    CO_ASSERT_OK(co_await meta.create({ua, "dir/file_ua", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(ua, "dir/file_ua", IFlags(FS_HUGE_FILE_FL))));
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(SUPER_USER, "dir/file_ua", IFlags())));
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setIFlags(ub, "dir/file_ua", IFlags(FS_HUGE_FILE_FL))),
                    MetaCode::kNoPermission);
  }());
}

TYPED_TEST(TestIFlags, FS_IMMUTABLE_FL) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    MockCluster::Config config;
    config.mock_meta().gc().set_enable(true);
    config.mock_meta().gc().set_gc_directory_delay(0_s);
    config.mock_meta().gc().set_gc_file_delay(0_s);
    auto cluster = this->createMockCluster(config);
    auto &meta = cluster.meta().getOperator();

    auto ua = flat::UserInfo{flat::Uid(1), flat::Gid(1), String()};
    auto ub = flat::UserInfo{flat::Uid(2), flat::Gid(2), String()};

    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "protected-file", {}, 0, p644}));

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir/protected-dir", p777, true}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "dir/file", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.create({ua, "dir/file_ua", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "dir/protected-file", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "dir/protected-dir/file", {}, 0, p644}));
    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "dir/protected-dir/protected-file", {}, 0, p644}));

    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir/normal-dir", p777, true}));
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "dir/normal-dir2", p777, true}));
    CO_ASSERT_OK(co_await meta.hardLink(
        {SUPER_USER, "dir/protected-file", "dir/normal-dir/protected-file", AtFlags(AT_SYMLINK_FOLLOW)}));
    CO_ASSERT_OK(co_await meta.hardLink(
        {SUPER_USER, "dir/protected-file", "dir/normal-dir2/protected-file", AtFlags(AT_SYMLINK_FOLLOW)}));

    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(ua, "dir/file_ua", IFlags())));
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(ub, "dir/file_ua", IFlags())));
    config.mock_meta().set_allow_owner_change_immutable(false);
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setIFlags(ua, "dir/file_ua", IFlags(FS_IMMUTABLE_FL))),
                    MetaCode::kNoPermission);
    config.mock_meta().set_allow_owner_change_immutable(true);
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(ua, "dir/file_ua", IFlags(FS_IMMUTABLE_FL))));
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(ub, "dir/file_ua", IFlags(FS_IMMUTABLE_FL))));
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setIFlags(ub, "dir/file_ua", IFlags())), MetaCode::kNoPermission);
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setIFlags(ua, "dir/file_ua", IFlags())));

    std::vector<InodeId> inodes;
    for (auto &path : std::vector<std::string>{"protected-file",
                                               "dir/protected-dir",
                                               "dir/protected-file",
                                               "dir/protected-dir/protected-file"}) {
      auto res = co_await meta.setAttr(SetAttrReq::setIFlags(SUPER_USER, path, IFlags(FS_IMMUTABLE_FL)));
      CO_ASSERT_OK(res);
      inodes.push_back(res->stat.id);
      CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, path, AtFlags(0), true}), MetaCode::kNoPermission);
    }
    auto res = co_await meta.stat({SUPER_USER, "dir/protected-dir/file", AtFlags()});
    CO_ASSERT_OK(res);
    inodes.push_back(res->stat.id);

    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "file", AtFlags(0), true}));
    CO_ASSERT_ERROR(co_await meta.remove({SUPER_USER, "dir", AtFlags(0), true}), MetaCode::kNoPermission);
    config.mock_meta().set_recursive_remove_perm_check(0);
    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, "dir", AtFlags(0), true}));
    co_await folly::coro::sleep(std::chrono::seconds(3));

    for (auto &inodeId : inodes) {
      CO_ASSERT_INODE_EXISTS(inodeId);
    }

    CO_ASSERT_OK(co_await printTree(meta));
    co_return;
  }());
}
}  // namespace hf3fs::meta::server