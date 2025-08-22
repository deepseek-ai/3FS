#pragma once

#include <atomic>
#include <folly/Synchronized.h>
#include <folly/logging/xlog.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <regex.h>
#include <string>
#include <vector>

#include "common/app/NodeId.h"
#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/serde/Serde.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Common.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {

class Distributor {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(update_interval, 1_s);
    CONFIG_HOT_UPDATED_ITEM(timeout, 30_s);
  };

  Distributor(const Config &config, flat::NodeId nodeId, std::shared_ptr<kv::IKVEngine> kvEngine)
      : config_(config),
        nodeId_(nodeId),
        kvEngine_(kvEngine) {
    XLOGF_IF(FATAL, !nodeId_, "invalid node id {}", nodeId_);
  }

  ~Distributor() { stopAndJoin(); }

  flat::NodeId nodeId() const { return nodeId_; }

  void start(CPUExecutorGroup &exec);
  void stopAndJoin(bool updateMap = true);

  flat::NodeId getServer(InodeId inodeId);

  CoTryTask<std::pair<bool, kv::Versionstamp>> checkOnServer(kv::IReadWriteTransaction &txn, InodeId inodeId);
  CoTryTask<std::pair<bool, kv::Versionstamp>> checkOnServer(kv::IReadWriteTransaction &txn,
                                                             InodeId inodeId,
                                                             flat::NodeId nodeId);

 private:
  static constexpr auto kPrefix = kv::toStr(kv::KeyPrefix::MetaDistributor);
  static constexpr auto kMapKey = kPrefix;

  struct ServerMap {
    SERDE_STRUCT_FIELD(active, std::vector<flat::NodeId>());
  };

  struct LatestServerMap : ServerMap {
    kv::Versionstamp versionstamp{0};
  };

  struct ServerStatus {
    std::string versionstamp;
    SteadyTime lastUpdate;
  };

  struct PerServerKey {
    static std::string pack(flat::NodeId nodeId);
    static flat::NodeId unpack(std::string_view key);
  };

  CoTryTask<kv::Versionstamp> loadVersion(kv::IReadOnlyTransaction &txn);
  CoTryTask<LatestServerMap> loadServerMap(kv::IReadOnlyTransaction &txn, bool update);

  CoTryTask<void> updateVersion(kv::IReadWriteTransaction &txn);
  CoTryTask<void> updateServerMap(kv::IReadWriteTransaction &txn, const ServerMap &map);

  CoTryTask<void> update(bool exit);
  CoTryTask<bool> update(kv::IReadWriteTransaction &txn, bool exit);

  const Config config_;
  flat::NodeId nodeId_;
  std::atomic<size_t> updated_{0};
  std::shared_ptr<kv::IKVEngine> kvEngine_;
  std::unique_ptr<BackgroundRunner> bgRunner_;

  folly::Synchronized<LatestServerMap> latest_;
  folly::Synchronized<std::map<flat::NodeId, ServerStatus>> servers_;
};

}  // namespace hf3fs::meta::server