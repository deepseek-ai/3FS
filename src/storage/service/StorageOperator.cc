#include "storage/service/StorageOperator.h"

#include <boost/range/adaptor/reversed.hpp>
#include <fmt/format.h>

#include "common/monitor/Recorder.h"
#include "common/net/RDMAControl.h"
#include "common/net/RequestOptions.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/SemaphoreGuard.h"
#include "storage/aio/BatchReadJob.h"
#include "storage/service/Components.h"
#include "storage/update/UpdateJob.h"

namespace hf3fs::storage {

monitor::OperationRecorder storageAioEnqueueRecorder{"storage.aio_enqueue"};
monitor::OperationRecorder storageWaitAioRecorder{"storage.wait_aio"};
monitor::OperationRecorder storageWaitSemRecorder{"storage.wait_sem"};
monitor::OperationRecorder storageWaitBatchRecorder{"storage.wait_batch"};
monitor::OperationRecorder storageWaitPostRecorder{"storage.wait_post"};
monitor::OperationRecorder storageWaitAioAndPostRecorder{"storage.wait_aio_and_post"};
monitor::OperationRecorder storageReadPrepareTarget{"storage.read_prepare_target"};
monitor::OperationRecorder storageReadPrepareBuffer{"storage.read_prepare_buffer"};

monitor::OperationRecorder storageReqReadRecorder{"storage.req_read"};
monitor::DistributionRecorder storageReqReadSize{"storage.req_read.size"};
monitor::CountRecorder storageReadCount{"storage.read.count"};
monitor::CountRecorder storageReadBytes{"storage.read.bytes"};
monitor::LambdaRecorder storageReadAvgBytes{"storage.read.avg_bytes"};
monitor::CountRecorder aioTotalHeadLength{"storage.aio_align.total_head_length"};
monitor::CountRecorder aioTotalTailLength{"storage.aio_align.total_tail_length"};
monitor::CountRecorder aioTotalAlignedLength{"storage.aio_align.total_length"};

monitor::OperationRecorder storageReqWriteRecorder{"storage.req_write"};
monitor::CountRecorder storageWriteBytes{"storage.req_write.bytes"};

monitor::OperationRecorder storageReqUpdateRecorder{"storage.req_update"};
monitor::CountRecorder storageUpdateBytes{"storage.req_update.bytes"};

monitor::CountRecorder storageTotalWriteBytes{"storage.write.bytes"};

monitor::OperationRecorder storageDoUpdateRecorder{"storage.do_update"};
monitor::OperationRecorder storageWriteWaitSemRecorder{"storage.write_wait_sem"};
monitor::OperationRecorder storageWriteWaitPostRecorder{"storage.write_wait_post"};
monitor::OperationRecorder storageDoCommitRecorder{"storage.do_commit"};
monitor::OperationRecorder storageDoQueryRecorder{"storage.do_query"};
monitor::CountRecorder storageNumChunksInQueryRes{"storage.do_query.num_chunks"};
monitor::OperationRecorder waitChunkLockRecorder{"storage.wait_chunk_lock"};
monitor::OperationRecorder storageDoTruncateRecorder{"storage.do_truncate"};
monitor::OperationRecorder storageDoRemoveRecorder{"storage.do_remove"};
monitor::CountRecorder storageNumChunksRemoved{"storage.do_remove.num_chunks"};
monitor::OperationRecorder syncStartRecorder{"storage.sync_start"};
monitor::OperationRecorder syncDoneRecorder{"storage.sync_done"};

monitor::OperationRecorder storageReqRemoveChunksRecorder{"storage.req_remove_chunks"};
monitor::OperationRecorder storageRemoveRangeRecorder{"storage.remove_range"};

Result<Void> StorageOperator::init(uint32_t numberOfDisks) {
  storageReadAvgBytes.setLambda([&] {
    auto totalReadBytes = totalReadBytes_.exchange(0);
    auto totalReadIOs = totalReadIOs_.exchange(0);
    return totalReadBytes / std::max(1ul, totalReadIOs);
  });

  if (!storageEventTrace_.open()) {
    XLOGF(CRITICAL, "Failed to open trace log in directory: {}", config_.event_trace_log().trace_file_dir());
    return makeError(StorageCode::kStorageInitFailed);
  }

  return updateWorker_.start(numberOfDisks);
}

Result<Void> StorageOperator::stopAndJoin() {
  storageReadAvgBytes.reset();
  updateWorker_.stopAndJoin();
  storageEventTrace_.close();
  return Void{};
}

CoTryTask<BatchReadRsp> StorageOperator::batchRead(ServiceRequestContext &requestCtx,
                                                   const BatchReadReq &req,
                                                   serde::CallContext &ctx) {
  XLOGF(DBG5, "Received batch read request {} with tag {} and {} IOs", fmt::ptr(&req), req.tag, req.payloads.size());

  auto recordGuard = storageReqReadRecorder.record(monitor::instanceTagSet(std::to_string(req.userInfo.uid)));

  auto prepareTargetRecordGuard = storageReadPrepareTarget.record();
  auto snapshot = components_.targetMap.snapshot();
  auto batchSize = req.payloads.size();
  BatchReadRsp rsp;
  rsp.results.resize(batchSize);
  BatchReadJob batch(req.payloads, rsp.results, req.checksumType);
  storageReadCount.addSample(batchSize);
  storageReqReadSize.addSample(batchSize);

  size_t totalLength = 0;
  size_t totalHeadLength = 0;
  size_t totalTailLength = 0;
  for (AioReadJobIterator it(&batch); it; it++) {
    // get target for batch read, need check public and local state.
    auto targetResult = FAULT_INJECTION_POINT(
        requestCtx.debugFlags.injectServerError(),
        makeError(StorageCode::kChainVersionMismatch),
        snapshot->getByChainId(it->readIO().key.vChainId, config_.batch_read_ignore_chain_version()));
    if (UNLIKELY(!targetResult)) {
      auto msg = fmt::format("read get target failed, req {}, error {}", it->readIO(), targetResult.error());
      XLOG(ERR, msg);
      co_return makeError(std::move(targetResult.error()));
    }
    auto target = std::move(*targetResult);
    if (UNLIKELY(!target->upToDate())) {
      auto msg = fmt::format("read target is not upToDate, req {}, target {}", it->readIO(), *target);
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
    }
    it->state().storageTarget = target->storageTarget.get();
    totalLength += it->readIO().length;
    totalHeadLength += it->state().headLength;
    totalTailLength += it->state().tailLength;
    if (FAULT_INJECTION_POINT(requestCtx.debugFlags.injectServerError(),
                              true,
                              UNLIKELY(it->readIO().length > it->readIO().rdmabuf.size()))) {
      auto msg = fmt::format("invalid read buffer size {}", it->readIO());
      XLOG(ERR, msg);
      co_return makeError(StatusCode::kInvalidArg, std::move(msg));
    }
    it->state().readUncommitted = BITFLAGS_CONTAIN(req.featureFlags, FeatureFlags::ALLOW_READ_UNCOMMITTED);
  }
  totalReadBytes_ += totalLength;
  totalReadIOs_ += batchSize;
  storageReadBytes.addSample(totalLength);
  aioTotalHeadLength.addSample(totalHeadLength);
  aioTotalTailLength.addSample(totalTailLength);
  aioTotalAlignedLength.addSample(totalLength + totalHeadLength + totalTailLength);
  prepareTargetRecordGuard.report(true);

  auto prepareBufferRecordGuard = storageReadPrepareBuffer.record();
  auto buffer = components_.rdmabufPool.get();
  for (AioReadJobIterator it(&batch); it; it++) {
    auto &job = *it;
    auto allocateResult = buffer.tryAllocate(job.alignedLength());
    if (UNLIKELY(!allocateResult)) {
      allocateResult = co_await buffer.allocate(job.alignedLength());
    }
    if (UNLIKELY(!allocateResult)) {
      auto msg = fmt::format("read allocate buffer failed, req {}, length {}", job.readIO(), job.alignedLength());
      XLOG(ERR, msg);
      co_return makeError(RPCCode::kRDMANoBuf, std::move(msg));
    }
    job.state().localbuf = std::move(*allocateResult);
    job.state().bufferIndex = buffer.index();
  }
  prepareBufferRecordGuard.report(true);

  if (BITFLAGS_CONTAIN(req.featureFlags, FeatureFlags::BYPASS_DISKIO)) {
    for (AioReadJobIterator it(&batch); it; it++) {
      it->result().lengthInfo = it->readIO().length;
      batch.finish(&*it);
    }
  } else {
    auto recordGuard = storageAioEnqueueRecorder.record();
    auto splitSize = config_.batch_read_job_split_size();
    for (uint32_t start = 0; start < batchSize; start += splitSize) {
      co_await components_.aioReadWorker.enqueue(AioReadJobIterator(&batch, start, splitSize));
    }
    recordGuard.report(true);
  }

  auto waitAioAndPostRecordGuard = storageWaitAioAndPostRecorder.record();
  auto waitAioRecordGuard = storageWaitAioRecorder.record();
  co_await batch.complete();
  waitAioRecordGuard.report(true);

  if (BITFLAGS_CONTAIN(req.featureFlags, FeatureFlags::SEND_DATA_INLINE)) {
    batch.copyToRespBuffer(rsp.inlinebuf.data);
  } else if (!BITFLAGS_CONTAIN(req.featureFlags, FeatureFlags::BYPASS_RDMAXMIT)) {
    auto ibSocket = ctx.transport()->ibSocket();
    if (UNLIKELY(ibSocket == nullptr)) {
      XLOGF(ERR, "batch read no RDMA socket");
      co_return makeError(StatusCode::kInvalidArg, "batch read no RDMA socket");
    }

    auto waitBatchRecordGuard = storageWaitBatchRecorder.record();
    auto writeBatch = ctx.writeTransmission();
    batch.addBufferToBatch(writeBatch);
    waitBatchRecordGuard.report(true);

    auto rdmaSemaphoreIter = concurrentRdmaWriteSemaphore_.find(ibSocket->device()->id());
    if (rdmaSemaphoreIter == concurrentRdmaWriteSemaphore_.end()) {
      XLOGF(CRITICAL,
            "Cannot find RDMA operation semaphore for IB device #{} {}",
            ibSocket->device()->id(),
            ibSocket->device()->name());
      co_return makeError(RPCCode::kIBDeviceNotFound);
    }

    auto RDMATransmissionReqTimeout = config_.rdma_transmission_req_timeout();
    bool applyTransmissionBeforeGettingSemaphore = config_.apply_transmission_before_getting_semaphore();
    if (ctx.packet().controlRDMA() && RDMATransmissionReqTimeout != 0_ms && applyTransmissionBeforeGettingSemaphore) {
      co_await writeBatch.applyTransmission(RDMATransmissionReqTimeout);
    }

    auto ibdevTagSet = monitor::instanceTagSet(ibSocket->device()->name());
    auto waitSemRecordGuard = storageWaitSemRecorder.record(ibdevTagSet);
    SemaphoreGuard guard(rdmaSemaphoreIter->second);
    co_await guard.coWait();
    waitSemRecordGuard.report(true);

    if (ctx.packet().controlRDMA() && RDMATransmissionReqTimeout != 0_ms && !applyTransmissionBeforeGettingSemaphore) {
      co_await writeBatch.applyTransmission(RDMATransmissionReqTimeout);
    }

    auto waitPostRecordGuard = storageWaitPostRecorder.record(ibdevTagSet);
    auto postResult = FAULT_INJECTION_POINT(requestCtx.debugFlags.injectServerError(),
                                            makeError(RPCCode::kRDMAPostFailed),
                                            (co_await writeBatch.post()));
    if (UNLIKELY(!postResult)) {
      for (AioReadJobIterator it(&batch); it; it++) {
        it->result().lengthInfo = makeError(std::move(postResult.error()));
      }
    } else {
      waitPostRecordGuard.succ();
    }
  }
  waitAioAndPostRecordGuard.report(true);

  recordGuard.succ();
  co_return rsp;
}

CoTryTask<WriteRsp> StorageOperator::write(ServiceRequestContext &requestCtx,
                                           const WriteReq &req,
                                           net::IBSocket *ibSocket) {
  auto recordGuard = storageReqWriteRecorder.record(monitor::instanceTagSet(std::to_string(req.userInfo.uid)));

  XLOGF(DBG1,
        "Received write request {} with tag {} to chunk {} on {}",
        fmt::ptr(&req),
        req.tag,
        req.payload.key.chunkId,
        req.payload.key.vChainId.chainId);

  WriteRsp rsp;
  rsp.tag = req.tag;

  // get target for write from client.
  auto targetResult = FAULT_INJECTION_POINT(requestCtx.debugFlags.injectServerError(),
                                            makeError(StorageCode::kChainVersionMismatch),
                                            components_.targetMap.getByChainId(req.payload.key.vChainId));
  if (UNLIKELY(!targetResult)) {
    rsp.result.lengthInfo = makeError(std::move(targetResult.error()));
    co_return rsp;
  }
  auto target = std::move(*targetResult);

  UpdateReq updateReq{req.payload, {}, req.tag, req.retryCount, req.userInfo, req.featureFlags};
  updateReq.options.fromClient = true;
  rsp.result = co_await components_.reliableUpdate.update(requestCtx, updateReq, ibSocket, target);
  if (LIKELY(bool(rsp.result.lengthInfo))) {
    XLOGF_IF(DFATAL,
             *rsp.result.lengthInfo != req.payload.length,
             "Length info in response {} not equal to write size in request {}, result: {}, write io: {}",
             *rsp.result.lengthInfo,
             req.payload.length,
             rsp.result,
             req.payload);
    storageWriteBytes.addSample(*rsp.result.lengthInfo);
    storageTotalWriteBytes.addSample(*rsp.result.lengthInfo);
    recordGuard.succ();
  }

  XLOGF(DBG1,
        "Processed write request {} with tag {} to chunk {} on {}, result: {}",
        fmt::ptr(&req),
        req.tag,
        req.payload.key.chunkId,
        req.payload.key.vChainId.chainId,
        rsp.result);
  co_return rsp;
}

CoTryTask<UpdateRsp> StorageOperator::update(ServiceRequestContext &requestCtx,
                                             const UpdateReq &updateReq,
                                             net::IBSocket *ibSocket) {
  auto recordGuard = storageReqUpdateRecorder.record(monitor::instanceTagSet(std::to_string(updateReq.userInfo.uid)));

  auto req = updateReq;
  XLOGF(DBG1,
        "Received update request {} with tag {} to chunk {} on {}",
        fmt::ptr(&req),
        req.tag,
        req.payload.key.chunkId,
        req.payload.key.vChainId.chainId);

  UpdateRsp rsp;
  rsp.tag = req.tag;

  // get target for update from predecessor server.
  auto targetResult = FAULT_INJECTION_POINT(requestCtx.debugFlags.injectServerError(),
                                            makeError(StorageCode::kChainVersionMismatch),
                                            components_.targetMap.getByChainId(req.payload.key.vChainId));
  if (UNLIKELY(!targetResult)) {
    rsp.result.lengthInfo = makeError(std::move(targetResult.error()));
    co_return rsp;
  }
  auto target = std::move(*targetResult);

  if (req.payload.updateType == UpdateType::REMOVE && req.tag.channel.id == ChannelId{0}) {
    rsp.result = co_await handleUpdate(requestCtx, req, ibSocket, target);
  } else {
    rsp.result = co_await components_.reliableUpdate.update(requestCtx, req, ibSocket, target);
  }

  if (LIKELY(bool(rsp.result.lengthInfo))) {
    storageUpdateBytes.addSample(*rsp.result.lengthInfo);
    storageTotalWriteBytes.addSample(*rsp.result.lengthInfo);
    recordGuard.succ();
  }

  XLOGF(DBG1,
        "Processed update request {} with tag {} to chunk {} on {}, result: {}",
        fmt::ptr(&req),
        req.tag,
        req.payload.key.chunkId,
        req.payload.key.vChainId.chainId,
        rsp.result);

  co_return rsp;
}

CoTask<IOResult> StorageOperator::handleUpdate(ServiceRequestContext &requestCtx,
                                               UpdateReq &req,
                                               net::IBSocket *ibSocket,
                                               TargetPtr &target) {
  // 1. get target.
  if (UNLIKELY(req.options.fromClient && !target->isHead)) {
    XLOGF(ERR, "non-head node receive a client update request");
    co_return makeError(StorageClientCode::kRoutingError, "non-head node receive a client update request");
  }
  if (UNLIKELY(req.options.fromClient && config_.read_only())) {
    auto msg = fmt::format("storage is readonly!");
    XLOG(ERR, msg);
    co_return makeError(StatusCode::kReadOnlyMode, std::move(msg));
  }
  if (UNLIKELY(req.payload.key.chunkId.data().empty())) {
    auto msg = fmt::format("update request with empty chunk id: {}", req);
    XLOG(ERR, msg);
    co_return makeError(StatusCode::kInvalidArg, std::move(msg));
  }

  XLOGF(DBG1, "Start the replication process, target: {}, tag: {}, req: {}", target->targetId, req.tag, req);

  const auto &appInfo = components_.getAppInfo();
  auto trace = storageEventTrace_.newEntry(StorageEventTrace{
      .clusterId = appInfo.clusterId,
      .nodeId = appInfo.nodeId,
      .targetId = target->targetId,
      .updateReq = req,
  });

  // 2. lock chunk.
  folly::coro::Baton baton;
  auto recordGuard = waitChunkLockRecorder.record();
  auto lockGuard = target->storageTarget->lockChunk(baton, req.payload.key.chunkId, fmt::to_string(req.tag));
  if (!lockGuard.locked()) {
    XLOGF(DBG1,
          "write wait lock on chunk {}, current owner: {}, req: {}",
          req.payload.key.chunkId,
          lockGuard.currentTag(),
          req);
    co_await lockGuard.lock();
  }
  recordGuard.report(true);

  // re-check chain version after acquiring the lock.
  auto targetResult = components_.targetMap.getByChainId(req.payload.key.vChainId);
  if (UNLIKELY(!targetResult)) {
    co_return makeError(std::move(targetResult.error()));
  }
  target = std::move(*targetResult);

  ChunkEngineUpdateJob chunkEngineJob{};

  // 3. update local target.
  auto buffer = components_.rdmabufPool.get();
  net::RDMARemoteBuf remoteBuf;
  auto updateResult = co_await doUpdate(requestCtx,
                                        req.payload,
                                        req.options,
                                        req.featureFlags,
                                        target->storageTarget,
                                        ibSocket,
                                        buffer,
                                        remoteBuf,
                                        chunkEngineJob,
                                        !(req.options.fromClient && target->rejectCreateChunk));
  trace->updateRes = updateResult;

  uint32_t code = updateResult.lengthInfo ? 0 : updateResult.lengthInfo.error().code();
  if (code == 0) {
    // 1. write success.
    if (req.payload.updateVer == 0) {
      req.payload.updateVer = updateResult.updateVer;
    } else if (UNLIKELY(req.payload.updateVer != updateResult.updateVer)) {
      auto msg = fmt::format("write update version mismatch, req {}, result {}", req, updateResult);
      XLOG(DFATAL, msg);
      co_return makeError(StorageCode::kChunkVersionMismatch, std::move(msg));
    }
  } else if (code == StorageCode::kChunkMissingUpdate) {
    // 2. missing update.
    XLOGF(DFATAL, "write missing update and block, req {}, result {}", req, updateResult);
    co_return updateResult;
  } else if (code == StorageCode::kChunkCommittedUpdate) {
    // 3. committed update, considered as a successful write.
    updateResult.lengthInfo = req.payload.length;
    updateResult.updateVer = req.payload.updateVer;
    updateResult.commitVer = req.payload.updateVer;
    XLOGF(DFATAL, "write committed update, req {}, result {}", req, updateResult);
    co_return updateResult;
  } else if (code == StorageCode::kChunkStaleUpdate) {
    // 3. stale update, considered as a successful write.
    updateResult.lengthInfo = req.payload.length;
    updateResult.updateVer = req.payload.updateVer;
    XLOGF(CRITICAL, "write stale update, req {}, result {}", req, updateResult);
  } else if (code == StorageCode::kChunkAdvanceUpdate) {
    // 4. advance update.
    XLOGF(DFATAL, "write advance update, req {}, result {}", req, updateResult);
    co_return updateResult;
  } else {
    XLOGF(CRITICAL, "write update failed, req {}, result {}", req, updateResult);
    co_return updateResult;
  }

  XLOGF(DBG1, "Updated local chunk, target: {}, tag: {}, result: {}", target->targetId, req.tag, updateResult);

  // 4. forward to successor.
  CommitIO commitIO;
  commitIO.key = req.payload.key;
  commitIO.commitVer = updateResult.updateVer;
  commitIO.isRemove = req.payload.isRemove();

  auto forwardResult = co_await components_.reliableForwarding
                           .forwardWithRetry(requestCtx, req, remoteBuf, chunkEngineJob, target, commitIO);
  if (UNLIKELY(commitIO.commitVer != updateResult.updateVer)) {
    auto msg = fmt::format("commit version mismatch, req: {}, successor {} != local {}",
                           req,
                           commitIO.commitVer,
                           updateResult.updateVer);
    XLOG(DFATAL, msg);
    co_return makeError(StorageCode::kChunkVersionMismatch, std::move(msg));
  }

  XLOGF(DBG1,
        "Forwarded update to successor {}, target: {}, tag: {}, result: {}",
        (target->successor ? target->successor->targetInfo.targetId : TargetId{0}),
        target->targetId,
        req.tag,
        forwardResult);
  trace->forwardRes = forwardResult;
  trace->commitIO = commitIO;

  if (forwardResult.lengthInfo) {
    if (commitIO.isRemove && (forwardResult.checksum.type == ChecksumType::NONE ||
                              updateResult.checksum.type == ChecksumType::NONE || commitIO.isSyncing)) {
      // The known issue is that during the delete operation, it is possible for one side to encounter a "chunk not
      // found" situation.
      XLOGF(INFO,
            "Remove op local checksum {} not equal to checksum {} generated by successor, key: {}, syncing: {}",
            updateResult.checksum,
            forwardResult.checksum,
            req.payload.key,
            commitIO.isSyncing);
    } else if (forwardResult.checksum != updateResult.checksum) {
      auto msg = fmt::format("Local checksum {} not equal to checksum {} generated by successor, key: {}",
                             updateResult.checksum,
                             forwardResult.checksum,
                             req.payload.key);
      XLOG_IF(DFATAL, !requestCtx.debugFlags.faultInjectionEnabled(), msg);
      co_return makeError(StorageClientCode::kChecksumMismatch, std::move(msg));
    }
  } else if (forwardResult.lengthInfo.error().code() != StorageCode::kNoSuccessorTarget) {
    co_return forwardResult;
  }

  // 5. commit.
  auto commitResult =
      co_await doCommit(requestCtx, commitIO, req.options, chunkEngineJob, req.featureFlags, target->storageTarget);

  code = commitResult.lengthInfo ? 0 : commitResult.lengthInfo.error().code();

  if (LIKELY(code == 0)) {
    // 1. commit success.
  } else if (code == StorageCode::kChunkStaleCommit) {
    // 2. stale commit, considered as a successful commit.
    XLOGF(INFO, "write stale commit, req {}, result {}", req, commitResult);
    commitResult.commitVer = updateResult.updateVer;
  } else {
    // 3. commit fail.
    XLOGF(ERR, "write commit fail, req {}, result {}", req, commitResult);
    co_return commitResult;
  }

  commitResult.lengthInfo = updateResult.lengthInfo;
  commitResult.checksum = updateResult.checksum;

  XLOGF(DBG1, "Committed local chunk, target: {}, tag: {}, result: {}", target->targetId, req.tag, commitResult);
  trace->commitRes = commitResult;

  // storageEventTrace_.append(const StorageEventTrace &obj)

  co_return commitResult;
}

CoTask<IOResult> StorageOperator::doUpdate(ServiceRequestContext &requestCtx,
                                           const UpdateIO &updateIO,
                                           const UpdateOptions &updateOptions,
                                           uint32_t featureFlags,
                                           const std::shared_ptr<StorageTarget> &target,
                                           net::IBSocket *ibSocket,
                                           BufferPool::Buffer &buffer,
                                           net::RDMARemoteBuf &remoteBuf,
                                           ChunkEngineUpdateJob &chunkEngineJob,
                                           bool allowToAllocate) {
  auto recordGuard = storageDoUpdateRecorder.record();
  UpdateJob job(requestCtx, updateIO, updateOptions, chunkEngineJob, target, allowToAllocate);

  if (BITFLAGS_CONTAIN(featureFlags, FeatureFlags::SEND_DATA_INLINE)) {
    if (updateIO.inlinebuf.data.size() != updateIO.length) {
      auto msg = fmt::format("[BUG] Inline buffer size {} not equal to update size {}, io: {}",
                             updateIO.inlinebuf.data.size(),
                             updateIO.length,
                             updateIO);
      XLOG(DFATAL, msg);
      co_return makeError(StorageClientCode::kFoundBug, std::move(msg));
    }
    job.state().data = updateIO.inlinebuf.data.data();
  } else if (updateIO.isWrite()) {
    if (UNLIKELY(ibSocket == nullptr)) {
      auto msg = fmt::format("update no RDMA socket, io: {}", updateIO);
      XLOG(ERR, msg);
      co_return makeError(StatusCode::kInvalidArg, std::move(msg));
    }

    auto allocateResult = buffer.tryAllocate(updateIO.rdmabuf.size());
    if (UNLIKELY(!allocateResult)) {
      allocateResult = co_await buffer.allocate(updateIO.rdmabuf.size());
    }
    if (UNLIKELY(!allocateResult)) {
      auto msg = fmt::format("write allocate buffer failed, req {}, error {}, length {}",
                             updateIO,
                             allocateResult.error(),
                             updateIO.rdmabuf.size());
      XLOG(ERR, msg);
      co_return makeError(RPCCode::kRDMANoBuf, std::move(msg));
    }
    job.state().data = allocateResult->ptr();
    remoteBuf = allocateResult->toRemoteBuf();
    if (!BITFLAGS_CONTAIN(featureFlags, FeatureFlags::BYPASS_RDMAXMIT)) {
      auto readBatch = ibSocket->rdmaReadBatch();
      auto batchAddResult = readBatch.add(updateIO.rdmabuf, std::move(*allocateResult));
      if (UNLIKELY(!batchAddResult)) {
        XLOGF(ERR, "write add to batch failed, req {}, error {}", updateIO, batchAddResult.error());
        co_return makeError(batchAddResult.error());
      }

      auto rdmaSemaphoreIter = concurrentRdmaReadSemaphore_.find(ibSocket->device()->id());
      if (rdmaSemaphoreIter == concurrentRdmaReadSemaphore_.end()) {
        auto msg = fmt::format("Cannot find RDMA operation semaphore for IB device #{} {}",
                               ibSocket->device()->id(),
                               ibSocket->device()->name());
        XLOG(CRITICAL, msg);
        co_return makeError(RPCCode::kIBDeviceNotFound, std::move(msg));
      }

      auto ibdevTagSet = monitor::instanceTagSet(ibSocket->device()->name());
      auto waitSemRecordGuard = storageWriteWaitSemRecorder.record(ibdevTagSet);
      SemaphoreGuard guard(rdmaSemaphoreIter->second);
      co_await guard.coWait();
      waitSemRecordGuard.report(true);

      auto waitPostRecordGuard = storageWriteWaitPostRecorder.record(ibdevTagSet);
      auto postResult = co_await readBatch.post();
      if (UNLIKELY(!postResult)) {
        XLOGF(ERR, "write post RDMA failed, req {}, error {}", updateIO, postResult.error());
        co_return makeError(std::move(postResult.error()));
      } else {
        waitPostRecordGuard.report(true);
      }
    }
  }

  if (BITFLAGS_CONTAIN(featureFlags, FeatureFlags::BYPASS_DISKIO)) {
    job.setResult(updateIO.length);
  } else {
    co_await updateWorker_.enqueue(&job);
    co_await job.complete();
  }
  if (LIKELY(bool(job.result().lengthInfo))) {
    recordGuard.succ();
  } else {
    auto code = job.result().lengthInfo.error().code();
    if (code == StorageCode::kChunkWriteFailed || code == StorageCode::kChunkMetadataSetError) {
      components_.targetMap.offlineTargets(target->path().parent_path());
    }
  }
  co_return std::move(job.result());
}

CoTask<IOResult> StorageOperator::doCommit(ServiceRequestContext &requestCtx,
                                           const CommitIO &commitIO,
                                           const UpdateOptions &updateOptions,
                                           ChunkEngineUpdateJob &chunkEngineJob,
                                           uint32_t featureFlags,
                                           const std::shared_ptr<StorageTarget> &target) {
  auto recordGuard = storageDoCommitRecorder.record();
  UpdateJob job(requestCtx, commitIO, updateOptions, chunkEngineJob, target);
  if (BITFLAGS_CONTAIN(featureFlags, FeatureFlags::BYPASS_DISKIO)) {
    job.setResult(0);
    job.result().commitVer = commitIO.commitVer;
    job.result().commitChainVer = commitIO.commitChainVer;
  } else {
    co_await updateWorker_.enqueue(&job);
    co_await job.complete();
  }
  if (LIKELY(bool(job.result().lengthInfo))) {
    recordGuard.succ();
  }
  co_return job.result();
}

Result<std::vector<std::pair<ChunkId, ChunkMetadata>>> StorageOperator::doQuery(ServiceRequestContext &requestCtx,
                                                                                const VersionedChainId &vChainId,
                                                                                const ChunkIdRange &chunkIdRange) {
  auto recordGuard = storageDoQueryRecorder.record();
  // get target for chunk query from client.
  CHECK_RESULT(target, components_.targetMap.getByChainId(vChainId));

  auto queryResult = FAULT_INJECTION_POINT(requestCtx.debugFlags.injectServerError(),
                                           makeError(StorageCode::kMetaStoreInvalidIterator),
                                           target->storageTarget->queryChunks(chunkIdRange));

  if (LIKELY(bool(queryResult))) {
    storageNumChunksInQueryRes.addSample(queryResult->size());
    recordGuard.succ();
  }

  return queryResult;
}

// returns number of processed chunks on success
CoTryTask<uint32_t> StorageOperator::processQueryResults(ServiceRequestContext &requestCtx,
                                                         const VersionedChainId &vChainId,
                                                         const ChunkIdRange &chunkIdRange,
                                                         ChunkMetadataProcessor processor,
                                                         bool &moreChunksInRange) {
  const uint32_t numChunksToProcess = chunkIdRange.maxNumChunkIdsToProcess
                                          ? std::min(chunkIdRange.maxNumChunkIdsToProcess, UINT32_MAX - 1)
                                          : (UINT32_MAX - 1);
  const uint32_t maxNumResultsPerQuery = config_.max_num_results_per_query();
  ChunkIdRange currentRange = {chunkIdRange.begin, chunkIdRange.end, 0};
  uint32_t numQueryResults = 0;
  Status status(StatusCode::kOK);

  while (true) {
    currentRange.maxNumChunkIdsToProcess = std::min(numChunksToProcess - numQueryResults + 1, maxNumResultsPerQuery);

    auto queryResult = doQuery(requestCtx, vChainId, currentRange);

    if (UNLIKELY(queryResult.hasError())) {
      status = queryResult.error();
      goto exit;
    }

    for (const auto &[chunkId, metadata] : *queryResult) {
      switch (metadata.recycleState) {
        case RecycleState::NORMAL:
          break;
        case RecycleState::REMOVAL_IN_PROGRESS:
          XLOGF(INFO,
                "Ignore chunk {} being removed, recycle state {}, commit version {}, update version {}",
                chunkId,
                int(metadata.recycleState),
                metadata.commitVer,
                metadata.updateVer);
          continue;
        case RecycleState::REMOVAL_IN_RETRYING:
          XLOGF(INFO,
                "Ignore dummy chunk {} being removed, recycle state {}, commit version {}, update version {}",
                chunkId,
                int(metadata.recycleState),
                metadata.commitVer,
                metadata.updateVer);
          continue;
      }

      if (numQueryResults < numChunksToProcess) {
        auto result = co_await processor(chunkId, metadata);

        if (UNLIKELY(result.hasError())) {
          status = result.error();
          goto exit;
        }
      }

      numQueryResults++;

      if (numQueryResults >= numChunksToProcess + 1) {
        XLOGF(DBG5,
              "Enough chunks in range found, number of results: {}/{}, current range: {}",
              numQueryResults,
              numChunksToProcess,
              currentRange);
        goto exit;
      }
    }

    if (queryResult->size() < currentRange.maxNumChunkIdsToProcess) {
      XLOGF(DBG5,
            "No more chunk in range, number of results: {}/{}, current range: {}",
            numQueryResults,
            numChunksToProcess,
            currentRange);
      goto exit;
    } else {
      // there could be more chunks in the range, update range for next query
      const auto &[chunkId, _] = *(queryResult->crbegin());
      currentRange.end = chunkId;
    }
  }

exit:
  if (status.code() != StatusCode::kOK) {
    XLOGF(ERR,
          "Failed to process chunk metadata in range: {}, error {}, {} chunks processed before failure",
          chunkIdRange,
          status,
          numQueryResults);
    co_return makeError(status);
  }

  moreChunksInRange = numQueryResults > numChunksToProcess;

  XLOGF(DBG3,
        "Processed metadata of {} chunks in range: {}, more chunks: {}",
        numQueryResults,
        chunkIdRange,
        moreChunksInRange);
  co_return std::min(numQueryResults, numChunksToProcess);
}

CoTask<IOResult> StorageOperator::doTruncate(ServiceRequestContext &requestCtx,
                                             const TruncateChunkOp &op,
                                             flat::UserInfo userInfo,
                                             uint32_t featureFlags) {
  auto recordGuard = storageDoTruncateRecorder.record();
  UpdateIO updateIO{0 /*offset*/,
                    op.chunkLen,
                    op.chunkSize,
                    GlobalKey{op.vChainId, op.chunkId},
                    {} /*rdmabuf*/,
                    ChunkVer(0) /*updateVer*/,
                    op.onlyExtendChunk ? UpdateType::EXTEND : UpdateType::TRUNCATE,
                    ChecksumInfo{ChecksumType::NONE, 0}};
  UpdateReq updateReq{updateIO, {}, op.tag, op.retryCount, userInfo, featureFlags};
  updateReq.options.fromClient = true;

  // get target for truncate from client.
  auto targetResult = components_.targetMap.getByChainId(op.vChainId);
  if (UNLIKELY(!targetResult)) {
    IOResult rsp;
    rsp.lengthInfo = makeError(std::move(targetResult.error()));
    co_return rsp;
  }
  auto target = std::move(*targetResult);

  auto updateRes = co_await components_.reliableUpdate.update(requestCtx, updateReq, nullptr /*ibSocket*/, target);

  XLOGF_IF(ERR,
           updateRes.lengthInfo.hasError(),
           "Failed to truncate chunk {} on {}, tag: {}, result: {}",
           updateIO.key.chunkId,
           updateIO.key.vChainId.chainId,
           updateReq.tag,
           updateRes);
  XLOGF_IF(INFO,
           !updateRes.lengthInfo.hasError(),
           "Truncated chunk {} on {}, tag: {}, result: {}",
           updateIO.key.chunkId,
           updateIO.key.vChainId.chainId,
           updateReq.tag,
           updateRes);

  if (LIKELY(bool(updateRes.lengthInfo))) {
    recordGuard.succ();
  }
  co_return updateRes;
}

CoTask<IOResult> StorageOperator::doRemove(ServiceRequestContext &requestCtx,
                                           const RemoveChunksOp &op,
                                           flat::UserInfo userInfo,
                                           uint32_t featureFlags) {
  auto recordGuard = storageDoRemoveRecorder.record();
  // this method requires that the chunk id range specifies one chunk
  assert(op.chunkIdRange.begin == op.chunkIdRange.end);
  UpdateIO updateIO{0 /*offset*/,
                    0 /*length*/,
                    0 /*chunkSize*/,
                    GlobalKey{op.vChainId, op.chunkIdRange.begin},
                    {} /*rdmabuf*/,
                    ChunkVer(0) /*updateVer*/,
                    UpdateType::REMOVE,
                    ChecksumInfo{ChecksumType::NONE, 0}};
  UpdateReq updateReq{updateIO, {}, op.tag, op.retryCount, userInfo, featureFlags};
  updateReq.options.fromClient = true;

  // get target for remove from client.
  auto targetResult = components_.targetMap.getByChainId(op.vChainId);

  if (UNLIKELY(!targetResult)) {
    IOResult rsp;
    rsp.lengthInfo = makeError(std::move(targetResult.error()));
    co_return rsp;
  }
  auto target = std::move(*targetResult);

  IOResult updateRes;

  if (op.tag.channel.id == ChannelId{0}) {
    updateRes = co_await handleUpdate(requestCtx, updateReq, nullptr /*ibSocket*/, target);
  } else {
    updateRes = co_await components_.reliableUpdate.update(requestCtx, updateReq, nullptr /*ibSocket*/, target);
  }

  XLOGF_IF(ERR,
           updateRes.lengthInfo.hasError(),
           "Failed to remove chunk {} on {}, tag: {}, result: {}",
           updateIO.key.chunkId,
           updateIO.key.vChainId.chainId,
           updateReq.tag,
           updateRes);
  XLOGF_IF(INFO,
           !updateRes.lengthInfo.hasError(),
           "Removed chunk {} on {}, tag: {}, result: {}",
           updateIO.key.chunkId,
           updateIO.key.vChainId.chainId,
           updateReq.tag,
           updateRes);

  if (LIKELY(bool(updateRes.lengthInfo))) {
    recordGuard.succ();
  }
  co_return updateRes;
}

CoTryTask<QueryLastChunkRsp> StorageOperator::queryLastChunk(ServiceRequestContext &requestCtx,
                                                             const QueryLastChunkReq &req) {
  XLOGF(DBG3, "Query request {} with {} ops", fmt::ptr(&req), req.payloads.size());

  QueryLastChunkRsp rsp;
  rsp.results.reserve(req.payloads.size());

  for (auto &payload : req.payloads) {
    QueryLastChunkResult queryResult{
        Void{},
        ChunkId(), /*lastChunkId*/
        0 /*lastChunkLen*/,
        0 /*totalChunkLen*/,
        0 /*totalNumChunks*/,
        false /*moreChunksInRange*/,
    };

    auto processMetadata = [&queryResult](const ChunkId &chunkId, const ChunkMetadata &metadata) -> CoTryTask<void> {
      if (queryResult.lastChunkId.data().empty() || queryResult.lastChunkId < chunkId) {
        queryResult.lastChunkId = chunkId;
        queryResult.lastChunkLen = metadata.size;
      }

      queryResult.totalChunkLen += metadata.size;
      queryResult.totalNumChunks++;

      XLOGF(DBG5,
            "Query chunk {}, lastChunkId {}, totalChunkLen {}, metadata: {}",
            chunkId,
            queryResult.lastChunkId,
            queryResult.totalChunkLen,
            metadata);
      co_return Void{};
    };

    XLOGF(DBG3, "Query request {}: start to query chunks in range: {}", fmt::ptr(&req), payload.chunkIdRange);

    auto processResult = co_await processQueryResults(requestCtx,
                                                      payload.vChainId,
                                                      payload.chunkIdRange,
                                                      processMetadata,
                                                      queryResult.moreChunksInRange);

    if (UNLIKELY(processResult.hasError())) {
      queryResult.statusCode = makeError(processResult.error());
    }

    XLOGF(DBG3,
          "Query request {}: found {} chunks in range {}, status code: {}",
          fmt::ptr(&req),
          queryResult.totalNumChunks,
          payload.chunkIdRange,
          queryResult.statusCode.hasError() ? queryResult.statusCode.error() : Status::OK);

    rsp.results.push_back(queryResult);
  }

  co_return rsp;
}

CoTryTask<TruncateChunksRsp> StorageOperator::truncateChunks(ServiceRequestContext &requestCtx,
                                                             const TruncateChunksReq &req) {
  XLOGF(INFO, "Truncate request {} with {} ops", fmt::ptr(&req), req.payloads.size());

  size_t numTruncatedChunks = 0;
  TruncateChunksRsp rsp;
  rsp.results.reserve(req.payloads.size());

  for (const auto &payload : req.payloads) {
    auto result = co_await doTruncate(requestCtx, payload, req.userInfo, req.featureFlags);
    rsp.results.push_back(result);
    numTruncatedChunks += result.lengthInfo.hasValue();
  }

  XLOGF(INFO, "Truncate request {}: {}/{} chunks truncated", fmt::ptr(&req), numTruncatedChunks, req.payloads.size());
  co_return rsp;
}

CoTryTask<RemoveChunksRsp> StorageOperator::removeChunks(ServiceRequestContext &requestCtx,
                                                         const RemoveChunksReq &req) {
  auto recordGuard = storageReqRemoveChunksRecorder.record();
  XLOGF(DBG7, "Remove request {} with {} ops", fmt::ptr(&req), req.payloads.size());

  RemoveChunksRsp rsp;
  rsp.results.reserve(req.payloads.size());

  for (const auto &payload : req.payloads) {
    auto recordGuard = storageRemoveRangeRecorder.record();
    RemoveChunksResult removeRes{Void{}, 0 /*numChunksRemoved*/, false /*moreChunksInRange*/};
    auto removeOp = payload;

    auto removeChunk = [req, &requestCtx, &removeOp, &removeRes, this](
                           const ChunkId &chunkId,
                           const ChunkMetadata &metadata) -> CoTryTask<void> {
      removeOp.chunkIdRange = {chunkId, chunkId};

      auto result = co_await doRemove(requestCtx, removeOp, req.userInfo, req.featureFlags);

      if (result.lengthInfo.hasError()) {
        if (result.lengthInfo.error().code() == StorageCode::kChunkMetadataNotFound) {
          XLOGF(WARN,
                "Chunk {} on {} is already removed by another concurrent remove request",
                chunkId,
                removeOp.vChainId.chainId);
        } else {
          co_return makeError(result.lengthInfo.error());
        }
      } else {
        removeRes.numChunksRemoved++;
      }

      removeOp.tag.channel.seqnum++;  // increment the sequence number for next remove
      co_return Void{};
    };

    XLOGF(DBG3, "Remove request {}: start to remove chunks in range: {}", fmt::ptr(&req), payload.chunkIdRange);

    auto processResult = co_await processQueryResults(requestCtx,
                                                      payload.vChainId,
                                                      payload.chunkIdRange,
                                                      removeChunk,
                                                      removeRes.moreChunksInRange);

    if (UNLIKELY(processResult.hasError())) {
      removeRes.statusCode = makeError(processResult.error());
    } else {
      storageNumChunksRemoved.addSample(removeRes.numChunksRemoved);
      recordGuard.succ();
    }

    XLOGF(DBG7,
          "Remove request {}: removed {} chunks in range {}, result: {}",
          fmt::ptr(&req),
          removeRes.numChunksRemoved,
          payload.chunkIdRange,
          removeRes);

    rsp.results.push_back(removeRes);
  }

  recordGuard.succ();
  co_return rsp;
}

CoTryTask<TargetSyncInfo> StorageOperator::syncStart(const SyncStartReq &req) {
  auto recordGuard = syncStartRecorder.record();

  // get target for sync start from predecessor.
  auto targetResult = components_.targetMap.getByChainId(req.vChainId);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("sync start {} get target failed: {}", req, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(std::move(targetResult.error()));
  }

  auto target = std::move(*targetResult);
  auto targetId = target->targetId;

  if (UNLIKELY(target->publicState != flat::PublicTargetState::SYNCING)) {
    auto msg = fmt::format("target {} check state failed: {}", targetId, magic_enum::enum_name(target->publicState));
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kSyncStartFailed, std::move(msg));
  }

