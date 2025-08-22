#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <vector>

#include "client/meta/MetaClient.h"
#include "client/meta/ServerSelectionStrategy.h"
#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/mgmtd/RoutingInfo.h"
#include "client/storage/StorageClient.h"
#include "common/app/ClientId.h"
#include "common/app/NodeId.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/Coroutine.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/MockService.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "meta/components/ChainAllocator.h"
#include "meta/service/MetaOperator.h"
#include "meta/service/MetaSerdeService.h"
#include "meta/store/Inode.h"
#include "meta/store/MetaStore.h"
#include "stubs/MetaService/MetaServiceStub.h"
#include "tests/FakeMgmtdClient.h"
#include "tests/GtestHelpers.h"

#define CO_ASSERT_INODE_DATA(expected, actual)                                                                        \
  do {                                                                                                                \
    const auto &_e = (expected);                                                                                      \
    const auto &_a = (actual);                                                                                        \
    CO_ASSERT_EQ(_e.getType(), _a.getType()) << "getType";                                                            \
    CO_ASSERT_EQ(_e.acl.uid, _a.acl.uid) << "getUid";                                                                 \
    CO_ASSERT_EQ(_e.acl.gid, _a.acl.gid) << "getGid";                                                                 \
    CO_ASSERT_EQ(_e.acl.perm, _a.acl.perm) << "getPermission";                                                        \
    switch (_e.getType()) {                                                                                           \
      case InodeType::File:                                                                                           \
        CO_ASSERT_EQ(_e.asFile(), _a.asFile()) << fmt::format("{} {}", _e.asFile(), _a.asFile());                     \
        break;                                                                                                        \
      case InodeType::Directory:                                                                                      \
        CO_ASSERT_EQ(_e.asDirectory(), _a.asDirectory()) << fmt::format("{} {}", _e.asDirectory(), _a.asDirectory()); \
        break;                                                                                                        \
      case InodeType::Symlink:                                                                                        \
        CO_ASSERT_EQ(_e.asSymlink(), _a.asSymlink()) << fmt::format("{} {}", _e.asSymlink(), _a.asSymlink());         \
        break;                                                                                                        \
    }                                                                                                                 \
  } while (false)

#define CO_ASSERT_STAT_EQ(inode, parent, path)                                     \
  do {                                                                             \
    auto r1 = co_await metaClient_->stat(rootUser, parent, path, true);            \
    CO_ASSERT_TRUE(!r1.hasError()) << "r1 has error " << r1.error().describe();    \
    CO_ASSERT_TRUE(inode == r1.value()) << "inode != r1";                          \
    auto r2 = co_await metaClient_->stat(rootUser, parent, path, false);           \
    CO_ASSERT_TRUE(!r2.hasError()) << "r2 has error " << r2.error().describe();    \
    CO_ASSERT_TRUE(inode == r2.value()) << "inode != r2";                          \
    auto r3 = co_await metaClient_->stat(rootUser, inode.id, std::nullopt, false); \
    CO_ASSERT_TRUE(!r3.hasError()) << "r3 has error " << r3.error().describe();    \
    CO_ASSERT_TRUE(inode == r3.value()) << "inode != r3";                          \
  } while (false)

#define CO_ASSERT_EXISTS_IMPL(result, userInfo, parent, path, followLastSymlink)            \
  do {                                                                                      \
    auto r = co_await metaClient_->stat((userInfo), (parent), (path), (followLastSymlink)); \
    if (result) {                                                                           \
      CO_ASSERT_OK(r);                                                                      \
    } else {                                                                                \
      CO_ASSERT_ERROR(r, MetaCode::kNotFound);                                              \
    }                                                                                       \
  } while (false)

#define CO_ASSERT_EXISTS(...) CO_ASSERT_EXISTS_IMPL(true, __VA_ARGS__)
#define CO_ASSERT_NOT_EXISTS(...) CO_ASSERT_EXISTS_IMPL(false, __VA_ARGS__)

namespace hf3fs::meta::client::tests {
namespace {
using flat::ChainId;
using flat::ChainRef;
using flat::ChainTableId;
using flat::ChainTableVersion;
using flat::Gid;
using flat::Uid;
using flat::UserInfo;
using meta::Permission;

using namespace stubs;

const UserInfo rootUser{Uid{0}, Gid{0}};
const UserInfo ua{Uid{1}, Gid{1}};
const UserInfo ub{Uid{2}, Gid{1}};
const UserInfo uc{Uid{3}, Gid{2}};
const Permission p777{0777};
const Permission p700{0700};
const Permission p644{0644};

const std::vector<std::tuple<ChainTableId, size_t, size_t>> kChainTables = {
    std::tuple<ChainTableId, size_t, size_t>(ChainTableId(1), 128, 1),
    std::tuple<ChainTableId, size_t, size_t>(ChainTableId(2), 512, 2),
    std::tuple<ChainTableId, size_t, size_t>(ChainTableId(3), 0, 1),
};

InodeData inodeDataWithoutLayout(InodeData d) {
  if (d.isFile()) {
    d.asFile().layout = {};
    d.asFile().dynStripe = 0;
  }
  if (d.isDirectory()) d.asDirectory().layout = {};
  return d;
}

class TestMetaClient : public ::testing::Test {
 public:
  TestMetaClient() {
    metaConfig_.set_dynamic_stripe(true);
    metaConfig_.set_dynamic_stripe_initial(1);
    metaConfig_.set_dynamic_stripe_growth(2);

    metaClientConfig_.set_dynamic_stripe(true);

    mgmtdClient_ = hf3fs::tests::FakeMgmtdClient::create(kChainTables, 3, 5);
    storageClientConfig_.set_implementation_type(storage::client::StorageClient::ImplementationType::InMem);
    storageClient_ = storage::client::StorageClient::create(ClientId::random(), storageClientConfig_, *mgmtdClient_);

    auto memKVEngine = std::make_shared<kv::MemKVEngine>();
    metaOperator_ = std::make_unique<meta::server::MetaOperator>(metaConfig_,
                                                                 flat::NodeId(50),
                                                                 memKVEngine,
                                                                 mgmtdClient_,
                                                                 storageClient_,
                                                                 std::unique_ptr<meta::server::Forward>());
    folly::coro::blockingWait([&]() -> CoTask<void> {
      CO_AWAIT_ASSERT_OK(metaOperator_->init(Layout::newEmpty(flat::ChainTableId(1), 512 << 10, 64)));
    }());
    metaOperator_->start(exec_);
    serde::ClientMockContext ctx;
    ctx.setService(std::make_unique<meta::server::MetaSerdeService>(*metaOperator_));
    metaClient_ = std::make_unique<MetaClient>(ClientId::random(),
                                               metaClientConfig_,
                                               std::make_unique<MetaClient::StubFactory>(ctx),
                                               mgmtdClient_,
                                               storageClient_,
                                               true);
    prepareEnv();
  }

  ~TestMetaClient() override {
    metaOperator_->beforeStop();
    metaOperator_->afterStop();
  }

  void prepareEnv() {
    folly::coro::blockingWait([&]() -> CoTask<void> {
      CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), Path("a/b/c"), p777, true));
      CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, InodeId::root(), Path("a/b/d"), std::nullopt, p644, O_EXCL));
      CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, InodeId::root(), Path("a/b/c/d"), std::nullopt, p644, O_EXCL));
      CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), Path("a/d"), Path("/a/b/c")));
      CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), Path("a/d/e"), Path("../b/d")));
      CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), Path("a/d/f"), Path("../../b/d")));
      CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), Path("e"), Path("a/d")));
      CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), Path("e/g"), p777, false));
    }());
  }

  static InodeData newFile(uint32_t uid, uint32_t gid, Permission perm, Layout layout = {}) {
    return {File(layout), Acl(Uid(uid), Gid(gid), Permission(perm))};
  }

  static InodeData newDir(uint32_t uid,
                          uint32_t gid,
                          Permission perm,
                          const InodeId &parent,
                          std::string name,
                          Layout layout = {}) {
    return {Directory{parent, layout, name}, Acl(Uid(uid), Gid(gid), Permission(perm))};
  }

  static InodeData newSymlink(uint32_t uid, uint32_t gid, Permission perm, const Path &target) {
    return {Symlink{target}, Acl(Uid(uid), Gid(gid), Permission(perm))};
  }

  Inode getInode(const Path &path, bool followLastSymlink) {
    Inode res;
    folly::coro::blockingWait([&]() -> CoTask<void> {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), path, followLastSymlink);
      CO_ASSERT_TRUE(!r.hasError()) << path.native() << " " << followLastSymlink << " " << r.error().describe();
      res = std::move(r.value());
    }());
    return res;
  }

  auto &storage() { return *storageClient_; }
  auto &config() { return metaClientConfig_; }

  meta::server::Config metaConfig_;
  storage::client::StorageClient::Config storageClientConfig_;
  MetaClient::Config metaClientConfig_;

  CPUExecutorGroup exec_{2, "TestMetaClient"};
  std::unique_ptr<meta::server::MetaOperator> metaOperator_;
  std::unique_ptr<MetaClient> metaClient_;
  std::shared_ptr<hf3fs::tests::FakeMgmtdClient> mgmtdClient_;
  std::shared_ptr<storage::client::StorageClient> storageClient_;
};

TEST_F(TestMetaClient, testTokenNotLeak) {
  UserInfo user(flat::Uid(1), flat::Gid(2), "secret-token");
  OpenReq req(user, InodeId::root(), std::nullopt, O_RDONLY);
  auto str = fmt::format("user {}, req {}", user, req);
  ASSERT_EQ(str.find("secret-token"), std::string::npos);
}

