#include "MgmtdMetricsUpdater.h"

#include "common/utils/StringUtils.h"
#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
template <NameWrapper MetricName, typename Map>
void recordCount(const Map &counts, auto &&instanceF, auto &&tagF) {
  using Key = typename Map::key_type;
  static std::map<Key, monitor::ValueRecorder> recorders;
  auto func = [&](auto &&f, const Key &k) {
    if constexpr (is_specialization<Key, std::tuple>) {
      return std::apply(f, k);
    } else {
      return f(k);
    }
  };
  for (const auto &[k, v] : counts) {
    auto it = recorders.find(k);
    if (it == recorders.end()) {
      auto instance = func(std::forward<decltype(instanceF)>(instanceF), k);
      auto tag = func(std::forward<decltype(tagF)>(tagF), k);
      std::optional<monitor::TagSet> tagSet;
      if (!instance.empty() || !tag.empty()) {
        tagSet = monitor::TagSet();
        if (!instance.empty()) tagSet->addTag("instance", instance);
        if (!tag.empty()) tagSet->addTag("tag", tag);
      }
      it = recorders.try_emplace(k, String(MetricName), std::move(tagSet)).first;
    }
    it->second.set(v);
  }
}

auto emptyString = [](auto &&...) -> String { return ""; };

struct Op : core::ServiceOperationWithMetric<"MgmtdService", "UpdateMetrics", "bg"> {
  String toStringImpl() const final { return "UpdateMetrics"; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    auto dataPtr = co_await state.data_.coSharedLock();
    auto clientSessionMap = co_await state.clientSessionMap_.coSharedLock();

    auto bootstrapping = dataPtr->leaseStartTs + state.config_.bootstrapping_length() > SteadyClock::now();

    reportNodeStatus(*dataPtr);
    reportConfigStatus(*dataPtr, *clientSessionMap);
    reportChainByReplicaCountStatus(*dataPtr);
    reportTargetCount(*dataPtr, bootstrapping);
    reportRoutingInfoVersion(*dataPtr);
    reportConfigVersions(*dataPtr);
    reportLeaseLasting(*dataPtr);
    reportReleaseVersionStatus(*dataPtr, *clientSessionMap);
    reportChainStatus(*dataPtr);
    reportOrphanTargets(*dataPtr);
    reportChainTargetCount(*dataPtr);

    co_return Void{};
  }

  void reportNodeStatus(const MgmtdData &data) {
    std::map<std::tuple<flat::NodeType, flat::NodeStatus>, int64_t> counts;
    std::map<std::tuple<flat::NodeType, flat::NodeId>, int64_t> nodeStatuses;
    for (const auto &[_, ni] : data.routingInfo.nodeMap) {
      auto &base = ni.base();
      ++counts[std::make_tuple(base.type, base.status)];
      auto key = std::make_tuple(base.type, base.app.nodeId);
      nodeStatuses[key] = static_cast<int64_t>(base.status) + 1;  // because PRIMARY_MGMTD = 0
    }
    recordCount<"MgmtdService.NodeStatusCount">(
        counts,
        [](auto type, auto) { return hf3fs::toString(type); },
        [](auto, auto status) { return hf3fs::toString(status); });

    recordCount<"MgmtdService.NodeStatus">(
        nodeStatuses,
        [](auto type, auto id) { return fmt::format("{}_{}", toStringView(type), id.toUnderType()); },
        emptyString);
  }

  void reportConfigStatus(const MgmtdData &data, const ClientSessionMap &clientSessionMap) {
    std::map<std::tuple<flat::NodeType, ConfigStatus>, int64_t> counts;
    auto add = [&](auto type, auto version, auto status) {
      if (version == 0) {
        status = ConfigStatus::UNKNOWN;
      } else {
        auto latestVersion = data.getLatestConfigVersion(type);
        assert(version <= latestVersion);
        if (version != latestVersion) {
          status = ConfigStatus::STALE;
        }
      }
      ++counts[std::make_tuple(type, status)];
    };
    for (const auto &[_, ni] : data.routingInfo.nodeMap) {
      auto &base = ni.base();
      add(base.type, base.configVersion, base.configStatus);
    }
    for (const auto &[_, cs] : clientSessionMap) {
      auto &base = cs.base();
      add(base.type, base.configVersion, base.configStatus);
    }
    recordCount<"MgmtdService.ConfigStatusCount">(
        counts,
        [](auto type, auto) { return hf3fs::toString(type); },
        [](auto, auto status) { return hf3fs::toString(status); });
  }

  void reportChainByReplicaCountStatus(const MgmtdData &data) {
    std::map<size_t, int64_t> counts;
    for (const auto &[_, ci] : data.routingInfo.chains) {
      auto rc = ci.targets.size();
      ++counts[rc];
    }
    recordCount<"MgmtdService.ChainCountByReplica">(
        counts,
        [](auto count) { return std::to_string(count); },
        emptyString);
  }

  void reportChainTargetCount(const MgmtdData &data) {
    static monitor::ValueRecorder chainCount("MgmtdService.ChainCount");
    static monitor::ValueRecorder targetCount("MgmtdService.TargetCount");

    chainCount.set(data.routingInfo.chains.size());
    targetCount.set(data.routingInfo.getTargets().size());
  }