  if (UNLIKELY(target->localState != hf3fs::flat::LocalTargetState::ONLINE)) {
    auto msg = fmt::format("target {} check state failed: {}", targetId, serde::toJsonString(target->localState));
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kSyncStartFailed, std::move(msg));
  }

  TargetSyncInfo info;
  auto result = target->storageTarget->getAllMetadata(info.metas);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "sync start {} failed: {}", req, result.error());
    co_return makeError(std::move(result.error()));
  }

  // re-check current chain version.
  targetResult = components_.targetMap.getByChainId(req.vChainId);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("sync start {} get target failed: {}", req, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(std::move(targetResult.error()));
  }

  recordGuard.succ();
  co_return Result<TargetSyncInfo>(std::move(info));
}

CoTryTask<SyncDoneRsp> StorageOperator::syncDone(const SyncDoneReq &req) {
  auto recordGuard = syncDoneRecorder.record();
  auto result = components_.targetMap.syncReceiveDone(req.vChainId);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "sync done {} failed: {}", req, result.error());
    co_return makeError(std::move(result.error()));
  }
  recordGuard.succ();
  SyncDoneRsp rsp;
  rsp.result.lengthInfo = 0;
  co_return rsp;
}

CoTryTask<SpaceInfoRsp> StorageOperator::spaceInfo(const SpaceInfoReq &req) {
  auto spaceInfoResult = components_.storageTargets.spaceInfos(req.force);
  if (UNLIKELY(!spaceInfoResult)) {
    co_return makeError(std::move(spaceInfoResult.error()));
  }
  SpaceInfoRsp rsp;
  rsp.spaceInfos = std::move(*spaceInfoResult);
  co_return rsp;
}

