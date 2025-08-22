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

class StorageClientBlobImpl : public StorageClient {
 public:
  StorageClientBlobImpl(const ClientId &clientId, const Config &config, hf3fs::client::ICommonMgmtdClient &mgmtdClient);

  ~StorageClientBlobImpl() override;

  Result<Void> start() override;

  void stop() override;

  hf3fs::client::ICommonMgmtdClient &getMgmtdClient() override;

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

  CoTryTask<void> listChunks(std::span<ListChunksOp> ops,
                             const flat::UserInfo &userInfo,
                             const ReadOptions &options = ReadOptions(),
                             std::vector<ListChunksOp *> *failedOps = nullptr) override;

  CoTryTask<void> batchCopy(std::span<CopyIO> copyIOs,
                            const flat::UserInfo &userInfo,
                            const WriteOptions &options = WriteOptions(),
                            std::vector<CopyIO *> *failedIOs = nullptr) override;

  CoTryTask<SpaceInfoRsp> querySpaceInfo(NodeId nodeId) override;

  CoTryTask<CreateTargetRsp> createTarget(NodeId nodeId, const CreateTargetReq &req) override;

  CoTryTask<std::vector<Result<QueryChunkRsp>>> queryChunk(const QueryChunkReq &req) override;

  CoTryTask<ChunkMetaVector> getAllChunkMetadata(const ChainId &chainId, const TargetId &targetId) override;
};

}  // namespace hf3fs::storage::client