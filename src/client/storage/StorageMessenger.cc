#include "StorageMessenger.h"

#include <boost/core/ignore_unused.hpp>

#include "common/serde/ClientContext.h"
#include "common/serde/Service.h"
#include "fbs/storage/Service.h"

namespace hf3fs::storage::client {

StorageMessenger::StorageMessenger(const hf3fs::net::Client::Config &config)
    : client_(config) {}

template <typename Req, typename Rsp, auto rpcMethod>
CoTryTask<Rsp> callSerdeRpcMethod(hf3fs::net::Client &client,
                                  const hf3fs::net::Address &address,
                                  const Req &request,
                                  const net::UserRequestOptions *options,
                                  serde::Timestamp *timestamp) {
  auto clientCtx = client.serdeCtx(address);

  auto packedRsp = co_await rpcMethod(clientCtx, request, options, timestamp);

  if (!packedRsp) {
    auto timeout = options && options->timeout ? options->timeout.value() : client.options()->timeout;
    XLOGF(ERR,
          "RPC communication error: {}, request: {}, timeout: {}, peer address: {}",
          packedRsp.error(),
          fmt::ptr(&request),
          timeout,
          address);
    co_return makeError(packedRsp.error());
  }

  co_return packedRsp;
}

CoTryTask<BatchReadRsp> StorageMessenger::batchRead(const hf3fs::net::Address &address,
                                                    const BatchReadReq &request,
                                                    const net::UserRequestOptions *options,
                                                    serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<BatchReadReq,
                                        BatchReadRsp,
                                        hf3fs::storage::StorageSerde<>::batchRead<serde::ClientContext>>(client_,
                                                                                                         address,
                                                                                                         request,
                                                                                                         options,
                                                                                                         timestamp);
}

CoTryTask<WriteRsp> StorageMessenger::write(const hf3fs::net::Address &address,
                                            const WriteReq &request,
                                            const net::UserRequestOptions *options,
                                            serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<WriteReq,
                                        WriteRsp,
                                        hf3fs::storage::StorageSerde<>::write<serde::ClientContext>>(client_,
                                                                                                     address,
                                                                                                     request,
                                                                                                     options,
                                                                                                     timestamp);
}

CoTryTask<UpdateRsp> StorageMessenger::update(const hf3fs::net::Address &address,
                                              const UpdateReq &request,
                                              const net::UserRequestOptions *options,
                                              serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<UpdateReq,
                                        UpdateRsp,
                                        hf3fs::storage::StorageSerde<>::update<serde::ClientContext>>(client_,
                                                                                                      address,
                                                                                                      request,
                                                                                                      options,
                                                                                                      timestamp);
}

CoTryTask<QueryLastChunkRsp> StorageMessenger::queryLastChunk(const hf3fs::net::Address &address,
                                                              const QueryLastChunkReq &request,
                                                              const net::UserRequestOptions *options,
                                                              serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<QueryLastChunkReq,
                                        QueryLastChunkRsp,
                                        hf3fs::storage::StorageSerde<>::queryLastChunk<serde::ClientContext>>(
      client_,
      address,
      request,
      options,
      timestamp);
}

CoTryTask<RemoveChunksRsp> StorageMessenger::removeChunks(const hf3fs::net::Address &address,
                                                          const RemoveChunksReq &request,
                                                          const net::UserRequestOptions *options,
                                                          serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<RemoveChunksReq,
                                        RemoveChunksRsp,
                                        hf3fs::storage::StorageSerde<>::removeChunks<serde::ClientContext>>(client_,
                                                                                                            address,
                                                                                                            request,
                                                                                                            options,
                                                                                                            timestamp);
}

CoTryTask<TruncateChunksRsp> StorageMessenger::truncateChunks(const hf3fs::net::Address &address,
                                                              const TruncateChunksReq &request,
                                                              const net::UserRequestOptions *options,
                                                              serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<TruncateChunksReq,
                                        TruncateChunksRsp,
                                        hf3fs::storage::StorageSerde<>::truncateChunks<serde::ClientContext>>(
      client_,
      address,
      request,
      options,
      timestamp);
}

CoTryTask<SpaceInfoRsp> StorageMessenger::querySpaceInfo(const hf3fs::net::Address &address,
                                                         const SpaceInfoReq &request,
                                                         const net::UserRequestOptions *options,
                                                         serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<SpaceInfoReq,
                                        SpaceInfoRsp,
                                        hf3fs::storage::StorageSerde<>::spaceInfo<serde::ClientContext>>(client_,
                                                                                                         address,
                                                                                                         request,
                                                                                                         options,
                                                                                                         timestamp);
}

CoTryTask<TargetSyncInfo> StorageMessenger::syncStart(const hf3fs::net::Address &address,
                                                      const SyncStartReq &request,
                                                      const net::UserRequestOptions *options,
                                                      serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<SyncStartReq,
                                        TargetSyncInfo,
                                        hf3fs::storage::StorageSerde<>::syncStart<serde::ClientContext>>(client_,
                                                                                                         address,
                                                                                                         request,
                                                                                                         options,
                                                                                                         timestamp);
}

CoTryTask<SyncDoneRsp> StorageMessenger::syncDone(const hf3fs::net::Address &address,
                                                  const SyncDoneReq &request,
                                                  const net::UserRequestOptions *options,
                                                  serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<SyncDoneReq,
                                        SyncDoneRsp,
                                        hf3fs::storage::StorageSerde<>::syncDone<serde::ClientContext>>(client_,
                                                                                                        address,
                                                                                                        request,
                                                                                                        options,
                                                                                                        timestamp);
}

CoTryTask<CreateTargetRsp> StorageMessenger::createTarget(const hf3fs::net::Address &address,
                                                          const CreateTargetReq &request,
                                                          const net::UserRequestOptions *options,
                                                          serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<CreateTargetReq,
                                        CreateTargetRsp,
                                        hf3fs::storage::StorageSerde<>::createTarget<serde::ClientContext>>(client_,
                                                                                                            address,
                                                                                                            request,
                                                                                                            options,
                                                                                                            timestamp);
}

CoTryTask<OfflineTargetRsp> StorageMessenger::offlineTarget(const hf3fs::net::Address &address,
                                                            const OfflineTargetReq &request,
                                                            const net::UserRequestOptions *options,
                                                            serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<OfflineTargetReq,
                                        OfflineTargetRsp,
                                        hf3fs::storage::StorageSerde<>::offlineTarget<serde::ClientContext>>(client_,
                                                                                                             address,
                                                                                                             request,
                                                                                                             options,
                                                                                                             timestamp);
}

CoTryTask<RemoveTargetRsp> StorageMessenger::removeTarget(const hf3fs::net::Address &address,
                                                          const RemoveTargetReq &request,
                                                          const net::UserRequestOptions *options,
                                                          serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<RemoveTargetReq,
                                        RemoveTargetRsp,
                                        hf3fs::storage::StorageSerde<>::removeTarget<serde::ClientContext>>(client_,
                                                                                                            address,
                                                                                                            request,
                                                                                                            options,
                                                                                                            timestamp);
}

CoTryTask<QueryChunkRsp> StorageMessenger::queryChunk(const hf3fs::net::Address &address,
                                                      const QueryChunkReq &request,
                                                      const net::UserRequestOptions *options,
                                                      serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<QueryChunkReq,
                                        QueryChunkRsp,
                                        hf3fs::storage::StorageSerde<>::queryChunk<serde::ClientContext>>(client_,
                                                                                                          address,
                                                                                                          request,
                                                                                                          options,
                                                                                                          timestamp);
}

CoTryTask<GetAllChunkMetadataRsp> StorageMessenger::getAllChunkMetadata(const hf3fs::net::Address &address,
                                                                        const GetAllChunkMetadataReq &request,
                                                                        const net::UserRequestOptions *options,
                                                                        serde::Timestamp *timestamp) {
  co_return co_await callSerdeRpcMethod<GetAllChunkMetadataReq,
                                        GetAllChunkMetadataRsp,
                                        hf3fs::storage::StorageSerde<>::getAllChunkMetadata<serde::ClientContext>>(
      client_,
      address,
      request,
      options,
      timestamp);
}

}  // namespace hf3fs::storage::client