  void reportTargetCount(const MgmtdData &data, bool bootstrapping) {
    using PS = flat::PublicTargetState;
    using LS = flat::LocalTargetState;
    std::map<std::tuple<PS, LS>, int64_t> counts;
    std::map<std::tuple<flat::ChainId, flat::TargetId>, int64_t> abnormalChains;

    for (const auto &[cid, ci] : data.routingInfo.chains) {
      bool abnormal = false;
      for (const auto &[tid, _] : ci.targets) {
        const auto &ti = data.routingInfo.getTargets().at(tid);
        auto ps = ti.base().publicState;
        auto ls = ti.base().localState;
        ++counts[std::make_tuple(ps, ls)];
        abnormal |= (!bootstrapping && ps != PS::SERVING);
      }
      if (abnormal) {
        for (const auto &cti : ci.targets) {
          abnormalChains[std::make_tuple(cid, cti.targetId)] = static_cast<int64_t>(cti.publicState);
        }
      }
    }
    recordCount<"MgmtdService.TargetStatusCount">(
        counts,
        [](auto ps, auto) { return hf3fs::toString(ps); },
        [](auto, auto ls) { return hf3fs::toString(ls); });

    recordCount<"MgmtdService.AbnormalChainStatus">(
        abnormalChains,
        [](auto cid, auto) { return std::to_string(cid); },
        [](auto, auto tid) { return std::to_string(tid); });
  }

  void reportRoutingInfoVersion(const MgmtdData &data) {
    static monitor::ValueRecorder recorder("MgmtdService.RoutingInfoVersion");
    recorder.set(data.routingInfo.routingInfoVersion);
  }

  void reportConfigVersions(const MgmtdData &data) {
    std::map<flat::NodeType, int64_t> versions;
    for (const auto &[k, vm] : data.configMap) {
      auto ver = vm.rbegin()->first;
      versions[k] = ver;
    }
    recordCount<"MgmtdService.ConfigVersions">(
        versions,
        [](auto type) { return hf3fs::toString(type); },
        emptyString);
  }

  void reportLeaseLasting(const MgmtdData &data) {
    static monitor::ValueRecorder recorder("MgmtdService.LeaseLasting");
    auto lasting = Duration(SteadyClock::now() - data.leaseStartTs);
    recorder.set(lasting.count());
  }

  void reportReleaseVersionStatus(const MgmtdData &data, const ClientSessionMap &clientSessionMap) {
    constexpr auto invalidType = static_cast<flat::NodeType>(255U);
    std::map<std::tuple<flat::NodeType, flat::ReleaseVersion>, int64_t> counts;
    auto activeNode = flat::selectActiveNode();
    for (const auto &[_, ni] : data.routingInfo.nodeMap) {
      if (!activeNode(ni.base())) continue;
      ++counts[std::make_tuple(ni.base().type, ni.base().app.releaseVersion)];
      ++counts[std::make_tuple(invalidType, ni.base().app.releaseVersion)];
    }
    for (const auto &[_, cs] : clientSessionMap) {
      ++counts[std::make_tuple(cs.base().type, cs.base().releaseVersion)];
    }
    recordCount<"MgmtdService.ReleaseVersionCount">(
        counts,
        [](auto type, auto) { return type == invalidType ? "SERVER" : hf3fs::toString(type); },
        [](auto, auto rv) { return fmt::format("{}", rv); });
  }

  void reportChainStatus(const MgmtdData &data) {
    using PS = flat::PublicTargetState;
    std::map<std::tuple<size_t, String>, int64_t> counts;
    for (const auto &[_, ci] : data.routingInfo.chains) {
      auto replicaCount = ci.targets.size();
      size_t serving = 0, syncing = 0;
      for (const auto &ti : ci.targets) {
        switch (ti.publicState) {
          case PS::SERVING:
            ++serving;
            break;
          case PS::SYNCING:
            ++syncing;
            break;
          default:
            break;
        }
      }
      String status = [&] {
        if (serving == replicaCount)
          return "SERVING-FULL";
        else if (serving)
          return "SERVING-PARTIAL";
        else if (syncing)
          return "SYNCING";
        else
          return "UNAVAILABLE";
      }();
      ++counts[std::make_tuple(replicaCount, status)];
    }
    recordCount<"MgmtdService.ChainStatus">(
        counts,
        [](auto replicaCount, auto) { return std::to_string(replicaCount); },
        [](auto, auto status) { return status; });
  }

  void reportOrphanTargets(const MgmtdData &data) {
    static constexpr auto invalidNodeId = flat::NodeId(0);
    std::map<flat::NodeId, int64_t> counts;
    std::map<flat::NodeId, int64_t> usedSizes;
    for (const auto &[tid, ti] : data.routingInfo.orphanTargetsByTargetId) {
      XLOGF_IF(FATAL, !ti.nodeId, "Orphan targets should always have nodeId. ti: {}", serde::toJsonString(ti));
      auto nodeId = *ti.nodeId;
      ++counts[nodeId];
      ++counts[invalidNodeId];

      usedSizes[nodeId] += ti.usedSize;
      usedSizes[invalidNodeId] += ti.usedSize;
    }
    recordCount<"MgmtdService.OrphanTargetCount">(
        counts,
        [](auto nodeId) { return nodeId == invalidNodeId ? "" : std::to_string(nodeId); },
        emptyString);
  }
};
}  // namespace

MgmtdMetricsUpdater::MgmtdMetricsUpdater(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdMetricsUpdater::update() {
  Op op;
  auto handler = [&]() -> CoTryTask<void> { CO_INVOKE_OP_INFO(op, "background", state_); };

  auto res = co_await doAsPrimary(state_, std::move(handler));
  if (res.hasError()) {
    if (res.error().code() == MgmtdCode::kNotPrimary)
      LOG_OP_INFO(op, "self is not primary, skip");
    else
      LOG_OP_ERR(op, "failed: {}", res.error());
  }
}
}  // namespace hf3fs::mgmtd
