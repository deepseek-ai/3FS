#include <boost/core/ignore_unused.hpp>
#include <cstring>

#ifdef HF3FS_ENABLE_GDR
#include <cuda_runtime.h>
#endif

#include "StorageClientImpl.h"
#include "StorageClientInMem.h"
#include "common/cuda/CudaMemory.h"
#include "common/monitor/ScopedMetricsWriter.h"
#include "common/net/ib/RDMABuf.h"

namespace hf3fs::storage::client {

static monitor::CountRecorder iobuf_reg_success_ops{"storage_client.iobuf_reg.success_ops"};
static monitor::CountRecorder iobuf_reg_failed_ops{"storage_client.iobuf_reg.failed_ops"};
static monitor::LatencyRecorder iobuf_reg_latency{"storage_client.iobuf_reg.latency"};
static monitor::DistributionRecorder iobuf_reg_size{"storage_client.iobuf_reg.size"};

const StorageClient::Config StorageClient::kDefaultConfig;

#ifdef HF3FS_ENABLE_GDR
Result<Void> detail::zeroCudaDeviceRange(void *devicePtr, size_t length, int deviceId) {
  auto guard = cuda::ScopedDevice::create(deviceId);
  if (!guard) {
    return makeError(
        StorageClientCode::kRemoteIOError,
        fmt::format("failed to select CUDA device {} while zeroing device memory: {}", deviceId, guard.error()));
  }

  auto cudaResult = cudaMemset(devicePtr, 0, length);
  if (cudaResult != cudaSuccess) {
    return makeError(StorageClientCode::kRemoteIOError,
                     fmt::format("cudaMemset failed while zeroing device memory: {}", cudaGetErrorString(cudaResult)));
  }

  cudaResult = cudaDeviceSynchronize();
  if (cudaResult != cudaSuccess) {
    return makeError(
        StorageClientCode::kRemoteIOError,
        fmt::format("cudaDeviceSynchronize failed after zeroing device memory: {}", cudaGetErrorString(cudaResult)));
  }
  return Void{};
}
#endif

std::shared_ptr<StorageClient> StorageClient::create(ClientId clientId,
                                                     const Config &config,
                                                     hf3fs::client::ICommonMgmtdClient &mgmtdClient) {
  const auto &trafficControl = config.traffic_control();

  if (trafficControl.max_concurrent_updates() > UpdateChannelAllocator::kMaxNumChannels) {
    XLOGF(CRITICAL,
          "Bad config: trafficControl.max_concurrent_updates {} > UpdateChannelAllocator::kMaxNumChannels {}",
          trafficControl.max_concurrent_updates(),
          UpdateChannelAllocator::kMaxNumChannels);
    return nullptr;
  }

  std::shared_ptr<StorageClient> client;

  if (config.implementation_type() == ImplementationType::RPC) {
    client = std::make_shared<StorageClientImpl>(clientId, config, mgmtdClient);
  } else if (config.implementation_type() == ImplementationType::InMem) {
    client = std::make_shared<StorageClientInMem>(clientId, config, mgmtdClient);
  }

  if (!client || !client->start()) {
    XLOGF(CRITICAL,
          "Failed to create and start storage client of type {}",
          magic_enum::enum_name(config.implementation_type()));
    client.reset();
  }

  return client;
}

ReadIO StorageClient::createReadIO(ChainId chainId,
                                   const ChunkId &chunkId,
                                   uint32_t offset,
                                   uint32_t length,
                                   uint8_t *data,
                                   IOBuffer *buffer,
                                   void *userCtx) {
  return ReadIO{chainId, chunkId, offset, length, data, buffer, userCtx};
}

WriteIO StorageClient::createWriteIO(ChainId chainId,
                                     const ChunkId &chunkId,
                                     uint32_t offset,
                                     uint32_t length,
                                     uint32_t chunkSize,
                                     uint8_t *data,
                                     IOBuffer *buffer,
                                     void *userCtx) {
  RequestId requestId(nextRequestId_.fetch_add(1));
  return WriteIO{requestId, chainId, chunkId, offset, length, chunkSize, data, buffer, userCtx};
}

QueryLastChunkOp StorageClient::createQueryOp(ChainId chainId,
                                              ChunkId chunkIdBegin,
                                              ChunkId chunkIdEnd,
                                              uint32_t maxNumChunkIdsToProcess,
                                              void *userCtx) {
  return QueryLastChunkOp{chainId, {chunkIdBegin, chunkIdEnd, maxNumChunkIdsToProcess}, userCtx};
}

RemoveChunksOp StorageClient::createRemoveOp(ChainId chainId,
                                             ChunkId chunkIdBegin,
                                             ChunkId chunkIdEnd,
                                             uint32_t maxNumChunkIdsToProcess,
                                             void *userCtx) {
  RequestId requestId(nextRequestId_.fetch_add(1));
  return RemoveChunksOp{requestId, chainId, {chunkIdBegin, chunkIdEnd, maxNumChunkIdsToProcess}, userCtx};
}

TruncateChunkOp StorageClient::createTruncateOp(ChainId chainId,
                                                const ChunkId &chunkId,
                                                uint32_t chunkLen,
                                                uint32_t chunkSize,
                                                bool onlyExtendChunk,
                                                void *userCtx) {
  RequestId requestId(nextRequestId_.fetch_add(1));
  return TruncateChunkOp(requestId, chainId, chunkId, chunkLen, chunkSize, onlyExtendChunk, userCtx);
}

Result<Void> IOBuffer::zeroRange(size_t offset, size_t length) const {
  if (!rdmabuf_.valid()) {
    return makeError(StorageClientCode::kInvalidArg, "cannot zero an invalid IOBuffer");
  }
  if (offset > size() || length > size() - offset) {
    return makeError(
        StorageClientCode::kInvalidArg,
        fmt::format("IOBuffer zero range offset {} length {} exceeds buffer size {}", offset, length, size()));
  }
  if (length == 0) {
    return Void{};
  }

  auto *ptr = dataAtOffset(offset);
  if (ptr == nullptr) {
    return makeError(StorageClientCode::kInvalidArg, "IOBuffer zero range has an invalid address");
  }

  if (!isDeviceMemory()) {
    std::memset(ptr, 0, length);
    return Void{};
  }

#ifdef HF3FS_ENABLE_GDR
  return detail::zeroCudaDeviceRange(ptr, length, cudaDeviceId());
#else
  return makeError(StorageClientCode::kRemoteIOError,
                   "cannot zero a device IOBuffer because CUDA/GDR support is disabled");
#endif
}

Result<PreparedWriteChecksum> prepareWriteChecksum(ChecksumType targetType,
                                                   bool verifyChecksum,
                                                   bool deviceMemory,
                                                   const uint8_t *data,
                                                   size_t length) {
  if (!verifyChecksum) {
    return PreparedWriteChecksum{ChecksumInfo{ChecksumType::NONE, 0}, false};
  }
  if (targetType == ChecksumType::NONE) {
    return PreparedWriteChecksum{ChecksumInfo{ChecksumType::NONE, 0}, false};
  }
  if (deviceMemory) {
    return PreparedWriteChecksum{ChecksumInfo{targetType, 0}, true};
  }
  if (data == nullptr && length != 0) {
    return makeError(StorageClientCode::kInvalidArg, "cannot checksum a null host buffer");
  }
  return PreparedWriteChecksum{ChecksumInfo::create(targetType, data, length), false};
}

Result<IOBuffer> StorageClient::registerIOBuffer(uint8_t *buf, size_t len) {
  monitor::ScopedLatencyWriter latencyWriter(iobuf_reg_latency);
  iobuf_reg_size.addSample(len);

  auto rdmabuf = hf3fs::net::RDMABuf::createFromUserBuffer(buf, len);

  if (rdmabuf.valid()) {
    iobuf_reg_success_ops.addSample(1);
    return IOBuffer{rdmabuf};
  } else {
    iobuf_reg_failed_ops.addSample(1);
    return makeError(StorageClientCode::kMemoryError);
  }
}

Result<IOBuffer> StorageClient::registerGpuIOBuffer(uint8_t *gpuPtr, size_t len) {
  (void)gpuPtr;
  (void)len;
  return makeError(StorageClientCode::kNotAvailable, "GPU IOBuffer registration requires an RDMA storage client");
}

Result<IOBuffer> StorageClientImpl::registerGpuIOBuffer(uint8_t *gpuPtr, size_t len) {
  monitor::ScopedLatencyWriter latencyWriter(iobuf_reg_latency);
  iobuf_reg_size.addSample(len);

  if (gpuPtr == nullptr || len == 0) {
    iobuf_reg_failed_ops.addSample(1);
    return makeError(StorageClientCode::kInvalidArg, "GPU IOBuffer pointer and length must be non-zero");
  }

#ifdef HF3FS_ENABLE_GDR
  auto gpuBuf = hf3fs::net::RDMABuf::createFromCudaBuffer(gpuPtr, len, -1);
  if (gpuBuf) {
    iobuf_reg_success_ops.addSample(1);
    return IOBuffer{std::move(*gpuBuf)};
  }

  iobuf_reg_failed_ops.addSample(1);
  auto code = gpuBuf.error().code() == StatusCode::kInvalidArg ? StorageClientCode::kInvalidArg
                                                               : StorageClientCode::kMemoryError;
  return makeError(
      code,
      fmt::format("failed to register CUDA device pointer {} for RDMA: {}", fmt::ptr(gpuPtr), gpuBuf.error()));
#else
  iobuf_reg_failed_ops.addSample(1);
  return makeError(StorageClientCode::kNotAvailable, "GPU IOBuffer registration requires CUDA/GDR support");
#endif
}

}  // namespace hf3fs::storage::client
