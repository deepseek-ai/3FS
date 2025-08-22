#include <algorithm>
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
#include "gtest/gtest.h"
#include "meta/store/DirEntry.h"
#include "meta/store/MetaStore.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestSetPermission : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestSetPermission, KVTypes);

TYPED_TEST(TestSetPermission, Basic) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    CO_ASSERT_OK(co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p777}));
    CO_ASSERT_OK(co_await meta.stat({SUPER_USER, "file", AtFlags(AT_SYMLINK_FOLLOW)}));

    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, "file", AtFlags(), {}, {}, p700)));

    flat::UserInfo ua(Uid(1), Gid(1), String());
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setPermission(ua, "file", AtFlags(), {}, {}, p755)),
                    MetaCode::kNoPermission);
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, "file", AtFlags(), ua.uid, ua.gid, {})));
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setPermission(ua, "file", AtFlags(), {}, {}, p700)));
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setPermission(ua, "file", AtFlags(), ua.uid, rootGid, {})),
                    MetaCode::kNoPermission);
  }());
}

TYPED_TEST(TestSetPermission, ByInodeId) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    auto file = (co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p777}))->stat;
    auto directory = (co_await meta.mkdirs({SUPER_USER, "directory", p777, false}))->stat;
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, file.id, AtFlags(), {}, {}, p700)));
    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, directory.id, AtFlags(), {}, {}, p700)));

    auto fileStat = (co_await meta.stat({SUPER_USER, "file", AtFlags()}))->stat;
    CO_ASSERT_EQ(fileStat.acl.perm, p700);
    auto dirStat = (co_await meta.stat({SUPER_USER, "directory", AtFlags()}))->stat;
    CO_ASSERT_EQ(dirStat.acl.perm, p700);
    auto dirStat2 = (co_await meta.stat({SUPER_USER, "directory", AtFlags()}))->stat;
    CO_ASSERT_EQ(dirStat2.acl.perm, p700);

    READ_WRITE_TRANSACTION_OK({
      directory.asDirectory().name = "";
      CO_ASSERT_OK(co_await meta::server::Inode(directory).store(*txn));
    });

    CO_ASSERT_OK(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, directory.id, AtFlags(), {}, {}, p755)));
    auto dirStat3 = (co_await meta.stat({SUPER_USER, "directory", AtFlags()}))->stat;
    CO_ASSERT_EQ(dirStat3.acl.perm, p755);
    CO_ASSERT_EQ(dirStat3.asDirectory().name, "directory");

    CO_ASSERT_OK(co_await meta.setAttr(
        SetAttrReq::setPermission(SUPER_USER, PathAt(directory.id, "."), AtFlags(), {}, {}, p700)));
    CO_ASSERT_EQ((co_await meta.stat({SUPER_USER, "directory", AtFlags()}))->stat.acl.perm, p700);
    READ_ONLY_TRANSACTION({
      CO_ASSERT_EQ((co_await DirEntry::snapshotLoad(*txn, InodeId::root(), "directory")).value()->dirAcl->perm, p700);
      CO_ASSERT_EQ((co_await DirEntry::snapshotLoad(*txn, directory.id, ".")).value(), std::nullopt);
    });

    CO_ASSERT_OK(co_await meta.remove({SUPER_USER, PathAt(InodeId::root(), "directory"), AtFlags(), false}));
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, directory.id, AtFlags(), {}, {}, p700)),
                    MetaCode::kNotFound);
    CO_ASSERT_ERROR(co_await meta.setAttr(
                        SetAttrReq::setPermission(SUPER_USER, PathAt(directory.id, "."), AtFlags(), {}, {}, p700)),
                    MetaCode::kNotFound);
    CO_ASSERT_OK(co_await meta.mkdirs({SUPER_USER, "directory", p777, false}));
    CO_ASSERT_ERROR(co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, directory.id, AtFlags(), {}, {}, p700)),
                    MetaCode::kNotFound);

    CO_ASSERT_ERROR(
        co_await meta.setAttr(SetAttrReq::setPermission(SUPER_USER, InodeId::root(), AtFlags(), {}, {}, p700)),
        MetaCode::kNoPermission);
  }());
}

TYPED_TEST(TestSetPermission, ConflictSet) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();
    auto &store = cluster.meta().getStore();
    // create file, directory
    auto createResult = co_await meta.create({SUPER_USER, "file", {}, O_RDONLY, p777});
    CO_ASSERT_OK(createResult);
    auto fileId = createResult->stat.id;

    auto mkdirResult = co_await meta.mkdirs({SUPER_USER, "directory", p777, false});
    CO_ASSERT_OK(mkdirResult);
    auto dirId = mkdirResult->stat.id;

    CHECK_CONFLICT_SET(
        [&](auto &txn) -> CoTask<void> {
          auto req = SetAttrReq::setPermission(SUPER_USER, "directory", AtFlags(0), {}, {}, p700);
          auto result = co_await store.setAttr(req)->run(txn);
          CO_ASSERT_OK(result);
        },
        (std::vector<String>{
            MetaTestHelper::getDirEntryKey(InodeId::root(), "directory"),  // dir entry
            MetaTestHelper::getInodeKey(dirId),                            // inode
        }),
        (std::vector<String>{
            MetaTestHelper::getDirEntryKey(InodeId::root(), "directory"),  // dir entry
            MetaTestHelper::getInodeKey(dirId),                            // inode
        }),
        true);

    CHECK_CONFLICT_SET(
        [&](auto &txn) -> CoTask<void> {
          auto req = SetAttrReq::setPermission(SUPER_USER, "file", AtFlags(0), {}, {}, p700);
          auto result = co_await store.setAttr(req)->run(txn);
          CO_ASSERT_OK(result);
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
