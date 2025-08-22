#pragma once

#include <fmt/core.h>
#include <folly/Function.h>
#include <folly/Synchronized.h>
#include <folly/TimeoutQueue.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "client/meta/ServerSelectionStrategy.h"
#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/storage/StorageClient.h"
#include "common/app/ClientId.h"
#include "common/app/NodeId.h"
#include "common/serde/MessagePacket.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/Duration.h"
#include "common/utils/ExponentialBackoffRetry.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/Semaphore.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "stubs/MetaService/IMetaServiceStub.h"
#include "stubs/MetaService/MetaServiceStub.h"
#include "stubs/MetaService/MockMetaServiceStub.h"
#include "stubs/common/StubFactory.h"

namespace hf3fs::meta::client {

using SessionId = hf3fs::Uuid;

using hf3fs::client::ICommonMgmtdClient;

class MetaClient {
 public:
  using StubFactory = stubs::SerdeStubFactory<MetaServiceStub, true>;
  using Stub = StubFactory::Stub;

  class RetryConfig : public ConfigBase<RetryConfig> {
    CONFIG_HOT_UPDATED_ITEM(rpc_timeout, 5_s);
    CONFIG_HOT_UPDATED_ITEM(retry_send, 1u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(retry_fast, 1_s);
    CONFIG_HOT_UPDATED_ITEM(retry_init_wait, 500_ms);
    CONFIG_HOT_UPDATED_ITEM(retry_max_wait, 5_s);
    CONFIG_HOT_UPDATED_ITEM(retry_total_time, 1_min);
    CONFIG_HOT_UPDATED_ITEM(max_failures_before_failover, 1u, ConfigCheckers::checkPositive);
  };

  class CloserConfig : public ConfigBase<CloserConfig> {
    CONFIG_HOT_UPDATED_ITEM(task_scan, 50_ms);
    CONFIG_HOT_UPDATED_ITEM(prune_session_batch_interval, 10_s);
    CONFIG_HOT_UPDATED_ITEM(prune_session_batch_count, 128);
    CONFIG_HOT_UPDATED_ITEM(retry_first_wait, 100_ms);
    CONFIG_HOT_UPDATED_ITEM(retry_max_wait, 10_s);
    CONFIG_OBJ(coroutine_pool, CoroutinesPoolBase::Config, [](CoroutinesPoolBase::Config &c) {
      c.set_coroutines_num(8);
      c.set_queue_size(128);
    });
  };

