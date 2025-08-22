#pragma once

#include <span>
#include <vector>

#include "TargetSelection.h"
#include "UpdateChannelAllocator.h"
#include "client/mgmtd/ICommonMgmtdClient.h"
#include "common/net/Client.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Semaphore.h"
#include "fbs/mgmtd/RoutingInfo.h"
#include "fbs/storage/Common.h"

namespace hf3fs::storage::client {

/* Dynamic routing info for accessing a storage target */

class RoutingTarget {
 public:
  RoutingTarget(ChainId chainId)
      : chainId(chainId) {}

  ~RoutingTarget() {
    XLOGF_IF(DFATAL,
             channel.id != ChannelId{0},
             "Leaked update channel, routing target: {}, stack trace: {}",
             *this,
             folly::symbolizer::getStackTraceStr());
  }

  const hf3fs::storage::VersionedChainId getVersionedChainId() const { return {chainId, chainVer}; };

 public:
  ChainId chainId;
  ChainVer chainVer;
  flat::RoutingInfoVersion routingInfoVer;
  SlimTargetInfo targetInfo;
  UpdateChannel channel;
};

/* Read/write IOs */

class IOBuffer : public folly::MoveOnly {
 public:
  uint8_t *data() const { return const_cast<uint8_t *>(rdmabuf.ptr()); }

  size_t size() const { return rdmabuf.size(); }

  bool contains(const uint8_t *data, uint32_t len) const { return rdmabuf.contains(data, len); }

  net::RDMABuf subrange(size_t offset, size_t length) const { return rdmabuf.subrange(offset, length); }

  IOBuffer(hf3fs::net::RDMABuf rdmabuf)
      : rdmabuf(rdmabuf) {}

 private:
  const hf3fs::net::RDMABuf rdmabuf;

  friend class IOBase;
  friend class StorageClient;
  friend class StorageClientImpl;
  friend class StorageClientInMem;
};

class IOBase : public folly::MoveOnly {
 private:
  IOBase(ChainId chainId,
         const ChunkId &chunkId,
         uint32_t offset,
         uint32_t length,
         uint32_t chunkSize,
         uint8_t *data,
         IOBuffer *buffer,
         void *userCtx)
      : routingTarget(chainId),
        chunkId(chunkId),
        offset(offset),
        length(length),
        chunkSize(chunkSize),
        data(data),
        buffer(buffer),
        userCtx(userCtx) {}

 public:
  Status status() const { return result.lengthInfo ? Status(StatusCode::kOK) : result.lengthInfo.error(); }
  status_code_t statusCode() const { return hf3fs::getStatusCode(result.lengthInfo); }
  uint32_t resultLen() const { return result.lengthInfo ? *result.lengthInfo : 0; }
  uint32_t dataLen() const { return length; }
  uint8_t *dataEnd() const { return data + length; }
  ChunkIdRange chunkRange() const { return {chunkId, chunkId, 1}; }
  uint32_t numProcessedChunks() const { return bool(result.lengthInfo); }
  void resetResult() { result = IOResult{}; }

 public:
  RoutingTarget routingTarget;
  ChunkId chunkId;
  const uint32_t offset;
  const uint32_t length;
  const uint32_t chunkSize;
  uint8_t *const data;
  IOBuffer *const buffer;
  void *const userCtx;
  IOResult result;

  friend class ReadIO;
  friend class WriteIO;
};

class ReadIO : public IOBase {
 private:
  ReadIO(ChainId chainId,
         const ChunkId &chunkId,
         uint32_t offset,
         uint32_t length,
         uint8_t *data,
         IOBuffer *buffer,
         void *userCtx)
      : IOBase(chainId, chunkId, offset, length, 0 /*chunkSize*/, data, buffer, userCtx) {}

  friend class StorageClient;
  friend class StorageClientImpl;
  friend class StorageClientInMem;