TEST_F(TestMetaClient, testStatFs) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto result = co_await metaClient_->statFs(rootUser);
    CO_ASSERT_OK(result);
    const auto &stat = result.value();
    // note: StorageClientInMem will generate fake info here.
    EXPECT_EQ(stat.capacity, 100ULL << 30);
    EXPECT_EQ(stat.used, 50ULL << 30);
    EXPECT_EQ(stat.free, 50ULL << 30);
  }());
}

TEST_F(TestMetaClient, testStat) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    Inode a;
    Inode ab;
    Inode abc;
    Inode abd;
    Inode ad;
    Inode ade;
    Inode e;
    {
      // authentication is disabled, always return success
      auto root = co_await metaClient_->authenticate(rootUser);
      CO_ASSERT_OK(root);
      CO_ASSERT_EQ(*root, rootUser);
      auto uaResult = co_await metaClient_->authenticate(ua);
      CO_ASSERT_OK(uaResult);
      CO_ASSERT_EQ(*uaResult, ua);
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a", false);
      CO_ASSERT_OK(r);
      a = r.value();
      CO_ASSERT_INODE_DATA(newDir(0, 0, p777, InodeId::root(), "a"), inodeDataWithoutLayout(a));

      CO_ASSERT_STAT_EQ(a, InodeId::root(), "a");
      CO_ASSERT_STAT_EQ(a, InodeId::root(), "./a");
      CO_ASSERT_STAT_EQ(a, InodeId::root(), "../a");
      CO_ASSERT_STAT_EQ(a, InodeId::root(), "a/.././a");
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/b", false);
      CO_ASSERT_OK(r);
      ab = r.value();
      CO_ASSERT_INODE_DATA(newDir(0, 0, p777, a.id, "b"), inodeDataWithoutLayout(ab));

      CO_ASSERT_STAT_EQ(ab, InodeId::root(), "a/b");
      CO_ASSERT_STAT_EQ(ab, InodeId::root(), "a/b");
      CO_ASSERT_STAT_EQ(ab, a.id, "b");
      CO_ASSERT_STAT_EQ(ab, a.id, "./b");
      CO_ASSERT_STAT_EQ(ab, a.id, "../a/b");
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/c", false);
      CO_ASSERT_OK(r);
      abc = r.value();
      CO_ASSERT_INODE_DATA(newDir(0, 0, p777, ab.id, "c"), inodeDataWithoutLayout(abc));

      CO_ASSERT_STAT_EQ(abc, InodeId::root(), "a/b/c");
      CO_ASSERT_STAT_EQ(abc, InodeId::root(), "a/b/c");
      CO_ASSERT_STAT_EQ(abc, a.id, "b/c");
      CO_ASSERT_STAT_EQ(abc, ab.id, "c");
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", false);
      CO_ASSERT_OK(r);
      abd = r.value();
      CO_ASSERT_INODE_DATA(newFile(0, 0, p644), inodeDataWithoutLayout(abd));

      CO_ASSERT_STAT_EQ(abd, InodeId::root(), "a/b/d");
      CO_ASSERT_STAT_EQ(abd, InodeId::root(), "a/b/d");
      CO_ASSERT_STAT_EQ(abd, a.id, "b/d");
      CO_ASSERT_STAT_EQ(abd, ab.id, "d");
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/d/e", false);
      CO_ASSERT_OK(r);
      ade = r.value();
      CO_ASSERT_INODE_DATA(newSymlink(0, 0, p777, "../b/d"), ade);

      r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/d/e", true);
      CO_ASSERT_ERROR(r, MetaCode::kNotFound);
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/d/f", false);
      CO_ASSERT_OK(r);
      auto adf = r.value();
      CO_ASSERT_INODE_DATA(newSymlink(0, 0, p777, "../../b/d"), adf);

      r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/d/f", true);
      CO_ASSERT_OK(r);
      CO_ASSERT_EQ(abd, r.value());
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "e", false);
      CO_ASSERT_OK(r);
      e = r.value();
      CO_ASSERT_INODE_DATA(newSymlink(0, 0, p777, "a/d"), e);

      r = co_await metaClient_->stat(rootUser, InodeId::root(), "e", true);
      CO_ASSERT_EQ(abc, r.value());
    }
    {
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "e/d", false);
      CO_ASSERT_OK(r);
      auto ed = r.value();
      CO_ASSERT_INODE_DATA(newFile(0, 0, p644), inodeDataWithoutLayout(ed));

      CO_ASSERT_STAT_EQ(ed, InodeId::root(), "e/d");
      CO_ASSERT_STAT_EQ(ed, InodeId::root(), "a/b/c/d");
      CO_ASSERT_STAT_EQ(ed, a.id, "b/c/d");
      CO_ASSERT_STAT_EQ(ed, ab.id, "c/d");
      CO_ASSERT_STAT_EQ(ed, abc.id, "d");
    }
    { CO_ASSERT_STAT_EQ(abd, InodeId::root(), "e/g/../../c/./../d"); }
    {
      CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), "a/b/c/x", "/a/b/c/x"));
      auto r = co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/c/x", false);
      CO_ASSERT_OK(r);
      CO_ASSERT_INODE_DATA(newSymlink(0, 0, p777, "/a/b/c/x"), r.value());

      CO_AWAIT_ASSERT_ERROR(MetaCode::kTooManySymlinks, metaClient_->stat(rootUser, InodeId::root(), "a/b/c/x", true));
    }
  }());
}

TEST_F(TestMetaClient, testExists) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "/", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), ".", true);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), ".", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "..", true);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "..", false);

    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a", true);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "./a", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "./a", true);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "../a", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "../a", true);

    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "b", false);
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "b", true);

    Inode a = (co_await metaClient_->stat(rootUser, InodeId::root(), "a", false)).value();
    Inode ab = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b", false)).value();

    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/b", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/b", true);
    CO_ASSERT_EXISTS(rootUser, a.id, "b", false);
    CO_ASSERT_EXISTS(rootUser, a.id, "b", true);
    CO_ASSERT_EXISTS(rootUser, ab.id, ".", false);
    CO_ASSERT_EXISTS(rootUser, ab.id, ".", true);
    CO_ASSERT_EXISTS(rootUser, ab.id, "../b", false);
    CO_ASSERT_EXISTS(rootUser, ab.id, "../b", true);

    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/b/c/d", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/b/c/d", true);

    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/d/e", false);
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "a/d/e", true);
  }());
}

TEST_F(TestMetaClient, testOpen) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "u", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "u/a", Permission(0750), false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "u/a/b", std::nullopt, Permission(0640), 0));

    {
      CO_AWAIT_ASSERT_ERROR(MetaCode::kIsDirectory,
                            metaClient_->open(ua, InodeId::root(), "u/a", SessionId::random(), O_WRONLY));
      CO_AWAIT_ASSERT_ERROR(MetaCode::kIsDirectory,
                            metaClient_->open(ua, InodeId::root(), "u/a", SessionId::random(), O_RDWR));
      CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->open(uc, InodeId::root(), "u/a", std::nullopt, 0));
      CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory,
                            metaClient_->open(ua, InodeId::root(), "u/a/b", std::nullopt, O_DIRECTORY));
      CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission,
                            metaClient_->open(ub, InodeId::root(), "u/a/b", SessionId::random(), O_RDWR));
      CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission,
                            metaClient_->open(uc, InodeId::root(), "u/a/b", SessionId::random(), O_RDONLY));
      CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                            metaClient_->open(rootUser, InodeId::root(), "u/a/b", std::nullopt, O_RDWR));
    }
    {
      auto r = co_await metaClient_->open(ua, InodeId::root(), "u/a", std::nullopt, 0);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a");

      r = co_await metaClient_->open(ua, r->id, std::nullopt, std::nullopt, 0);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a");
    }
    {
      auto r = co_await metaClient_->open(ub, InodeId::root(), "u/a", std::nullopt, 0);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a");

      r = co_await metaClient_->open(ub, r->id, std::nullopt, std::nullopt, 0);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a");
    }
    {
      auto r = co_await metaClient_->open(ua, InodeId::root(), "u/a/b", std::nullopt, 0);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a/b");

      r = co_await metaClient_->open(ua, r->id, std::nullopt, std::nullopt, 0);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a/b");
    }
    {
      auto r = co_await metaClient_->open(ua, InodeId::root(), "u/a/b", SessionId::random(), O_RDWR);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a/b");

      r = co_await metaClient_->open(ua, r->id, std::nullopt, SessionId::random(), O_RDWR);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a/b");
    }
    {
      auto r = co_await metaClient_->open(ub, InodeId::root(), "u/a/b", std::nullopt, O_RDONLY);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a/b");

      r = co_await metaClient_->open(ua, r->id, std::nullopt, std::nullopt, O_RDONLY);
      CO_ASSERT_OK(r);
      CO_ASSERT_STAT_EQ(r.value(), InodeId::root(), "u/a/b");
    }
  }());
}