  struct Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(selection_mode, ServerSelectionMode::RandomFollow);
    CONFIG_HOT_UPDATED_ITEM(network_type, net::Address::Type::RDMA);
    CONFIG_HOT_UPDATED_ITEM(check_server_interval, 5_s);
    CONFIG_HOT_UPDATED_ITEM(max_concurrent_requests, 128u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(remove_chunks_batch_size, uint32_t(32), ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(remove_chunks_max_iters, uint32_t(1024), ConfigCheckers::checkPositive);  // deprecated

    CONFIG_HOT_UPDATED_ITEM(dynamic_stripe, false);

    CONFIG_OBJ(retry_default, RetryConfig);
    CONFIG_OBJ(background_closer, CloserConfig);
  };

  MetaClient(ClientId clientId,
             const Config &config,
             std::unique_ptr<StubFactory> factory,
             std::shared_ptr<ICommonMgmtdClient> mgmtd,
             std::shared_ptr<storage::client::StorageClient> storage,
             bool dynStripe);

  void start(CPUExecutorGroup &exec);
  void stop();

  CoTryTask<UserInfo> authenticate(const UserInfo &userInfo);

  CoTryTask<Inode> stat(const UserInfo &userInfo,
                        InodeId inodeId,
                        const std::optional<Path> &path,
                        bool followLastSymlink);

  CoTryTask<std::vector<std::optional<Inode>>> batchStat(const UserInfo &userInfo, std::vector<InodeId> inodeIds);

  CoTryTask<std::vector<Result<Inode>>> batchStatByPath(const UserInfo &userInfo,
                                                        std::vector<PathAt> paths,
                                                        bool followLastSymlink);

  CoTryTask<StatFsRsp> statFs(const UserInfo &userInfo);

  CoTryTask<Path> getRealPath(const UserInfo &userInfo, InodeId parent, const std::optional<Path> &path, bool absolute);

  CoTryTask<Inode> open(const UserInfo &userInfo,
                        InodeId inodeId,
                        const std::optional<Path> &path,
                        std::optional<SessionId> sessionId,
                        int flags);

  CoTryTask<Inode> close(const UserInfo &userInfo,
                         InodeId inodeId,
                         std::optional<SessionId> sessionId,
                         bool read,
                         bool written);
  CoTryTask<Inode> close(const UserInfo &userInfo,
                         InodeId inodeId,
                         std::optional<SessionId> sessionId,
                         bool updateLength,
                         const std::optional<UtcTime> atime,
                         const std::optional<UtcTime> mtime);

  CoTryTask<Inode> setPermission(const UserInfo &userInfo,
                                 InodeId parent,
                                 const std::optional<Path> &path,
                                 bool followLastSymlink,
                                 std::optional<Uid> uid,
                                 std::optional<Gid> gid,
                                 std::optional<Permission> perm,
                                 std::optional<IFlags> iflags = std::nullopt);

  CoTryTask<Inode> setIFlags(const UserInfo &userInfo, InodeId inode, IFlags iflags);

  CoTryTask<Inode> utimes(const UserInfo &userInfo,
                          InodeId inodeId,
                          const std::optional<Path> &path,
                          bool followLastSymlink,
                          std::optional<UtcTime> atime,
                          std::optional<UtcTime> mtime);

  CoTryTask<Inode> create(const UserInfo &userInfo,
                          InodeId parent,
                          const Path &path,
                          std::optional<SessionId> sessionId,
                          Permission perm,
                          int flags,
                          std::optional<Layout> layout = std::nullopt);

  CoTryTask<Inode> mkdirs(const UserInfo &userInfo,
                          InodeId parent,
                          const Path &path,
                          Permission perm,
                          bool recursive,
                          std::optional<Layout> layout = std::nullopt);

  CoTryTask<Inode> symlink(const UserInfo &userInfo, InodeId parent, const Path &path, const Path &target);

  // remove without check inode type
  CoTryTask<void> remove(const UserInfo &userInfo, InodeId parent, const std::optional<Path> &path, bool recursive);

  // only remove directory
  CoTryTask<void> rmdir(const UserInfo &userInfo, InodeId parent, const std::optional<Path> &path, bool recursive);

  // unlink file
  CoTryTask<void> unlink(const UserInfo &userInfo, InodeId parent, const Path &path);

  CoTryTask<Inode> rename(const UserInfo &userInfo,
                          InodeId srcParent,
                          const Path &src,
                          InodeId dstParent,
                          const Path &dst,
                          bool moveToTrash = false);

  CoTryTask<ListRsp> list(const UserInfo &userInfo,
                          InodeId inodeId,
                          const std::optional<Path> &path,
                          std::string_view prev,
                          int32_t limit,
                          bool needStatus);

  CoTryTask<Inode> setLayout(const UserInfo &userInfo, InodeId inodeId, const std::optional<Path> &path, Layout layout);

  CoTryTask<Inode> truncate(const UserInfo &userInfo, InodeId inodeId, uint64_t length);

  CoTryTask<Inode> sync(const UserInfo &userInfo,
                        InodeId inode,
                        bool read,
                        bool written,
                        std::optional<VersionedLength> hint);
  CoTryTask<Inode> sync(const UserInfo &userInfo,
                        InodeId inode,
                        bool updateLength,
                        const std::optional<UtcTime> atime,
                        const std::optional<UtcTime> mtime,
                        std::optional<VersionedLength> hint);

  CoTryTask<Inode> hardLink(const UserInfo &userInfo,
                            InodeId oldParent,
                            const std::optional<Path> &oldPath,
                            InodeId newParent,
                            const Path &newPath,
                            bool followLastSymlink);

  CoTryTask<void> lockDirectory(const UserInfo &userInfo, InodeId inode, LockDirectoryReq::LockAction action);

  CoTryTask<Inode> extendStripe(const UserInfo &userInfo, InodeId inodeId, uint32_t stripe);

  CoTryTask<Void> testRpc();

 private:
  CoTryTask<Inode> openCreate(auto func, auto req);

  CoTryTask<void> removeImpl(RemoveReq &req);

  struct CloseTask {
    std::variant<CloseReq, PruneSessionReq> req;
    Duration backoff;

    std::string describe() const {
      return fmt::format("{{{}, backoff {}}}",
                         std::visit([](const auto &req) -> std::string { return fmt::format("{}", req); }, req),
                         backoff);
    }
  };

  class PrunSessionBatch {
    SteadyTime lastTime;
    std::vector<SessionId> sessions;

   public:
    std::vector<SessionId> take(size_t batchSize, Duration batchDur) {
      if (sessions.size() < batchSize && (sessions.empty() || SteadyClock::now() - lastTime < batchDur)) {
        return {};
      }
      return std::exchange(sessions, {});
    }

    std::vector<SessionId> push(SessionId session, size_t batchSize) {
      if (sessions.empty()) {
        lastTime = SteadyClock::now();
        sessions.reserve(batchSize);
      }
      sessions.push_back(session);

      if (sessions.size() >= batchSize) {
        return std::exchange(sessions, {});
      }
      return {};
    }
  };

  CoTryTask<Inode> tryClose(const CloseReq &req, bool &needRetry);
  CoTryTask<void> tryPrune(const PruneSessionReq &req, bool &needRetry);
  CoTryTask<void> updateLayout(Layout &layout);

  CoTask<void> pruneSession(SessionId session);
  void enqueueCloseTask(CloseTask task);
  CoTask<void> scanCloseTask();
  CoTask<void> runCloseTask(CloseTask task);

  folly::Synchronized<PrunSessionBatch, std::mutex> batchPrune_;
  std::unique_ptr<BackgroundRunner> bgRunner_;
  std::unique_ptr<CoroutinesPool<CloseTask>> bgCloser_;
  folly::Synchronized<std::multimap<SteadyTime, CloseTask>, std::mutex> bgCloseTasks_;

  CoTryTask<Inode> truncateImpl(const UserInfo &userInfo, const Inode &inode, size_t targetLength);

 private:
  struct ServerNode {
    ServerSelectionStrategy::NodeInfo node;
    StubFactory::Stub stub;
    size_t failure;

    ServerNode(ServerSelectionStrategy::NodeInfo node, StubFactory::Stub stub)
        : node(std::move(node)),
          stub(std::move(stub)),
          failure(0) {}
  };
  folly::Synchronized<std::set<flat::NodeId>> errNodes_;
  CoTryTask<Stub> getStub();
  CoTryTask<ServerNode> getServerNode();
  CoTask<void> checkServers();

  template <typename Func, typename Req>
  auto retry(Func &&func, Req &&req)
      -> std::invoke_result_t<Func, Stub::IStub, Req &&, const net::UserRequestOptions &, serde::Timestamp *> {
    co_return co_await retry(func, req, config_.retry_default());
  }
  template <typename Func, typename Req>
  auto retry(Func &&func, Req &&req, RetryConfig retryConfig, std::function<void(const Status &)> onError = {})
      -> std::invoke_result_t<Func, Stub::IStub, Req &&, const net::UserRequestOptions &, serde::Timestamp *>;

  template <typename Rsp>
  CoTryTask<Void> waitRoutingInfo(const Rsp &rsp, const RetryConfig &retryConfig);

 private:
  [[maybe_unused]] ClientId clientId_;
  const Config &config_;
  const bool dynStripe_;  // only fuse client support dynamic stripe.
  std::unique_ptr<StubFactory> factory_;
  std::shared_ptr<ICommonMgmtdClient> mgmtd_;
  std::shared_ptr<storage::client::StorageClient> storage_;

 private:
  folly::atomic_shared_ptr<ServerSelectionStrategy> serverSelection_;
  std::unique_ptr<ConfigCallbackGuard> onConfigUpdated_;
  Semaphore concurrentReqSemaphore_;
};
}  // namespace hf3fs::meta::client
