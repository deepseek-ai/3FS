#include "StorageClientImpl.h"

#include <boost/core/ignore_unused.hpp>
#include <folly/Random.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/symbolizer/Symbolizer.h>
#include <memory>
#include <random>

#include "TargetSelection.h"
#include "common/logging/LogHelper.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/ScopedMetricsWriter.h"
#include "common/utils/ExponentialBackoffRetry.h"
#include "common/utils/RequestInfo.h"
#include "common/utils/Result.h"
#include "common/utils/SemaphoreGuard.h"
#include "common/utils/StatusCodeConversion.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::storage::client {

/* Performance metrics */

static std::unordered_map<StorageClient::MethodType, std::atomic_int64_t> atomic_concurrent_user_calls;
static std::unordered_map<StorageClient::MethodType, std::atomic_int64_t> atomic_num_pending_ops;
static std::unordered_map<StorageClient::MethodType, std::atomic_int64_t> atomic_inflight_requests;

static monitor::DistributionRecorder concurrent_user_calls{"storage_client.concurrent_user_calls"};
static monitor::DistributionRecorder num_pending_ops{"storage_client.num_pending_ops"};
static monitor::DistributionRecorder inflight_requests{"storage_client.inflight_requests"};
static monitor::DistributionRecorder accessed_servers_per_user_call{"storage_client.accessed_servers_per_user_call"};
static monitor::DistributionRecorder requests_per_user_call{"storage_client.requests_per_user_call"};
static monitor::DistributionRecorder requests_per_server{"storage_client.requests_per_server"};
static monitor::DistributionRecorder bytes_per_operation{"storage_client.bytes_per_operation"};
static monitor::DistributionRecorder bytes_per_request{"storage_client.bytes_per_request"};
static monitor::DistributionRecorder bytes_per_user_call{"storage_client.bytes_per_user_call"};
static monitor::DistributionRecorder ops_per_request{"storage_client.ops_per_request"};
static monitor::DistributionRecorder ops_per_user_call{"storage_client.ops_per_user_call"};
static monitor::DistributionRecorder user_call_bw{"storage_client.user_call_bw"};
static monitor::DistributionRecorder request_bw{"storage_client.request_bw"};

static monitor::LatencyRecorder overall_latency{"storage_client.overall_latency"};
static monitor::LatencyRecorder waiting_time{"storage_client.waiting_time"};
static monitor::LatencyRecorder request_latency{"storage_client.request_latency"};
static monitor::LatencyRecorder inflight_time{"storage_client.inflight_time"};
static monitor::LatencyRecorder server_latency{"storage_client.server_latency"};
static monitor::LatencyRecorder network_latency{"storage_client.network_latency"};

static monitor::CountRecorder num_processed_chunks{"storage_client.num_processed_chunks", true /*reset*/};
static monitor::CountRecorder num_completed_ops{"storage_client.num_completed_ops", true /*reset*/};
static monitor::CountRecorder num_failed_ops{"storage_client.num_failed_ops", true /*reset*/};
static monitor::CountRecorder num_retried_ops{"storage_client.num_retried_ops", true /*reset*/};
static monitor::CountRecorder num_error_codes{"storage_client.num_error_codes", true /*reset*/};
static monitor::CountRecorder data_payload_bytes{"storage_client.data_payload_bytes", true /*reset*/};

static monitor::CountRecorder num_processed_chunks_per_user{"storage_client.num_processed_chunks_per_user",
                                                            true /*reset*/};
static monitor::CountRecorder num_completed_ops_per_user{"storage_client.num_completed_ops_per_user", true /*reset*/};
static monitor::CountRecorder num_failed_ops_per_user{"storage_client.num_failed_ops_per_user", true /*reset*/};
static monitor::CountRecorder num_retried_ops_per_user{"storage_client.num_retried_ops_per_user", true /*reset*/};
static monitor::CountRecorder data_payload_bytes_per_user{"storage_client.data_payload_bytes_per_user", true /*reset*/};

/* Request context data */

using TargetOnChain = std::tuple<TargetId, ChainId, ChainVer>;

class ClientRequestContext {
  static constexpr std::string_view kMetricUserTag = "uid";

 public:
  ClientRequestContext(const StorageClient::MethodType &methodType,
                       const flat::UserInfo &userInfo,
                       const DebugOptions &debugOptions,
                       const StorageClient::Config &clientConfig,
                       size_t numOps = 1,
                       uint32_t retryCount = 0,
                       Duration requestTimeout = Duration::zero())
      : methodType(methodType),
        requestTagSet(monitor::instanceTagSet(std::string{magic_enum::enum_name(methodType)})),
        userInfo(userInfo),
        userTagSet(requestTagSet.newTagSet(std::string(kMetricUserTag), fmt::to_string(userInfo.uid))),
        debugOptions(debugOptions),
        clientConfig(clientConfig),
        userCallId(nextUserCallId.fetch_add(1)),
        retryCount(retryCount),
        debugFlags(debugOptions.toDebugFlags()),
        requestTimeout(requestTimeout),
        userCalls(concurrent_user_calls, atomic_concurrent_user_calls.at(methodType), 1, requestTagSet),
        pendingOps(num_pending_ops, atomic_num_pending_ops.at(methodType), numOps, requestTagSet),
        overallLatency(overall_latency, requestTagSet) {}

  ~ClientRequestContext() {
    double bytesPerSec = computeBandwidth(userCallBytes, getElapsedTime());
    if (bytesPerSec > 0.0) user_call_bw.addSample(bytesPerSec, requestTagSet);
  }

  Duration getElapsedTime() { return Duration(overallLatency.getElapsedTime()); }

  void logWaitingTime() { waiting_time.addSample(getElapsedTime(), this->requestTagSet); }

  bool isTimeout() { return getElapsedTime() > requestTimeout; }

  void initDebugFlags() { debugFlags = debugOptions.toDebugFlags(); }

  double computeBandwidth(size_t nbytes, const Duration &duration) {
    size_t microseconds = duration.asUs().count();
    if (nbytes == 0 || microseconds == 0) return 0.0;
    return (double)nbytes * 1000000 / (double)(microseconds);
  }

 public:
  const StorageClient::MethodType methodType;
  const monitor::TagSet requestTagSet;
  const flat::UserInfo userInfo;
  const monitor::TagSet userTagSet;
  const DebugOptions &debugOptions;
  const StorageClient::Config &clientConfig;
  static std::atomic_uint64_t nextUserCallId;
  uint64_t userCallId;
  uint32_t retryCount;
  DebugFlags debugFlags;
  Duration requestTimeout;
  std::unordered_map<TargetOnChain, uint32_t> numFailures;
  size_t userCallBytes = 0;

  monitor::ScopedCounterWriter userCalls;
  monitor::ScopedCounterWriter pendingOps;
  monitor::ScopedLatencyWriter overallLatency;
};

std::atomic_uint64_t ClientRequestContext::nextUserCallId = 1;

/* Helper functions for status code */

static bool isPermanentError(status_code_t statusCode) {
  switch (statusCode) {
    case StorageClientCode::kMemoryError:
    case StorageClientCode::kInvalidArg:
    case StorageClientCode::kChunkNotFound:
    case StorageClientCode::kBadConfig:
    case StorageClientCode::kProtocolMismatch:
    case StorageClientCode::kRequestCanceled:
    case StorageClientCode::kReadOnlyServer:
    case StorageClientCode::kNoSpace:
    case StorageClientCode::kFoundBug:
    case StorageClientCode::kChecksumMismatch:
      return true;
  }

  return false;
}

static bool isTemporarilyUnavailable(status_code_t statusCode) {
  switch (statusCode) {
    case StorageClientCode::kCommError:
    case StorageClientCode::kTimeout:
    case StorageClientCode::kRemoteIOError:
      return true;
  }

  return false;
}

static bool isFastRetryError(status_code_t statusCode) {
  switch (statusCode) {
    case StorageClientCode::kRoutingVersionMismatch:
    case StorageClientCode::kChunkNotCommit:
      return true;
  }

  return false;
}

/* Helper functions for operations */

#define setResultOfOp(op, res)                                                             \
  do {                                                                                     \
    op->result = (res);                                                                    \
    XLOGF_IF(WARN,                                                                         \
             (op->statusCode() != StatusCode::kOK && !isFastRetryError(op->statusCode())), \
             "Operation {} with id {}, routing target: {}, chunk range: {}, status: {}",   \
             fmt::ptr(op),                                                                 \
             op->requestId,                                                                \
             op->routingTarget,                                                            \
             op->chunkRange(),                                                             \
             op->status());                                                                \
  } while (false)

#define setErrorCodeOfOp(op, errorCode)                                                    \
  do {                                                                                     \
    op->result = (errorCode);                                                              \
    XLOGF_IF(WARN,                                                                         \
             (op->statusCode() != StatusCode::kOK && !isFastRetryError(op->statusCode())), \
             "Operation {} with id {}, routing target: {}, chunk range: {}, status: {}",   \
             fmt::ptr(op),                                                                 \
             op->requestId,                                                                \
             op->routingTarget,                                                            \
             op->chunkRange(),                                                             \
             op->status());                                                                \
  } while (false)

#define setErrorCodeOfOps(ops, errorCode) \
  do {                                    \
    for (auto op : ops) {                 \
      setErrorCodeOfOp(op, (errorCode));  \
    }                                     \
  } while (false)

#define collectFailedOps(ops, failedOps, requestCtx)                                                \
  do {                                                                                              \
    for (size_t opIndex = 0; opIndex < ops.size(); opIndex++) {                                     \
      const auto &op = ops[opIndex];                                                                \
      const auto code = op->statusCode();                                                           \
      ERRLOGF_IF(DBG5,                                                                              \
                 code != StatusCode::kOK && code != StorageClientCode::kChunkNotFound,              \
                 "#{}/{} {} {} operation {} with id {}, "                                           \
                 "routing target: {}, chunk range: {}, user: {}/{}, usercall: #{}, status: {}",     \
                 opIndex + 1,                                                                       \
                 ops.size(),                                                                        \
                 op->statusCode() != StatusCode::kOK ? "Failed" : "Done",                           \
                 magic_enum::enum_name((requestCtx).methodType),                                    \
                 fmt::ptr(op),                                                                      \
                 op->requestId,                                                                     \
                 op->routingTarget,                                                                 \
                 op->chunkRange(),                                                                  \
                 (requestCtx).userInfo.uid,                                                         \
                 (requestCtx).userInfo.gid,                                                         \
                 (requestCtx).userCallId,                                                           \
                 op->status());                                                                     \
      if (op->statusCode() != StatusCode::kOK) {                                                    \
        (failedOps).push_back(op);                                                                  \
      }                                                                                             \
    }                                                                                               \
    XLOGF_IF(ERR,                                                                                   \
             !(failedOps).empty(),                                                                  \
             "Finally collected {}/{} failed {} operations {} issued by user {}/{}, usercall: #{}", \
             (failedOps).size(),                                                                    \
             ops.size(),                                                                            \
             magic_enum::enum_name((requestCtx).methodType),                                        \
             fmt::ptr(&ops),                                                                        \
             (requestCtx).userInfo.uid,                                                             \
             (requestCtx).userInfo.gid,                                                             \
             (requestCtx).userCallId);                                                              \
    num_completed_ops.addSample(ops.size(), (requestCtx).requestTagSet);                            \
    num_completed_ops_per_user.addSample(ops.size(), (requestCtx).userTagSet);                      \
  } while (false)

#define reportNumFailedOps(failedOps, requestCtx)                                                                    \
  do {                                                                                                               \
    std::unordered_map<status_code_t, uint32_t> errorCodeCount;                                                      \
                                                                                                                     \
    for (auto op : (failedOps)) {                                                                                    \
      errorCodeCount[op->statusCode()]++;                                                                            \
      XLOGF_IF(DFATAL,                                                                                               \
               op->routingTarget.channel.id != ChannelId{0},                                                         \
               "Found {} assigned to failed operation {} not released, user: {}/{}, usercall: #{}",                  \
               op->routingTarget.channel,                                                                            \
               fmt::ptr(op),                                                                                         \
               (requestCtx).userInfo.uid,                                                                            \
               (requestCtx).userInfo.gid,                                                                            \
               (requestCtx).userCallId);                                                                             \
    }                                                                                                                \
                                                                                                                     \
    for (const auto &[error, count] : errorCodeCount) {                                                              \
      constexpr std::string_view kMetricStatusCodeTag = "statusCode";                                                \
      auto errorCodeTagSet =                                                                                         \
          (requestCtx)                                                                                               \
              .requestTagSet.newTagSet(std::string(kMetricStatusCodeTag), std::string(StatusCode::toString(error))); \
      num_error_codes.addSample(count, errorCodeTagSet);                                                             \
    }                                                                                                                \
                                                                                                                     \
    num_failed_ops.addSample((failedOps).size(), (requestCtx).requestTagSet);                                        \
    num_failed_ops_per_user.addSample((failedOps).size(), (requestCtx).userTagSet);                                  \
  } while (false)