TEST_F(TestMetaClient, testClose) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto abd = (co_await metaClient_->open(rootUser, InodeId::root(), "a/b/d", SessionId::random(), O_RDWR)).value();

    {
      CO_AWAIT_ASSERT_OK(metaClient_->close(rootUser, abd.id, std::nullopt, false, false));
      auto abd2 = (co_await metaClient_->open(rootUser, InodeId::root(), "a/b/d", SessionId::random(), O_RDWR)).value();
      // CO_ASSERT_EQ(abd2.asFile().length, 1000);
      CO_ASSERT_EQ(abd2.atime, abd.atime);
      CO_ASSERT_EQ(abd2.ctime, abd.ctime);
      CO_ASSERT_EQ(abd2.mtime, abd.mtime);
    }
    {
      co_await folly::coro::sleep(std::chrono::seconds(2));
      CO_AWAIT_ASSERT_OK(metaClient_->close(rootUser, abd.id, SessionId::random(), true, true));
      auto abd2 = (co_await metaClient_->open(rootUser, InodeId::root(), "a/b/d", SessionId::random(), O_RDWR)).value();
      // CO_ASSERT_EQ(abd2.asFile().length, 1000);
      CO_ASSERT_GT(abd2.atime, abd.atime);
      CO_ASSERT_GT(abd2.mtime, abd.mtime);
      CO_ASSERT_EQ(abd2.ctime, abd.ctime);
      auto atime = UtcTime::fromMicroseconds(UtcClock::now().toMicroseconds() - 60000000).castGranularity(1_s);
      auto mtime = UtcTime::fromMicroseconds(UtcClock::now().toMicroseconds() + 60000000).castGranularity(1_s);
      CO_AWAIT_ASSERT_OK(metaClient_->close(rootUser, abd.id, SessionId::random(), false, atime, mtime));
      auto abd3 = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", false)).value();
      CO_ASSERT_EQ(abd3.atime, abd2.atime);
      CO_ASSERT_EQ(abd3.mtime, mtime);
      CO_ASSERT_EQ(abd3.ctime, abd.ctime);
    }
    {
      // written without session
      CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->close(rootUser, abd.id, std::nullopt, false, true));
    }
    {
      CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound,
                            metaClient_->close(rootUser, InodeId(folly::Random::rand64()), std::nullopt, false, false));
      CO_AWAIT_ASSERT_ERROR(
          MetaCode::kNotFound,
          metaClient_->close(rootUser, InodeId(folly::Random::rand64()), SessionId::random(), true, true));
    }
  }());
}

TEST_F(TestMetaClient, testSetPermission) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    [[maybe_unused]] static constexpr auto t = UtcTime::fromMicroseconds(1000);
    auto abd = (co_await metaClient_->open(rootUser, InodeId::root(), "a/b/d", SessionId::random(), O_RDWR)).value();
    auto data = abd;
    auto expectedAcl = data.acl;
    for (int i = 0; i < 100; i++) {
      co_await folly::coro::sleep(std::chrono::milliseconds(50));
      std::optional<Uid> uid = folly::Random::oneIn(2) ? std::nullopt : std::optional(Uid(folly::Random::rand32(5)));
      std::optional<Gid> gid = folly::Random::oneIn(2) ? std::nullopt : std::optional(Gid(folly::Random::rand32(5)));
      std::optional<Permission> perm =
          folly::Random::oneIn(2) ? std::nullopt : std::optional(Permission(folly::Random::rand32(5)));
      CO_AWAIT_ASSERT_OK(metaClient_->setPermission(rootUser, InodeId::root(), "a/b/d", true, uid, gid, perm));
      if (uid) expectedAcl.uid = *uid;
      if (gid) expectedAcl.gid = *gid;
      if (perm) expectedAcl.perm = *perm;
      auto d = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", true)).value();
      CO_ASSERT_EQ(d.acl, expectedAcl);
      CO_ASSERT_EQ(d.atime, data.atime);
      CO_ASSERT_EQ(d.mtime, data.mtime);
      if (uid || gid || perm) {
        CO_ASSERT_GE(d.ctime, data.ctime);
      } else {
        CO_ASSERT_EQ(d.ctime, data.ctime);
      }
      data = d;
    }
  }());
}

TEST_F(TestMetaClient, testUtimes) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto abd = (co_await metaClient_->open(rootUser, InodeId::root(), "a/b/d", SessionId::random(), O_RDWR)).value();
    {
      auto t = UtcClock::now().castGranularity(1_s);
      auto r = (co_await metaClient_->utimes(rootUser, InodeId::root(), "a/b/d", true, t, t)).value();
      auto d = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", true)).value();
      CO_ASSERT_EQ(r, d);
      CO_ASSERT_EQ(d.atime, t);
      CO_ASSERT_EQ(d.mtime, t);
    }
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto t = UtcClock::now().castGranularity(1_s);
      CO_AWAIT_ASSERT_OK(
          metaClient_->utimes(rootUser, InodeId::root(), "a/b/d", true, SETATTR_TIME_NOW, SETATTR_TIME_NOW));
      auto d = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", true)).value();
      CO_ASSERT_GE(d.atime, t);
      CO_ASSERT_GE(d.mtime, t);
    }
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto t = UtcClock::now().castGranularity(1_s);
      CO_AWAIT_ASSERT_OK(metaClient_->utimes(rootUser, abd.id, std::nullopt, true, SETATTR_TIME_NOW, SETATTR_TIME_NOW));
      auto d = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", true)).value();
      CO_ASSERT_GE(d.atime, t);
      CO_ASSERT_GE(d.mtime, t);
    }
    {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      auto t = UtcTime::fromMicroseconds(1 * 1000 * 1000);
      CO_AWAIT_ASSERT_OK(metaClient_->utimes(rootUser, abd.id, std::nullopt, true, t, std::nullopt));
      auto d = (co_await metaClient_->stat(rootUser, InodeId::root(), "a/b/d", true)).value();
      CO_ASSERT_EQ(d.atime, t);
      CO_ASSERT_GE(d.mtime, t);
    }
    {
      CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission,
                            metaClient_->utimes(ua, abd.id, std::nullopt, true, SETATTR_TIME_NOW, SETATTR_TIME_NOW));
      CO_AWAIT_ASSERT_OK(metaClient_->utimes(ua, abd.id, std::nullopt, true, std::nullopt, std::nullopt));
    }
  }());
}

TEST_F(TestMetaClient, testTruncate) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto session = SessionId::random();
    auto result = co_await metaClient_->create(rootUser, InodeId::root(), "new-file", session, p644, 0);
    CO_ASSERT_OK(result);
    auto inodeId = result->id;
    CO_AWAIT_ASSERT_OK(metaClient_->truncate(rootUser, inodeId, 10_MB + 13));
    CO_AWAIT_ASSERT_OK(metaClient_->close(rootUser, inodeId, session, true, true));
    auto stat = co_await metaClient_->stat(rootUser, InodeId::root(), "new-file", true);
    CO_ASSERT_OK(stat);
    CO_ASSERT_EQ(stat->asFile().length, 10_MB + 13);
  }());
}

CoTask<void> randomWrite(MetaClient &meta,
                         storage::client::StorageClient &client,
                         Inode &inode,
                         uint64_t offset,
                         uint64_t length) {
  auto stripe = std::min((uint32_t)folly::divCeil(offset + length, (uint64_t)inode.asFile().layout.chunkSize),
                         inode.asFile().layout.stripeSize);
  if (inode.asFile().dynStripe && inode.asFile().dynStripe < stripe) {
    auto result = co_await meta.extendStripe(flat::UserInfo{}, inode.id, stripe);
    CO_ASSERT_OK(result);
    inode = *result;
  }

  uint64_t chunkSize = inode.asFile().layout.chunkSize;
  std::vector<uint8_t> writeData(chunkSize, 0x00);
  std::vector<folly::SemiFuture<folly::Unit>> tasks;
  while (length) {
    auto offsetInChunk = offset % chunkSize;
    auto lengthInChunk = std::min(length, chunkSize - offsetInChunk);
    auto chunkId = inode.asFile().getChunkId(inode.id, offset);
    auto routingInfo = client.getMgmtdClient().getRoutingInfo()->raw();
    XLOGF_IF(FATAL, !routingInfo, "No routingInfo");
    auto chainId = inode.asFile().getChainId(inode, offset, *routingInfo);
    XLOGF_IF(FATAL, !chainId, "resolve chainId failed: {}", chainId);

    auto task = [=, &client, &writeData]() -> CoTask<void> {
      auto writeIO = client.createWriteIO(storage::ChainId(*chainId),
                                          storage::ChunkId(chunkId->pack()),
                                          offsetInChunk,
                                          lengthInChunk,
                                          chunkSize,
                                          writeData.data(),
                                          nullptr);
      XLOGF(DBG, "write {} offset {}, offsetInChunk {} length {}", chunkId, offset, offsetInChunk, lengthInChunk);
      auto result = co_await client.write(writeIO, flat::UserInfo());
      CO_ASSERT_FALSE(result.hasError()) << result.error().describe();
      CO_ASSERT_FALSE(writeIO.result.lengthInfo.hasError()) << writeIO.result.lengthInfo.error().describe();
      CO_ASSERT_EQ(*writeIO.result.lengthInfo, lengthInChunk);
    };

    tasks.push_back(folly::coro::co_invoke(task).scheduleOn(co_await folly::coro::co_current_executor).start());

    offset += lengthInChunk;
    length -= lengthInChunk;
  }

  co_await folly::collectAll(tasks.begin(), tasks.end());
}

