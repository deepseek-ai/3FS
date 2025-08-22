#pragma once

#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/Mutex.h>
#include <gtest/gtest_prod.h>
#include <map>
#include <mutex>
#include <span>
#include <unordered_map>
#include <vector>

#include "StorageClient.h"
#include "StorageMessenger.h"
#include "common/net/Client.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/storage/Common.h"

namespace hf3fs::storage::client {

class StorageClientInMem : public StorageClient {
 public:
  StorageClientInMem(ClientId clientId, const Config &config, hf3fs::client::ICommonMgmtdClient &mgmtdClient);

  ~StorageClientInMem() override = default;

  hf3fs::client::ICommonMgmtdClient &getMgmtdClient() override { return mgmtdClient_; }

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

  CoTryTask<SpaceInfoRsp> querySpaceInfo(NodeId) override;

  CoTryTask<CreateTargetRsp> createTarget(NodeId nodeId, const CreateTargetReq &req) override;

  CoTryTask<OfflineTargetRsp> offlineTarget(NodeId nodeId, const OfflineTargetReq &req) override;

  CoTryTask<RemoveTargetRsp> removeTarget(NodeId nodeId, const RemoveTargetReq &req) override;

  CoTryTask<std::vector<Result<QueryChunkRsp>>> queryChunk(const QueryChunkReq &req) override;

  CoTryTask<ChunkMetaVector> getAllChunkMetadata(const ChainId &chainId, const TargetId &targetId) override;

  // for meta test
  CoTask<void> injectErrorOnChain(ChainId chainId, Result<Void> error);

 private:
  struct ChunkData {
    std::vector<uint8_t> content;
    uint32_t capacity;
    uint32_t version;
  };

  struct Chain {
    folly::coro::Mutex mutex;
    std::map<ChunkId, ChunkData> chunks;
    Result<Void> error = Void{};
  };

  CoTask<std::pair<Chain *, std::unique_lock<folly::coro::Mutex>>> getChain(ChainId chainId) {
    auto &chain = (*chains_.lock())[chainId];
    auto guard = co_await chain.mutex.co_scoped_lock();
    co_return std::make_pair(&chain, std::move(guard));
  }

  using ChunkDataProcessor = std::function<CoTryTask<void>(const ChunkId &, const ChunkData &)>;

  CoTryTask<std::vector<std::pair<ChunkId, ChunkData>>> doQuery(const ChainId &chainId, const ChunkIdRange &range);

  CoTryTask<uint32_t> processQueryResults(const ChainId chainId,
                                          const ChunkIdRange &range,
                                          ChunkDataProcessor processor,
                                          bool &moreChunksInRange);

  hf3fs::client::ICommonMgmtdClient &mgmtdClient_;
  folly::Synchronized<std::unordered_map<ChainId, Chain>, std::mutex> chains_;
};

}  // namespace hf3fs::storage::client