 public:
  RequestId requestId;
  std::vector<ReadIO> splittedIOs;
};

class WriteIO : public IOBase {
 private:
  WriteIO(RequestId requestId,
          ChainId chainId,
          const ChunkId &chunkId,
          uint32_t offset,
          uint32_t length,
          uint32_t chunkSize,
          uint8_t *data,
          IOBuffer *buffer,
          void *userCtx)
      : IOBase(chainId, chunkId, offset, length, chunkSize, data, buffer, userCtx),
        requestId(requestId) {}

 public:
  const ChecksumInfo &localChecksum() const { return checksum; }

 private:
  friend class StorageClient;
  friend class StorageClientImpl;
  friend class StorageClientInMem;

 public:
  const RequestId requestId;
  ChecksumInfo checksum;
};

/* Read/write options */

class DebugOptions : public hf3fs::ConfigBase<DebugOptions> {
  CONFIG_HOT_UPDATED_ITEM(bypass_disk_io, false);
  CONFIG_HOT_UPDATED_ITEM(bypass_rdma_xmit, false);
  CONFIG_HOT_UPDATED_ITEM(inject_random_server_error, false);
  CONFIG_HOT_UPDATED_ITEM(inject_random_client_error, false);
  CONFIG_HOT_UPDATED_ITEM(max_num_of_injection_points, 100);

 public:
  DebugFlags toDebugFlags() const {
#ifndef NDEBUG
    return DebugFlags{
        .injectRandomServerError = inject_random_server_error(),
        .injectRandomClientError = inject_random_client_error(),
        .numOfInjectPtsBeforeFail = (uint16_t)folly::Random::rand32(1, max_num_of_injection_points() + 1)};
#else
    return DebugFlags{};
#endif
  }
};

class RetryOptions : public hf3fs::ConfigBase<RetryOptions> {
  CONFIG_HOT_UPDATED_ITEM(init_wait_time, Duration::zero());  // if set to zero, use the value from client config
  CONFIG_HOT_UPDATED_ITEM(max_wait_time, Duration::zero());   // if set to zero, use the value from client config
  CONFIG_HOT_UPDATED_ITEM(max_retry_time, Duration::zero());  // if set to zero, use the value from client config
  CONFIG_HOT_UPDATED_ITEM(retry_permanent_error, false);
};

class ReadOptions : public hf3fs::ConfigBase<ReadOptions> {
  CONFIG_OBJ(debug, DebugOptions);
  CONFIG_OBJ(retry, RetryOptions);
  CONFIG_OBJ(targetSelection, TargetSelectionOptions);
  CONFIG_HOT_UPDATED_ITEM(enableChecksum, false);
  CONFIG_HOT_UPDATED_ITEM(allowReadUncommitted, false);

 public:
  bool verifyChecksum() const {
#ifndef NDEBUG
    bool enabled = true;
#else
    bool enabled = enableChecksum();
#endif

    return enabled && !debug().bypass_disk_io() && !debug().bypass_rdma_xmit();
  }
};

class WriteOptions : public hf3fs::ConfigBase<WriteOptions> {
  CONFIG_OBJ(debug, DebugOptions);
  CONFIG_OBJ(retry, RetryOptions);
  CONFIG_OBJ(targetSelection, TargetSelectionOptions);  // for test only
  CONFIG_HOT_UPDATED_ITEM(enableChecksum, true);

 public:
  bool verifyChecksum() const {
#ifndef NDEBUG
    bool enabled = true;
#else
    bool enabled = enableChecksum();
#endif

    return enabled && !debug().bypass_disk_io() && !debug().bypass_rdma_xmit();
  }
};

class IoOptions : public ConfigBase<IoOptions> {
  CONFIG_OBJ(read, ReadOptions);
  CONFIG_OBJ(write, WriteOptions);
};

/* queryLastChunk */

class QueryLastChunkOp : public folly::MoveOnly {
 private:
  QueryLastChunkOp(ChainId chainId, ChunkIdRange range, void *userCtx)
      : requestId(0),
        routingTarget(chainId),
        range(range),
        userCtx(userCtx) {}

