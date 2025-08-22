#include "meta/components/SessionManager.h"

#include <cassert>
#include <cstdint>
#include <fmt/format.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

#include "common/app/ApplicationBase.h"
#include "common/app/NodeId.h"
#include "common/kv/ITransaction.h"
#include "common/kv/WithTransaction.h"
#include "common/monitor/Recorder.h"
#include "common/serde/Serde.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/Coroutine.h"
#include "common/utils/OptionalUtils.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "common/utils/Uuid.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "fdb/FDBRetryStrategy.h"
#include "meta/components/FileHelper.h"
#include "meta/store/FileSession.h"

namespace hf3fs::meta::server {

namespace {

monitor::CountRecorder pruned("meta_server.sessions_pruned");
monitor::CountRecorder pruneFailed("meta_server.sessions_prune_failed");

CoTryTask<std::set<ClientId>> getActiveClients(client::ICommonMgmtdClient &mgmtd,
                                               bool allowBootstrapping,
                                               std::optional<Duration> timeout = std::nullopt) {
  auto result = co_await mgmtd.listClientSessions();
  if (result.hasError()) {
    XLOGF(ERR, "Failed to list active clients, error {}", result.error());
    CO_RETURN_ERROR(result);
  }
  if (result->bootstrapping && !allowBootstrapping) {
    XLOGF(INFO, "Failed to list active clients, mgmtd is bootstrapping.");
    co_return makeError(MgmtdClientCode::kRoutingInfoNotReady);
  }
  std::set<ClientId> clients;
  for (const auto &session : result->sessions) {
    auto uuid = Uuid::fromHexString(session.clientId);
    if (uuid.hasError()) {
      XLOGF(DFATAL, "Failed to parse client {} id {}, error {}", session.description, session.clientId, uuid.error());
      continue;
    }
    if (*uuid == Uuid::zero()) {
      XLOGF(DFATAL, "Client {} uuid {} is zero", session.description, session.clientId);
      continue;
    }
    if (timeout.has_value() && session.lastExtend + *timeout + 10_s < UtcClock::now()) {
      XLOGF(WARN, "Client {} timeout, last extended {}, ", session.description, session.lastExtend);
      continue;
    }
    clients.emplace(*uuid);
  }
  co_return clients;
}

}  // namespace

/** SessionManager::ScanTask */
CoTryTask<size_t> SessionManager::ScanTask::run(SessionManager &manager) {
  XLOGF(DBG, "ScanTask-{} start", shard_);
  std::map<std::string, uint64_t> map;
  SCOPE_EXIT {
    for (const auto &[host, cnt] : map) {
      XLOGF(INFO, "SessionManager found {} sessions for dead clients {}", cnt, host);
    }
  };

  // get all active clients
  auto ts = UtcClock::now();
  auto active = co_await getActiveClients(*manager.mgmtd_, false, manager.config_.session_timeout());
  CO_RETURN_ON_ERROR(active);

  size_t total = 0;
  std::optional<FileSession> prev;
  while (true) {
    // scan sessions
    auto txn = manager.kvEngine_->createReadonlyTransaction();
    auto sessions = co_await kv::WithTransaction(kv::FDBRetryStrategy{})
                        .run(std::move(txn), [&](auto &txn) -> CoTryTask<std::vector<FileSession>> {
                          co_return co_await FileSession::scan(txn, shard_, prev);
                        });
    CO_RETURN_ON_ERROR(sessions);
    if (sessions->empty()) {
      break;
    }

    // filter dead sessions
    std::vector<FileSession> deadSessions;
    for (auto &session : *sessions) {
      if (prune_->sessions.rlock()->contains(session.sessionId)) {
        // need prune this session
        XLOGF(INFO, "Need prune session {}", session);
        prune_->sessions.wlock()->erase(session.sessionId);
      } else {
        // check client is active or not
        if (active->contains(session.clientId)) {
          continue;
        }
        if (session.timestamp + 1_min > ts) {
          // concurrent create session and scan
          auto now = UtcClock::now();
          XLOGF_IF(WARN, session.timestamp > now + 5_s, "Session timestamp {} > now {}", session.timestamp, now);
          continue;
        }
        XLOGF(WARN, "SessionManager found dead session {}", session);
      }
      deadSessions.push_back(session);
    }
    prev = sessions->back();

    // prune dead sessions
    if (manager.config_.sync_on_prune_session()) {
      for (auto &session : deadSessions) {
        co_await manager.closeWorkers_->enqueue(std::make_unique<CloseTask>(session));
      }
    } else {
      auto txn = manager.kvEngine_->createReadWriteTransaction();
      auto result =
          co_await kv::WithTransaction(kv::FDBRetryStrategy{}).run(std::move(txn), [&](auto &txn) -> CoTryTask<Void> {
            for (auto &session : deadSessions) {
              CO_RETURN_ON_ERROR(co_await session.remove(txn));
            }
            co_return Void{};
          });
      if (result.hasError()) {
        pruneFailed.addSample(deadSessions.size());
        XLOGF(ERR, "ScanTask-{} prune failed, error {}", shard_, result.error());
        CO_RETURN_ERROR(result);
      }
      total += deadSessions.size();
      pruned.addSample(deadSessions.size());
    }
  }

  auto finished = prune_->finished.fetch_add(1) + 1;
  if (finished < FileSession::kShard) {
    co_return total;
  }

  while (!prune_->sessions.rlock()->empty()) {
    static constexpr size_t kBatch = 64;
    std::vector<FileSession> batch;
    batch.reserve(kBatch);
    auto wlock = prune_->sessions.wlock();
    auto iter = wlock->begin();
    while (iter != wlock->end() && batch.size() < kBatch) {
      XLOGF(INFO, "Need prune session {}", iter->second);
      batch.push_back(iter->second);
      iter = wlock->erase(iter);
    }
    wlock.unlock();
    auto txn = manager.kvEngine_->createReadWriteTransaction();
    auto prune = co_await kv::WithTransaction(kv::FDBRetryStrategy{})
                     .run(std::move(txn), [&](IReadWriteTransaction &txn) -> CoTryTask<Void> {
                       for (auto &session : batch) {
                         CO_RETURN_ON_ERROR(co_await session.remove(txn));
                       }
                       co_return Void{};
                     });
    if (prune.hasError()) {
      pruneFailed.addSample(batch.size());
      XLOGF(WARN, "Prune session failed, error {}", prune.error());
    }
  }

  co_return total;
}

/** SessionManager::CloseTask */
CoTryTask<void> SessionManager::CloseTask::run(SessionManager &manager) {
  XLOGF_IF(FATAL, !manager.close_, "close_ not set");
  auto req = CloseReq({}, session_.inodeId, SessionInfo(session_.clientId, session_.sessionId), true, {}, {});
  req.client = session_.clientId;
  req.pruneSession = true;
  auto close = co_await manager.close_(req);
  if (!close.hasError()) {
    pruned.addSample(1);
    co_return Void{};
  }

  if (close.error().code() != MetaCode::kNotFound) {
    XLOGF(ERR, "SessionManager failed to close {}, error {}", req.inode, close.error());
    pruneFailed.addSample(1);
  } else {
    pruned.addSample(1);
  }

  auto txn = manager.kvEngine_->createReadWriteTransaction();
  auto prune = co_await kv::WithTransaction(kv::FDBRetryStrategy{})
                   .run(std::move(txn), folly::partial(&FileSession::remove, &session_));
  if (prune.hasError()) {
    pruneFailed.addSample(1);
    XLOGF(WARN, "Prune session {} failed, error {}", session_, prune.error());
  }
  co_return prune;
}

/** SessionManager */

void SessionManager::start(CPUExecutorGroup &exec) {
  XLOGF(DBG, "SessionManager start");

  closeWorkers_ = std::make_unique<CoroutinesPool<std::unique_ptr<CloseTask>>>(config_.close_workers());
  closeWorkers_->start([&](auto task) -> CoTask<void> { co_await task->run(*this); }, exec);

  scanWorkers_ = std::make_unique<CoroutinesPool<std::unique_ptr<ScanTask>>>(config_.scan_workers());
  scanWorkers_->start([&](auto task) -> CoTask<void> { co_await task->run(*this); }, exec);

  scanRunner_ = std::make_unique<BackgroundRunner>(&exec.pickNext());
  scanRunner_->start("SessionScan", folly::partial(&SessionManager::scanTask, this), config_.scan_interval_getter());

  XLOGF(INFO, "SessionManager started!");
}

void SessionManager::stopAndJoin() {
  XLOGF(DBG, "SessionManager stop.");
  if (scanRunner_) {
    folly::coro::blockingWait(scanRunner_->stopAll());
    scanRunner_.reset();
  }
  if (scanWorkers_) {
    scanWorkers_->stopAndJoin();
    scanWorkers_.reset();
  }
  if (closeWorkers_) {
    closeWorkers_->stopAndJoin();
    closeWorkers_.reset();
  }

  XLOGF(INFO, "SessionManager stopped.");
}

CoTask<void> SessionManager::scanTask() {
  if (!config_.enable()) {
    XLOGF_EVERY_MS(INFO, 10000, "SessionManager scan disabled");
    co_return;
  }
  if (!isFirstMeta(*mgmtd_, nodeId_)) {
    co_return;
  }
  XLOGF(INFO, "MetaServer {} is first active meta, scan sessions", nodeId_);

  auto prune = co_await loadPrune();
  if (!prune) {
    XLOGF(ERR, "Failed to load sessions need to be pruned");
    co_return;
  }
  for (size_t shard = 0; shard < FileSession::kShard; shard++) {
    co_await scanWorkers_->enqueue(std::make_unique<ScanTask>(shard, *prune));
  }

  co_return;
}

CoTryTask<std::shared_ptr<SessionManager::PruneSessions>> SessionManager::loadPrune() {
  auto result = co_await kv::WithTransaction(kv::FDBRetryStrategy{})
                    .run(kvEngine_->createReadonlyTransaction(), [&](auto &txn) -> CoTryTask<std::vector<FileSession>> {
                      co_return co_await FileSession::listPrune(txn, 128 << 10 /* at most 128k sessions to prune */);
                    });
  CO_RETURN_ON_ERROR(result);
  XLOGF_IF(INFO, !result->empty(), "SessionManager found {} sessions to prune", result->size());

  auto prune = std::make_shared<PruneSessions>();
  auto guard = prune->sessions.wlock();
  for (auto &session : *result) {
    guard->emplace(session.sessionId, session);
  }
  co_return prune;
}

CoTryTask<std::vector<FileSession>> SessionManager::listSessions() {
  // todo: should we add this?
  std::vector<FileSession> sessions;
  for (size_t shard = 0; shard < FileSession::kShard; shard++) {
    std::optional<FileSession> prev;
    while (true) {
      auto result =
          co_await kv::WithTransaction(kv::FDBRetryStrategy{})
              .run(kvEngine_->createReadonlyTransaction(), [&](auto &txn) -> CoTryTask<std::vector<FileSession>> {
                co_return co_await FileSession::scan(txn, shard, prev);
              });
      CO_RETURN_ON_ERROR(result);
      if (result->empty()) {
        break;
      }
      prev = result->back();
      sessions.insert(sessions.end(), result->begin(), result->end());
    }
  }
  co_return sessions;
}

CoTryTask<std::vector<FileSession>> SessionManager::listSessions(InodeId inodeId) {
  auto txn = kvEngine_->createReadonlyTransaction();
  auto handler = [&](IReadOnlyTransaction &txn) { return FileSession::list(txn, inodeId, true); };
  co_return co_await kv::WithTransaction<kv::FDBRetryStrategy>({}).run(std::move(txn), handler);
}

CoTryTask<size_t> SessionManager::pruneManually() {
  XLOGF(INFO, "SessionManager pruneManually");

  auto prune = co_await loadPrune();
  CO_RETURN_ON_ERROR(prune);

  size_t total = 0;
  for (size_t shard = 0; shard < FileSession::kShard; shard++) {
    auto task = ScanTask(shard, *prune);
    auto result = co_await task.run(*this);
    CO_RETURN_ON_ERROR(result);
    total += *result;
  }
  co_return total;
}

}  // namespace hf3fs::meta::server