CoTryTask<CreateTargetRsp> StorageOperator::createTarget(const CreateTargetReq &req) {
  auto createResult = components_.storageTargets.create(req);
  if (UNLIKELY(!createResult)) {
    XLOGF(ERR, "create target {} failed {}", req, createResult.error());
    co_return makeError(std::move(createResult.error()));
  }
  co_return CreateTargetRsp{};
}

CoTryTask<OfflineTargetRsp> StorageOperator::offlineTarget(const OfflineTargetReq &req) {
  auto targetResult = components_.targetMap.getByTargetId(req.targetId);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("offline target failed: {}, {}", req, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(std::move(targetResult.error()));
  }
  auto &target = **targetResult;

  if (target.isHead && target.isTail && !req.force) {
    auto msg = fmt::format("offline failed: target is the last online target! {}", target);
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
  }

  CO_RETURN_AND_LOG_ON_ERROR(components_.targetMap.offlineTarget(req.targetId));
  co_return OfflineTargetRsp{};
}

CoTryTask<RemoveTargetRsp> StorageOperator::removeTarget(const RemoveTargetReq &req) {
  // 1. get storage target.
  auto targetResult = components_.targetMap.getByTargetId(req.targetId);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("remove target failed: {}, {}", req, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(std::move(targetResult.error()));
  }
  auto &target = **targetResult;

  // 2. check status.
  if (!target.unrecoverableOffline()) {
    auto msg = fmt::format("remove failed: target is not offline! {}", target);
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
  }

  if (target.vChainId != VersionedChainId{} && !req.force) {
    auto msg = fmt::format("remove failed: target is still in a chain! {}", target);
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
  }

  if (!target.weakStorageTarget.expired()) {
    auto msg = fmt::format("remove failed: target is still in use! {}", target);
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
  }

  // 3. do remove.
  if (target.useChunkEngine) {
    if (target.chainId == ChainId{}) {
      auto msg = fmt::format("remove failed: chain id is empty! {}", target);
      XLOG(ERR, msg);
      co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
    }
    auto result = components_.storageTargets.removeChunkEngineTarget(target.chainId, target.diskIndex);
    CO_RETURN_AND_LOG_ON_ERROR(result);
  }

  boost::system::error_code ec{};
  boost::filesystem::remove_all(target.path, ec);
  if (ec.failed()) {
    auto msg = fmt::format("remove failed: remove path failed! {}, {}", target, ec.message());
    XLOG(ERR, msg);
    co_return makeError(StorageCode::kTargetStateInvalid, std::move(msg));
  }

  CO_RETURN_AND_LOG_ON_ERROR(components_.targetMap.removeTarget(req.targetId));

  co_return RemoveTargetRsp{};
}