#define releaseChannelsForOp(chanAllocator, op)                                                                 \
  do {                                                                                                          \
    if (op->routingTarget.channel.id == ChannelId{0}) break;                                                    \
    XLOGF(DBG7, "Released {} of operation {} with {}", op->routingTarget.channel, fmt::ptr(op), op->requestId); \
    chanAllocator.release(op->routingTarget.channel);                                                           \
    op->routingTarget.channel.id = ChannelId{0};                                                                \
  } while (false)

#define releaseChannelsForOps(chanAllocator, ops) \
  do {                                            \
    for (auto &op : ops) {                        \
      releaseChannelsForOp(chanAllocator, op);    \
    }                                             \
  } while (false)

#define allocateChannelsForOps(chanAllocator, ops, reallocate)                                                 \
  [&]() -> bool {                                                                                              \
    std::remove_const<std::remove_reference<decltype(ops)>::type>::type opsWithChan;                           \
                                                                                                               \
    for (auto &op : ops) {                                                                                     \
      if ((reallocate) || op->routingTarget.channel.id == ChannelId{0}) {                                      \
        bool ok = chanAllocator.allocate(op->routingTarget.channel, op->chunkRange().maxNumChunkIdsToProcess); \
                                                                                                               \
        if (ok) {                                                                                              \
          XLOGF(DBG7,                                                                                          \
                "Allocated {}#{} to operation {} with {}",                                                     \
                op->routingTarget.channel,                                                                     \
                op->chunkRange().maxNumChunkIdsToProcess,                                                      \
                fmt::ptr(op),                                                                                  \
                op->requestId);                                                                                \
          opsWithChan.push_back(op);                                                                           \
        } else {                                                                                               \
          goto fail;                                                                                           \
        }                                                                                                      \
      }                                                                                                        \
    }                                                                                                          \
                                                                                                               \
    XLOGF(DBG7,                                                                                                \
          "Allocated {} new channel ids to {} ops, first op {} assigned to {}",                                \
          opsWithChan.size(),                                                                                  \
          ops.size(),                                                                                          \
          fmt::ptr(ops.front()),                                                                               \
          ops.front()->routingTarget.channel);                                                                 \
                                                                                                               \
    return true;                                                                                               \
                                                                                                               \
  fail:                                                                                                        \
    releaseChannelsForOps(chanAllocator, opsWithChan);                                                         \
    return false;                                                                                              \
  }()

/* Helper functions to get and check routing info */