TEST_F(TestMetaClient, testRemoveChunksBatchSize) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    config().set_remove_chunks_batch_size(1);
    config().set_remove_chunks_max_iters(10000000);

    auto file = co_await metaClient_->create(rootUser,
                                             InodeId::root(),
                                             "file",
                                             Uuid::random(),
                                             p644,
                                             O_RDWR,
                                             Layout::newEmpty(flat::ChainTableId(1), 1 << 10, 8));
    CO_ASSERT_OK(file);

    co_await randomWrite(*metaClient_, storage(), *file, 0, 10 << 20);
    auto sync = co_await metaClient_->sync(rootUser, file->id, false, true, std::nullopt);
    CO_ASSERT_OK(sync);
    CO_ASSERT_EQ(sync->asFile().length, 10 << 20);

    auto otrunc = co_await metaClient_->open(rootUser, file->id, std::nullopt, Uuid::random(), O_TRUNC | O_RDWR);
    CO_ASSERT_OK(otrunc);
    CO_ASSERT_EQ(otrunc->asFile().length, 0);

    sync = co_await metaClient_->sync(rootUser, file->id, false, true, std::nullopt);
    CO_ASSERT_OK(sync);
    CO_ASSERT_EQ(sync->asFile().length, 0);

    co_await randomWrite(*metaClient_, storage(), *file, 0, 10 << 20);
    auto ctrunc =
        co_await metaClient_->create(rootUser, InodeId::root(), "file", Uuid::random(), p644, O_TRUNC | O_RDWR);
    CO_ASSERT_OK(ctrunc);
    CO_ASSERT_EQ(ctrunc->asFile().length, 0);

    co_await randomWrite(*metaClient_, storage(), *file, 0, 10 << 20);
    // truncate up
    auto l1 = (10 << 20) + folly::Random::rand32(4 << 10, 64 << 10);
    auto r1 = co_await metaClient_->truncate(rootUser, file->id, l1);
    CO_ASSERT_OK(r1);
    CO_ASSERT_EQ(r1->asFile().length, l1);

    // truncate down
    co_await randomWrite(*metaClient_, storage(), *file, 0, 10 << 20);
    auto l2 = (10 << 20) + folly::Random::rand32(128 << 10, 10 << 20);
    auto r2 = co_await metaClient_->truncate(rootUser, file->id, l2);
    CO_ASSERT_OK(r2);
    CO_ASSERT_EQ(r2->asFile().length, l2);
    auto r3 = co_await metaClient_->stat(rootUser, file->id, std::nullopt, true);
    CO_ASSERT_OK(r3);
    CO_ASSERT_EQ(r3->asFile().length, l2);

    co_return;
  }());
}

TEST_F(TestMetaClient, testCreate) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound,
                          metaClient_->create(rootUser, InodeId::root(), "nonexists/x", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory,
                          metaClient_->create(rootUser, InodeId::root(), "a/b/d/x", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->create(rootUser, InodeId::root(), "a/b/.", std::nullopt, p644, 0));
    // CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->create(rootUser, std::nullopt, "a/b/..", p644, 0));
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, InodeId::root(), "a/b/d", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kIsDirectory,
                          metaClient_->create(rootUser, InodeId::root(), "a/b/c", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kExists,
                          metaClient_->create(rootUser, InodeId::root(), "a/b/d", std::nullopt, p644, O_EXCL));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "u", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "u/a", Permission(0750), false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "u/a/b", std::nullopt, Permission(0640), 0));

    CO_AWAIT_ASSERT_OK(metaClient_->create(ub, InodeId::root(), "u/a/b", std::nullopt, Permission(0640), 0));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission,
                          metaClient_->create(uc, InodeId::root(), "u/a/b", std::nullopt, Permission(0640), 0));

    {
      // create with default layout
      CO_AWAIT_ASSERT_OK(
          metaClient_->create(rootUser, InodeId::root(), "file-default-layout", std::nullopt, p644, O_EXCL));
      auto inode = co_await metaClient_->stat(rootUser, InodeId::root(), "file-default-layout", true);
      CO_ASSERT_OK(inode);
      CO_ASSERT_TRUE(inode->isFile());
      auto &file = inode->asFile();
      CO_ASSERT_TRUE(file.layout.valid(false));
      CO_ASSERT_EQ(file.layout.chunkSize, 512 << 10);
      CO_ASSERT_EQ(file.layout.stripeSize, 64);
      CO_ASSERT_EQ(flat::ChainTableId(1), file.layout.tableId);
      CO_ASSERT_EQ(file.layout.getChainIndexList().size(), file.layout.stripeSize);
      std::set<uint32_t> set;
      for (auto index : file.layout.getChainIndexList()) {
        CO_ASSERT_FALSE(set.contains(index));
        set.insert(index);
      }
    }

    {
      // create with custom layout
      auto layout = Layout::newEmpty(ChainTableId(1), 4096, 111);
      auto createResult =
          co_await metaClient_
              ->create(rootUser, InodeId::root(), "file-custom-layout", std::nullopt, p644, O_EXCL, layout);
      CO_ASSERT_OK(createResult);
      CO_ASSERT_TRUE(createResult->isFile());
      auto &file = createResult->asFile();
      CO_ASSERT_TRUE(file.layout.valid(false));
      CO_ASSERT_EQ(file.layout.chunkSize, layout.chunkSize);
      CO_ASSERT_EQ(file.layout.stripeSize, layout.stripeSize);
      CO_ASSERT_EQ(file.layout.getChainIndexList().size(), layout.stripeSize);
      std::set<uint32_t> set;
      for (auto index : file.layout.getChainIndexList()) {
        CO_ASSERT_FALSE(set.contains(index));
        set.insert(index);
      }
    }

    {
      // create with custom chain layout
      auto table = ChainTableId(1);
      auto layout = Layout::newChainList(table, ChainTableVersion(1), 4096, {1, 2, 3});
      auto createResult =
          co_await metaClient_
              ->create(rootUser, InodeId::root(), "file-custom-layout-chainlist", std::nullopt, p644, O_EXCL, layout);
      CO_ASSERT_OK(createResult);
      CO_ASSERT_TRUE(createResult->isFile());
      auto &file = createResult->asFile();
      CO_ASSERT_TRUE(file.layout.valid(false));
      CO_ASSERT_EQ(file.layout, layout) << fmt::format("{} {}", file.layout, layout);

      // can't setLayout on file
      auto result = co_await metaClient_->setLayout(rootUser, InodeId::root(), "file-custom-layout-chainlist", layout);
      CO_ASSERT_ERROR(result, MetaCode::kNotDirectory);
    }

    {
      // chain table 1 only have 128 chains
      CO_AWAIT_ASSERT_ERROR(MetaCode::kInvalidFileLayout,
                            metaClient_->create(rootUser,
                                                InodeId::root(),
                                                "file-on-chain-table-1",
                                                std::nullopt,
                                                p644,
                                                O_EXCL,
                                                Layout::newEmpty(ChainTableId(1), 4096, 129)));
      CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser,
                                             InodeId::root(),
                                             "file-on-chain-table-2",
                                             std::nullopt,
                                             p644,
                                             O_EXCL,
                                             Layout::newEmpty(ChainTableId(2), 4096, 129)));

      // chain table 3 contains no chains
      CO_AWAIT_ASSERT_ERROR(MetaCode::kInvalidFileLayout,
                            metaClient_->create(rootUser,
                                                InodeId::root(),
                                                "file-on-chain-table-3",
                                                std::nullopt,
                                                p644,
                                                O_EXCL,
                                                Layout::newEmpty(ChainTableId(3), 4096, 1)));
    }

    {
      // create with invalid layout
      std::vector<std::tuple<ChainTableId, uint32_t, uint32_t>> params{
          {ChainTableId(3), 4096, 1},
          {ChainTableId(1), 4096 - 1, 1},
          {ChainTableId(1), 4096, 0},
      };
      for (auto param : params) {
        auto [table, chunk, stripe] = param;
        auto result = co_await metaClient_->create(rootUser,
                                                   InodeId::root(),
                                                   "file-invalid-layout",
                                                   std::nullopt,
                                                   p644,
                                                   O_EXCL,
                                                   Layout::newEmpty(table, chunk, stripe));
        CO_ASSERT_TRUE(result.hasError());
        CO_ASSERT_TRUE(result.error().code() == MetaCode::kInvalidFileLayout ||
                       result.error().code() == StatusCode::kInvalidArg);
      }
    }
  }());
}

