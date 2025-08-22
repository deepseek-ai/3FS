#pragma once

#include <span>
#include <vector>

#include "StorageClient.h"
#include "StorageMessenger.h"
#include "UpdateChannelAllocator.h"
#include "common/net/Client.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/storage/Common.h"

namespace hf3fs::storage::client {

class ClientRequestContext;

class StorageClientImpl : public StorageClient {
 public:
  StorageClientImpl(const ClientId &clientId, const Config &config, hf3fs::client::ICommonMgmtdClient &mgmtdClient);

  ~StorageClientImpl() override;

  Result<Void> start() override;

  hf3fs::client::ICommonMgmtdClient &getMgmtdClient() override { return mgmtdClient_; }
  void stop() override;

  CoTryTask<void> batchRead(std::span<ReadIO> readIOs,
                            const flat::UserInfo &userInfo,
                            const ReadOptions &options = ReadOptions(),
                            std::vector<ReadIO *> *failedIOs = nullptr) override;

  CoTryTask<void> batchWrite(std::span<WriteIO> writeIOs,
                             const flat::UserInfo &userInfo,
                             const WriteOptions &options = WriteOptions(),
                             std::vector<WriteIO *> *failedIOs = nullptr) override;

  CoTryTask<void> read(ReadIO &readIO,
                       const flat::UserInfo &userInfo,
                       const ReadOptions &options = ReadOptions()) override;

  CoTryTask<void> write(WriteIO &writeIO,
                        const flat::UserInfo &userInfo,
                        const WriteOptions &options = WriteOptions()) override;

  CoTryTask<void> queryLastChunk(std::span<QueryLastChunkOp> ops,
                                 const flat::UserInfo &userInfo,
                                 const ReadOptions &options = ReadOptions(),
                                 std::vector<QueryLastChunkOp *> *failedOps = nullptr) override;

  CoTryTask<void> removeChunks(std::span<RemoveChunksOp> ops,
                               const flat::UserInfo &userInfo,
                               const WriteOptions &options = WriteOptions(),
                               std::vector<RemoveChunksOp *> *failedOps = nullptr) override;

  CoTryTask<void> truncateChunks(std::span<TruncateChunkOp> ops,
                                 const flat::UserInfo &userInfo,
                                 const WriteOptions &options = WriteOptions(),
                                 std::vector<TruncateChunkOp *> *failedOps = nullptr) override;

  CoTryTask<SpaceInfoRsp> querySpaceInfo(NodeId nodeId) override;

  CoTryTask<CreateTargetRsp> createTarget(NodeId nodeId, const CreateTargetReq &req) override;

  CoTryTask<OfflineTargetRsp> offlineTarget(NodeId nodeId, const OfflineTargetReq &req) override;

  CoTryTask<RemoveTargetRsp> removeTarget(NodeId nodeId, const RemoveTargetReq &req) override;

  CoTryTask<std::vector<Result<QueryChunkRsp>>> queryChunk(const QueryChunkReq &req) override;

  CoTryTask<ChunkMetaVector> getAllChunkMetadata(const ChainId &chainId, const TargetId &targetId) override;

 private:
  CoTryTask<void> batchReadWithRetry(ClientRequestContext &requestCtx,
                                     const std::vector<ReadIO *> &readIOs,
                                     const flat::UserInfo &userInfo,
                                     const ReadOptions &options,
                                     std::vector<ReadIO *> &failedIOs);

  CoTryTask<void> batchReadWithoutRetry(ClientRequestContext &requestCtx,
                                        const std::vector<ReadIO *> &readIOs,
                                        const flat::UserInfo &userInfo,
                                        const ReadOptions &options);

  CoTryTask<void> batchWriteWithRetry(ClientRequestContext &requestCtx,
                                      const std::vector<WriteIO *> &writeIOs,
                                      const flat::UserInfo &userInfo,
                                      const WriteOptions &options,
                                      std::vector<WriteIO *> &failedIOs);

  CoTryTask<void> batchWriteWithoutRetry(ClientRequestContext &requestCtx,
                                         const std::vector<WriteIO *> &writeIOs,
                                         const flat::UserInfo &userInfo,
                                         const WriteOptions &options);

  CoTryTask<void> sendWriteRequest(ClientRequestContext &requestCtx,
                                   WriteIO *writeIO,
                                   const hf3fs::flat::NodeInfo &nodeInfo,
                                   const flat::UserInfo &userInfo,
                                   const WriteOptions &options);

  CoTryTask<void> sendWriteRequestsSequentially(ClientRequestContext &requestCtx,
                                                const std::vector<WriteIO *> &writeIOs,
                                                std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo,
                                                NodeId nodeId,
                                                const flat::UserInfo &userInfo,
                                                const WriteOptions &options);

  CoTryTask<void> queryLastChunkWithoutRetry(ClientRequestContext &requestCtx,
                                             const std::vector<QueryLastChunkOp *> &ops,
                                             const flat::UserInfo &userInfo,
                                             const ReadOptions &options);

  CoTryTask<void> removeChunksWithoutRetry(ClientRequestContext &requestCtx,
                                           const std::vector<RemoveChunksOp *> &ops,
                                           const flat::UserInfo &userInfo,
                                           const WriteOptions &options);