static Result<hf3fs::flat::ChainInfo> getChainInfo(const std::shared_ptr<hf3fs::client::RoutingInfo const> &routingInfo,
                                                   hf3fs::flat::ChainId chainId) {
  auto chainInfo = routingInfo->getChain(chainId);

  if (!chainInfo) {
    XLOGF(ERR,
          "Cannot find chain info with id {}, routing version {}",
          chainId,
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  if (chainInfo->chainId != chainId) {
    XLOGF(ERR,
          "Unexpected chain id {}, expected value {}, routing version {}",
          chainInfo->chainId,
          chainId,
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  return *chainInfo;
}

static Result<hf3fs::flat::NodeInfo> getNodeInfo(const std::shared_ptr<hf3fs::client::RoutingInfo const> &routingInfo,
                                                 hf3fs::flat::NodeId nodeId) {
  auto nodeInfo = routingInfo->getNode(nodeId);

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot find node info with id {}, routing version {}", nodeId, routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  if (nodeInfo->app.nodeId != nodeId) {
    XLOGF(ERR,
          "Unexpected node id {}, expected value {}, routing version {}",
          nodeInfo->app.nodeId,
          nodeId,
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  if (nodeInfo->type != hf3fs::flat::NodeType::STORAGE) {
    XLOGF(ERR,
          "Unexpected node type {} for node with id {}, routing version {}",
          toStringView(nodeInfo->type),
          nodeId,
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  return *nodeInfo;
}

static Result<hf3fs::flat::TargetInfo> getTargetInfo(
    const std::shared_ptr<hf3fs::client::RoutingInfo const> &routingInfo,
    hf3fs::flat::TargetId targetId) {
  auto targetInfo = routingInfo->getTarget(targetId);

  if (!targetInfo) {
    XLOGF(ERR,
          "Cannot find target info with id {}, routing version {}",
          targetId,
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  if (targetInfo->targetId != targetId) {
    XLOGF(ERR,
          "Unexpected target id {}, expected value {}, routing version {}",
          targetInfo->targetId,
          targetId,
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kRoutingError);
  }

  if (targetInfo->publicState != hf3fs::flat::PublicTargetState::SERVING) {
    XLOGF(ERR,
          "Unexpected target state {}, routing version {}",
          toStringView(targetInfo->publicState),
          routingInfo->raw()->routingInfoVersion);
    return makeError(StorageClientCode::kNotAvailable);
  }

  return *targetInfo;
}

template <UnaryPredicate<flat::NodeInfo> NodeSelector>
static Result<std::vector<SlimTargetInfo>> selectServingTargets(
    const std::shared_ptr<hf3fs::client::RoutingInfo const> &routingInfo,
    const hf3fs::flat::ChainInfo &chainInfo,
    const NodeSelector &nodeSelector) {
  std::vector<SlimTargetInfo> servingTargets;
  servingTargets.reserve(chainInfo.targets.size());

  for (const auto &target : chainInfo.targets) {
    XLOGF(DBG7,
          "Storage target {} on {}@{}, state: {}, routing version {}",
          target.targetId,
          chainInfo.chainId,
          chainInfo.chainVersion,
          toStringView(target.publicState),
          routingInfo->raw()->routingInfoVersion);

    if (target.publicState != hf3fs::flat::PublicTargetState::SERVING) {
      XLOGF(DBG5,
            "Found the first non-serving target {} on {}@{}, routing version {}: {}",
            target.targetId,
            chainInfo.chainId,
            chainInfo.chainVersion,
            routingInfo->raw()->routingInfoVersion,
            chainInfo);
      break;
    }

    auto targetInfo = getTargetInfo(routingInfo, target.targetId);
    if (!targetInfo) return makeError(targetInfo.error().code());

    if (!targetInfo->nodeId) {
      XLOGF(WARN,
            "Host node id of target {} on {}@{} is unknown, routing version {}: {}",
            target.targetId,
            chainInfo.chainId,
            chainInfo.chainVersion,
            routingInfo->raw()->routingInfoVersion,
            targetInfo);
      break;
    }

    auto nodeInfo = getNodeInfo(routingInfo, *targetInfo->nodeId);
    if (!nodeInfo) return makeError(nodeInfo.error().code());

    if (!nodeSelector(*nodeInfo)) {
      XLOGF(DBG5,
            "Target {} on {}@{} is skipped since its host {} is filtered out: {}",
            targetInfo->targetId,
            chainInfo.chainId,
            chainInfo.chainVersion,
            *targetInfo->nodeId,
            *nodeInfo);
      continue;
    }

    servingTargets.push_back({TargetId(targetInfo->targetId), NodeId(*targetInfo->nodeId)});
  }

  XLOGF_IF(DBG3,
           servingTargets.size() < chainInfo.targets.size(),
           "Number of reachable targets {} is less than {} targets on {}@{}, routing version {}: {}",
           servingTargets.size(),
           chainInfo.targets.size(),
           chainInfo.chainId,
           chainInfo.chainVersion,
           routingInfo->raw()->routingInfoVersion,
           chainInfo);
  return servingTargets;
}

template <typename Op>
std::vector<Op *> selectRoutingTargetForOps(ClientRequestContext &requestCtx,
                                            const std::shared_ptr<hf3fs::client::RoutingInfo const> &routingInfo,
                                            const TargetSelectionOptions &options,
                                            const std::vector<Op *> &ops) {
  if (routingInfo == nullptr) {
    XLOGF(ERR, "Unexpected: routing info is nullptr");
    setErrorCodeOfOps(ops, StorageClientCode::kRoutingError);
    return {};
  }

  XLOGF_IF(INFO,
           requestCtx.retryCount > 0,
           "Start to select routing targets for {} {} ops {} based on {}: "
           "bootstrapping? {}, {} nodes, {} chain tables, {} chains, {} targets",
           ops.size(),
           magic_enum::enum_name(requestCtx.methodType),
           fmt::ptr(&ops),
           routingInfo->raw()->routingInfoVersion,
           routingInfo->raw()->bootstrapping,
           routingInfo->raw()->nodes.size(),
           routingInfo->raw()->chainTables.size(),
           routingInfo->raw()->chains.size(),
           routingInfo->raw()->targets.size());

  std::unordered_map<ChainId, SlimChainInfo> slimChains(ops.size());
  std::unordered_map<ChainId, hf3fs::flat::ChainInfo> chainInfos(ops.size());
  auto targetSelectionStrategy = TargetSelectionStrategy::create(options);
  auto selectNodeInTrafficZone = flat::selectNodeByTrafficZone(options.trafficZone());

  std::vector<Op *> targetedOps;
  targetedOps.reserve(ops.size());

  for (auto op : ops) {
    auto chainId = op->routingTarget.chainId;

    if (slimChains.count(chainId) == 0) {
      auto chainInfo = getChainInfo(routingInfo, hf3fs::flat::ChainId(chainId));

      if (!chainInfo) {
        setErrorCodeOfOp(op, chainInfo.error().code());
        continue;
      }

      chainInfos.emplace(chainId, *chainInfo);

      auto servingTargets = selectServingTargets(routingInfo, *chainInfo, selectNodeInTrafficZone);

      if (!servingTargets) {
        setErrorCodeOfOp(op, servingTargets.error().code());
        continue;
      }

      if (servingTargets->empty()) {
        XLOGF(DBG3,
              "All targets on the chain not serving or not in traffic zone ({}): {}",
              options.trafficZone(),
              *chainInfo);
      } else if (targetSelectionStrategy->selectAnyTarget()) {
        std::vector<SlimTargetInfo> reachableTargets;
        reachableTargets.reserve(servingTargets->size());

        for (const auto &[targetId, nodeId] : *servingTargets) {
          auto targetOnChain = std::make_tuple(targetId, chainInfo->chainId, chainInfo->chainVersion);
          auto iter = requestCtx.numFailures.find(targetOnChain);
          if (iter != requestCtx.numFailures.end() &&
              iter->second >= requestCtx.clientConfig.retry().max_failures_before_failover()) {
            XLOGF(DBG5,
                  "Target {} on {}@{} should be skipped if possible since its host {} cannot be reached",
                  targetId,
                  chainInfo->chainId,
                  chainInfo->chainVersion,
                  nodeId);
          } else {
            reachableTargets.push_back({targetId, nodeId});
          }
        }

        if (reachableTargets.empty()) {
          XLOGF(DBG3, "All serving targets on the chain not reachable: {}", *chainInfo);
        } else {
          XLOGF_IF(DBG5,
                   reachableTargets.size() < servingTargets->size(),
                   "Found {}/{} reachable targets on the chain: {}",
                   reachableTargets.size(),
                   servingTargets->size(),
                   *chainInfo);
          servingTargets->swap(reachableTargets);
        }
      }

      SlimChainInfo &slimChain = slimChains[chainId];
      slimChain.chainId = chainId;
      slimChain.version = chainInfo->chainVersion;
      slimChain.routingInfoVer = routingInfo->raw()->routingInfoVersion;
      slimChain.totalNumTargets = chainInfo->targets.size();
      slimChain.servingTargets.swap(*servingTargets);

      if (slimChain.servingTargets.size() < chainInfo->targets.size()) {
        targetSelectionStrategy->reset();
      }
    }

    const SlimChainInfo &slimChain = slimChains[chainId];

    op->routingTarget.chainVer = slimChain.version;
    op->routingTarget.routingInfoVer = slimChain.routingInfoVer;

    if (slimChain.servingTargets.empty()) {
      XLOGF(WARN,
            "All targets on chain {}@{} not serving/reachable or not in traffic zone ({}): {}",
            slimChain.chainId,
            slimChain.version,
            options.trafficZone(),
            chainInfos[chainId]);
      setErrorCodeOfOp(op, StorageClientCode::kNotAvailable);
      continue;
    }

    auto selectedTarget = targetSelectionStrategy->selectTarget(slimChain);

    if (!selectedTarget) {
      XLOGF(WARN,
            "Unable to select a routing target from {}/{} serving targets on {}@{}, selection mode {}: {}",
            slimChain.servingTargets.size(),
            slimChain.totalNumTargets,
            slimChain.chainId,
            slimChain.version,
            toStringView(options.mode()),
            chainInfos[chainId]);
      setErrorCodeOfOp(op, selectedTarget.error().code());
      continue;
    }

    op->routingTarget.targetInfo = *selectedTarget;

    XLOGF(DBG7, "Selected routing target for operation {}: {}", fmt::ptr(op), op->routingTarget);

    targetedOps.push_back(op);
  }

  return targetedOps;
}

#define isLatestRoutingInfo(routingInfo, ops)                                                                       \
  [&]() -> bool {                                                                                                   \
    auto currentRoutingInfo = getCurrentRoutingInfo();                                                              \
    bool isLatestVersion = routingInfo->raw()->routingInfoVersion == currentRoutingInfo->raw()->routingInfoVersion; \
    if (isLatestVersion) return true;                                                                               \
    for (const auto op : ops) {                                                                                     \
      auto chainId = op->routingTarget.chainId;                                                                     \
      auto chainVer = op->routingTarget.chainVer;                                                                   \
      auto currentChainInfo = getChainInfo(currentRoutingInfo, hf3fs::flat::ChainId(chainId));                      \
      if (!currentChainInfo || currentChainInfo->chainVersion != chainVer) return false;                            \
    }                                                                                                               \
    return true;                                                                                                    \
  }()

/* Helper functions for building requests */

uint32_t buildFeatureFlagsFromOptions(const DebugOptions &debugOptions) {
  uint32_t featureFlags = 0;
  if (debugOptions.bypass_disk_io()) BITFLAGS_SET(featureFlags, hf3fs::storage::FeatureFlags::BYPASS_DISKIO);
  if (debugOptions.bypass_rdma_xmit()) BITFLAGS_SET(featureFlags, hf3fs::storage::FeatureFlags::BYPASS_RDMAXMIT);
  return featureFlags;
}

template <typename Op, typename BatchReq, typename Options>
BatchReq buildBatchRequest(const ClientRequestContext &requestCtx,
                           const ClientId &clientId,
                           std::atomic_uint64_t &nextRequestId,
                           const StorageClient::Config &config,
                           const Options &options,
                           const flat::UserInfo &userInfo,
                           const std::vector<Op *> &ops) {
  boost::ignore_unused(clientId, nextRequestId, requestCtx, config, options, userInfo, ops);
  return BatchReq();
}

// ReadIO

template <>
typename hf3fs::storage::BatchReadReq buildBatchRequest(const ClientRequestContext &requestCtx,
                                                        const ClientId &clientId,
                                                        std::atomic_uint64_t &nextRequestId,
                                                        const StorageClient::Config &config,
                                                        const ReadOptions &options,
                                                        const flat::UserInfo &userInfo,
                                                        const std::vector<ReadIO *> &ops) {
  std::vector<hf3fs::storage::ReadIO> payloads;
  payloads.reserve(ops.size());
  size_t requestedBytes = 0;
  auto requestTagSet = monitor::instanceTagSet("batchRead");
  auto tagged_bytes_per_operation = bytes_per_operation.getRecorderWithTag(requestTagSet);

  hf3fs::storage::RequestId requestId(nextRequestId.fetch_add(1));
  hf3fs::storage::MessageTag tag{clientId, requestId};

  for (auto &op : ops) {
    hf3fs::storage::GlobalKey key{op->routingTarget.getVersionedChainId(), op->chunkId};

    size_t offset = op->data - op->buffer->data();
    auto iobuf = op->buffer->subrange(offset, op->length);

    requestedBytes += op->length;
    tagged_bytes_per_operation->addSample(op->length);

    op->requestId = requestId;
    payloads.push_back({op->offset, op->length, std::move(key), iobuf.toRemoteBuf()});
  }

  bytes_per_request.addSample(requestedBytes, requestTagSet);
  ops_per_request.addSample(ops.size(), requestTagSet);

  auto checksumType = options.verifyChecksum() ? config.chunk_checksum_type() : ChecksumType::NONE;
  uint32_t featureFlags = buildFeatureFlagsFromOptions(options.debug());

  if (requestedBytes < requestCtx.clientConfig.max_inline_read_bytes()) {
    BITFLAGS_SET(featureFlags, hf3fs::storage::FeatureFlags::SEND_DATA_INLINE);
  }

  if (options.allowReadUncommitted()) {
    BITFLAGS_SET(featureFlags, hf3fs::storage::FeatureFlags::ALLOW_READ_UNCOMMITTED);
  }

  return hf3fs::storage::BatchReadReq{std::move(payloads),
                                      tag,
                                      requestCtx.retryCount,
                                      userInfo,
                                      featureFlags,
                                      checksumType,
                                      requestCtx.debugFlags};
}

// QueryLastChunkOp

template <>
typename hf3fs::storage::QueryLastChunkReq buildBatchRequest(const ClientRequestContext &requestCtx,
                                                             const ClientId &clientId,
                                                             std::atomic_uint64_t &nextRequestId,
                                                             const StorageClient::Config &config,
                                                             const ReadOptions &options,
                                                             const flat::UserInfo &userInfo,
                                                             const std::vector<QueryLastChunkOp *> &ops) {
  boost::ignore_unused(config);

  std::vector<hf3fs::storage::QueryLastChunkOp> payloads;
  payloads.reserve(ops.size());

  hf3fs::storage::RequestId requestId(nextRequestId.fetch_add(1));
  hf3fs::storage::MessageTag tag{clientId, requestId};

  for (auto &op : ops) {
    op->requestId = requestId;
    payloads.push_back({op->routingTarget.getVersionedChainId(), op->range});
  }

  ops_per_request.addSample(ops.size(), requestCtx.requestTagSet);

  uint32_t featureFlags = buildFeatureFlagsFromOptions(options.debug());
  return hf3fs::storage::QueryLastChunkReq{payloads,
                                           tag,
                                           requestCtx.retryCount,
                                           userInfo,
                                           featureFlags,
                                           requestCtx.debugFlags};
}

// RemoveChunksOp

template <>
typename hf3fs::storage::RemoveChunksReq buildBatchRequest(const ClientRequestContext &requestCtx,
                                                           const ClientId &clientId,
                                                           std::atomic_uint64_t &nextRequestId,
                                                           const StorageClient::Config &config,
                                                           const WriteOptions &options,
                                                           const flat::UserInfo &userInfo,
                                                           const std::vector<RemoveChunksOp *> &ops) {
  boost::ignore_unused(nextRequestId, config);

  std::vector<hf3fs::storage::RemoveChunksOp> payloads;
  payloads.reserve(ops.size());

  for (auto &op : ops) {
    hf3fs::storage::MessageTag tag{clientId, op->requestId, op->routingTarget.channel};
    payloads.push_back({op->routingTarget.getVersionedChainId(), op->range, tag, requestCtx.retryCount});
  }

  ops_per_request.addSample(ops.size(), requestCtx.requestTagSet);

  uint32_t featureFlags = buildFeatureFlagsFromOptions(options.debug());
  return hf3fs::storage::RemoveChunksReq{payloads, userInfo, featureFlags, requestCtx.debugFlags};
}

// TruncateChunkOp

template <>
typename hf3fs::storage::TruncateChunksReq buildBatchRequest(const ClientRequestContext &requestCtx,
                                                             const ClientId &clientId,
                                                             std::atomic_uint64_t &nextRequestId,
                                                             const StorageClient::Config &config,
                                                             const WriteOptions &options,
                                                             const flat::UserInfo &userInfo,
                                                             const std::vector<TruncateChunkOp *> &ops) {
  boost::ignore_unused(nextRequestId, config);

  std::vector<hf3fs::storage::TruncateChunkOp> payloads;
  payloads.reserve(ops.size());

  for (auto &op : ops) {
    hf3fs::storage::MessageTag tag{clientId, op->requestId, op->routingTarget.channel};
    payloads.push_back({op->routingTarget.getVersionedChainId(),
                        op->chunkId,
                        op->chunkLen,
                        op->chunkSize,
                        op->onlyExtendChunk,
                        tag,
                        requestCtx.retryCount});
  }

  ops_per_request.addSample(ops.size(), requestCtx.requestTagSet);

  uint32_t featureFlags = buildFeatureFlagsFromOptions(options.debug());
  return hf3fs::storage::TruncateChunksReq{payloads, userInfo, featureFlags, requestCtx.debugFlags};
}

/* Helper functions for rpc communication */

template <typename Req, typename Rsp>
using MessengerMethod = CoTryTask<Rsp> (StorageMessenger::*)(const hf3fs::net::Address &,
                                                             const Req &,
                                                             const net::UserRequestOptions *,
                                                             serde::Timestamp *);

template <typename Req, typename Rsp, auto Method>
CoTryTask<Rsp> StorageClientImpl::callMessengerMethod(StorageMessenger &messenger,
                                                      ClientRequestContext &requestCtx,
                                                      const hf3fs::flat::NodeInfo &nodeInfo,
                                                      const Req &request) {
  monitor::ScopedLatencyWriter latencyWriter(request_latency, requestCtx.requestTagSet);
  monitor::ScopedCounterWriter inflightRequests(inflight_requests,
                                                atomic_inflight_requests.at(requestCtx.methodType),
                                                1,
                                                requestCtx.requestTagSet);

  net::UserRequestOptions options;
  options.timeout = requestCtx.requestTimeout;
  options.sendRetryTimes = 1;

  if (auto reqInfo = RequestInfo::get(); reqInfo && reqInfo->canceled()) {
    XLOGF(WARN, "Request {} {} canceled", reqInfo->describe(), fmt::ptr(&request));
    co_return makeError(StorageClientCode::kRequestCanceled);
  }

  for (const auto &serviceGroup : nodeInfo.app.serviceGroups) {
    for (const auto &address : serviceGroup.endpoints) {
      if (address.type == net::Address::Type::RDMA) {
        serde::Timestamp timestamp;
        auto response =
            FAULT_INJECTION_POINT(requestCtx.debugFlags.injectClientError(),
                                  makeError(StorageClientCode::kCommError),
                                  (co_await std::invoke(Method, messenger, address, request, &options, &timestamp)));

        XLOGF_IF(WARN,
                 latencyWriter.getElapsedTime() > requestCtx.requestTimeout * 2,
                 "Latency higher than expected, request: {}, timeout: {}, latency: {}, host: {}, address: {}",
                 fmt::ptr(&request),
                 requestCtx.requestTimeout,
                 Duration(latencyWriter.getElapsedTime()).asMs(),
                 nodeInfo.app.hostname,
                 address);

        if (response) {
          inflight_time.addSample(timestamp.inflightLatency(), requestCtx.requestTagSet);
          server_latency.addSample(timestamp.serverLatency(), requestCtx.requestTagSet);
          network_latency.addSample(timestamp.networkLatency(), requestCtx.requestTagSet);
        } else {
          XLOGF(ERR,
                "Call messenger error: {}, {} request: {}, usercall: #{}, timeout: {}, latency: {}, host: {}, address: "
                "{}",
                response.error(),
                magic_enum::enum_name(requestCtx.methodType),
                fmt::ptr(&request),
                requestCtx.userCallId,
                requestCtx.requestTimeout,
                Duration(latencyWriter.getElapsedTime()).asMs(),
                nodeInfo.app.hostname,
                address);
        }

        co_return response;
      }
    }
  }

  // No RDMA interface is found for the target node
  XLOGF(DBG1, "No RDMA interface found on node: {:?}", nodeInfo);
  co_return makeError(StorageClientCode::kNoRDMAInterface);
}

template <typename IO>
std::vector<IO *> validateDataRange(const std::vector<IO *> &ios, bool checkOverlappingBuffers) {
  std::vector<IO *> sortedIOs(begin(ios), end(ios));

  if (checkOverlappingBuffers) {
    std::sort(begin(sortedIOs), end(sortedIOs), [](const IO *a, const IO *b) { return a->data < b->data; });
  }

  std::vector<IO *> validIOs;
  validIOs.reserve(ios.size());

  IO *lastIO = nullptr;

  for (size_t ioIndex = 0; ioIndex < sortedIOs.size(); ioIndex++) {
    auto io = sortedIOs[ioIndex];

    if (io->buffer == nullptr || io->data == nullptr) {
      XLOGF(ERR,
            "#{}/{} IO buffer {} or user data {} is null",
            ioIndex + 1,
            ios.size(),
            fmt::ptr(io->buffer),
            fmt::ptr(io->data));
      setErrorCodeOfOps(sortedIOs, StorageClientCode::kInvalidArg);
      return {};
    }

    if (!io->buffer->contains(io->data, io->length)) {
      XLOGF(ERR,
            "#{}/{} User data does not fall in the range of IO buffer {} (address {}, size {}): data address {}, "
            "length {}",
            ioIndex + 1,
            ios.size(),
            fmt::ptr(io->buffer),
            fmt::ptr(io->buffer->data()),
            io->buffer->size(),
            fmt::ptr(io->data),
            io->length);
      setErrorCodeOfOps(sortedIOs, StorageClientCode::kInvalidArg);
      return {};
    }

    if (checkOverlappingBuffers && lastIO != nullptr && io->data < lastIO->dataEnd()) {
      XLOGF(ERR,
            "#{}/{} User data overlaps: current data starts at {}, length {}; "
            "last data starts at {}, ends at {}, length {}",
            ioIndex + 1,
            ios.size(),
            fmt::ptr(io->data),
            io->length,
            fmt::ptr(lastIO->data),
            fmt::ptr(lastIO->dataEnd()),
            lastIO->length);
      setErrorCodeOfOps(sortedIOs, StorageClientCode::kInvalidArg);
      return {};
    }

    validIOs.push_back(io);
    lastIO = io;
  }

  return validIOs;
}

std::vector<ReadIO *> splitReadIOs(StorageClientImpl &client,
                                   const std::vector<ReadIO *> &readIOs,
                                   const size_t maxIOLen) {
  std::vector<ReadIO *> splittedIOs;

  for (auto parentIO : readIOs) {
    parentIO->resetResult();
    parentIO->splittedIOs.clear();
    parentIO->splittedIOs.reserve((parentIO->chunkSize + maxIOLen - 1) / maxIOLen);

    for (uint32_t offset = parentIO->offset, length = parentIO->length; length > 0;) {
      uint32_t ioEnd = ALIGN_LOWER(offset, maxIOLen) + maxIOLen;
      uint32_t ioLen = std::min(ioEnd - offset, length);
      assert(ioLen > 0);

      parentIO->splittedIOs.push_back(client.createReadIO(parentIO->routingTarget.chainId,
                                                          parentIO->chunkId,
                                                          offset,
                                                          ioLen,
                                                          parentIO->data + (offset - parentIO->offset),
                                                          parentIO->buffer));

      XLOGF(DBG5,
            "New splitted read IO #{} of parent IO {}, chainId: {}, chunkId: {}, offset: {}, length: {}",
            parentIO->splittedIOs.size(),
            fmt::ptr(parentIO),
            parentIO->routingTarget.chainId,
            parentIO->chunkId,
            offset,
            ioLen);

      offset += ioLen;
      length -= ioLen;
    }

    for (auto &splittedIO : parentIO->splittedIOs) splittedIOs.push_back(&splittedIO);
  }

  return splittedIOs;
}

std::vector<WriteIO *> validateWriteDataRange(const std::vector<WriteIO *> &ios, bool checkOverlappingBuffers) {
  std::vector<WriteIO *> validIOs;
  validIOs.reserve(ios.size());

  for (size_t ioIndex = 0; ioIndex < ios.size(); ioIndex++) {
    auto io = ios[ioIndex];
    if (io->chunkSize == 0 || io->offset + io->length > io->chunkSize) {
      XLOGF(ERR,
            "#{}/{} Write range is out of the chunk boundary: offset {}, length {}, chunk size {}",
            ioIndex + 1,
            ios.size(),
            io->offset,
            io->length,
            io->chunkSize);
      setErrorCodeOfOp(io, StorageClientCode::kInvalidArg);
    } else {
      validIOs.push_back(io);
    }
  }

  return validateDataRange(validIOs, checkOverlappingBuffers);
}

template <typename Op>
std::vector<Op *> createVectorOfPtrsFromOps(std::span<Op> ops) {
  std::vector<Op *> ptrs;
  ptrs.reserve(ops.size());

  for (auto &op : ops) {
    ptrs.push_back(&op);
  }

  return ptrs;
}

template <typename Op>
std::vector<std::pair<NodeId, std::vector<Op *>>> groupOpsByNodeId(ClientRequestContext &requestCtx,
                                                                   const std::vector<Op *> &ops,
                                                                   const size_t maxBatchSize,
                                                                   const size_t maxBatchBytes,
                                                                   const bool randomShuffle) {
  std::unordered_map<NodeId, std::vector<Op *>> opsWithChannelId;
  std::unordered_map<NodeId, std::vector<Op *>> opsGroupedByNode;
  std::unordered_map<NodeId, size_t> opsGroupBytes;

  for (auto op : ops) {
    // put ops with channel ids to a separate map
    if (op->routingTarget.channel.id != ChannelId{0}) {
      XLOGF(DBG7, "Operation {} already assigned to {}", fmt::ptr(op), op->routingTarget.channel);
      opsWithChannelId[op->routingTarget.targetInfo.nodeId].push_back(op);
    } else {
      opsGroupedByNode[op->routingTarget.targetInfo.nodeId].push_back(op);
      opsGroupBytes[op->routingTarget.targetInfo.nodeId] += op->dataLen();
    }
  }

  static auto calcAvgSize = [](size_t total, size_t max) {
    if (total == 0) return max;
    size_t n = (total + max - 1) / max;
    size_t avg = (total + n - 1) / n;
    return std::min(avg, max);
  };

  std::vector<std::pair<NodeId, std::vector<Op *>>> batches;

  for (const auto &[nodeId, opsGroup] : opsGroupedByNode) {
    size_t batchBytes = 0;
    size_t numBatches = 0;
    std::vector<Op *> batchOps;

    size_t remainingOps = opsGroup.size();
    size_t remainingBytes = opsGroupBytes[nodeId];
    size_t avgBatchSize = calcAvgSize(remainingOps, maxBatchSize);
    size_t avgBatchBytes = calcAvgSize(remainingBytes, maxBatchBytes);

    for (size_t opIndex = 0; opIndex < opsGroup.size(); opIndex++) {
      auto op = opsGroup[opIndex];

      if ((batchOps.size() >= avgBatchSize && batchOps.size() + remainingOps > maxBatchSize) ||
          (batchBytes + op->dataLen() > avgBatchBytes && batchBytes + remainingBytes > maxBatchBytes)) {
        // add a batch of ops
        batches.emplace_back(nodeId, std::move(batchOps));
        batchOps.clear();
        batchBytes = 0;
        numBatches++;
      }

      batchOps.push_back(op);
      batchBytes += op->dataLen();
      remainingOps--;
      remainingBytes -= op->dataLen();
    }

    if (!batchOps.empty()) {
      // add the last batch of ops
      batches.emplace_back(nodeId, batchOps);
      numBatches++;
    }

    requests_per_server.addSample(numBatches, requestCtx.requestTagSet);
    requestCtx.userCallBytes += batchBytes;
  }

  accessed_servers_per_user_call.addSample(opsGroupedByNode.size(), requestCtx.requestTagSet);
  bytes_per_user_call.addSample(requestCtx.userCallBytes, requestCtx.requestTagSet);
  requests_per_user_call.addSample(batches.size(), requestCtx.requestTagSet);
  ops_per_user_call.addSample(ops.size(), requestCtx.requestTagSet);

  if (randomShuffle) {
    static thread_local std::mt19937 generator;
    std::shuffle(batches.begin(), batches.end(), generator);
  }

  // move ops with channel ids to the front of batches

  if (UNLIKELY(!opsWithChannelId.empty())) {
    for (auto &[nodeId, opsGroup] : opsWithChannelId) {
      XLOGF(DBG3,
            "Move {} ops with channel ids to the front of batches, routed to {}, first op {} assigned to {}",
            opsGroup.size(),
            nodeId,
            fmt::ptr(opsGroup.front()),
            opsGroup.front()->routingTarget.channel);
      batches.emplace(batches.begin(), nodeId, std::move(opsGroup));
    }
  }

  return batches;
}

template <typename Op, typename Ops = std::vector<Op *>>
CoTask<void> processBatches(const std::vector<std::pair<NodeId, Ops>> &batches, auto &&func, bool parallel) {
  std::vector<CoTask<bool>> tasks;
  if (parallel) tasks.reserve(batches.size());

  for (size_t index = 0; index < batches.size(); index++) {
    const auto &[nodeId, ops] = batches[index];
    if (!ops.empty()) {
      if (parallel) {
        tasks.push_back(func(index, nodeId, ops));
      } else {
        co_await func(index, nodeId, ops);
      }
    }
  }

  if (parallel) co_await folly::coro::collectAllRange(std::move(tasks));
}

template <typename Op>
CoTryTask<void> sendOpsWithRetry(ClientRequestContext &requestCtx,
                                 UpdateChannelAllocator &chanAllocator,
                                 const std::vector<Op *> &ops,
                                 auto &&sendOps,
                                 const RetryOptions &options) {
  ExponentialBackoffRetry backoffRetry(options.init_wait_time().asMs(),
                                       options.max_wait_time().asMs(),
                                       options.max_retry_time().asMs());
  std::chrono::milliseconds requestTimeout;
  uint32_t retryCount = 0;

  std::vector<Op *> pendingOps(begin(ops), end(ops));
  for (auto &op : pendingOps) op->resetResult();

  while (true) {
    requestTimeout = backoffRetry.getWaitTime();

    if (requestTimeout.count() == 0) {
      XLOGF(ERR,
            "Give up retrying {}/{} ops {} after #{} retries, "
            "elapsed time: {}, max retry time: {}, user: {}/{}, usercall: #{}",
            pendingOps.size(),
            ops.size(),
            fmt::ptr(&ops),
            retryCount,
            backoffRetry.getElapsedTime(),
            options.max_retry_time().asMs(),
            requestCtx.userInfo.uid,
            requestCtx.userInfo.gid,
            requestCtx.userCallId);

      for (const auto &op : pendingOps) {
        releaseChannelsForOp(chanAllocator, op);
        if (op->statusCode() == StorageClientCode::kNotInitialized) {
          op->result = StorageClientCode::kTimeout;
        }
      }

      goto exit;
    }

    XLOGF_IF(INFO,
             retryCount > 0,
             "Sending request with {}/{} ops {}, #{} retry, "
             "timeout {}, elapsed time: {}, max retry time: {}, user: {}/{}, usercall: #{}",
             pendingOps.size(),
             ops.size(),
             fmt::ptr(&ops),
             retryCount,
             requestTimeout,
             backoffRetry.getElapsedTime(),
             options.max_retry_time().asMs(),
             requestCtx.userInfo.uid,
             requestCtx.userInfo.gid,
             requestCtx.userCallId);

    requestCtx.retryCount = retryCount;
    requestCtx.requestTimeout = Duration(requestTimeout);
    requestCtx.initDebugFlags();

    auto requestStartTime = SteadyClock::now();
    co_await sendOps(pendingOps);
    auto requestLatency = std::chrono::duration_cast<std::chrono::milliseconds>(SteadyClock::now() - requestStartTime);
    auto waitTime = std::max(std::chrono::milliseconds(0), requestTimeout - requestLatency);

    XLOGF_IF(INFO,
             (retryCount > 0 || requestLatency > requestTimeout * 2),
             "Completed request with {}/{} ops {}, #{} retry, timeout {}, latency {}, user: {}/{}, usercall: #{}",
             pendingOps.size(),
             ops.size(),
             fmt::ptr(&ops),
             retryCount,
             requestTimeout,
             requestLatency,
             requestCtx.userInfo.uid,
             requestCtx.userInfo.gid,
             requestCtx.userCallId);

    std::vector<Op *> remainingOps;
    std::unordered_set<TargetOnChain> failedTargets;

    for (const auto op : pendingOps) {
      if constexpr (requires { op->result.statusCode; }) {
        op->result.statusCode = StatusCodeConversion::convertToStorageClientCode(op->result.statusCode);
      } else {
        op->result.lengthInfo = StatusCodeConversion::convertToStorageClientCode(op->result.lengthInfo);
      }

      if (!options.retry_permanent_error() && isPermanentError(op->statusCode())) {
        releaseChannelsForOp(chanAllocator, op);
      } else if (op->statusCode() != StatusCode::kOK) {
        remainingOps.push_back(op);

        if (isTemporarilyUnavailable(op->statusCode()))
          failedTargets.emplace(op->routingTarget.targetInfo.targetId,
                                op->routingTarget.chainId,
                                op->routingTarget.chainVer);

        if (isFastRetryError(op->statusCode())) waitTime = std::min(waitTime, options.init_wait_time().asMs() / 2);
      }
    }

    for (const auto &failedTarget : failedTargets) {
      const auto &[targetId, chainId, chainVer] = failedTarget;
      requestCtx.numFailures[failedTarget]++;
      XLOGF(INFO,
            "Cannot access storage target {} on {}@{} for {} times during processing of {}/{} ops {}, "
            "user: {}/{}, usercall: #{}",
            targetId,
            chainId,
            chainVer,
            requestCtx.numFailures[failedTarget],
            pendingOps.size(),
            ops.size(),
            fmt::ptr(&ops),
            requestCtx.userInfo.uid,
            requestCtx.userInfo.gid,
            requestCtx.userCallId);
    }

    if (remainingOps.empty()) {
      XLOGF_IF(INFO,
               retryCount > 0,
               "All {}/{} pending ops {} processed after #{} retries, elapsed time: {}, user: {}/{}, usercall: #{}",
               pendingOps.size(),
               ops.size(),
               fmt::ptr(&ops),
               retryCount,
               backoffRetry.getElapsedTime(),
               requestCtx.userInfo.uid,
               requestCtx.userInfo.gid,
               requestCtx.userCallId);
      goto exit;
    }

    num_retried_ops.addSample(remainingOps.size(), requestCtx.requestTagSet);
    num_retried_ops_per_user.addSample(remainingOps.size(), requestCtx.userTagSet);
    pendingOps.swap(remainingOps);

    XLOGF(INFO,
          "Waiting {} before #{} retrying {}/{} remaining ops {}, user: {}/{}, usercall: #{}",
          waitTime,
          ++retryCount,
          pendingOps.size(),
          ops.size(),
          fmt::ptr(&ops),
          requestCtx.userInfo.uid,
          requestCtx.userInfo.gid,
          requestCtx.userCallId);

    if (waitTime.count() > 0) co_await folly::coro::sleep(waitTime);
  }

exit:

  co_return Void{};
}

template <typename Op, typename BatchReq, typename BatchRsp, auto Method>
CoTryTask<BatchRsp> StorageClientImpl::sendBatchRequest(StorageMessenger &messenger,
                                                        ClientRequestContext &requestCtx,
                                                        std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo,
                                                        const NodeId &nodeId,
                                                        const BatchReq &batchReq,
                                                        const std::vector<Op *> &ops) {
  auto nodeInfo = getNodeInfo(routingInfo, hf3fs::flat::NodeId(nodeId));

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot get node info to communicate with {}", nodeId);
    setErrorCodeOfOps(ops, nodeInfo.error().code());
    co_return makeError(nodeInfo.error().code());
  }

  auto startTime = hf3fs::SteadyClock::now();
  auto response = co_await callMessengerMethod<BatchReq, BatchRsp, Method>(messenger, requestCtx, *nodeInfo, batchReq);

  if (!response) {
    XLOGF(ERR,
          "Cannot communicate with node {}, error: {}, request: {} {} ops, usercall: #{}",
          nodeInfo->app.nodeId,
          response.error(),
          ops.size(),
          magic_enum::enum_name(requestCtx.methodType),
          requestCtx.userCallId);
    setErrorCodeOfOps(ops, StatusCodeConversion::convertToStorageClientCode(response.error()).code());
    co_return response;
  }

  auto &results = (*response).results;

  if (results.size() != ops.size()) {
    XLOGF(DFATAL, "[BUG] Unexpected length of results: {}, expected value: {}", results.size(), ops.size());
    setErrorCodeOfOps(ops, StorageClientCode::kFoundBug);
    co_return makeError(StorageClientCode::kFoundBug);
  }

  size_t totalDataLen = 0;
  size_t totalNumChunks = 0;

  for (uint32_t opIdx = 0; opIdx < results.size(); opIdx++) {
    if constexpr (requires { results[opIdx].statusCode; }) {
      results[opIdx].statusCode = FAULT_INJECTION_POINT(requestCtx.debugFlags.injectClientError(),
                                                        makeError(StorageClientCode::kRemoteIOError),
                                                        results[opIdx].statusCode);
    } else {
      results[opIdx].lengthInfo = FAULT_INJECTION_POINT(requestCtx.debugFlags.injectClientError(),
                                                        makeError(StorageClientCode::kRemoteIOError),
                                                        results[opIdx].lengthInfo);
    }

    setResultOfOp(ops[opIdx], results[opIdx]);

    XLOGF_IF(DFATAL,
             ops[opIdx]->statusCode() == StorageClientCode::kDuplicateUpdate,
             "[BUG] Operation {} {} has an unexpected status code: {}, routing target: {}",
             fmt::ptr(ops[opIdx]),
             ops[opIdx]->requestId,
             ops[opIdx]->statusCode(),
             ops[opIdx]->routingTarget);

    if (ops[opIdx]->statusCode() == StatusCode::kOK) {
      XLOGF_IF(INFO,
               requestCtx.retryCount > 0,
               "Operation {} {} with id {} done, #{} retry, routing target: {}, chunk range: {}",
               magic_enum::enum_name(requestCtx.methodType),
               fmt::ptr(ops[opIdx]),
               ops[opIdx]->requestId,
               requestCtx.retryCount,
               ops[opIdx]->routingTarget,
               ops[opIdx]->chunkRange());

      totalDataLen += ops[opIdx]->resultLen();
      totalNumChunks += ops[opIdx]->numProcessedChunks();
      releaseChannelsForOp(chanAllocator_, ops[opIdx]);
    }
  }

  Duration elapsedTime(hf3fs::SteadyClock::now() - startTime);
  double bytesPerSec = requestCtx.computeBandwidth(totalDataLen, elapsedTime);
  if (bytesPerSec > 0.0) request_bw.addSample(bytesPerSec, requestCtx.requestTagSet);

  data_payload_bytes.addSample(totalDataLen, requestCtx.requestTagSet);
  data_payload_bytes_per_user.addSample(totalDataLen, requestCtx.userTagSet);
  num_processed_chunks.addSample(totalNumChunks, requestCtx.requestTagSet);
  num_processed_chunks_per_user.addSample(totalNumChunks, requestCtx.userTagSet);

  co_return response;
}

/* StorageClientImpl */

StorageClientImpl::StorageClientImpl(const ClientId &clientId,
                                     const Config &config,
                                     hf3fs::client::ICommonMgmtdClient &mgmtdClient)
    : StorageClient(clientId, config),
      clientStarted_(false),
      mgmtdClient_(mgmtdClient),
      messenger_(config_.net_client()),
      messengerForUpdates_(config_.net_client_for_updates()),
      chanAllocator_(config_.traffic_control().max_concurrent_updates()),
      readConcurrencyLimit_(config_.traffic_control().read()),
      writeConcurrencyLimit_(config_.traffic_control().write()),
      queryConcurrencyLimit_(config_.traffic_control().query()),
      removeConcurrencyLimit_(config_.traffic_control().remove()),
      truncateConcurrencyLimit_(config_.traffic_control().truncate()) {}

StorageClientImpl::~StorageClientImpl() { stop(); }

Result<Void> StorageClientImpl::start() {
  XLOGF(INFO, "Starting storage client {}", clientId_);

  if (clientStarted_) {
    XLOGF(INFO, "Storage client {} already started", clientId_);
    return Void{};
  }

  for (const auto methodType : magic_enum::enum_values<MethodType>()) {
    atomic_concurrent_user_calls.emplace(methodType, 0);
    atomic_num_pending_ops.emplace(methodType, 0);
    atomic_inflight_requests.emplace(methodType, 0);
  }

  auto msgrInit = messenger_.start("StorageMsgr");

  if (!msgrInit) {
    XLOGF(CRITICAL, "Failed to start storage messenger of {}", clientId_);
    return msgrInit;
  }

  if (config_.create_net_client_for_updates()) {
    auto msgrInit = messengerForUpdates_.start("StorageMsgrUpdates");

    if (!msgrInit) {
      XLOGF(CRITICAL, "Failed to start storage messenger (for updates) of {}", clientId_);
      return msgrInit;
    }
  }

  XLOGF(INFO, "Storage client {} is getting the first routing info", clientId_);
  auto routingInfo = mgmtdClient_.getRoutingInfo();

  if (routingInfo == nullptr || routingInfo->raw() == nullptr) {
    XLOGF(CRITICAL, "Failed to get the first routing info");
    return makeError(StorageClientCode::kRoutingError);
  }

  setCurrentRoutingInfo(routingInfo);

  bool addListenerOK = mgmtdClient_.addRoutingInfoListener(fmt::to_string(clientId_), [this](auto &&routingInfo) {
    setCurrentRoutingInfo(std::forward<decltype(routingInfo)>(routingInfo));
  });

  if (!addListenerOK) {
    XLOGF(CRITICAL, "Failed to add routing info listener with name {}", fmt::to_string(clientId_));
    return makeError(StorageClientCode::kRoutingError);
  }

  XLOGF(INFO, "Storage client {} started", clientId_);
  clientStarted_ = true;

  return Void{};
}

void StorageClientImpl::stop() {
  XLOGF(INFO, "Stopping storage client {}", clientId_);

  if (!clientStarted_) {
    XLOGF(INFO, "Storage client {} already stopped or not started", clientId_);
    return;
  }

  bool removeListenerOK = mgmtdClient_.removeRoutingInfoListener(fmt::to_string(clientId_));

  if (!removeListenerOK) {
    XLOGF(DFATAL, "Failed to remove routing info listener with name {}", fmt::to_string(clientId_));
  }

  XLOGF(INFO, "Stopping storage messenger of {}", clientId_);
  messenger_.stopAndJoin();

  if (config_.create_net_client_for_updates()) {
    XLOGF(INFO, "Stopping storage messenger (for updates) of {}", clientId_);
    messengerForUpdates_.stopAndJoin();
  }

  clientStarted_ = false;

  XLOGF(INFO, "Storage client {} stopped", clientId_);
}

void StorageClientImpl::setCurrentRoutingInfo(std::shared_ptr<hf3fs::client::RoutingInfo const> latestRoutingInfo) {
  if (latestRoutingInfo == nullptr || latestRoutingInfo->raw() == nullptr) {
    XLOGF(DFATAL, "Latest (raw) routing info is null");
    return;
  }

  auto currentRoutingInfo = currentRoutingInfo_.load();

  if (currentRoutingInfo == nullptr) {
    XLOGF(INFO, "First routing info obtained: {}", latestRoutingInfo->raw()->routingInfoVersion);
    currentRoutingInfo_.store(latestRoutingInfo);
    return;
  }

  if (currentRoutingInfo->raw()->routingInfoVersion == latestRoutingInfo->raw()->routingInfoVersion) {
    XLOGF(INFO, "Routing info version not updated: {}", latestRoutingInfo->raw()->routingInfoVersion);
  } else {
    XLOGF(INFO,
          "Routing info version updated: {} ==> {}",
          currentRoutingInfo->raw()->routingInfoVersion,
          latestRoutingInfo->raw()->routingInfoVersion);
  }

  for (const auto &[chainId, oldChainInfo] : currentRoutingInfo->raw()->chains) {
    if (oldChainInfo.targets.size() <= 1) continue;  // skip single replica chain

    auto newChainInfo = getChainInfo(latestRoutingInfo, chainId);

    if (!newChainInfo) {
      XLOGF(ERR, "Cannot find {} in latest routing info {}", chainId, latestRoutingInfo->raw()->routingInfoVersion);
      continue;
    }

    if (oldChainInfo.chainVersion != newChainInfo->chainVersion) {
      XLOGF(INFO,
            "Replication chain updated {}@{} ==> {}@{}: {}",
            oldChainInfo.chainId,
            oldChainInfo.chainVersion,
            newChainInfo->chainId,
            newChainInfo->chainVersion,
            *newChainInfo);
    }

    if (newChainInfo->targets.empty()) {
      XLOGF(DFATAL,
            "Target list of {}@{} is empty: {}",
            newChainInfo->chainId,
            newChainInfo->chainVersion,
            *newChainInfo);
    }

    for (size_t targetIndex = 0; targetIndex < newChainInfo->targets.size(); targetIndex++) {
      const auto &targetInfo = newChainInfo->targets[targetIndex];
      XLOGF_IF(WARN,
               targetInfo.publicState != flat::PublicTargetState::SERVING,
               "#{} storage target {} on chain {}@{} not serving: {}",
               targetIndex,
               targetInfo.targetId,
               newChainInfo->chainId,
               newChainInfo->chainVersion,
               magic_enum::enum_name(targetInfo.publicState));
    }
  }

  currentRoutingInfo_.store(latestRoutingInfo);
};

// read operation

CoTryTask<void> StorageClientImpl::batchRead(std::span<ReadIO> readIOs,
                                             const flat::UserInfo &userInfo,
                                             const ReadOptions &options,
                                             std::vector<ReadIO *> *failedIOs) {
  ClientRequestContext requestCtx(MethodType::batchRead, userInfo, options.debug(), config_, readIOs.size());

  std::vector<ReadIO *> readIOPtrs = createVectorOfPtrsFromOps(readIOs);

  std::vector<ReadIO *> failedIOVec;
  if (failedIOs == nullptr) failedIOs = &failedIOVec;

  co_return co_await batchReadWithRetry(requestCtx, readIOPtrs, userInfo, options, *failedIOs);
}

CoTryTask<void> StorageClientImpl::batchReadWithRetry(ClientRequestContext &requestCtx,
                                                      const std::vector<ReadIO *> &readIOs,
                                                      const flat::UserInfo &userInfo,
                                                      const ReadOptions &options,
                                                      std::vector<ReadIO *> &failedIOs) {
  const auto maxIOBytes = config_.max_read_io_bytes();
  const bool splitLargeIOs = maxIOBytes > 0;
  std::vector<ReadIO *> splittedIOs;

  auto sendOps = [ this, &requestCtx, &userInfo, &options ](const std::vector<ReadIO *> &ops) -> auto{
    return batchReadWithoutRetry(requestCtx, ops, userInfo, options);
  };

  auto validIOs = validateDataRange(readIOs, config_.check_overlapping_read_buffers());

  if (validIOs.empty()) {
    XLOGF(ERR, "Empty list of IOs: all the {} read IOs are invalid", readIOs.size());
    goto exit;
  }

  if (splitLargeIOs) {
    splittedIOs = splitReadIOs(*this, validIOs, maxIOBytes);
  }

  co_await sendOpsWithRetry<ReadIO>(requestCtx,
                                    chanAllocator_,
                                    splitLargeIOs ? splittedIOs : validIOs,
                                    sendOps,
                                    config_.retry().mergeWith(options.retry()));

  if (splitLargeIOs) {
    for (auto parentIO : validIOs) {
      for (size_t ioIndex = 0; ioIndex < parentIO->splittedIOs.size(); ioIndex++) {
        const auto &splittedIO = parentIO->splittedIOs[ioIndex];

        ERRLOGF_IF(DBG5,
                   !bool(splittedIO.result.lengthInfo),
                   "{} splitted read IO #{}/{} of parent IO {}, status: {}",
                   splittedIO.statusCode() != StatusCode::kOK ? "Failed" : "Done",
                   ioIndex + 1,
                   parentIO->splittedIOs.size(),
                   fmt::ptr(parentIO),
                   splittedIO.status());

        if (!bool(splittedIO.result.lengthInfo)) {
          parentIO->result = splittedIO.result;
          break;
        } else if (ioIndex == 0) {
          parentIO->result = splittedIO.result;
          parentIO->requestId = splittedIO.requestId;
          parentIO->routingTarget = splittedIO.routingTarget;
        } else {
          parentIO->result.lengthInfo = *parentIO->result.lengthInfo + *splittedIO.result.lengthInfo;
          parentIO->result.checksum.combine(splittedIO.result.checksum, *splittedIO.result.lengthInfo);
        }
      }
    }
  }

exit:
  collectFailedOps(readIOs, failedIOs, requestCtx);
  reportNumFailedOps(failedIOs, requestCtx);
  co_return Void{};
}

CoTryTask<void> StorageClientImpl::batchReadWithoutRetry(ClientRequestContext &requestCtx,
                                                         const std::vector<ReadIO *> &readIOs,
                                                         const flat::UserInfo &userInfo,
                                                         const ReadOptions &options) {
  // collect target/chain infos
  TargetSelectionOptions targetSelectionOptions = options.targetSelection();
  targetSelectionOptions.set_mode(options.targetSelection().mode() == TargetSelectionMode::Default
                                      ? TargetSelectionMode::LoadBalance
                                      : options.targetSelection().mode());

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();
  auto targetedOps = selectRoutingTargetForOps(requestCtx, routingInfo, targetSelectionOptions, readIOs);

  // select storage target for each IO and group by node id

  auto batches = groupOpsByNodeId(requestCtx,
                                  targetedOps,
                                  config_.traffic_control().read().max_batch_size(),
                                  config_.traffic_control().read().max_batch_bytes(),
                                  config_.traffic_control().read().random_shuffle_requests());

  // create batch read request and communicate with storage service

  auto sendReq =
      [&, this](size_t batchIndex, const NodeId &nodeId, const std::vector<ReadIO *> &batchIOs) -> CoTask<bool> {
    SemaphoreGuard perServerReq(*readConcurrencyLimit_.getPerServerSemaphore(nodeId));
    co_await perServerReq.coWait();

    SemaphoreGuard concurrentReq(readConcurrencyLimit_.getConcurrencySemaphore());
    co_await concurrentReq.coWait();

    if (!isLatestRoutingInfo(routingInfo, batchIOs)) {
      setErrorCodeOfOps(batchIOs, StorageClientCode::kRoutingVersionMismatch);
      co_return false;
    }

    // log the waiting time before communication starts
    requestCtx.logWaitingTime();

    auto batchReq = buildBatchRequest<ReadIO, BatchReadReq>(requestCtx,
                                                            clientId_,
                                                            nextRequestId_,
                                                            config_,
                                                            options,
                                                            userInfo,
                                                            batchIOs);

    auto response =
        co_await sendBatchRequest<ReadIO, BatchReadReq, BatchReadRsp, &StorageMessenger::batchRead>(messenger_,
                                                                                                    requestCtx,
                                                                                                    routingInfo,
                                                                                                    nodeId,
                                                                                                    batchReq,
                                                                                                    batchIOs);

    if (BITFLAGS_CONTAIN(batchReq.featureFlags, FeatureFlags::SEND_DATA_INLINE) && bool(response)) {
      size_t totalDataLen = 0;
      for (auto readIO : batchIOs) {
        totalDataLen += readIO->resultLen();
      }

      if (response->inlinebuf.data.size() != totalDataLen) {
        XLOGF(DFATAL,
              "[BUG] Inline buffer size {} not equal to total data size {} of {} read IOs",
              response->inlinebuf.data.size(),
              totalDataLen,
              batchIOs.size());
        setErrorCodeOfOps(batchIOs, StorageClientCode::kFoundBug);
        co_return false;
      }

      auto inlinebuf = &response->inlinebuf.data[0];
      for (auto readIO : batchIOs) {
        std::memcpy(readIO->data, inlinebuf, readIO->resultLen());
        inlinebuf += readIO->resultLen();
      }
    }

    if (options.verifyChecksum()) {
      for (auto readIO : batchIOs) {
        if (readIO->result.lengthInfo && *readIO->result.lengthInfo > 0) {
          auto checksum = ChecksumInfo::create(readIO->result.checksum.type, readIO->data, *readIO->result.lengthInfo);
          if (FAULT_INJECTION_POINT(requestCtx.debugFlags.injectClientError(),
                                    true,
                                    checksum != readIO->result.checksum)) {
            XLOGF_IF(DFATAL,
                     !requestCtx.debugFlags.faultInjectionEnabled(),
                     "Local checksum {} not equal to checksum {} generated by server, routing target: {}",
                     checksum,
                     readIO->result.checksum,
                     readIO->routingTarget);
            setErrorCodeOfOp(readIO, StorageClientCode::kChecksumMismatch);
          }
        }
      }
    }

    co_return bool(response);
  };

  bool parallelProcessing = config_.traffic_control().read().process_batches_in_parallel();
  co_await processBatches<ReadIO>(batches, sendReq, parallelProcessing);

  co_return Void{};
}

CoTryTask<void> StorageClientImpl::read(ReadIO &readIO, const flat::UserInfo &userInfo, const ReadOptions &options) {
  ClientRequestContext requestCtx(MethodType::read, userInfo, options.debug(), config_);
  std::vector<ReadIO *> readIOs{&readIO};
  std::vector<ReadIO *> failedIOs;
  co_return co_await batchReadWithRetry(requestCtx, readIOs, userInfo, options, failedIOs);
}

// write operation

CoTryTask<void> StorageClientImpl::batchWrite(std::span<WriteIO> writeIOs,
                                              const flat::UserInfo &userInfo,
                                              const WriteOptions &options,
                                              std::vector<WriteIO *> *failedIOs) {
  ClientRequestContext requestCtx(MethodType::batchWrite, userInfo, options.debug(), config_, writeIOs.size());

  std::vector<WriteIO *> writeIOPtrs = createVectorOfPtrsFromOps(writeIOs);

  std::vector<WriteIO *> failedIOVec;
  if (failedIOs == nullptr) failedIOs = &failedIOVec;

  co_return co_await batchWriteWithRetry(requestCtx, writeIOPtrs, userInfo, options, *failedIOs);
}

CoTryTask<void> StorageClientImpl::batchWriteWithRetry(ClientRequestContext &requestCtx,
                                                       const std::vector<WriteIO *> &writeIOs,
                                                       const flat::UserInfo &userInfo,
                                                       const WriteOptions &options,
                                                       std::vector<WriteIO *> &failedIOs) {
  auto sendOps = [ this, &requestCtx, userInfo, options ](const std::vector<WriteIO *> &ops) -> auto{
    return batchWriteWithoutRetry(requestCtx, ops, userInfo, options);
  };

  auto validIOs = validateWriteDataRange(writeIOs, config_.check_overlapping_write_buffers());

  if (validIOs.empty()) {
    XLOGF(ERR, "Empty list of IOs: all the {} write IOs are invalid", writeIOs.size());
    goto exit;
  }

  co_await sendOpsWithRetry<WriteIO>(requestCtx,
                                     chanAllocator_,
                                     validIOs,
                                     sendOps,
                                     config_.retry().mergeWith(options.retry()));

exit:
  collectFailedOps(writeIOs, failedIOs, requestCtx);
  reportNumFailedOps(failedIOs, requestCtx);
  co_return Void{};
}

CoTryTask<void> StorageClientImpl::batchWriteWithoutRetry(ClientRequestContext &requestCtx,
                                                          const std::vector<WriteIO *> &writeIOs,
                                                          const flat::UserInfo &userInfo,
                                                          const WriteOptions &options) {
  // collect target/chain infos

  TargetSelectionOptions targetSelectionOptions = options.targetSelection();
  targetSelectionOptions.set_mode(options.targetSelection().mode() == TargetSelectionMode::Default
                                      ? TargetSelectionMode::HeadTarget
                                      : options.targetSelection().mode());

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();
  auto targetedIOs = selectRoutingTargetForOps(requestCtx, routingInfo, targetSelectionOptions, writeIOs);

  // select storage target for each IO and group by node id

  auto batches = groupOpsByNodeId(requestCtx,
                                  targetedIOs,
                                  config_.traffic_control().write().max_batch_size(),
                                  config_.traffic_control().write().max_batch_bytes(),
                                  config_.traffic_control().write().random_shuffle_requests());

  // create batch write request and communicate with storage service

  auto sendReq =
      [&, this](size_t batchIndex, const NodeId &nodeId, const std::vector<WriteIO *> &batchIOs) -> CoTask<bool> {
    SemaphoreGuard perServerReq(*writeConcurrencyLimit_.getPerServerSemaphore(nodeId));
    co_await perServerReq.coWait();

    SemaphoreGuard concurrentReq(writeConcurrencyLimit_.getConcurrencySemaphore());
    co_await concurrentReq.coWait();

    if (!isLatestRoutingInfo(routingInfo, batchIOs)) {
      setErrorCodeOfOps(batchIOs, StorageClientCode::kRoutingVersionMismatch);
      co_return false;
    }

    if (!allocateChannelsForOps(chanAllocator_, batchIOs, false /*reallocate*/)) {
      XLOGF(WARN,
            "Cannot allocate channel ids for {} write IOs, first IO {}",
            batchIOs.size(),
            fmt::ptr(batchIOs.front()));
      setErrorCodeOfOps(batchIOs, StorageClientCode::kResourceBusy);
      co_return false;
    }

    // log the waiting time before communication starts
    requestCtx.logWaitingTime();

    auto statusCode =
        co_await sendWriteRequestsSequentially(requestCtx, batchIOs, routingInfo, nodeId, userInfo, options);

    co_return bool(statusCode);
  };

  bool parallelProcessing = config_.traffic_control().write().process_batches_in_parallel();
  co_await processBatches<WriteIO>(batches, sendReq, parallelProcessing);

  co_return Void{};
}

CoTryTask<void> StorageClientImpl::sendWriteRequest(ClientRequestContext &requestCtx,
                                                    WriteIO *writeIO,
                                                    const hf3fs::flat::NodeInfo &nodeInfo,
                                                    const flat::UserInfo &userInfo,
                                                    const WriteOptions &options) {
  // create write request and communicate with storage service

  hf3fs::storage::VersionedChainId vChainId{writeIO->routingTarget.chainId,
                                            hf3fs::storage::ChainVer(writeIO->routingTarget.chainVer)};
  hf3fs::storage::GlobalKey key{vChainId, hf3fs::storage::ChunkId(writeIO->chunkId)};

  size_t offset = writeIO->data - writeIO->buffer->data();
  auto iobuf = writeIO->buffer->subrange(offset, writeIO->length);

  bytes_per_operation.addSample(writeIO->length, requestCtx.requestTagSet);
  bytes_per_request.addSample(writeIO->length, requestCtx.requestTagSet);
  ops_per_request.addSample(1, requestCtx.requestTagSet);

  if (options.verifyChecksum()) {
    writeIO->checksum = FAULT_INJECTION_POINT(
        requestCtx.debugFlags.injectClientError(),
        ChecksumInfo::create(config_.chunk_checksum_type(), writeIO->data, std::max(writeIO->length / 2, 1U)),
        ChecksumInfo::create(config_.chunk_checksum_type(), writeIO->data, writeIO->length));
  }

  hf3fs::storage::RequestId requestId(writeIO->requestId);
  hf3fs::storage::MessageTag tag{clientId_, requestId, writeIO->routingTarget.channel};
  uint32_t featureFlags = buildFeatureFlagsFromOptions(options.debug());

  hf3fs::storage::UpdateIO payload{writeIO->offset,
                                   writeIO->length,
                                   writeIO->chunkSize,
                                   key,
                                   iobuf.toRemoteBuf(),
                                   hf3fs::storage::ChunkVer(0) /*updateVer*/,
                                   UpdateType::WRITE,
                                   writeIO->checksum};

  if (writeIO->length <= requestCtx.clientConfig.max_inline_write_bytes()) {
    payload.inlinebuf.data.assign(writeIO->data, writeIO->data + writeIO->length);
    BITFLAGS_SET(featureFlags, hf3fs::storage::FeatureFlags::SEND_DATA_INLINE);
  }

  hf3fs::storage::WriteReq request{payload, tag, requestCtx.retryCount, userInfo, featureFlags, requestCtx.debugFlags};

  auto response =
      co_await callMessengerMethod<WriteReq, WriteRsp, &StorageMessenger::write>(getStorageMessengerForUpdates(),
                                                                                 requestCtx,
                                                                                 nodeInfo,
                                                                                 request);

  if (!response) {
    XLOGF(ERR,
          "Cannot communicate with node {}, error: {}, usercall: #{}, write key: {}",
          nodeInfo.app.nodeId,
          response.error(),
          requestCtx.userCallId,
          key);
    setErrorCodeOfOp(writeIO, makeError(StatusCodeConversion::convertToStorageClientCode(response.error()).code()));
    co_return Void{};
  }

  setResultOfOp(writeIO, response->result);

  co_return Void{};
}

CoTryTask<void> StorageClientImpl::sendWriteRequestsSequentially(
    ClientRequestContext &requestCtx,
    const std::vector<WriteIO *> &writeIOs,
    std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo,
    NodeId nodeId,
    const flat::UserInfo &userInfo,
    const WriteOptions &options) {
  auto nodeInfo = getNodeInfo(routingInfo, hf3fs::flat::NodeId(nodeId));

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot get node info to communicate with {}", nodeId);
    setErrorCodeOfOps(writeIOs, nodeInfo.error().code());
    co_return makeError(nodeInfo.error().code());
  }

  status_code_t firstError = StatusCode::kOK;
  size_t totalDataLen = 0;
  size_t totalNumChunksUpdated = 0;
  auto startTime = hf3fs::SteadyClock::now();

  for (auto &writeIO : writeIOs) {
    co_await sendWriteRequest(requestCtx, writeIO, *nodeInfo, userInfo, options);

    XLOGF_IF(DFATAL,
             writeIO->statusCode() == StorageClientCode::kDuplicateUpdate,
             "[BUG] Operation {} {} has an unexpected status code: {}, routing target: {}",
             fmt::ptr(writeIO),
             writeIO->requestId,
             writeIO->statusCode(),
             writeIO->routingTarget);

    if (writeIO->statusCode() == StatusCode::kOK) {
      XLOGF_IF(INFO,
               requestCtx.retryCount > 0,
               "Write IO {} with id {} done, #{} retry, routing target: {}, chunk id: {}",
               fmt::ptr(writeIO),
               writeIO->requestId,
               requestCtx.retryCount,
               writeIO->routingTarget,
               writeIO->chunkId);
      totalDataLen += writeIO->resultLen();
      totalNumChunksUpdated += writeIO->numProcessedChunks();
      releaseChannelsForOp(chanAllocator_, writeIO);
    } else {
      firstError = writeIO->statusCode();
      break;
    }
  }

  Duration elapsedTime(hf3fs::SteadyClock::now() - startTime);
  double bytesPerSec = requestCtx.computeBandwidth(totalDataLen, elapsedTime);
  if (bytesPerSec > 0.0) request_bw.addSample(bytesPerSec, requestCtx.requestTagSet);

  data_payload_bytes.addSample(totalDataLen, requestCtx.requestTagSet);
  data_payload_bytes_per_user.addSample(totalDataLen, requestCtx.userTagSet);
  num_processed_chunks.addSample(totalNumChunksUpdated, requestCtx.requestTagSet);
  num_processed_chunks_per_user.addSample(totalNumChunksUpdated, requestCtx.userTagSet);

  if (firstError == StatusCode::kOK) {
    co_return Void{};
  } else {
    co_return makeError(firstError);
  }
}

CoTryTask<void> StorageClientImpl::write(WriteIO &writeIO,
                                         const flat::UserInfo &userInfo,
                                         const WriteOptions &options) {
  ClientRequestContext requestCtx(MethodType::write, userInfo, options.debug(), config_);
  std::vector<WriteIO *> writeIOs{&writeIO};
  std::vector<WriteIO *> failedIOs;
  co_return co_await batchWriteWithRetry(requestCtx, writeIOs, userInfo, options, failedIOs);
}

// query last chunk

template <typename Op>
std::vector<Op *> validateChunkIdRange(const std::vector<Op *> &ops) {
  std::vector<Op *> validOps;
  validOps.reserve(ops.size());

  for (size_t opIndex = 0; opIndex < ops.size(); opIndex++) {
    auto op = ops[opIndex];
    if (op->range.begin > op->range.end || op->range.maxNumChunkIdsToProcess == 0) {
      XLOGF(ERR,
            "#{}/{} Invalid chunk id range: begin {} > end {} or maxNumChunkIdsToProcess == 0",
            opIndex + 1,
            ops.size(),
            op->range.begin,
            op->range.end,
            op->range.maxNumChunkIdsToProcess);
      setErrorCodeOfOp(op, StorageClientCode::kInvalidArg);
    } else {
      validOps.push_back(op);
    }
  }

  return validOps;
}

CoTryTask<void> StorageClientImpl::queryLastChunk(std::span<QueryLastChunkOp> ops,
                                                  const flat::UserInfo &userInfo,
                                                  const ReadOptions &options,
                                                  std::vector<QueryLastChunkOp *> *failedOps) {
  ClientRequestContext requestCtx(MethodType::queryLastChunk, userInfo, options.debug(), config_, ops.size());

  auto opsPtrs = createVectorOfPtrsFromOps(ops);

  std::vector<QueryLastChunkOp *> failedIOVec;
  if (failedOps == nullptr) failedOps = &failedIOVec;

  auto sendOps = [ this, &requestCtx, userInfo, options ](const std::vector<QueryLastChunkOp *> &ops) -> auto{
    return queryLastChunkWithoutRetry(requestCtx, ops, userInfo, options);
  };

  auto validOps = validateChunkIdRange(opsPtrs);

  if (validOps.empty()) {
    XLOGF(ERR, "Empty list of ops: all the {} query ops are invalid", opsPtrs.size());
    goto exit;
  }

  co_await sendOpsWithRetry<QueryLastChunkOp>(requestCtx,
                                              chanAllocator_,
                                              opsPtrs,
                                              sendOps,
                                              config_.retry().mergeWith(options.retry()));

exit:
  collectFailedOps(opsPtrs, *failedOps, requestCtx);
  reportNumFailedOps(*failedOps, requestCtx);
  co_return Void{};
}

CoTryTask<void> StorageClientImpl::queryLastChunkWithoutRetry(ClientRequestContext &requestCtx,
                                                              const std::vector<QueryLastChunkOp *> &ops,
                                                              const flat::UserInfo &userInfo,
                                                              const ReadOptions &options) {
  // collect target/chain infos

  TargetSelectionOptions targetSelectionOptions = options.targetSelection();
  targetSelectionOptions.set_mode(options.targetSelection().mode() == TargetSelectionMode::Default
                                      ? TargetSelectionMode::HeadTarget
                                      : options.targetSelection().mode());

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();
  auto targetedOps = selectRoutingTargetForOps(requestCtx, routingInfo, targetSelectionOptions, ops);

  // select head target for each operation and group by node id

  auto batches = groupOpsByNodeId(requestCtx,
                                  targetedOps,
                                  config_.traffic_control().query().max_batch_size(),
                                  SIZE_MAX,
                                  config_.traffic_control().query().random_shuffle_requests());

  // create batch request and communicate with storage service

  auto sendReq = [&, this](size_t batchIndex,
                           const NodeId &nodeId,
                           const std::vector<QueryLastChunkOp *> &batchOps) -> CoTask<bool> {
    SemaphoreGuard perServerReq(*queryConcurrencyLimit_.getPerServerSemaphore(nodeId));
    co_await perServerReq.coWait();

    SemaphoreGuard concurrentReq(queryConcurrencyLimit_.getConcurrencySemaphore());
    co_await concurrentReq.coWait();

    if (!isLatestRoutingInfo(routingInfo, batchOps)) {
      setErrorCodeOfOps(batchOps, StorageClientCode::kRoutingVersionMismatch);
      co_return false;
    }

    // log the waiting time before communication starts
    requestCtx.logWaitingTime();

    auto batchReq = buildBatchRequest<QueryLastChunkOp, QueryLastChunkReq>(requestCtx,
                                                                           clientId_,
                                                                           nextRequestId_,
                                                                           config_,
                                                                           options,
                                                                           userInfo,
                                                                           batchOps);

    auto response = co_await sendBatchRequest<QueryLastChunkOp,
                                              QueryLastChunkReq,
                                              QueryLastChunkRsp,
                                              &StorageMessenger::queryLastChunk>(messenger_,
                                                                                 requestCtx,
                                                                                 routingInfo,
                                                                                 nodeId,
                                                                                 batchReq,
                                                                                 batchOps);

    co_return bool(response);
  };

  bool parallelProcessing = config_.traffic_control().query().process_batches_in_parallel();
  co_await processBatches<QueryLastChunkOp>(batches, sendReq, parallelProcessing);

  co_return Void{};
}

// remove chunks

CoTryTask<void> StorageClientImpl::removeChunks(std::span<RemoveChunksOp> ops,
                                                const flat::UserInfo &userInfo,
                                                const WriteOptions &options,
                                                std::vector<RemoveChunksOp *> *failedOps) {
  ClientRequestContext requestCtx(MethodType::removeChunks, userInfo, options.debug(), config_, ops.size());

  auto opsPtrs = createVectorOfPtrsFromOps(ops);
  std::vector<RemoveChunksOp *> failedIOVec;
  if (failedOps == nullptr) failedOps = &failedIOVec;

  auto sendOps = [ this, &requestCtx, userInfo, options ](const std::vector<RemoveChunksOp *> &ops) -> auto{
    return removeChunksWithoutRetry(requestCtx, ops, userInfo, options);
  };

  auto validOps = validateChunkIdRange(opsPtrs);

  if (validOps.empty()) {
    XLOGF(ERR, "Empty list of ops: all the {} remove ops are invalid", opsPtrs.size());
    goto exit;
  }

  co_await sendOpsWithRetry<RemoveChunksOp>(requestCtx,
                                            chanAllocator_,
                                            validOps,
                                            sendOps,
                                            config_.retry().mergeWith(options.retry()));

exit:
  collectFailedOps(opsPtrs, *failedOps, requestCtx);
  reportNumFailedOps(*failedOps, requestCtx);
  co_return Void{};
}

CoTryTask<void> StorageClientImpl::removeChunksWithoutRetry(ClientRequestContext &requestCtx,
                                                            const std::vector<RemoveChunksOp *> &ops,
                                                            const flat::UserInfo &userInfo,
                                                            const WriteOptions &options) {
  // collect target/chain infos

  TargetSelectionOptions targetSelectionOptions = options.targetSelection();
  targetSelectionOptions.set_mode(options.targetSelection().mode() == TargetSelectionMode::Default
                                      ? TargetSelectionMode::HeadTarget
                                      : options.targetSelection().mode());

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();
  auto targetedOps = selectRoutingTargetForOps(requestCtx, routingInfo, targetSelectionOptions, ops);

  // select head target for each operation and group by node id

  auto batches = groupOpsByNodeId(requestCtx,
                                  targetedOps,
                                  config_.traffic_control().remove().max_batch_size(),
                                  SIZE_MAX,
                                  config_.traffic_control().remove().random_shuffle_requests());

  // create batch request and communicate with storage service

  auto sendReq = [&, this](size_t batchIndex,
                           const NodeId &nodeId,
                           const std::vector<RemoveChunksOp *> &batchOps) -> CoTask<bool> {
    SemaphoreGuard perServerReq(*removeConcurrencyLimit_.getPerServerSemaphore(nodeId));
    co_await perServerReq.coWait();

    SemaphoreGuard concurrentReq(removeConcurrencyLimit_.getConcurrencySemaphore());
    co_await concurrentReq.coWait();

    if (!isLatestRoutingInfo(routingInfo, batchOps)) {
      setErrorCodeOfOps(batchOps, StorageClientCode::kRoutingVersionMismatch);
      co_return false;
    }

    if (!allocateChannelsForOps(chanAllocator_, batchOps, true /*reallocate*/)) {
      XLOGF(WARN, "Cannot allocate channel ids for {} ops, first op {}", batchOps.size(), fmt::ptr(batchOps.front()));
      setErrorCodeOfOps(batchOps, StorageClientCode::kResourceBusy);
      co_return false;
    }

    // log the waiting time before communication starts
    requestCtx.logWaitingTime();

    auto batchReq = buildBatchRequest<RemoveChunksOp, RemoveChunksReq>(requestCtx,
                                                                       clientId_,
                                                                       nextRequestId_,
                                                                       config_,
                                                                       options,
                                                                       userInfo,
                                                                       batchOps);

    auto response =
        co_await sendBatchRequest<RemoveChunksOp, RemoveChunksReq, RemoveChunksRsp, &StorageMessenger::removeChunks>(
            getStorageMessengerForUpdates(),
            requestCtx,
            routingInfo,
            nodeId,
            batchReq,
            batchOps);

    co_return bool(response);
  };

  bool parallelProcessing = config_.traffic_control().remove().process_batches_in_parallel();
  co_await processBatches<RemoveChunksOp>(batches, sendReq, parallelProcessing);

  co_return Void{};
}

// truncate chunks

template <typename TruncateChunkOp>
std::vector<TruncateChunkOp *> validateChunkSize(const std::vector<TruncateChunkOp *> &ops) {
  std::vector<TruncateChunkOp *> validOps;
  validOps.reserve(ops.size());

  for (size_t opIndex = 0; opIndex < ops.size(); opIndex++) {
    auto op = ops[opIndex];
    if (op->chunkLen > op->chunkSize) {
      XLOGF(ERR,
            "#{}/{} Invalid chunk length/size: length {} > size {}",
            opIndex + 1,
            ops.size(),
            op->chunkLen,
            op->chunkSize);
      setErrorCodeOfOp(op, StorageClientCode::kInvalidArg);
    } else {
      validOps.push_back(op);
    }
  }

  return validOps;
}

CoTryTask<void> StorageClientImpl::truncateChunks(std::span<TruncateChunkOp> ops,
                                                  const flat::UserInfo &userInfo,
                                                  const WriteOptions &options,
                                                  std::vector<TruncateChunkOp *> *failedOps) {
  ClientRequestContext requestCtx(MethodType::truncateChunks, userInfo, options.debug(), config_, ops.size());

  auto opsPtrs = createVectorOfPtrsFromOps(ops);
  std::vector<TruncateChunkOp *> failedIOVec;
  if (failedOps == nullptr) failedOps = &failedIOVec;

  auto sendOps = [ this, &requestCtx, userInfo, options ](const std::vector<TruncateChunkOp *> &ops) -> auto{
    return truncateChunksWithoutRetry(requestCtx, ops, userInfo, options);
  };

  auto validOps = validateChunkSize(opsPtrs);

  if (validOps.empty()) {
    XLOGF(ERR, "Empty list of ops: all the {} truncate ops are invalid", opsPtrs.size());
    goto exit;
  }

  co_await sendOpsWithRetry<TruncateChunkOp>(requestCtx,
                                             chanAllocator_,
                                             validOps,
                                             sendOps,
                                             config_.retry().mergeWith(options.retry()));

exit:
  collectFailedOps(opsPtrs, *failedOps, requestCtx);
  reportNumFailedOps(*failedOps, requestCtx);
  co_return Void{};
}

CoTryTask<void> StorageClientImpl::truncateChunksWithoutRetry(ClientRequestContext &requestCtx,
                                                              const std::vector<TruncateChunkOp *> &ops,
                                                              const flat::UserInfo &userInfo,
                                                              const WriteOptions &options) {
  // collect target/chain infos

  TargetSelectionOptions targetSelectionOptions = options.targetSelection();
  targetSelectionOptions.set_mode(options.targetSelection().mode() == TargetSelectionMode::Default
                                      ? TargetSelectionMode::HeadTarget
                                      : options.targetSelection().mode());

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();
  auto targetedOps = selectRoutingTargetForOps(requestCtx, routingInfo, targetSelectionOptions, ops);

  // select head target for each operation and group by node id

  auto batches = groupOpsByNodeId(requestCtx,
                                  targetedOps,
                                  config_.traffic_control().truncate().max_batch_size(),
                                  SIZE_MAX,
                                  config_.traffic_control().truncate().random_shuffle_requests());

  // create batch request and communicate with storage service

  auto sendReq = [&, this](size_t batchIndex,
                           const NodeId &nodeId,
                           const std::vector<TruncateChunkOp *> &batchOps) -> CoTask<bool> {
    SemaphoreGuard perServerReq(*truncateConcurrencyLimit_.getPerServerSemaphore(nodeId));
    co_await perServerReq.coWait();

    SemaphoreGuard concurrentReq(truncateConcurrencyLimit_.getConcurrencySemaphore());
    co_await concurrentReq.coWait();

    if (!isLatestRoutingInfo(routingInfo, batchOps)) {
      setErrorCodeOfOps(batchOps, StorageClientCode::kRoutingVersionMismatch);
      co_return false;
    }

    if (!allocateChannelsForOps(chanAllocator_, batchOps, false /*reallocate*/)) {
      XLOGF(WARN, "Cannot allocate channel ids for {} ops, first op {}", batchOps.size(), fmt::ptr(batchOps.front()));
      setErrorCodeOfOps(batchOps, StorageClientCode::kResourceBusy);
      co_return false;
    }

    // log the waiting time before communication starts
    requestCtx.logWaitingTime();

    auto batchReq = buildBatchRequest<TruncateChunkOp, TruncateChunksReq>(requestCtx,
                                                                          clientId_,
                                                                          nextRequestId_,
                                                                          config_,
                                                                          options,
                                                                          userInfo,
                                                                          batchOps);

    auto response = co_await sendBatchRequest<TruncateChunkOp,
                                              TruncateChunksReq,
                                              TruncateChunksRsp,
                                              &StorageMessenger::truncateChunks>(getStorageMessengerForUpdates(),
                                                                                 requestCtx,
                                                                                 routingInfo,
                                                                                 nodeId,
                                                                                 batchReq,
                                                                                 batchOps);

    co_return bool(response);
  };

  bool parallelProcessing = config_.traffic_control().truncate().process_batches_in_parallel();
  co_await processBatches<TruncateChunkOp>(batches, sendReq, parallelProcessing);

  co_return Void{};
}

CoTryTask<SpaceInfoRsp> StorageClientImpl::querySpaceInfo(NodeId nodeId) {
  ClientRequestContext requestCtx(MethodType::querySpaceInfo,
                                  flat::UserInfo(),
                                  DebugOptions(),
                                  config_,
                                  1 /*numOps*/,
                                  0 /*retryCount*/,
                                  config_.retry().max_wait_time() /*requestTimeout*/);

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();

  auto nodeInfo = getNodeInfo(routingInfo, hf3fs::flat::NodeId(nodeId));

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot get node info to communicate with {}", nodeId);
    co_return makeError(StorageClientCode::kRoutingError);
  }

  SpaceInfoReq req;
  co_return co_await callMessengerMethod<SpaceInfoReq, SpaceInfoRsp, &StorageMessenger::querySpaceInfo>(messenger_,
                                                                                                        requestCtx,
                                                                                                        *nodeInfo,
                                                                                                        req);
}

CoTryTask<CreateTargetRsp> StorageClientImpl::createTarget(NodeId nodeId, const CreateTargetReq &req) {
  ClientRequestContext requestCtx(MethodType::createTarget,
                                  flat::UserInfo(),
                                  DebugOptions(),
                                  config_,
                                  1 /*numOps*/,
                                  0 /*retryCount*/,
                                  config_.retry().max_wait_time() /*requestTimeout*/);

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();

  auto nodeInfo = getNodeInfo(routingInfo, hf3fs::flat::NodeId(nodeId));

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot get node info to communicate with {}", nodeId);
    co_return makeError(StorageClientCode::kRoutingError);
  }

  co_return co_await callMessengerMethod<CreateTargetReq, CreateTargetRsp, &StorageMessenger::createTarget>(messenger_,
                                                                                                            requestCtx,
                                                                                                            *nodeInfo,
                                                                                                            req);
}

CoTryTask<OfflineTargetRsp> StorageClientImpl::offlineTarget(NodeId nodeId, const OfflineTargetReq &req) {
  ClientRequestContext requestCtx(MethodType::offlineTarget,
                                  flat::UserInfo(),
                                  DebugOptions(),
                                  config_,
                                  1 /*numOps*/,
                                  0 /*retryCount*/,
                                  config_.retry().max_wait_time() /*requestTimeout*/);

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();

  auto nodeInfo = getNodeInfo(routingInfo, hf3fs::flat::NodeId(nodeId));

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot get node info to communicate with {}", nodeId);
    co_return makeError(StorageClientCode::kRoutingError);
  }

  co_return co_await callMessengerMethod<OfflineTargetReq, OfflineTargetRsp, &StorageMessenger::offlineTarget>(
      messenger_,
      requestCtx,
      *nodeInfo,
      req);
}

CoTryTask<RemoveTargetRsp> StorageClientImpl::removeTarget(NodeId nodeId, const RemoveTargetReq &req) {
  ClientRequestContext requestCtx(MethodType::removeTarget,
                                  flat::UserInfo(),
                                  DebugOptions(),
                                  config_,
                                  1 /*numOps*/,
                                  0 /*retryCount*/,
                                  config_.retry().max_wait_time() /*requestTimeout*/);

  std::shared_ptr<hf3fs::client::RoutingInfo const> routingInfo = getCurrentRoutingInfo();

  auto nodeInfo = getNodeInfo(routingInfo, hf3fs::flat::NodeId(nodeId));

  if (!nodeInfo) {
    XLOGF(ERR, "Cannot get node info to communicate with {}", nodeId);
    co_return makeError(StorageClientCode::kRoutingError);
  }

  co_return co_await callMessengerMethod<RemoveTargetReq, RemoveTargetRsp, &StorageMessenger::removeTarget>(messenger_,
                                                                                                            requestCtx,
                                                                                                            *nodeInfo,
                                                                                                            req);
}

CoTryTask<std::vector<Result<QueryChunkRsp>>> StorageClientImpl::queryChunk(const QueryChunkReq &req) {
  ClientRequestContext requestCtx(MethodType::queryChunk,
                                  flat::UserInfo(),
                                  DebugOptions(),
                                  config_,
                                  1 /*numOps*/,
                                  0 /*retryCount*/,
                                  config_.retry().max_wait_time() /*requestTimeout*/);

  auto routingInfo = getCurrentRoutingInfo();
  auto chainInfoResult = getChainInfo(routingInfo, req.chainId);
  CO_RETURN_ON_ERROR(chainInfoResult);
  auto &chainInfo = *chainInfoResult;

  std::vector<Result<QueryChunkRsp>> rsp;
  for (auto &target : chainInfo.targets) {
    auto targetInfo = getTargetInfo(routingInfo, target.targetId);
    if (UNLIKELY(!targetInfo)) {
      rsp.push_back(makeError(targetInfo.error()));
      continue;
    }
    if (!targetInfo->nodeId.has_value()) {
      auto msg = fmt::format("target {} without node id", *targetInfo);
      XLOG(ERR, msg);
      rsp.push_back(makeError(StorageClientCode::kRoutingError, std::move(msg)));
      continue;
    }

    auto nodeInfo = getNodeInfo(routingInfo, *targetInfo->nodeId);
    if (!nodeInfo) {
      auto msg = fmt::format("target {} get node info failed", *targetInfo);
      XLOG(ERR, msg);
      rsp.push_back(makeError(StorageClientCode::kRoutingError, std::move(msg)));
      continue;
    }

    rsp.push_back(co_await callMessengerMethod<QueryChunkReq, QueryChunkRsp, &StorageMessenger::queryChunk>(messenger_,
                                                                                                            requestCtx,
                                                                                                            *nodeInfo,
                                                                                                            req));
  }
  co_return rsp;
}

CoTryTask<ChunkMetaVector> StorageClientImpl::getAllChunkMetadata(const ChainId &chainId, const TargetId &targetId) {
  ClientRequestContext requestCtx(MethodType::getAllChunkMetadata,
                                  flat::UserInfo(),
                                  DebugOptions(),
                                  config_,
                                  1 /*numOps*/,
                                  0 /*retryCount*/,
                                  config_.retry().max_wait_time() /*requestTimeout*/);

  auto routingInfo = getCurrentRoutingInfo();
  auto chainInfo = getChainInfo(routingInfo, chainId);
  CO_RETURN_ON_ERROR(chainInfo);

  auto foundTarget = std::any_of(chainInfo->targets.begin(), chainInfo->targets.end(), [targetId](const auto &info) {
    return info.targetId == targetId;
  });

  if (!foundTarget) {
    co_return makeError(StorageClientCode::kInvalidArg,
                        fmt::format("Cannot find target {} on chain {}", targetId, chainId));
  }

  auto targetInfo = getTargetInfo(routingInfo, targetId);
  CO_RETURN_ON_ERROR(targetInfo);

  if (!targetInfo->nodeId) {
    XLOGF(ERR,
          "Host node id of target {} on {}@{} is unknown, routing version {}: {}",
          targetId,
          chainInfo->chainId,
          chainInfo->chainVersion,
          routingInfo->raw()->routingInfoVersion,
          targetInfo);
    co_return makeError(StorageClientCode::kRoutingError);
  }

  auto nodeInfo = getNodeInfo(routingInfo, *targetInfo->nodeId);
  CO_RETURN_ON_ERROR(nodeInfo);

  GetAllChunkMetadataReq req{.targetId = targetId};
  auto response =
      co_await callMessengerMethod<GetAllChunkMetadataReq,
                                   GetAllChunkMetadataRsp,
                                   &StorageMessenger::getAllChunkMetadata>(messenger_, requestCtx, *nodeInfo, req);
  CO_RETURN_ON_ERROR(response);

  co_return response->chunkMetaVec;
}

}  // namespace hf3fs::storage::client