 public:
  Status status() const { return result.statusCode ? Status(StatusCode::kOK) : result.statusCode.error(); }
  status_code_t statusCode() const { return hf3fs::getStatusCode(result.statusCode); }
  uint32_t resultLen() const { return 0; }
  uint32_t dataLen() const { return 0; }
  ChunkIdRange chunkRange() const { return range; }
  uint32_t numProcessedChunks() const { return result.statusCode ? result.totalNumChunks : 0; }
  void resetResult() { result = QueryLastChunkResult{}; }

 public:
  RequestId requestId;
  RoutingTarget routingTarget;
  const ChunkIdRange range;
  void *const userCtx;
  QueryLastChunkResult result;

 public:
  friend class StorageClient;
  friend class StorageClientImpl;
  friend class StorageClientInMem;
};

/* removeChunks */

class RemoveChunksOp : public folly::MoveOnly {
 private:
  RemoveChunksOp(RequestId requestId, ChainId chainId, ChunkIdRange range, void *userCtx)
      : requestId(requestId),
        routingTarget(chainId),
        range(range),
        userCtx(userCtx) {}

 public:
  Status status() const { return result.statusCode ? Status(StatusCode::kOK) : result.statusCode.error(); }
  status_code_t statusCode() const { return hf3fs::getStatusCode(result.statusCode); }
  uint32_t resultLen() const { return 0; }
  uint32_t dataLen() const { return 0; }
  ChunkIdRange chunkRange() const { return range; }
  uint32_t numProcessedChunks() const { return result.statusCode ? result.numChunksRemoved : 0; }
  void resetResult() { result = RemoveChunksResult{}; }

 public:
  const RequestId requestId;
  RoutingTarget routingTarget;
  const ChunkIdRange range;
  void *const userCtx;
  RemoveChunksResult result;

  friend class StorageClient;
  friend class StorageClientImpl;
  friend class StorageClientInMem;
};

/* truncateChunks */

class TruncateChunkOp : public folly::MoveOnly {
 private:
  TruncateChunkOp(RequestId requestId,
                  ChainId chainId,
                  const ChunkId &chunkId,
                  uint32_t chunkLen,
                  uint32_t chunkSize,
                  bool onlyExtendChunk,
                  void *userCtx)
      : requestId(requestId),
        routingTarget(chainId),
        chunkId(chunkId),
        chunkLen(chunkLen),
        chunkSize(chunkSize),
        onlyExtendChunk(onlyExtendChunk),
        userCtx(userCtx) {}

 public:
  Status status() const { return result.lengthInfo ? Status(StatusCode::kOK) : result.lengthInfo.error(); }
  status_code_t statusCode() const { return hf3fs::getStatusCode(result.lengthInfo); }
  uint32_t resultLen() const { return 0; }
  uint32_t dataLen() const { return 0; }
  ChunkIdRange chunkRange() const { return {chunkId, chunkId, 1}; }
  uint32_t numProcessedChunks() const { return bool(result.lengthInfo); }
  void resetResult() { result = IOResult{}; }

 public:
  const RequestId requestId;
  RoutingTarget routingTarget;
  const ChunkId chunkId;
  const uint32_t chunkLen;
  const uint32_t chunkSize;
  bool onlyExtendChunk;
  void *const userCtx;
  IOResult result;  // result.lengthInfo == chunkLen if the op succeeds

  friend class StorageClient;
  friend class StorageClientImpl;
  friend class StorageClientInMem;
};

/* Storage client */

class StorageClient : public folly::MoveOnly {
 public:
  enum class ImplementationType {
    RPC,
    InMem,
  };