TEST_F(TestMetaClient, testMkdirs) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_ERROR(MetaCode::kExists, metaClient_->mkdirs(rootUser, InodeId::root(), "a", p777, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kExists, metaClient_->mkdirs(rootUser, InodeId::root(), "a", p777, true));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kExists, metaClient_->mkdirs(rootUser, InodeId::root(), "a/b/c", p777, true));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound, metaClient_->mkdirs(rootUser, InodeId::root(), "a/c/d", p777, false));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->mkdirs(rootUser, InodeId::root(), "a/c/./d", p777, true));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory,
                          metaClient_->mkdirs(rootUser, InodeId::root(), "a/b/d/a", p777, true));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "u", p777, true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "u/a", p777, true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "u/b", Permission(0775), true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "u/c", Permission(0760), true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "u/d", Permission(0700), true));

    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->mkdirs(ub, InodeId::root(), "u/d/b", p777, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->mkdirs(uc, InodeId::root(), "u/d/c", p777, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->mkdirs(ub, InodeId::root(), "u/c/b", p777, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->mkdirs(uc, InodeId::root(), "u/c/c", p777, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->mkdirs(uc, InodeId::root(), "u/b/c", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ub, InodeId::root(), "u/b/b", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ub, InodeId::root(), "u/a/b", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(uc, InodeId::root(), "u/a/c", p777, false));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "e/a", p777, false));
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/b/c/a", false);

    {
      // create directory without layout, should inherit root
      CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "no-layout", Permission(0700), false));
      auto inode = co_await metaClient_->stat(rootUser, InodeId::root(), "no-layout", true);
      CO_ASSERT_OK(inode);
      CO_ASSERT_TRUE(inode->isDirectory());
      auto rootInode = co_await metaClient_->stat(rootUser, InodeId::root(), std::nullopt, true);
      CO_ASSERT_OK(rootInode);
      auto dir = inode->asDirectory();
      CO_ASSERT_EQ(dir.layout, rootInode->asDirectory().layout);
    }

    {
      // create directory with layout
      auto layout = Layout::newEmpty(ChainTableId(1), 4096, 32);
      auto inode =
          co_await metaClient_->mkdirs(rootUser, InodeId::root(), "dir-custom-layout", Permission(0700), false, layout);
      CO_ASSERT_TRUE(inode->isDirectory());
      auto dir = inode->asDirectory();
      CO_ASSERT_EQ(dir.layout, layout);

      // create file under directory
      auto createResult = co_await metaClient_->create(rootUser,
                                                       InodeId::root(),
                                                       "dir-custom-layout/file",
                                                       std::nullopt,
                                                       Permission(0777),
                                                       O_EXCL);
      CO_ASSERT_OK(createResult);
      auto f1 = createResult.value().asFile();
      f1.layout.chains = Layout::Empty();
      CO_ASSERT_NE(f1.layout.tableVersion, ChainTableVersion(0));
      f1.layout.tableVersion = ChainTableVersion(0);
      CO_ASSERT_EQ(f1.layout, layout);

      // create file under directory with custom layout
      auto fileLayout = Layout::newEmpty(ChainTableId(1), 8192, 8);
      createResult = co_await metaClient_->create(rootUser,
                                                  InodeId::root(),
                                                  "dir-custom-layout/file-custome-layout",
                                                  std::nullopt,
                                                  Permission(0777),
                                                  O_EXCL,
                                                  fileLayout);
      CO_ASSERT_OK(createResult);
      auto f2 = createResult.value().asFile();
      f2.layout.chains = Layout::Empty();
      CO_ASSERT_NE(f2.layout.tableVersion, ChainTableVersion(0));
      f2.layout.tableVersion = ChainTableVersion(0);
      CO_ASSERT_EQ(f2.layout, fileLayout);

      // create directory under directory, should inherit layout.
      auto mkdirResult = co_await metaClient_->mkdirs(rootUser,
                                                      InodeId::root(),
                                                      "dir-custom-layout/no-layout",
                                                      Permission(0700),
                                                      false);
      CO_ASSERT_OK(mkdirResult);
      auto subdir = mkdirResult->asDirectory();
      CO_ASSERT_EQ(subdir.layout, layout);
    }

    {
      // set invalid layout
      auto layout = Layout::newEmpty(ChainTableId(100), 100, 644);
      auto result = co_await metaClient_->setLayout(rootUser, InodeId::root(), "dir-custom-layout", layout);
      CO_ASSERT_ERROR(result, MetaCode::kInvalidFileLayout);
    }

    {
      // set valid layout
      auto layout = Layout::newEmpty(ChainTableId(2), 512 * 1024, 32);
      auto result = co_await metaClient_->setLayout(rootUser, InodeId::root(), "dir-custom-layout", layout);
      // create file under directory, should use new layout
      auto createResult = co_await metaClient_->create(rootUser,
                                                       InodeId::root(),
                                                       "dir-custom-layout/file-2",
                                                       std::nullopt,
                                                       Permission(0777),
                                                       O_EXCL);
      CO_ASSERT_OK(createResult);
      auto f = createResult.value().asFile();
      f.layout.chains = Layout::Empty();
      f.layout.tableVersion = ChainTableVersion(0);
      CO_ASSERT_EQ(f.layout, layout) << fmt::format("{}", f.layout);
    }

    {
      // mkdir should also accept non-empty layout
      auto layout = Layout::newChainList(ChainTableId(1), ChainTableVersion(1), 512 << 10, {1, 2, 3, 4, 5});
      CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "dir-non-empty-layout", p644, O_EXCL, layout));
    }

    {
      // mkdir with chain list
      std::vector<flat::ChainId> chains;
      for (size_t i = 0; i < 4; i++) {
        auto chain = mgmtdClient_->getRoutingInfo()->raw()->getChainId(
            flat::ChainRef{flat::ChainTableId(1), flat::ChainTableVersion(0), i + 1});
        CO_ASSERT_TRUE(chain.has_value());
        chains.push_back(*chain);
      }
      auto layout = Layout::newChainList(512 << 10, chains);
      CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "dir-chain-list", p644, O_EXCL, layout));

      auto createResult = co_await metaClient_->create(rootUser,
                                                       InodeId::root(),
                                                       "dir-chain-list/filE",
                                                       std::nullopt,
                                                       Permission(0777),
                                                       O_EXCL);
      CO_ASSERT_OK(createResult);
      auto f = createResult.value().asFile();
      CO_ASSERT_EQ(f.layout, layout) << fmt::format("{}", f.layout);
    }

    {
      auto routing = mgmtdClient_->getRoutingInfo()->raw();
      CO_ASSERT_FALSE(routing->chains.empty());
      for (auto &chain : routing->chains) {
        CO_ASSERT_EQ(
            routing->getChainId(flat::ChainRef{flat::ChainTableId(0), flat::ChainTableVersion(0), chain.first}),
            chain.first);
      }
    }
  }());
}

TEST_F(TestMetaClient, testSymlink) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_ERROR(MetaCode::kExists, metaClient_->symlink(rootUser, InodeId::root(), "e", "a"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kExists, metaClient_->symlink(rootUser, InodeId::root(), "a/b/d", "e"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->symlink(ua, InodeId::root(), "f", "a"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound, metaClient_->symlink(rootUser, InodeId::root(), "a/c/d", "e"));

    CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), "a/b/c/x", "/a/b/c/x"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kTooManySymlinks,
                          metaClient_->symlink(rootUser, InodeId::root(), "a/b/c/x/y", "/a/b/d"));
  }());
}

TEST_F(TestMetaClient, testHardLink) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_OK(metaClient_->hardLink(rootUser, InodeId::root(), "a/b/d", InodeId::root(), "d-hardlink", false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kIsDirectory,
                          metaClient_->hardLink(rootUser, InodeId::root(), "a", InodeId::root(), "a-hardlink", false));
    CO_AWAIT_ASSERT_OK(metaClient_->remove(rootUser, InodeId::root(), "a/b/d", false));
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "d-hardlink", false);
    auto inode = (co_await metaClient_->stat(rootUser, InodeId::root(), "d-hardlink", true))->id;
    auto result = co_await metaClient_->hardLink(rootUser, inode, std::nullopt, InodeId::root(), "hardlink-2", false);
    CO_ASSERT_OK(result);
    CO_ASSERT_EQ(inode, result->id);
  }());
}

TEST_F(TestMetaClient, testRemove) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), ".", true));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), ".", false));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), "..", true));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), "..", false));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), "a/.", true));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), "a/..", true));

    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound, metaClient_->remove(rootUser, InodeId::root(), "notexists", false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound, metaClient_->remove(rootUser, InodeId::root(), "notexists", true));

    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg, metaClient_->remove(rootUser, InodeId::root(), "/", true));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->remove(ua, InodeId::root(), "a", true));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "empty", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->remove(rootUser, InodeId::root(), "empty", false));

    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotEmpty, metaClient_->remove(rootUser, InodeId::root(), "a/b/c", false));
    CO_AWAIT_ASSERT_OK(metaClient_->remove(rootUser, InodeId::root(), "a/b/c", true));
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "a/b/c/d", false);

    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "a/x", std::nullopt, p700, 0));
    CO_AWAIT_ASSERT_OK(metaClient_->remove(ub, InodeId::root(), "a/x", false));

    CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), "a/x", "/a/b/d"));
    CO_AWAIT_ASSERT_OK(metaClient_->remove(rootUser, InodeId::root(), "a/x", false));
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "a/x", false);
    CO_ASSERT_EXISTS(rootUser, InodeId::root(), "a/b/d", false);

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "dir/subdir", p700, true));
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, InodeId::root(), "file", {}, p700, O_RDONLY));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kIsDirectory, metaClient_->unlink(rootUser, InodeId::root(), "dir"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory, metaClient_->rmdir(rootUser, InodeId::root(), "file", false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory, metaClient_->rmdir(rootUser, InodeId::root(), "file", true));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotEmpty, metaClient_->rmdir(rootUser, InodeId::root(), "dir", false));
    CO_AWAIT_ASSERT_OK(metaClient_->rmdir(rootUser, InodeId::root(), "dir", true));
    CO_AWAIT_ASSERT_OK(metaClient_->unlink(rootUser, InodeId::root(), "file"));

    auto dir = co_await metaClient_->mkdirs(rootUser, InodeId::root(), "dir", p700, true);
    CO_ASSERT_OK(dir);
    auto file = co_await metaClient_->create(rootUser, InodeId::root(), "dir/file", {}, p700, O_RDONLY);
    CO_ASSERT_OK(file);
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory, metaClient_->remove(rootUser, file->id, std::nullopt, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotEmpty, metaClient_->rmdir(rootUser, dir->id, std::nullopt, false));
    CO_AWAIT_ASSERT_OK(metaClient_->rmdir(rootUser, dir->id, std::nullopt, true));
  }());
}

