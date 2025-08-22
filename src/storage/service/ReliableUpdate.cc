#include "storage/service/ReliableUpdate.h"

#include "common/monitor/Recorder.h"
#include "common/utils/Duration.h"
#include "storage/service/Components.h"
#include "storage/service/StorageOperator.h"

namespace hf3fs::storage {

monitor::OperationRecorder reliableUpdateRecorder{"storage.reliable_update"};
monitor::CountRecorder reliableUpdateWaited{"storage.reliable_update.waited"};
monitor::CountRecorder reliableUpdateDuplidate{"storage.reliable_update.duplicate"};
monitor::CountRecorder reliableUpdateCached{"storage.reliable_update.cached"};
monitor::OperationRecorder waitChannelLockRecorder{"storage.wait_channel_lock"};

CoTask<IOResult> ReliableUpdate::update(ServiceRequestContext &requestCtx,
                                        UpdateReq &req,
                                        net::IBSocket *ibSocket,
                                        TargetPtr &target) {
  XLOGF(DBG1, "Start reliable update, tag: {}, req: {}", req.tag, req);

  if (UNLIKELY(stopped_)) {
    auto msg = fmt::format("req is refused because of stopping, req {}", req);
    XLOG(ERR, msg);
    co_return makeError(RPCCode::kRequestRefused, std::move(msg));
  }

  // 1. check if channel id is valid.
  if (req.tag.channel.id == ChannelId{0}) {
    XLOGF(DFATAL,
          "{} request has invalid message tag {}: {}",
          magic_enum::enum_name(req.payload.updateType),
          req.tag,
          req);
    co_return makeError(StorageClientCode::kFoundBug);
  }

  // 2. get cached.
  auto clientId = req.tag.clientId;
  auto reqResult = shards_.withLock(
      [&](ClientMap &map) {
        auto &clientStatus = map[clientId];
        if (clientStatus == nullptr) {
          clientStatus = std::make_shared<ClientStatus>();
        }
        auto key = std::pair<ChainId, ChannelId>(req.payload.key.vChainId.chainId, req.tag.channel.id);
        auto &reqResult = clientStatus->channelMap[key];
        clientStatus->lastUsedTime = UtcClock::now();
        return std::shared_ptr<ReqResult>(clientStatus, &reqResult);
      },
      clientId);

  // 3. lock channel.
  auto lockRecordGuard = waitChannelLockRecorder.record();
  folly::coro::Baton baton;
  auto lock = target->storageTarget->tryLockChannel(baton, fmt::format("{}:{}", clientId, req.tag.channel.id));
  if (!lock.locked()) {
    reliableUpdateWaited.addSample(1);
    XLOGF(ERR, "Channel is locked, need retry, tag: {}, req: {}", req.tag, req);
    co_return makeError(StorageCode::kChannelIsLocked);
  }
  lockRecordGuard.report(true);

  IOResult updateResult;
  if (req.tag.channel.seqnum < reqResult->channelSeqnum) {
    reliableUpdateDuplidate.addSample(1);
    XLOGF(WARN, "Find a duplicate update, tag: {}, cached result: {}, req: {}", req.tag, *reqResult, req);
    co_return makeError(StorageClientCode::kDuplicateUpdate);
  }

  // 4. return cached result.
  if (req.tag.channel.seqnum == reqResult->channelSeqnum &&
      target->storageTarget->generationId() == reqResult->generationId) {
    if (req.tag.requestId != reqResult->requestId) {
      XLOGF(DFATAL,
            "[BUG] Message tag {} is already assigned to another update, cached result: {}, req: {}",
            req.tag,
            *reqResult,
            req);
      co_return makeError(StorageClientCode::kFoundBug);
    }

    if (reqResult->updateResult.lengthInfo.hasValue()) {
      if (req.payload.updateVer == 0 || req.payload.updateVer == reqResult->updateResult.updateVer) {
        updateResult = reqResult->updateResult;

        if (*updateResult.lengthInfo != req.payload.length && !req.payload.isExtend()) {
          updateResult.lengthInfo = req.payload.length;
          XLOGF(WARN,
                "Cached length info {} not equal to write size in request {}, fixed update result: {}",
                reqResult->updateResult.lengthInfo,
                req,
                updateResult);
        }

        reliableUpdateCached.addSample(1);
        XLOGF(DBG1, "Return cached update result, tag: {}, cached result: {}, req: {}", req.tag, *reqResult, req);
        co_return updateResult;
      } else {
        XLOGF(CRITICAL,
              "Cached update version not equal to request update version, req:{}, cached result: {}",
              req,
              *reqResult);
      }
    } else if (req.payload.updateVer == 0 && !target->storageTarget->useChunkEngine() &&
               reqResult->succUpdateVer != 0) {
      XLOGF(CRITICAL, "Pick up previous update version, tag: {}, cached result: {}, req: {}", req.tag, *reqResult, req);
      req.payload.updateVer = reqResult->succUpdateVer;
    }
  }

  // 5. start a new task.
  auto recordGuard = reliableUpdateRecorder.record();
  updateResult = co_await components_.storageOperator.handleUpdate(requestCtx, req, ibSocket, target);
  if (LIKELY(bool(updateResult.lengthInfo))) {
    recordGuard.succ();
  }

  *reqResult = {req.tag.channel.seqnum,
                req.tag.requestId,
                updateResult,
                req.payload.updateVer,
                target->storageTarget->generationId()};

  XLOGF(DBG1, "Completed reliable update, tag: {}, result: {}", req.tag, *reqResult);
  co_return updateResult;
}

Result<Void> ReliableUpdate::cleanUpExpiredClients(const robin_hood::unordered_set<std::string> &activeClients) {
  if (!config_.clean_up_expired_clients()) {
    return Void{};
  }
  if (activeClients.empty()) {
    XLOGF(ERR, "activeClients is empty!");
    return Void{};
  }
  auto allZero = ClientId::zero();
  std::size_t cleanUpClientCount = 0;
  shards_.iterate([&](ClientMap &map) {
    auto now = UtcClock::now();
    auto expiredClientsTimeout = config_.expired_clients_timeout();
    for (auto it = map.begin(); it != map.end();) {
      const auto &[clientId, clientStatus] = *it;
      if (!activeClients.contains(clientId.uuid.toHexString()) && clientId != allZero &&
          now >= clientStatus->lastUsedTime + expiredClientsTimeout) {
        XLOGF(WARNING, "clean up expired client {}, last used time: {}", clientId, clientStatus->lastUsedTime);
        it = map.erase(it);
        ++cleanUpClientCount;
      } else {
        ++it;
      }
    }
  });
  XLOGF(WARNING, "clean up {} expired clients", cleanUpClientCount);
  return Void{};
}

}  // namespace hf3fs::storage