CoTryTask<QueryChunkRsp> StorageOperator::queryChunk(const QueryChunkReq &req) {
  // get target for query chunk from client.
  auto targetResult = components_.targetMap.getByChainId(VersionedChainId{req.chainId, {}}, true);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("queryChunk {} get target failed: {}", req, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(std::move(targetResult.error()));
  }

  QueryChunkRsp rsp;
  rsp.target = **targetResult;
  if (rsp.target.storageTarget && !req.chunkId.data().empty()) {
    rsp.meta = rsp.target.storageTarget->queryChunk(req.chunkId);
  }
  rsp.target.storageTarget = nullptr;
  rsp.target.weakStorageTarget.reset();
  co_return rsp;
}

CoTryTask<GetAllChunkMetadataRsp> StorageOperator::getAllChunkMetadata(const GetAllChunkMetadataReq &req) {
  auto targetResult = components_.targetMap.getByTargetId(req.targetId);
  if (UNLIKELY(!targetResult)) {
    auto msg = fmt::format("get all chunk metadata: {}, get target failed: {}", req, targetResult.error());
    XLOG(ERR, msg);
    co_return makeError(std::move(targetResult.error()));
  }

  auto target = std::move(*targetResult);
  auto targetId = target->targetId;

  if (UNLIKELY(target->publicState != flat::PublicTargetState::SERVING)) {
    auto msg = fmt::format("target {} check state failed: {}", targetId, magic_enum::enum_name(target->publicState));
    XLOG(ERR, msg);
    co_return makeError(StorageClientCode::kNotAvailable, std::move(msg));
  }

  if (UNLIKELY(target->localState != hf3fs::flat::LocalTargetState::UPTODATE)) {
    auto msg = fmt::format("target {} check state failed: {}", targetId, serde::toJsonString(target->localState));
    XLOG(ERR, msg);
    co_return makeError(StorageClientCode::kNotAvailable, std::move(msg));
  }

  GetAllChunkMetadataRsp response;
  auto result = target->storageTarget->getAllMetadata(response.chunkMetaVec);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "get all chunk metadata, {} failed: {}", req, result.error());
    co_return makeError(std::move(result.error()));
  }

  co_return Result<GetAllChunkMetadataRsp>(std::move(response));
}

}  // namespace hf3fs::storage