TEST_F(TestMetaClient, testRename) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), ".", InodeId::root(), "."));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), ".", InodeId::root(), ".."));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "..", InodeId::root(), "."));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "..", InodeId::root(), ".."));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "."));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), ".."));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), ".", InodeId::root(), "a"));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "..", InodeId::root(), "a"));

    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotFound,
                          metaClient_->rename(rootUser, InodeId::root(), "notexists", InodeId::root(), "x"));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "a/b/a"));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "a/b/c/a"));
    CO_AWAIT_ASSERT_ERROR(StatusCode::kInvalidArg,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "e/a"));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "a/a1", Permission(0770), true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ub, InodeId::root(), "a/b1", Permission(0770), true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ub, InodeId::root(), "a/b2", Permission(0760), true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ub, InodeId::root(), "a/b3", Permission(0750), true));

    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "a/a1/x", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_OK(metaClient_->rename(ua, InodeId::root(), "a/a1/x", InodeId::root(), "a/b1/x"));
    CO_AWAIT_ASSERT_OK(metaClient_->remove(ua, InodeId::root(), "a/b1/x", false));

    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "a/a1/x", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission,
                          metaClient_->rename(ua, InodeId::root(), "a/a1/x", InodeId::root(), "a/b2/x"));

    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "a/a1/x", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission,
                          metaClient_->rename(ua, InodeId::root(), "a/a1/x", InodeId::root(), "a/b3/x"));

    auto a = getInode("a", false);
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "a/b/c", a.id, "b/c"));

    auto abd = getInode("a/b/d", false);
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "a/b/d", InodeId::root(), "a/b/c/d"));
    auto abcd = getInode("a/b/c/d", false);
    CO_ASSERT_EQ(abd, abcd);
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "a/b/d", false);

    CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), "x", "/a"));
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "a/b/c/d", InodeId::root(), "x"));
    auto x = getInode("x", false);
    CO_ASSERT_EQ(abd, x);
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "a/b/c/d", false);

    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotEmpty,
                          metaClient_->rename(rootUser, InodeId::root(), "x", InodeId::root(), "a/b"));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "empty", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "x", InodeId::root(), "empty"));
    CO_ASSERT_EQ(abd, getInode("empty", false));
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "x", false);

    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "empty", InodeId::root(), "e"));
    CO_ASSERT_EQ(abd, getInode("e", false));
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "empty", false);

    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, InodeId::root(), "file", std::nullopt, p644, 0));
    CO_AWAIT_ASSERT_OK(metaClient_->symlink(rootUser, InodeId::root(), "link", "/file"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "file"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory,
                          metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "link"));

    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "e", InodeId::root(), "a/b/d"));
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, InodeId::root(), "a", InodeId::root(), "b"));
    CO_ASSERT_EQ(abd, getInode("b/b/d", false));
    CO_ASSERT_NOT_EXISTS(rootUser, InodeId::root(), "a", true);
  }());
}

TEST_F(TestMetaClient, testSticky) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, InodeId::root(), "ua", Permission(0777), true));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "ua/stickyA", Permission(0777 | S_ISVTX), false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "ua/stickyB", Permission(0777 | S_ISVTX), false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "ua/nostickyC", Permission(0777), false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "ua/stickyA/file", {}, Permission(0777), O_RDONLY, {}));
    CO_AWAIT_ASSERT_OK(metaClient_->create(ua, InodeId::root(), "ua/stickyB/file", {}, Permission(0777), O_RDONLY, {}));
    CO_AWAIT_ASSERT_OK(
        metaClient_->create(ua, InodeId::root(), "ua/nostickyC/file", {}, Permission(0777), O_RDONLY, {}));

    // user b
    CO_AWAIT_ASSERT_ERROR(
        MetaCode::kNoPermission,
        metaClient_->rename(ub, InodeId::root(), "ua/stickyA/file", InodeId::root(), "ua/stickyB/file"));
    CO_AWAIT_ASSERT_ERROR(
        MetaCode::kNoPermission,
        metaClient_->rename(ub, InodeId::root(), "ua/stickyA/file", InodeId::root(), "ua/nostickyC/file"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->unlink(ub, InodeId::root(), "ua/stickyA/file"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->unlink(ub, InodeId::root(), "ua/stickyB/file"));
    CO_AWAIT_ASSERT_OK(metaClient_->unlink(ub, InodeId::root(), "ua/nostickyC/file"));

    // user A
    CO_AWAIT_ASSERT_OK(metaClient_->rename(ua, InodeId::root(), "ua/stickyA/file", InodeId::root(), "ua/stickyB/file"));
    CO_AWAIT_ASSERT_OK(
        metaClient_->rename(ua, InodeId::root(), "ua/stickyB/file", InodeId::root(), "ua/nostickyC/file"));
    CO_AWAIT_ASSERT_OK(metaClient_->unlink(ua, InodeId::root(), "ua/nostickyC/file"));
  }());
}

TEST_F(TestMetaClient, testBatchStat) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto a = *(co_await metaClient_->stat(ua, InodeId::root(), "a", false));
    auto b = *(co_await metaClient_->stat(ua, InodeId::root(), "a/b", false));
    auto c = *(co_await metaClient_->stat(ua, InodeId::root(), "a/b/c", false));
    auto d = *(co_await metaClient_->stat(ua, InodeId::root(), "a/d", false));
    auto inodes =
        *(co_await metaClient_->batchStat(ua, {a.id, b.id, c.id, d.id, InodeId(std::numeric_limits<uint32_t>::max())}));
    CO_ASSERT_EQ(a, inodes[0]);
    CO_ASSERT_EQ(b, inodes[1]);
    CO_ASSERT_EQ(c, inodes[2]);
    CO_ASSERT_EQ(d, inodes[3]);
    CO_ASSERT_EQ(std::nullopt, inodes[4]);
  }());
}

TEST_F(TestMetaClient, testBatchStatByPath) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_ASSERT_OK(co_await metaClient_
                     ->setPermission(rootUser, InodeId::root(), "a/b/c", true, std::nullopt, std::nullopt, p700));
    auto a = co_await metaClient_->stat(ua, InodeId::root(), "a", false);
    auto b = co_await metaClient_->stat(ua, InodeId::root(), "a/b", false);
    auto c = co_await metaClient_->stat(ua, InodeId::root(), "a/b/c", false);
    auto cd = co_await metaClient_->stat(ua, InodeId::root(), "a/b/c/d", false);
    auto ef = co_await metaClient_->stat(ua, InodeId::root(), "e", true);
    auto enf = co_await metaClient_->stat(ua, InodeId::root(), "e", false);
    CO_ASSERT_ERROR(cd, MetaCode::kNoPermission);
    for (const auto &r : std::vector<Result<Inode>>{a, b, c, ef, enf}) {
      CO_ASSERT_OK(r);
    }
    std::vector<PathAt> paths{PathAt("a"),
                              PathAt("a/b"),
                              PathAt("a/b/c"),
                              PathAt("a/b/c/d"),
                              PathAt("e"),
                              PathAt("not-found")};
    std::vector<Result<Inode>> expected{a, b, c, cd};
    for (size_t i = 0; i < 2; i++) {
      auto inodes = *(co_await metaClient_->batchStatByPath(ua, paths, i));
      for (size_t j = 0; j < expected.size(); j++) {
        CO_ASSERT_EQ(expected[j], inodes[j]) << fmt::format("{} != {}", expected[j], inodes[j]);
      }
      if (i) {
        CO_ASSERT_EQ(inodes[4], ef) << fmt::format("{} != {}", inodes[5], ef);
      } else {
        CO_ASSERT_EQ(inodes[4], enf) << fmt::format("{} != {}", inodes[5], enf);
      }
      CO_ASSERT_ERROR(inodes[5], MetaCode::kNotFound);
    }
  }());
}

TEST_F(TestMetaClient, testList) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNotDirectory, metaClient_->list(rootUser, InodeId::root(), "a/b/d", "", 1, false));

    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(ua, InodeId::root(), "a/a", Permission(0743), 0));

    auto a = co_await metaClient_->stat(ua, InodeId::root(), "a", true);
    CO_ASSERT_OK(a);
    auto aa = co_await metaClient_->stat(ua, InodeId::root(), "a/a", true);
    CO_ASSERT_OK(aa);

    CO_AWAIT_ASSERT_OK(metaClient_->list(ub, InodeId::root(), "a/a", "", 1, false));
    CO_AWAIT_ASSERT_OK(metaClient_->list(ub, a->id, std::nullopt, "", 1, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoPermission, metaClient_->list(uc, InodeId::root(), "a/a", "", 1, false));

    for (int i = 0; i < 2; i++) {
      {
        auto r = (i == 0) ? (co_await metaClient_->list(rootUser, InodeId::root(), "a", "", 10, false))
                          : (co_await metaClient_->list(ub, a->id, std::nullopt, "", 10, false));
        CO_ASSERT_OK(r);
        const auto &result = r.value();

        CO_ASSERT_TRUE(!result.entries.empty());
        CO_ASSERT_EQ(result.entries.size(), 3);
        CO_ASSERT_TRUE(!result.more);
        CO_ASSERT_TRUE(result.inodes.empty());

        auto a = getInode("a", false);
        auto aa = getInode("a/a", false);
        const auto &e0 = result.entries.at(0);
        CO_ASSERT_EQ(e0.parent, a.id);
        CO_ASSERT_EQ(e0.name, "a");
        CO_ASSERT_EQ(e0.id, aa.id);
        CO_ASSERT_EQ(e0.type, meta::InodeType::Directory);

        auto ab = getInode("a/b", false);
        const auto &e1 = result.entries.at(1);
        CO_ASSERT_EQ(e1.parent, a.id);
        CO_ASSERT_EQ(e1.name, "b");
        CO_ASSERT_EQ(e1.id, ab.id);
        CO_ASSERT_EQ(e1.type, meta::InodeType::Directory);

        auto ad = getInode("a/d", false);
        const auto &e2 = result.entries.at(2);
        CO_ASSERT_EQ(e2.parent, a.id);
        CO_ASSERT_EQ(e2.name, "d");
        CO_ASSERT_EQ(e2.id, ad.id);
        CO_ASSERT_EQ(e2.type, meta::InodeType::Symlink);
      }
      {
        auto r = (i == 0) ? (co_await metaClient_->list(rootUser, InodeId::root(), "a", "a", 10, false))
                          : (co_await metaClient_->list(ub, a->id, std::nullopt, "a", 10, false));
        CO_ASSERT_OK(r);
        const auto &result = r.value();

        CO_ASSERT_TRUE(!result.entries.empty());
        CO_ASSERT_EQ(result.entries.size(), 2);
        CO_ASSERT_TRUE(!result.more);
        CO_ASSERT_TRUE(result.inodes.empty());

        auto a = getInode("a", false);
        auto ab = getInode("a/b", false);
        const auto &e1 = result.entries.at(0);
        CO_ASSERT_EQ(e1.parent, a.id);
        CO_ASSERT_EQ(e1.name, "b");
        CO_ASSERT_EQ(e1.id, ab.id);
        CO_ASSERT_EQ(e1.type, meta::InodeType::Directory);

        auto ad = getInode("a/d", false);
        const auto &e2 = result.entries.at(1);
        CO_ASSERT_EQ(e2.parent, a.id);
        CO_ASSERT_EQ(e2.name, "d");
        CO_ASSERT_EQ(e2.id, ad.id);
        CO_ASSERT_EQ(e2.type, meta::InodeType::Symlink);
      }
    }
  }());
}