  enum class MethodType {
    batchRead = 1,
    batchWrite,
    read,
    write,
    queryLastChunk,
    removeChunks,
    truncateChunks,
    querySpaceInfo,
    createTarget,
    offlineTarget,
    removeTarget,
    queryChunk,
    getAllChunkMetadata,
  };

  class RetryConfig : public hf3fs::ConfigBase<RetryConfig> {
   public:
    CONFIG_HOT_UPDATED_ITEM(init_wait_time, 10_s);
    CONFIG_HOT_UPDATED_ITEM(max_wait_time, 30_s);
    CONFIG_HOT_UPDATED_ITEM(max_retry_time, 60_s);
    CONFIG_HOT_UPDATED_ITEM(max_failures_before_failover,
                            1U);  // the max number of failed retries before switching to alternative targets

   public:
    RetryOptions mergeWith(RetryOptions options) const {
      if (options.init_wait_time() == Duration::zero()) options.set_init_wait_time(this->init_wait_time());
      if (options.max_wait_time() == Duration::zero()) options.set_max_wait_time(this->max_wait_time());
      if (options.max_retry_time() == Duration::zero()) options.set_max_retry_time(this->max_retry_time());
      return options;
    }
  };

  class OperationConcurrency : public hf3fs::ConfigBase<OperationConcurrency> {
    CONFIG_ITEM(max_batch_size, 128U);
    CONFIG_ITEM(max_batch_bytes, 4_MB);
    CONFIG_ITEM(max_concurrent_requests, 32U);
    CONFIG_ITEM(max_concurrent_requests_per_server, 8U);
    CONFIG_HOT_UPDATED_ITEM(random_shuffle_requests, true);
    CONFIG_HOT_UPDATED_ITEM(process_batches_in_parallel, true);
  };

  class HotLoadOperationConcurrency : public hf3fs::ConfigBase<HotLoadOperationConcurrency> {
    CONFIG_HOT_UPDATED_ITEM(max_batch_size, 128U);
    CONFIG_HOT_UPDATED_ITEM(max_batch_bytes, 4_MB);
    CONFIG_HOT_UPDATED_ITEM(max_concurrent_requests, 32U);
    CONFIG_HOT_UPDATED_ITEM(max_concurrent_requests_per_server, 8U);
    CONFIG_HOT_UPDATED_ITEM(random_shuffle_requests, true);
    CONFIG_HOT_UPDATED_ITEM(process_batches_in_parallel, true);
  };

  class TrafficControlConfig : public hf3fs::ConfigBase<TrafficControlConfig> {
    CONFIG_OBJ(read, HotLoadOperationConcurrency);
    CONFIG_OBJ(write, OperationConcurrency);
    CONFIG_OBJ(query, HotLoadOperationConcurrency);
    CONFIG_OBJ(remove, OperationConcurrency);
    CONFIG_OBJ(truncate, OperationConcurrency);

   public:
    size_t max_concurrent_updates() const {
      return write().max_concurrent_requests() * write().max_batch_size() +
             remove().max_concurrent_requests() * remove().max_batch_size() +
             truncate().max_concurrent_requests() * truncate().max_batch_size();
    }
  };

  class Config : public hf3fs::ConfigBase<Config> {
   public:
    CONFIG_OBJ(net_client, hf3fs::net::Client::Config);
    CONFIG_OBJ(net_client_for_updates, hf3fs::net::Client::Config);
    CONFIG_OBJ(retry, RetryConfig);
    CONFIG_OBJ(traffic_control, TrafficControlConfig);
    CONFIG_ITEM(implementation_type, ImplementationType::RPC);
    CONFIG_ITEM(chunk_checksum_type, ChecksumType::CRC32C);
    CONFIG_ITEM(create_net_client_for_updates, false);
    CONFIG_HOT_UPDATED_ITEM(check_overlapping_read_buffers, true);
    CONFIG_HOT_UPDATED_ITEM(check_overlapping_write_buffers, false);
    CONFIG_HOT_UPDATED_ITEM(max_inline_read_bytes, Size{0});
    CONFIG_HOT_UPDATED_ITEM(max_inline_write_bytes, Size{0});
    CONFIG_HOT_UPDATED_ITEM(max_read_io_bytes, Size{0});
  };