  CoTryTask<void> truncateChunksWithoutRetry(ClientRequestContext &requestCtx,
                                             const std::vector<TruncateChunkOp *> &ops,
                                             const flat::UserInfo &userInfo,
                                             const WriteOptions &options);

 private:
  void setCurrentRoutingInfo(std::shared_ptr<hf3fs::client::RoutingInfo const> latestRoutingInfo);

  std::shared_ptr<hf3fs::client::RoutingInfo const> getCurrentRoutingInfo() { return currentRoutingInfo_.load(); }

  StorageMessenger &getStorageMessengerForUpdates() {
    return config_.create_net_client_for_updates() ? messengerForUpdates_ : messenger_;
  }

  template <typename Req, typename Rsp, auto Method>
  CoTryTask<Rsp> callMessengerMethod(StorageMessenger &messenger,
                                     ClientRequestContext &requestCtx,
                                     const hf3fs::flat::NodeInfo &nodeInfo,
                                     const Req &request);

  template <typename Op, typename BatchReq, typename BatchRsp, auto Method>
  CoTryTask<BatchRsp> sendBatchRequest(StorageMessenger &messenger,
                                       ClientRequestContext &requestCtx,
                                       std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo,
                                       const NodeId &nodeId,
                                       const BatchReq &batchReq,
                                       const std::vector<Op *> &ops);

 private:
  class OperationConcurrencyLimit {
   public:
    OperationConcurrencyLimit(size_t maxConcurrentRequests, size_t maxConcurrentRequestsPerServer)
        : maxConcurrentRequests_(maxConcurrentRequests),
          maxConcurrentRequestsPerServer_(maxConcurrentRequestsPerServer),
          concurrencySemaphore_(maxConcurrentRequests_) {}

    OperationConcurrencyLimit(const OperationConcurrency &config)
        : OperationConcurrencyLimit(config.max_concurrent_requests(), config.max_concurrent_requests_per_server()) {}

    virtual ~OperationConcurrencyLimit() = default;

    hf3fs::Semaphore &getConcurrencySemaphore() { return concurrencySemaphore_; }

    virtual std::shared_ptr<hf3fs::Semaphore> getPerServerSemaphore(const NodeId &nodeId) {
      return getPerServerSemaphore(nodeId, maxConcurrentRequestsPerServer_);
    }

   protected:
    std::shared_ptr<hf3fs::Semaphore> getPerServerSemaphore(const NodeId &nodeId, size_t initTokens) {
      {
        std::shared_lock rlock(perServerSemaphoreMutex_);
        auto iter = perServerSemaphore_.find(nodeId);
        if (iter != perServerSemaphore_.end()) {
          return iter->second;
        }
      }

      std::unique_lock wlock(perServerSemaphoreMutex_);

      auto iter = perServerSemaphore_.find(nodeId);
      if (iter != perServerSemaphore_.end()) {
        return iter->second;
      }

      auto semaphore = std::make_shared<hf3fs::Semaphore>(initTokens);
      perServerSemaphore_.emplace(nodeId, semaphore);
      return semaphore;
    }

   protected:
    const size_t maxConcurrentRequests_;
    const size_t maxConcurrentRequestsPerServer_;
    hf3fs::Semaphore concurrencySemaphore_;
    std::shared_mutex perServerSemaphoreMutex_;  // protect the following maps
    std::unordered_map<NodeId, std::shared_ptr<hf3fs::Semaphore>> perServerSemaphore_;
  };

  class HotLoadOperationConcurrencyLimit : public OperationConcurrencyLimit {
   public:
    HotLoadOperationConcurrencyLimit(const HotLoadOperationConcurrency &config)
        : OperationConcurrencyLimit(config.max_concurrent_requests(), config.max_concurrent_requests_per_server()),
          config_(config),
          onConfigUpdated_(config_.addCallbackGuard([this]() { updateUsableTokens(); })) {}

    std::shared_ptr<hf3fs::Semaphore> getPerServerSemaphore(const NodeId &nodeId) override {
      return OperationConcurrencyLimit::getPerServerSemaphore(nodeId, config_.max_concurrent_requests_per_server());
    }

   private:
    void updateUsableTokens() {
      concurrencySemaphore_.changeUsableTokens(config_.max_concurrent_requests());

      std::shared_lock rlock(perServerSemaphoreMutex_);
      for (auto &[_, semaphore] : perServerSemaphore_) {
        semaphore->changeUsableTokens(config_.max_concurrent_requests_per_server());
      }
    }

   private:
    const HotLoadOperationConcurrency &config_;
    std::unique_ptr<ConfigCallbackGuard> onConfigUpdated_;
  };

 private:
  bool clientStarted_;
  hf3fs::client::ICommonMgmtdClient &mgmtdClient_;
  StorageMessenger messenger_;
  StorageMessenger messengerForUpdates_;
  UpdateChannelAllocator chanAllocator_;
  folly::atomic_shared_ptr<hf3fs::client::RoutingInfo const> currentRoutingInfo_;

  HotLoadOperationConcurrencyLimit readConcurrencyLimit_;
  OperationConcurrencyLimit writeConcurrencyLimit_;
  HotLoadOperationConcurrencyLimit queryConcurrencyLimit_;
  OperationConcurrencyLimit removeConcurrencyLimit_;
  OperationConcurrencyLimit truncateConcurrencyLimit_;
};

}  // namespace hf3fs::storage::client
