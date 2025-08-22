#include "Distributor.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstring>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/app/NodeId.h"
#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/kv/WithTransaction.h"
#include "common/monitor/Recorder.h"
#include "common/serde/Serde.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/Coroutine.h"
#include "common/utils/MurmurHash3.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Utils.h"
#include "fdb/FDBRetryStrategy.h"
#include "fdb/FDBTransaction.h"
#include "fmt/core.h"
#include "fmt/format.h"

#define FMT_KEY(key) fmt::join((const uint8_t *)(key).data(), (const uint8_t *)(key).data() + (key).size(), ",")

namespace hf3fs::meta::server {

namespace {
monitor::CountRecorder setMapCounter("meta_server.dist_set_map");
}  // namespace

std::string Distributor::PerServerKey::pack(flat::NodeId nodeId) {
  return fmt::format("{}-{:08d}", kPrefix, nodeId.toUnderType());
}

flat::NodeId Distributor::PerServerKey::unpack(std::string_view key) {
  uint32_t nodeId;
  auto fmt = fmt::format("{}-{{}}", kPrefix);
  auto ret = scn::scan(key, fmt, nodeId);
  if (!ret) {
    return flat::NodeId(0);
  } else {
    return flat::NodeId(nodeId);
  }
}

void Distributor::start(CPUExecutorGroup &exec) {
  auto result = folly::coro::blockingWait(update(false));
  XLOGF_IF(ERR, result.hasError(), "failed to update server map on start, error {}", result.error());

  bgRunner_ = std::make_unique<BackgroundRunner>(&exec.randomPick());
  bgRunner_->start(
      fmt::format("distributor_update@{}", nodeId_),
      [this]() -> CoTask<void> {
        auto result = co_await update(false);
        XLOGF_IF(CRITICAL, result.hasError(), "Distributor update failed, {}", result.error());
      },
      [&]() { return config_.update_interval() * folly::Random::randDouble(0.8, 1.2); });
}

void Distributor::stopAndJoin(bool updateMap) {
  XLOGF(INFO, "{} stop, update map {}", nodeId_, updateMap);
  if (bgRunner_) {
    folly::coro::blockingWait(bgRunner_->stopAll());
    bgRunner_.reset();
  }
  if (updateMap) {
    XLOGF(INFO, "{} update map on stop", nodeId_);
    auto result = folly::coro::blockingWait(update(true));
    XLOGF_IF(ERR, result.hasError(), "failed to update server map on stop, error {}", result.error());
  }
  XLOGF(INFO, "{} stopped", nodeId_);
}

flat::NodeId Distributor::getServer(InodeId inodeId) {
  auto guard = latest_.rlock();
  return Weight::select(guard->active, inodeId);
}

CoTryTask<std::pair<bool, kv::Versionstamp>> Distributor::checkOnServer(kv::IReadWriteTransaction &txn,
                                                                        InodeId inodeId) {
  co_return co_await checkOnServer(txn, inodeId, nodeId_);
}

CoTryTask<std::pair<bool, kv::Versionstamp>> Distributor::checkOnServer(kv::IReadWriteTransaction &txn,
                                                                        InodeId inodeId,
                                                                        flat::NodeId nodeId) {
  auto versionstamp = co_await loadVersion(txn);
  CO_RETURN_ON_ERROR(versionstamp);

  auto rlock = latest_.rlock();
  if (*versionstamp > rlock->versionstamp) {
    rlock.unlock();
    CO_RETURN_ON_ERROR(co_await loadServerMap(txn, false));
    rlock = latest_.rlock();
  }

  XLOGF_IF(FATAL, *versionstamp > rlock->versionstamp, "{} > {}", FMT_KEY(*versionstamp), FMT_KEY(rlock->versionstamp));
  if (*versionstamp < rlock->versionstamp) {
    XLOGF(WARN, "version {} < {}, need retry", FMT_KEY(*versionstamp), FMT_KEY(rlock->versionstamp));
    co_return makeError(TransactionCode::kTooOld, "distributor versionstamp changed");
  }

  auto server = Weight::select(rlock->active, inodeId);
  co_return std::pair(server == nodeId, *versionstamp);
}

CoTryTask<kv::Versionstamp> Distributor::loadVersion(kv::IReadOnlyTransaction &txn) {
  co_return (co_await txn.get(kv::kMetadataVersionKey)).then([](auto &value) {
    if (!value.has_value()) {
      return kv::Versionstamp{0};
    }

    auto version = kv::Versionstamp{0};
    XLOGF_IF(FATAL,
             value->size() != version.size(),
             "kMetadataVersionKey -> value {}, size not match",
             FMT_KEY(*value));
    memcpy(version.data(), value->data(), version.size());
    return version;
  });
}

CoTryTask<void> Distributor::updateVersion(kv::IReadWriteTransaction &txn) {
  std::array<char, sizeof(kv::Versionstamp)> buf{0};
  co_return co_await txn.setVersionstampedValue(kv::kMetadataVersionKey, {buf.data(), buf.size()}, 0);
}

CoTryTask<Distributor::LatestServerMap> Distributor::loadServerMap(kv::IReadOnlyTransaction &txn, bool update) {
  auto versionstamp = co_await loadVersion(txn);
  CO_RETURN_ON_ERROR(versionstamp);

  auto load = co_await txn.get(kMapKey);
  CO_RETURN_ON_ERROR(load);
  XLOGF_IF(DFATAL,
           (!load->has_value() && updated_ != 0),
           "{} distributor server map not found, shouldn't happen",
           nodeId_);

  ServerMap map;
  if (load->has_value()) {
    auto des = serde::deserialize(map, **load);
    if (des.hasError()) {
      XLOGF(DFATAL, "Failed to deserializa server map, {}", des.error());
      co_return makeError(MetaCode::kInconsistent, "Invalid distributor server map");
    }
  } else {
    XLOGF(INFO, "server map not found");
  }

  if (*versionstamp <= latest_.rlock()->versionstamp) {
    co_return LatestServerMap{map, *versionstamp};
  }

  {
    auto wlock = latest_.wlock();
    if (*versionstamp > wlock->versionstamp) {
      XLOGF(INFO,
            "{} get new server map: {}, versionstamp: {}, update {}",
            nodeId_,
            map,
            FMT_KEY(*versionstamp),
            update);
      *wlock = {map, *versionstamp};
    }
  }

  co_return LatestServerMap{map, *versionstamp};
}

CoTryTask<void> Distributor::updateServerMap(kv::IReadWriteTransaction &txn, const Distributor::ServerMap &map) {
  XLOGF(INFO, "{} try set new server map: {}", nodeId_, map);
  setMapCounter.addSample(1);
  auto key = kMapKey;
  auto value = serde::serialize(map);
  CO_RETURN_ON_ERROR(co_await txn.set(key, value));
  CO_RETURN_ON_ERROR(co_await updateVersion(txn));

  co_return Void{};
}

CoTryTask<void> Distributor::update(bool exit) {
  for (size_t i = 0; i < 10; i++) {
    auto strategy = kv::FDBRetryStrategy();
    auto result = co_await kv::WithTransaction<kv::FDBRetryStrategy>(strategy).run(
        kvEngine_->createReadWriteTransaction(),
        [&](kv::IReadWriteTransaction &txn) -> CoTryTask<bool> { co_return co_await update(txn, exit); });
    XLOGF_IF(ERR, result.hasError(), "{} update failed, error {}", nodeId_, result.error());
    CO_RETURN_ON_ERROR(result);

    // if update generate a new map, we need update again to load it.
    auto newMap = *result;
    XLOGF(INFO, "{} updated map, new {}, exit {}", nodeId_, newMap, exit);
    if (!newMap || exit) {
      co_return Void{};
    }

    updated_.fetch_add(1);
  }

  XLOGF(CRITICAL, "{} update not finished after too many times", nodeId_);
  co_return makeError(MetaCode::kBusy, "update not finished after too many times");
}

CoTryTask<bool> Distributor::update(kv::IReadWriteTransaction &txn, bool exit) {
  XLOGF(INFO, "{} update, exit {}", nodeId_, exit);
  auto current = co_await loadServerMap(txn, true);
  CO_RETURN_ON_ERROR(current);
  auto startCheck = SteadyClock::now();

  {
    auto rlock = latest_.rlock();
    XLOGF_IF(FATAL,
             current->versionstamp > rlock->versionstamp,
             "{} > {}",
             FMT_KEY(current->versionstamp),
             FMT_KEY(rlock->versionstamp));
    if (current->versionstamp < rlock->versionstamp) {
      XLOGF(WARN, "version {} < {}, need retry", FMT_KEY(current->versionstamp), FMT_KEY(rlock->versionstamp));
      co_return makeError(TransactionCode::kTooOld, "distributor versionstamp changed");
    } else {
      XLOGF_IF(DFATAL,
               current->active != rlock->active,
               "versionstamp {}, {} != {}",
               FMT_KEY(current->versionstamp),
               fmt::join(current->active.begin(), current->active.end(), ","),
               fmt::join(rlock->active.begin(), rlock->active.end(), ","));
    }
  }

  auto opts = kv::TransactionHelper::ListByPrefixOptions().withSnapshot(true).withInclusive(false).withLimit(0);
  auto result = co_await kv::TransactionHelper::listByPrefix(txn, fmt::format("{}-", kPrefix), opts);
  CO_RETURN_ON_ERROR(result);

  std::set<flat::NodeId> dead;
  servers_.withWLock([&](auto &servers) {
    bool self = false;
    for (auto &[key, versionstamp] : *result) {
      auto nodeId = PerServerKey::unpack(key);
      if (!nodeId) {
        XLOGF(DFATAL, "Failed to unpack key {}", key);
        continue;
      } else if (nodeId == nodeId_) {
        self = true;
        continue;
      }
      if (!servers.contains(nodeId) || servers[nodeId].versionstamp != versionstamp) {
        XLOGF(INFO,
              "{} found {} alive, prev {}, curr {}",
              nodeId_,
              nodeId,
              FMT_KEY(servers[nodeId].versionstamp),
              FMT_KEY(versionstamp));
        servers[nodeId] = {versionstamp, SteadyClock::now()};
      }
    }
    XLOGF_IF(DFATAL, (updated_ != 0 && !self), "self {} not found!!!", nodeId_);

    auto timeout = config_.timeout();
    for (auto nodeId : current->active) {
      auto state = servers[nodeId];
      if (nodeId != nodeId_ && state.lastUpdate + timeout < startCheck) {
        XLOGF(CRITICAL, "{} mark {} as dead, not update in {}", nodeId_, nodeId, timeout);
        dead.emplace(nodeId);
      }
    }
  });

  auto key = PerServerKey::pack(nodeId_);
  std::array<char, sizeof(kv::Versionstamp)> buf{0};
  CO_RETURN_ON_ERROR(co_await txn.setVersionstampedValue(key, {buf.data(), buf.size()}, 0));

  bool update = false;
  if (!exit && std::find(current->active.begin(), current->active.end(), nodeId_) == current->active.end()) {
    XLOGF(INFO, "{} not in server map, create a new map", nodeId_);
    update = true;
  }
  if (!dead.empty()) {
    XLOGF(INFO, "{} found dead servers {}, create a new map", nodeId_, fmt::join(dead.begin(), dead.end(), ","));
    update = true;
  }
  if (exit) {
    XLOGF(INFO, "{} exiting, create a new map", nodeId_);
    dead.insert(nodeId_);
    update = true;
  }

  if (!update) {
    co_return false;
  }

  std::set<flat::NodeId> active;
  if (current) {
    for (auto node : current->active) {
      if (!dead.contains(node)) {
        active.insert(node);
      }
    }
  }
  if (!exit) {
    active.insert(nodeId_);
  }
  ServerMap map{std::vector<flat::NodeId>(active.begin(), active.end())};
  CO_RETURN_ON_ERROR(co_await updateServerMap(txn, map));
  co_return true;
}

}  // namespace hf3fs::meta::server