 public:
  StorageClient(const ClientId &clientId, const Config &config)
      : clientId_(clientId),
        config_(config) {}

  StorageClient()
      : StorageClient(ClientId::random(), kDefaultConfig) {}

  virtual ~StorageClient() = default;

  static std::shared_ptr<StorageClient> create(ClientId clientId,
                                               const Config &config,
                                               hf3fs::client::ICommonMgmtdClient &mgmtdClient);

  virtual hf3fs::client::ICommonMgmtdClient &getMgmtdClient() = 0;

  virtual Result<Void> start() { return Void{}; }

  // If the user does not call `stop()', the client is stopped in destructor.
  virtual void stop() {}

  /* Read `length' bytes from `offset' of the chunk.
     The memory pointed by `data' should be large enough to store the data, fall in the range of
     the registered `buffer' and does not overlap with other IOs in the same batch (can be disable by
     setting `check_overlapping_read_buffers').
   */
  virtual ReadIO createReadIO(ChainId chainId,
                              const ChunkId &chunkId,
                              uint32_t offset,
                              uint32_t length,
                              uint8_t *data,
                              IOBuffer *buffer,
                              void *userCtx = nullptr);

  /* Write `length' bytes of data at `offset' of the chunk.
     The memory pointed by `data' should be large enough to store the data, fall in the range of
     the registered `buffer' and does not overlap with other IOs in the same batch (can be disable by
     setting `check_overlapping_write_buffers').
     If option `chunk_checksum_type' is not none, a checksum will be calculated for the write buffer.
   */
  virtual WriteIO createWriteIO(ChainId chainId,
                                const ChunkId &chunkId,
                                uint32_t offset,
                                uint32_t length,
                                uint32_t chunkSize,
                                uint8_t *data,
                                IOBuffer *buffer,
                                void *userCtx = nullptr);

  /* Query the chunk with largest lexicographical id in range [chunkIdBegin, chunkIdEnd).
     `totalChunkLen' and `totalNumChunks' of chunks in the range are calculated and included in
     `QueryLastChunkResult'.
     If `moreChunksInRange' in `QueryLastChunkResult' is true, there exist more than
     `maxNumChunkIdsToProcess' chunks in range [chunkIdBegin, chunkIdEnd).
  */
  virtual QueryLastChunkOp createQueryOp(ChainId chainId,
                                         ChunkId chunkIdBegin,
                                         ChunkId chunkIdEnd,
                                         uint32_t maxNumChunkIdsToProcess = 1,
                                         void *userCtx = nullptr);

  /* Remove chunks in the range [chunkIdBegin, chunkIdEnd).
     Note that the `numChunksRemoved' in `RemoveChunksResult' might be less or equal to
     the number of chunks actually removed by storage service if the request fails and is
     automatically retried until it succeeds.
     If `moreChunksInRange' in `RemoveChunksResult' is true, there exist more than
     `maxNumChunkIdsToProcess' chunks in range [chunkIdBegin, chunkIdEnd).
   */
  virtual RemoveChunksOp createRemoveOp(ChainId chainId,
                                        ChunkId chunkIdBegin,
                                        ChunkId chunkIdEnd,
                                        uint32_t maxNumChunkIdsToProcess = 1,
                                        void *userCtx = nullptr);

  /* Truncate the chunk to `chunkLen' and create the chunk if it does not exist.
     `chunkSize' must equal to the size when the chunk was created if it already exists.
     A chunk of size `chunkSize' is created if does not exist.
     If `onlyExtendChunk' = true, extend the chunk if its length is less than `chunkLen';
     noop if its length is already greater or equal to `chunkLen'.
     The truncated/extended chunk size is returned as `lengthInfo' in the IO result;
     the user should check if the chunk size is expected.
  */
  virtual TruncateChunkOp createTruncateOp(ChainId chainId,
                                           const ChunkId &chunkId,
                                           uint32_t chunkLen,
                                           uint32_t chunkSize,
                                           bool onlyExtendChunk = false,
                                           void *userCtx = nullptr);

