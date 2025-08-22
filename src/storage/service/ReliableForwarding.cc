#include "storage/service/ReliableForwarding.h"

#include <folly/experimental/coro/Sleep.h>

#include "common/app/ApplicationBase.h"
#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "common/utils/ExponentialBackoffRetry.h"
#include "fbs/storage/Common.h"
#include "storage/service/Components.h"
#include "storage/service/TargetMap.h"

namespace hf3fs::storage {
namespace {

monitor::OperationRecorder reliableForwardRecorder("storage.reliable_forward");
monitor::OperationRecorder syncingReadRecorder("storage.syncing_read");
monitor::OperationRecorder updateRemoteRecorder("storage.update_remote");

monitor::CountRecorder forwardWriteBytes("storage.forward.write_bytes");
monitor::DistributionRecorder forwardWriteDist("storage.forward.write_dist");
monitor::CountRecorder forwardSyncingBytes("storage.forward.syncing_bytes");
monitor::DistributionRecorder forwardSyncingDist("storage.forward.syncing_dist");

}  // namespace

using namespace std::chrono_literals;

Result<Void> ReliableForwarding::init() { return Void{}; }

Result<Void> ReliableForwarding::stopAndJoin() { return Void{}; }

CoTask<IOResult> ReliableForwarding::forwardWithRetry(ServiceRequestContext &requestCtx,
                                                      const UpdateReq &req,
                                                      const net::RDMARemoteBuf &rdmabuf,
                                                      const ChunkEngineUpdateJob &chunkEngineJob,
                                                      TargetPtr &target,
                                                      CommitIO &commitIO,
                                                      bool allowOutdatedChainVer /* = true */) {
  auto startTime = RelativeTime::now();

  auto recordGuard = reliableForwardRecorder.record();
  IOResult ioResult;

  ExponentialBackoffRetry retry(config_.retry_first_wait().asMs(),
                                config_.retry_max_wait().asMs(),
                                config_.retry_total_time().asMs());
  for (uint32_t retryCount = 0; !stopped_; ++retryCount) {
    auto waitTime = retry.getWaitTime();

    auto targetResult = components_.targetMap.getByChainId(req.payload.key.vChainId, allowOutdatedChainVer);
    CO_RETURN_ON_ERROR(targetResult);
    target = std::move(*targetResult);

    auto ioResult = co_await forward(req, retryCount, rdmabuf, chunkEngineJob, target, commitIO, waitTime);
    if (LIKELY(bool(ioResult.lengthInfo))) {
      recordGuard.succ();
      co_return ioResult;
    } else if (ioResult.lengthInfo.error().code() == StorageCode::kNoSuccessorTarget) {
      recordGuard.succ();
      co_return ioResult;
    }

    // TODO(SF): fine-grained error handling.
    auto code = ioResult.lengthInfo.error().code();
    if (!allowOutdatedChainVer && code == StorageClientCode::kRoutingVersionMismatch) {
      XLOGF(ERR,
            "forwarding routing version mismatch, req {}, result {}, elapsed {}",
            req,
            ioResult,
            (RelativeTime::now() - startTime).asMs());
      co_return ioResult;
    }

    if (waitTime.count() == 0) {
      XLOGF_IF(DFATAL,
               !requestCtx.debugFlags.faultInjectionEnabled(),
               "forwarding timeout with error, req {}, result {}",
               req,
               ioResult);
      co_return ioResult;
    } else if (code != RPCCode::kTimeout) {
      XLOGF(WARNING,
            "forwarding wait and retry, req {}, error {}, elapsed {}",
            req,
            ioResult,
            (RelativeTime::now() - startTime).asMs());
      constexpr auto checkInterval = 100ms;
      for (auto elapsed = 0ms; elapsed < waitTime && !stopped_; elapsed += checkInterval) {
        auto targetResult = components_.targetMap.getByChainId(req.payload.key.vChainId, allowOutdatedChainVer);
        CO_RETURN_ON_ERROR(targetResult);
        target = std::move(*targetResult);
        if (!target->successor.has_value()) {
          break;
        }
        co_await folly::coro::sleep(std::min(checkInterval, waitTime - elapsed));
      }
    }
  }

  auto msg = fmt::format("req is refused because of stopping, req {}", req);
  XLOG(ERR, msg);
  co_return makeError(RPCCode::kRequestRefused, std::move(msg));
}

CoTask<IOResult> ReliableForwarding::forward(const UpdateReq &req,
                                             uint32_t retryCount,
                                             const net::RDMARemoteBuf &rdmabuf,
                                             const ChunkEngineUpdateJob &chunkEngineJob,
                                             TargetPtr &target,
                                             CommitIO &commitIO,
                                             std::chrono::milliseconds timeout) {
  if (!target->successor.has_value()) {
    // use the latest chain version.
    commitIO.commitChainVer = target->vChainId.chainVer;
    co_return makeError(StorageCode::kNoSuccessorTarget);
  }

  auto ioResult = co_await doForward(req, rdmabuf, chunkEngineJob, retryCount, *target, commitIO.isSyncing, timeout);
  if (ioResult.lengthInfo) {
    commitIO.commitVer = ioResult.commitVer;
    // use successor's chain version.
    commitIO.commitChainVer = ioResult.commitChainVer;

    if (ioResult.commitChainVer > target->vChainId.chainVer) {
      // the remote obtains a higher chain version, and the local need to obtain the latest version by retry.
      auto msg = fmt::format("the remote obtains a higher chain version {} > current {}, req {}",
                             ioResult.commitChainVer,
                             target->vChainId.chainVer,
                             req);
      XLOGF(WARNING, "{}", msg);
      co_return makeError(StorageCode::kChainVersionMismatch, std::move(msg));
    }
  }
  co_return ioResult;
}

CoTask<IOResult> ReliableForwarding::doForward(const UpdateReq &req,
                                               const net::RDMARemoteBuf &rdmabuf,
                                               const ChunkEngineUpdateJob &chunkEngineJob,
                                               uint32_t retryCount,
                                               const Target &target,
                                               bool &isSyncing,
                                               std::chrono::milliseconds timeout) {
  UpdateReq updateReq = req;
  updateReq.options.fromClient = false;
  updateReq.retryCount = retryCount;
  updateReq.payload.rdmabuf = rdmabuf;
  updateReq.payload.key.vChainId.chainVer = target.vChainId.chainVer;

  auto buffer = components_.rdmabufPool.get();
  isSyncing = target.successor->targetInfo.publicState == hf3fs::flat::PublicTargetState::SYNCING;
  if (isSyncing) {
    updateReq.options.isSyncing = true;
    updateReq.options.commitChainVer = target.vChainId.chainVer;
  }

  bool readForSyncing = req.payload.isWriteTruncateExtend() && isSyncing &&
                        (req.options.isSyncing || req.payload.length != req.payload.chunkSize);
  if (readForSyncing) {
    auto recordGuard = syncingReadRecorder.record();

    // read the entire chunk.
    IOResult readResult;
    auto allocateResult = buffer.tryAllocate(req.payload.chunkSize);
    if (UNLIKELY(!allocateResult)) {
      allocateResult = co_await buffer.allocate(req.payload.chunkSize);
    }
    if (UNLIKELY(!allocateResult)) {
      readResult.lengthInfo = makeError(std::move(allocateResult.error()));
      co_return readResult;
    }
    auto &readBuf = *allocateResult;

    ReadIO payload;
    payload.key = updateReq.payload.key;
    payload.offset = 0;
    payload.length = req.payload.chunkSize;
    BatchReadJob batch(payload, target.storageTarget.get(), readResult, req.payload.checksum.type);
    batch.setRecalculateChecksum();
    batch.front().state().localbuf = readBuf;
    batch.front().state().bufferIndex = buffer.index();
    batch.front().state().readUncommitted = true;
    if (chunkEngineJob.chunk()) {
      batch.front().state().chunkEngineJob.set(nullptr, chunkEngineJob.chunk()->raw_chunk());
    }

    co_await components_.aioReadWorker.enqueue(&batch);
    co_await batch.complete();
    CO_RETURN_ON_ERROR(readResult.lengthInfo);  // OK.

    // clear the inline data if the update is built from full chunk read
    if (BITFLAGS_CONTAIN(updateReq.featureFlags, FeatureFlags::SEND_DATA_INLINE)) {
      BITFLAGS_CLEAR(updateReq.featureFlags, FeatureFlags::SEND_DATA_INLINE);
      updateReq.payload.inlinebuf.data.clear();
    }

    auto length = *readResult.lengthInfo;
    updateReq.payload.updateVer = readResult.updateVer;
    if (req.options.isSyncing) {
      updateReq.options.commitChainVer = batch.front().result().commitChainVer;
    }
    updateReq.payload.offset = 0;
    updateReq.payload.length = length;
    updateReq.payload.rdmabuf = readBuf.first(length).toRemoteBuf();
    updateReq.payload.checksum = batch.front().state().chunkChecksum;
    updateReq.payload.updateType = UpdateType::WRITE;

    if (length <= config_.max_inline_forward_bytes()) {
      updateReq.payload.inlinebuf.data.assign(readBuf.ptr(), readBuf.ptr() + length);
      BITFLAGS_SET(updateReq.featureFlags, hf3fs::storage::FeatureFlags::SEND_DATA_INLINE);
    }

    recordGuard.succ();
  } else if (isSyncing && !req.payload.isRemove() && chunkEngineJob.chunk() == nullptr) {
    auto chunkResult = target.storageTarget->queryChunk(req.payload.key.chunkId);
    if (UNLIKELY(!chunkResult)) {
      XLOGF(ERR, "forward query chunk failed, req {}, error {}", updateReq, chunkResult.error());
      co_return makeError(std::move(chunkResult.error()));
    }
    updateReq.payload.updateVer = chunkResult->updateVer;
  }

  auto recordGuard = updateRemoteRecorder.record();
  auto addrResult = target.getSuccessorAddr();
  if (UNLIKELY(!addrResult)) {
    XLOGF(ERR, "target forward addr invalid, target {}", target);
    co_return makeError(std::move(addrResult.error()));
  }
  net::UserRequestOptions reqOptions;
  reqOptions.timeout = Duration{timeout};
  auto updateResult = co_await components_.messenger.update(*addrResult, updateReq, &reqOptions);
  if (UNLIKELY(!updateResult)) {
    XLOGF(ERR, "forward timeout, req {}, result {}", updateReq, updateResult);
    co_return makeError(std::move(updateResult.error()));
  }
  if (LIKELY(bool(updateResult->result.lengthInfo))) {
    if (target.vChainId.chainVer < updateResult->result.commitChainVer) {
      auto msg = fmt::format("chain version local < remote, req {} local {} remote {}",
                             updateReq,
                             target,
                             updateResult->result);
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kChainVersionMismatch, std::move(msg));
    }

    auto length = *updateResult->result.lengthInfo;
    monitor::TagSet tag;
    tag.addTag("instance", fmt::format("{}", target.targetId));
    if (isSyncing) {
      updateResult->result.updateVer = req.payload.updateVer;
      forwardSyncingBytes.addSample(length, tag);
      forwardSyncingDist.addSample(length, tag);
    } else {
      forwardWriteBytes.addSample(length, tag);
      forwardWriteDist.addSample(length, tag);
    }

    recordGuard.succ();
  } else {
    XLOGF(ERR, "forward failed, req {}, result {}", updateReq, updateResult->result);
    auto errorCode = updateResult->result.lengthInfo.error().code();
    if (errorCode == StorageCode::kChecksumMismatch) {
      auto reqChecksum = updateReq.payload.checksum;
      auto realChecksum = ChecksumInfo::create(reqChecksum.type,
                                               (const uint8_t *)updateReq.payload.rdmabuf.addr(),
                                               updateReq.payload.length);
      if (reqChecksum != realChecksum) {
        XLOGF(DFATAL,
              "local rdma buffer is corrupted local {} != client {}, req: {}, kill self...",
              realChecksum,
              reqChecksum,
              req);
        ApplicationBase::handleSignal(SIGUSR2);
      }
    }
  }
  co_return updateResult->result;
}

}  // namespace hf3fs::storage
