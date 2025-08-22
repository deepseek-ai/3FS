#include "MgmtdHeartbeatChecker.h"

#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
flat::NodeInfo onNodeFailed(const flat::NodeInfo &oldNodeInfo, UtcTime) {
  auto sn = oldNodeInfo;
  sn.status = flat::NodeStatus::HEARTBEAT_FAILED;
  return sn;
}

struct Op : core::ServiceOperationWithMetric<"MgmtdService", "CheckHeartbeat", "bg"> {
  String toStringImpl() const final { return "CheckHeartbeat"; }

  auto handle(MgmtdState &state) -> CoTryTask<void> {
    auto heartbeatFailInterval = state.config_.heartbeat_fail_interval().asUs();
    auto start = state.utcNow();

    auto writerLock = co_await state.coScopedLock<"CheckHeartbeat">();

    auto steadyNow = SteadyClock::now();

    std::vector<flat::NodeId> timeoutedNodeIds;
    std::vector<flat::NodeInfo> timeoutedNodes;
    std::vector<flat::TargetId> candidateTargetIds;

    {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &ri = dataPtr->routingInfo;
      for (const auto &[nodeId, nodeInfo] : ri.nodeMap) {
        switch (nodeInfo.base().status) {
          case flat::NodeStatus::DISABLED:
          case flat::NodeStatus::HEARTBEAT_FAILED:
          case flat::NodeStatus::PRIMARY_MGMTD:
            break;  // do nothing
          default:
            if (nodeInfo.ts() + heartbeatFailInterval < steadyNow) {
              timeoutedNodeIds.push_back(nodeId);
              timeoutedNodes.push_back(onNodeFailed(nodeInfo.base(), start));
            }
        }
      }

      for (const auto &[tid, ti] : ri.getTargets()) {
        if (ti.base().localState != flat::LocalTargetState::OFFLINE && ti.ts() + heartbeatFailInterval < steadyNow) {
          candidateTargetIds.push_back(tid);
        }
      }

      if (timeoutedNodes.empty() && candidateTargetIds.empty()) co_return Void{};

      LOG_OP_INFO(*this,
                  "found timeouted nodes:[{}] targets:[{}]",
                  fmt::join(timeoutedNodeIds, ","),
                  fmt::join(candidateTargetIds, ","));
    }

    if (!candidateTargetIds.empty() || !timeoutedNodes.empty()) {
      auto dataPtr = co_await state.data_.coLock();
      auto &ri = dataPtr->routingInfo;
      auto steadyNow = SteadyClock::now();
      for (auto tid : candidateTargetIds) {
        ri.updateTarget(tid, [steadyNow](auto &ti) {
          ti.base().localState = flat::LocalTargetState::OFFLINE;
          ti.updateTs(steadyNow);
        });
      }

      for (auto &info : timeoutedNodes) ri.nodeMap[info.app.nodeId].base() = std::move(info);
      ri.routingInfoChanged = true;
    }

    co_return Void{};
  }
};
}  // namespace

MgmtdHeartbeatChecker::MgmtdHeartbeatChecker(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdHeartbeatChecker::check() {
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
