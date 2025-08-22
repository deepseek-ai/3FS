#pragma once

#include <span>
#include <vector>

#include "common/net/Client.h"
#include "common/serde/MessagePacket.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/storage/Common.h"

namespace hf3fs::storage::client {

class StorageMessenger {
 public:
  StorageMessenger(const hf3fs::net::Client::Config &config);

  Result<Void> start(const std::string &name = "StorageMsgr") { return client_.start(name); }

  void stopAndJoin() { client_.stopAndJoin(); }

  CoTryTask<BatchReadRsp> batchRead(const hf3fs::net::Address &address,
                                    const BatchReadReq &request,
                                    const net::UserRequestOptions *options = nullptr,
                                    serde::Timestamp *timestamp = nullptr);

  CoTryTask<WriteRsp> write(const hf3fs::net::Address &address,
                            const WriteReq &request,
                            const net::UserRequestOptions *options = nullptr,
                            serde::Timestamp *timestamp = nullptr);

  CoTryTask<UpdateRsp> update(const hf3fs::net::Address &address,
                              const UpdateReq &request,
                              const net::UserRequestOptions *options = nullptr,
                              serde::Timestamp *timestamp = nullptr);

  CoTryTask<QueryLastChunkRsp> queryLastChunk(const hf3fs::net::Address &address,
                                              const QueryLastChunkReq &request,
                                              const net::UserRequestOptions *options = nullptr,
                                              serde::Timestamp *timestamp = nullptr);

  CoTryTask<RemoveChunksRsp> removeChunks(const hf3fs::net::Address &address,
                                          const RemoveChunksReq &request,
                                          const net::UserRequestOptions *options = nullptr,
                                          serde::Timestamp *timestamp = nullptr);

  CoTryTask<TruncateChunksRsp> truncateChunks(const hf3fs::net::Address &address,
                                              const TruncateChunksReq &request,
                                              const net::UserRequestOptions *options = nullptr,
                                              serde::Timestamp *timestamp = nullptr);

  CoTryTask<SpaceInfoRsp> querySpaceInfo(const hf3fs::net::Address &address,
                                         const SpaceInfoReq &request,
                                         const net::UserRequestOptions *options = nullptr,
                                         serde::Timestamp *timestamp = nullptr);

  CoTryTask<TargetSyncInfo> syncStart(const hf3fs::net::Address &address,
                                      const SyncStartReq &request,
                                      const net::UserRequestOptions *options = nullptr,
                                      serde::Timestamp *timestamp = nullptr);

  CoTryTask<SyncDoneRsp> syncDone(const hf3fs::net::Address &address,
                                  const SyncDoneReq &request,
                                  const net::UserRequestOptions *options = nullptr,
                                  serde::Timestamp *timestamp = nullptr);

  CoTryTask<CreateTargetRsp> createTarget(const hf3fs::net::Address &address,
                                          const CreateTargetReq &request,
                                          const net::UserRequestOptions *options = nullptr,
                                          serde::Timestamp *timestamp = nullptr);

  CoTryTask<OfflineTargetRsp> offlineTarget(const hf3fs::net::Address &address,
                                            const OfflineTargetReq &request,
                                            const net::UserRequestOptions *options = nullptr,
                                            serde::Timestamp *timestamp = nullptr);

  CoTryTask<RemoveTargetRsp> removeTarget(const hf3fs::net::Address &address,
                                          const RemoveTargetReq &request,
                                          const net::UserRequestOptions *options = nullptr,
                                          serde::Timestamp *timestamp = nullptr);

  CoTryTask<QueryChunkRsp> queryChunk(const hf3fs::net::Address &address,
                                      const QueryChunkReq &request,
                                      const net::UserRequestOptions *options = nullptr,
                                      serde::Timestamp *timestamp = nullptr);

  CoTryTask<GetAllChunkMetadataRsp> getAllChunkMetadata(const hf3fs::net::Address &address,
                                                        const GetAllChunkMetadataReq &request,
                                                        const net::UserRequestOptions *options = nullptr,
                                                        serde::Timestamp *timestamp = nullptr);

 private:
  hf3fs::net::Client client_;
};

}  // namespace hf3fs::storage::client