  // delete the returned IOBuffer object to deregister the buffer
  virtual Result<IOBuffer> registerIOBuffer(uint8_t *buf, size_t len);

  virtual CoTryTask<void> batchRead(std::span<ReadIO> readIOs,
                                    const flat::UserInfo &userInfo,
                                    const ReadOptions &options = ReadOptions(),
                                    std::vector<ReadIO *> *failedIOs = nullptr) = 0;

  virtual CoTryTask<void> batchWrite(std::span<WriteIO> writeIOs,
                                     const flat::UserInfo &userInfo,
                                     const WriteOptions &options = WriteOptions(),
                                     std::vector<WriteIO *> *failedIOs = nullptr) = 0;

  virtual CoTryTask<void> read(ReadIO &readIO,
                               const flat::UserInfo &userInfo,
                               const ReadOptions &options = ReadOptions()) = 0;

  virtual CoTryTask<void> write(WriteIO &writeIO,
                                const flat::UserInfo &userInfo,
                                const WriteOptions &options = WriteOptions()) = 0;

  // the following interfaces are assumed to be used at server-side (e.g. in meta service)

  virtual CoTryTask<void> queryLastChunk(std::span<QueryLastChunkOp> ops,
                                         const flat::UserInfo &userInfo,
                                         const ReadOptions &options = ReadOptions(),
                                         std::vector<QueryLastChunkOp *> *failedOps = nullptr) = 0;

  virtual CoTryTask<void> removeChunks(std::span<RemoveChunksOp> ops,
                                       const flat::UserInfo &userInfo,
                                       const WriteOptions &options = WriteOptions(),
                                       std::vector<RemoveChunksOp *> *failedOps = nullptr) = 0;

  virtual CoTryTask<void> truncateChunks(std::span<TruncateChunkOp> ops,
                                         const flat::UserInfo &userInfo,
                                         const WriteOptions &options = WriteOptions(),
                                         std::vector<TruncateChunkOp *> *failedOps = nullptr) = 0;

  virtual CoTryTask<SpaceInfoRsp> querySpaceInfo(NodeId nodeId) = 0;

  virtual CoTryTask<CreateTargetRsp> createTarget(NodeId nodeId, const CreateTargetReq &req) = 0;

  virtual CoTryTask<OfflineTargetRsp> offlineTarget(NodeId nodeId, const OfflineTargetReq &req) = 0;

  virtual CoTryTask<RemoveTargetRsp> removeTarget(NodeId nodeId, const RemoveTargetReq &req) = 0;

  virtual CoTryTask<std::vector<Result<QueryChunkRsp>>> queryChunk(const QueryChunkReq &req) = 0;

  virtual CoTryTask<ChunkMetaVector> getAllChunkMetadata(const ChainId &chainId, const TargetId &targetId) = 0;

 protected:
  static const Config kDefaultConfig;
  const ClientId clientId_;
  const Config &config_;
  std::atomic_uint64_t nextRequestId_ = 1;
};

}  // namespace hf3fs::storage::client

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::storage::client::RoutingTarget> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::storage::client::RoutingTarget &routingTarget, FormatContext &ctx) const {
    return fmt::format_to(ctx.out(),
                          "{}@{}@{}:{}@{}:{}#{}",
                          routingTarget.chainId,
                          routingTarget.chainVer,
                          routingTarget.routingInfoVer,
                          routingTarget.targetInfo.targetId,
                          routingTarget.targetInfo.nodeId,
                          routingTarget.channel.id,
                          routingTarget.channel.seqnum);
  }
};

FMT_END_NAMESPACE