TEST_F(TestMetaClient, extendStripe) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto layout = Layout::newEmpty(flat::ChainTableId(1), 4 << 10, 128);
    auto file1 = co_await metaClient_->create(rootUser, InodeId::root(), "file1", Uuid::random(), p644, O_RDWR, layout);
    CO_ASSERT_OK(file1);
    auto file2 = co_await metaClient_->create(rootUser, InodeId::root(), "file2", Uuid::random(), p644, O_RDWR, layout);
    CO_ASSERT_OK(file2);
    auto file3 = co_await metaClient_->create(rootUser, InodeId::root(), "file3", Uuid::random(), p644, O_RDWR, layout);
    CO_ASSERT_OK(file3);

    CO_ASSERT_EQ(file1->asFile().dynStripe, 1);
    CO_ASSERT_EQ(file2->asFile().dynStripe, 1);

    auto open1 = co_await metaClient_->open(rootUser, file1->id, std::nullopt, Uuid::random(), O_RDWR);
    CO_ASSERT_OK(open1);
    CO_ASSERT_EQ(open1->asFile().dynStripe, 1);

    for (auto &[req, rsp] : std::vector<std::pair<uint32_t, uint32_t>>{{1, 1},
                                                                       {2, 2},
                                                                       {3, 4},
                                                                       {38, 64},
                                                                       {32, 64},
                                                                       {129, 128},
                                                                       {10024, 128}}) {
      auto extend = co_await metaClient_->extendStripe(rootUser, file1->id, req);
      CO_ASSERT_OK(extend);
      CO_ASSERT_EQ(extend->asFile().dynStripe, rsp);
    }

    auto open2 = co_await metaClient_->open(rootUser, file2->id, std::nullopt, Uuid::random(), O_RDWR);
    CO_ASSERT_OK(open2);
    CO_ASSERT_EQ(open2->asFile().dynStripe, 1);
    auto truncate = co_await metaClient_->truncate(rootUser, file2->id, 64ULL << 30);
    CO_ASSERT_OK(truncate);
    CO_ASSERT_EQ(truncate->asFile().dynStripe, 128);

    metaClientConfig_.set_dynamic_stripe(false);
    auto open3 = co_await metaClient_->open(rootUser, file3->id, std::nullopt, Uuid::random(), O_RDWR);
    CO_ASSERT_OK(open3);
    CO_ASSERT_EQ(open3->asFile().dynStripe, 0);

    for (auto &[req, rsp] :
         std::vector<std::pair<uint32_t, uint32_t>>{{1, 0}, {2, 0}, {3, 0}, {38, 0}, {129, 0}, {10024, 0}}) {
      auto extend = co_await metaClient_->extendStripe(rootUser, file3->id, req);
      CO_ASSERT_OK(extend);
      CO_ASSERT_EQ(extend->asFile().dynStripe, rsp);
    }
  }());
}

TEST_F(TestMetaClient, lockDirectory) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto locked = (co_await metaClient_->mkdirs(rootUser, InodeId::root(), "lock-dir", p777, false)).value();
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, locked.id, "subdir1", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, locked.id, "subdir2", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, locked.id, "file1", std::nullopt, p644, O_RDONLY));

    auto dir = (co_await metaClient_->mkdirs(rootUser, InodeId::root(), "nolock-dir", p777, false)).value();
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, dir.id, "subdir1", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, dir.id, "subdir2", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, dir.id, "file1", std::nullopt, p644, O_RDONLY));

    LockDirectoryReq lockReq(rootUser, locked.id, LockDirectoryReq::LockAction::TryLock);
    lockReq.client = ClientId::random();
    CO_AWAIT_ASSERT_OK(metaOperator_->lockDirectory(lockReq));
    CO_AWAIT_ASSERT_OK(metaOperator_->lockDirectory(lockReq));
    // can't do anything under locked dir
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoLock,
                          metaClient_->lockDirectory(rootUser, locked.id, LockDirectoryReq::LockAction::TryLock));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoLock, metaClient_->mkdirs(rootUser, locked.id, "subdir3", p777, false));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoLock,
                          metaClient_->create(rootUser, locked.id, "file2", std::nullopt, p644, O_RDONLY));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoLock, metaClient_->rename(rootUser, locked.id, "subdir1", locked.id, "subdir2"));
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoLock,
                          metaClient_->hardLink(rootUser, locked.id, "file1", locked.id, "file1-link", false));
    // can do under not locked dir
    CO_AWAIT_ASSERT_OK(metaClient_->lockDirectory(rootUser, dir.id, LockDirectoryReq::LockAction::TryLock));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, dir.id, "subdir3", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, dir.id, "file2", std::nullopt, p644, O_RDONLY));
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, dir.id, "subdir1", dir.id, "subdir2"));
    CO_AWAIT_ASSERT_OK(metaClient_->hardLink(rootUser, dir.id, "file1", dir.id, "file1-link", false));

    // preempt_lock lock, then can do anything
    CO_AWAIT_ASSERT_OK(metaClient_->lockDirectory(rootUser, locked.id, LockDirectoryReq::LockAction::PreemptLock));
    CO_AWAIT_ASSERT_OK(metaClient_->mkdirs(rootUser, locked.id, "subdir3", p777, false));
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, locked.id, "file2", std::nullopt, p644, O_RDONLY));
    CO_AWAIT_ASSERT_OK(metaClient_->rename(rootUser, locked.id, "subdir1", locked.id, "subdir2"));
    CO_AWAIT_ASSERT_OK(metaClient_->hardLink(rootUser, locked.id, "file1", locked.id, "file1-link", false));

    lockReq.action = LockDirectoryReq::LockAction::UnLock;
    CO_AWAIT_ASSERT_ERROR(MetaCode::kNoLock, metaOperator_->lockDirectory(lockReq));
    lockReq.action = LockDirectoryReq::LockAction::Clear;
    CO_AWAIT_ASSERT_OK(metaOperator_->lockDirectory(lockReq));

    // create without lock
    CO_AWAIT_ASSERT_OK(metaClient_->create(rootUser, locked.id, "file2", std::nullopt, p644, O_RDONLY));
  }());
}

TEST_F(TestMetaClient, updateSelection) {
  auto worker = [&]() -> CoTask<void> {
    auto begin = SteadyClock::now();
    while (SteadyClock::now() < begin + 5_s) {
      CO_ASSERT_OK(co_await metaClient_->testRpc());
    }
  };
  std::vector<folly::SemiFuture<Void>> tasks;
  folly::CPUThreadPoolExecutor exec(8);
  for (size_t i = 0; i < 8; i++) {
    tasks.push_back(worker().scheduleOn(&exec).start());
  }

  // todo: make sure update is triggered
  auto begin = SteadyClock::now();
  while (SteadyClock::now() < begin + 5_s) {
    auto cfg = config();
    switch (folly::Random::rand32(4)) {
      case 0:
        cfg.set_selection_mode(ServerSelectionMode::UniformRandom);
        break;
      case 1:
        cfg.set_selection_mode(ServerSelectionMode::RoundRobin);
        break;
      case 2:
        cfg.set_selection_mode(ServerSelectionMode::RandomFollow);
        break;
      case 3:
        mgmtdClient_->addNodes(1, 0);
        break;
    }
    ASSERT_OK(config().update(cfg.toToml(), true));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  for (auto &task : tasks) {
    task.wait();
  }
}

// todo: use multiple meta servers and randomly disable some of them, should success no matter what server selection
// logic is used.
struct MockMetaService : public meta::MockMetaService {
  struct State {
    std::mutex mutex;
    std::atomic<bool> enableInject{false};
    std::set<InodeId> closed;
    std::set<Uuid> pruned;

    bool checkClosed(InodeId inode) {
      std::lock_guard<std::mutex> lock(mutex);
      return closed.count(inode);
    }
    bool checkPruned(Uuid session) {
      std::lock_guard<std::mutex> lock(mutex);
      return pruned.count(session);
    }
  };

  MockMetaService(bool inject, std::shared_ptr<State> state)
      : state(state),
        inject(inject) {}

  std::shared_ptr<State> state;
  bool inject;

  bool doInject() { return inject && state->enableInject; }

  CoTryTask<StatRsp> stat(serde::CallContext &, const StatReq &req) override {
    if (doInject()) {
      XLOGF(WARN, "Inject fault in open.");
      co_return makeError(RPCCode::kTimeout);
    }
    Inode inode({InodeId(folly::Random::rand64()), meta::InodeData{File()}});
    co_return {inode};
  }

  CoTryTask<OpenRsp> open(serde::CallContext &, const OpenReq &req) override {
    if (doInject()) {
      XLOGF(WARN, "Inject fault in open.");
      co_return makeError(RPCCode::kTimeout);
    }
    Inode inode({InodeId(folly::Random::rand64()), meta::InodeData{File()}});
    co_return OpenRsp(inode, false);
  }

  CoTryTask<CloseRsp> close(serde::CallContext &, const CloseReq &req) override {
    if (doInject()) {
      XLOGF(WARN, "Inject fault in close.");
      co_return makeError(RPCCode::kTimeout);
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    state->closed.emplace(req.inode);
    co_return CloseRsp{Inode{}};
  }

  CoTryTask<PruneSessionRsp> pruneSession(serde::CallContext &, const PruneSessionReq &req) override {
    if (doInject()) {
      XLOGF(WARN, "Inject fault in pruneSession.");
      co_return makeError(RPCCode::kTimeout);
    }
    std::lock_guard<std::mutex> lock(state->mutex);
    for (auto session : req.sessions) {
      state->pruned.emplace(session);
    }
    co_return PruneSessionRsp{};
  }
};

std::unique_ptr<MetaClient> createInjectedMeta(MetaClient::Config &config,
                                               std::shared_ptr<MockMetaService::State> state) {
  auto mgmtd = hf3fs::tests::FakeMgmtdClient::create({{flat::ChainTableId(0), 8, 1}}, 5, 0);
  robin_hood::unordered_map<net::Address, serde::ClientMockContext> contextMap;
  std::set<flat::NodeId> injectNodes;
  for (auto [nodeId, nodeInfo] : mgmtd->getRoutingInfo()->raw()->nodes) {
    bool inject = false;
    if (injectNodes.size() < 4) {
      injectNodes.insert(nodeId);
      inject = true;
      XLOGF(WARN, "Inject on node {}", nodeId);
    }
    for (auto addr : nodeInfo.extractAddresses("MetaSerde")) {
      auto ctx =
          serde::ClientMockContext::create(std::unique_ptr<meta::MockMetaService>(new MockMetaService(inject, state)));
      contextMap.emplace(addr, std::move(ctx));
    }
  }
  auto factory = std::make_unique<MetaClient::StubFactory>(std::move(contextMap));
  auto meta = std::make_unique<MetaClient>(ClientId::random(), config, std::move(factory), mgmtd, nullptr, true);
  return meta;
}

TEST(TestMetaClientMock, Retry) {
  auto doTest = [](ServerSelectionMode mode) -> CoTask<void> {
    MetaClient::Config config;
    config.retry_default().set_retry_init_wait(1_ms);
    config.retry_default().set_retry_max_wait(1_ms);
    config.retry_default().set_retry_total_time(5_s);
    config.set_selection_mode(mode);

    auto state = std::make_shared<MockMetaService::State>();
    state->enableInject = true;
    auto meta = createInjectedMeta(config, state);

    for (size_t i = 0; i < 10; i++) {
      auto result = co_await meta->stat({}, InodeId::root(), "random-path", true);
      CO_ASSERT_OK(result);
    }
    for (size_t i = 0; i < 10; i++) {
      auto session = Uuid::random();
      auto result = co_await meta->open({}, InodeId::root(), "random-path", session, O_RDWR);
      CO_ASSERT_OK(result);
    }
  };

  magic_enum::enum_for_each<ServerSelectionMode>([&](auto mode) { folly::coro::blockingWait(doTest(mode)); });
}

TEST(TestMetaClientMock, CloseAndPruneSession) {
  MetaClient::Config config;
  config.retry_default().set_retry_max_wait(0_s);
  config.set_selection_mode(ServerSelectionMode::UniformRandom);
  config.background_closer().set_prune_session_batch_count(4);
  config.background_closer().set_prune_session_batch_interval(100_ms);
  config.background_closer().set_task_scan(10_ms);
  config.background_closer().set_retry_first_wait(20_ms);
  config.background_closer().set_retry_max_wait(20_ms);

  auto state = std::make_shared<MockMetaService::State>();
  state->enableInject = true;
  auto meta = createInjectedMeta(config, state);
  CPUExecutorGroup exec(2, "TestMetaClientMock");
  meta->start(exec);

  folly::coro::blockingWait([&]() -> CoTask<void> {
    std::vector<std::pair<InodeId, Uuid>> opened;
    std::vector<Uuid> sessions;
    state->enableInject = true;
    for (size_t i = 0; i < 32; i++) {
      auto session = Uuid::random();
      auto result = co_await meta->open({}, InodeId::root(), "random-path", session, O_RDWR);
      if (result.hasError()) {
        sessions.push_back(session);
      } else {
        opened.push_back(std::pair<InodeId, Uuid>(result->id, session));
      }
    }
    for (auto [inode, session] : opened) {
      co_await meta->close({}, inode, session, false, false);
    }
    co_await folly::coro::sleep(std::chrono::seconds(1));
    state->enableInject = false;
    co_await folly::coro::sleep(std::chrono::seconds(5));
    for (auto [inode, session] : opened) {
      CO_ASSERT_TRUE(state->checkClosed(inode));
    }
    for (auto session : sessions) {
      CO_ASSERT_TRUE(state->checkPruned(session)) << session.toHexString();
    }
  }());
  meta->stop();
}

TEST(TestMetaClientMock, ServerSelection) {
  auto mgmtd = hf3fs::tests::FakeMgmtdClient::create({}, 5, 0);
  auto metas =
      mgmtd->getRoutingInfo()->getNodeBy(flat::selectNodeByType(flat::NodeType::META) && flat::selectActiveNode());
  auto checkDist = [](ServerSelectionMode mode, std::map<flat::NodeId, size_t> count) {
    std::vector<std::pair<size_t, flat::NodeId>> vec;
    vec.reserve(count.size());
    for (auto [k, v] : count) {
      vec.push_back({v, k});
    }
    std::sort(vec.begin(), vec.end());
    ASSERT_GE(vec.begin()->first * 2, vec.rbegin()->first) << magic_enum::enum_name(mode);
    fmt::print("mode {}, min {} {}, max {} {}\n",
               magic_enum::enum_name(mode),
               vec.begin()->first,
               vec.begin()->second,
               vec.rbegin()->first,
               vec.rbegin()->second);
  };

  // check distribution
  magic_enum::enum_for_each<ServerSelectionMode>([&](auto mode) {
    std::map<flat::NodeId, size_t> totalCount;
    bool follow = mode == ServerSelectionMode::RandomFollow;
    for (size_t i = 0; i < (follow ? 1000 : 10); i++) {
      std::map<flat::NodeId, size_t> count;
      auto strategy = ServerSelectionStrategy::create(mode, mgmtd, net::Address::Type::RDMA);
      for (size_t j = 0; j < (follow ? 100 : 10000); j++) {
        auto result = strategy->select({});
        ASSERT_OK(result);
        count[result->nodeId]++;
        totalCount[result->nodeId]++;

        // update server without change meta servers
        mgmtd->setRoutingInfo(mgmtd->cloneRoutingInfo());
      }
      if (mode == ServerSelectionMode::RandomFollow) {
        // should only chost one server
        ASSERT_EQ(count.size(), 1);
      }
    }
    checkDist(mode, totalCount);
  });

  // check skip error and update config
  magic_enum::enum_for_each<ServerSelectionMode>([&](auto mode) {
    for (size_t i = 0; i < 1000; i++) {
      auto strategy = ServerSelectionStrategy::create(mode, mgmtd, net::Address::Type::RDMA);
      std::set<flat::NodeId> skip;
      for (size_t j = 0; j < 2 * 5; j++) {
        if (folly::Random::oneIn(10)) {
          mgmtd->clearNodes();
          mgmtd->addNodes(5, 0);
        }
        auto result = strategy->select(skip);
        ASSERT_OK(result);
        auto nodeId = result->nodeId;
        ASSERT_TRUE(mgmtd->getRoutingInfo()->raw()->nodes.contains(nodeId));
        if (skip.contains(nodeId)) {
          for (auto &[node, v] : mgmtd->getRoutingInfo()->raw()->nodes) {
            ASSERT_TRUE(skip.contains(node));
          }
        }
      }
    }
  });
}

TEST(TestMetaClientError, Error) {
  for (status_code_t code = 1; code < std::numeric_limits<status_code_t>::max(); code++) {
    auto success = ErrorHandling::success(Status(code));
    auto retryable = ErrorHandling::retryable(Status(code));
    auto serverError = ErrorHandling::serverError(Status(code));
    if (serverError) {
      ASSERT_FALSE(success);
      if (code != RPCCode::kInvalidMethodID) {
        ASSERT_TRUE(retryable);
      }
    }
  }
}

TEST_F(TestMetaClient, CheckRoutingInfo) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    Inode file = meta::server::Inode::newFile(
        InodeId(),
        Acl(),
        Layout::newChainRange(flat::ChainTableId(5), flat::ChainTableVersion(2), 4096, 16, 1),
        {});
    auto dir = meta::server::Inode::newDirectory(InodeId(), InodeId(), "name", Acl(), Layout(), {});

    auto routing = mgmtdClient_->getRoutingInfo()->raw();
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(file, *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(dir, *routing));

    mgmtdClient_->addChainTable(flat::ChainTableId(5), 1024);
    routing = mgmtdClient_->getRoutingInfo()->raw();
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(file, *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(dir, *routing));
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(OpenRsp(file, false), *routing));
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(CloseRsp(file), *routing));
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(StatRsp(file), *routing));
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(SyncRsp(file), *routing));
    CO_ASSERT_FALSE(RoutingInfoChecker::checkRoutingInfo(BatchStatRsp({std::optional(file)}), *routing));

    mgmtdClient_->addChainTable(flat::ChainTableId(5), 1024);
    routing = mgmtdClient_->getRoutingInfo()->raw();
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(file, *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(dir, *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(OpenRsp(file, false), *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(CloseRsp(file), *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(StatRsp(file), *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(SyncRsp(file), *routing));
    CO_ASSERT_TRUE(RoutingInfoChecker::checkRoutingInfo(BatchStatRsp({std::optional(file)}), *routing));
  }());
}

}  // namespace
}  // namespace hf3fs::meta::client::tests
