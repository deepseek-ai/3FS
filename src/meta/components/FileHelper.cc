#include "meta/components/FileHelper.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <fmt/core.h>
#include <folly/Likely.h>
#include <folly/Math.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <linux/fs.h>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "client/storage/StorageClient.h"
#include "common/app/NodeId.h"
#include "common/monitor/Recorder.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/FileOperation.h"
#include "fbs/meta/Schema.h"

#define GET_RAW_ROUTING_INFO()                                  \
  auto routingInfo = mgmtdClient_->getRoutingInfo();            \
  if (!routingInfo || !routingInfo->raw()) {                    \
    XLOGF(ERR, "RoutingInfo not ready");                        \
    co_return makeError(MgmtdClientCode::kRoutingInfoNotReady); \
  }                                                             \
  auto rawRoutingInfo = routingInfo->raw()

namespace hf3fs::meta::server {

namespace {

FileOperation::Recorder recorder("meta_server");

template <typename T>
constexpr folly::ordering compare(const T &a, const T &b) {
  return a < b ? folly::ordering::lt : a > b ? folly::ordering::gt : folly::ordering::eq;
}

auto findMinMax(auto &&map) {
  auto min = map.begin();
  auto max = map.begin();
  for (auto iter = std::next(map.begin()); iter != map.end(); iter++) {
    if (min->second > iter->second) {
      min = iter;
    }
    if (max->second < iter->second) {
      max = iter;
    }
  }
  return std::make_pair(min, max);
}

}  // namespace

void FileHelper::start(CPUExecutorGroup &exec) {
  bgRunner_ = std::make_unique<BackgroundRunner>(&exec.pickNext());
  bgRunner_->start(
      "statFs",
      [&]() -> CoTask<void> {
        auto cached = *cachedFsStatus_.rlock();
        if (cached.status_.has_value() && RelativeTime::now() - cached.update_ < config_.statfs_update_interval() &&
            RelativeTime::now() - cached.update_ < config_.statfs_cache_time()) {
          // don't need update statFs
          co_return;
        }
        co_await updateStatFs();
      },
      []() { return 200_ms; });
}

void FileHelper::stopAndJoin() {
  if (bgRunner_) {
    folly::coro::blockingWait(bgRunner_->stopAll());
    bgRunner_.reset();
  }
}

CoTryTask<uint64_t> FileHelper::queryLength(const UserInfo &userInfo, const Inode &inode, bool *hasHole) {
  GET_RAW_ROUTING_INFO();
  FileOperation fop(*storageClient_, *rawRoutingInfo, userInfo, inode, recorder);
  auto queryResult = co_await fop.queryChunks(hasHole != nullptr, config_.dynamic_stripe());
  CO_RETURN_ON_ERROR(queryResult);

#define QUERY_DETAIL                                                                                            \
  "file {}, length {}, chunk num {}, total chunk length {}, total chunk num {}", inode.id, queryResult->length, \
      chunkNum, queryResult->totalChunkLen, queryResult->totalNumChunks

  if (hasHole != nullptr) {
    auto chunkNum = folly::divCeil(queryResult->length, inode.asFile().layout.chunkSize.u64());
    *hasHole = queryResult->length != queryResult->totalChunkLen || chunkNum != queryResult->totalNumChunks;
    if (*hasHole) {
      XLOGF(ERR, "FileHelper found hole in " QUERY_DETAIL);
    } else {
      XLOGF(DBG, "FileHelper check hole for " QUERY_DETAIL);
    }
  }
  XLOGF(DBG, "FileHelper query length for {}, {}.", inode.id, queryResult->length);
#undef QUERY_DETAIL

  co_return queryResult->length;
}

CoTryTask<size_t> FileHelper::remove(const UserInfo &userInfo,
                                     const Inode &inode,
                                     RetryOptions retry,
                                     uint32_t removeChunksBatchSize) {
  if (inode.acl.iflags & FS_IMMUTABLE_FL) {
    auto msg = fmt::format("try remove file {} with FS_IMMUTABLE_FL", inode.id);
    XLOG(DFATAL, msg);
    co_return makeError(MetaCode::kFoundBug, msg);
  }

  GET_RAW_ROUTING_INFO();
  FileOperation fop(*storageClient_, *rawRoutingInfo, userInfo, inode, recorder);
  size_t total = 0;
  while (true) {
    auto result = co_await fop.removeChunks(0, removeChunksBatchSize, config_.dynamic_stripe(), retry);
    CO_RETURN_ON_ERROR(result);
    auto [removed, more] = *result;
    total += removed;
    if (!more) {
      break;
    }
    XLOGF(DBG, "File {} has more chunks to remove after removed {} chunks", inode.id, removed);
  }
  co_return total;
}

CoTryTask<FsStatus> FileHelper::statFs(const UserInfo &userInfo, std::chrono::milliseconds cacheDuration) {
  auto cached = *cachedFsStatus_.rlock();
  if (!cached.status_.has_value() || RelativeTime::now() - cached.update_ > config_.statfs_cache_time()) {
    if (UNLIKELY(!bgRunner_)) {
      XLOGF(DFATAL, "FileHelper not started");
      co_return makeError(MetaCode::kFoundBug, "FileHelper not started!");
    }
    co_return makeError(StorageClientCode::kResourceBusy, "cached statfs outdate, try again");
  }
  co_return *cached.status_;
}

CoTryTask<void> FileHelper::updateStatFs() {
  static constexpr double kTiB = 1ULL << 40;

  std::vector<folly::SemiFuture<Result<storage::SpaceInfoRsp>>> reqs;
  auto nodes = mgmtdClient_->getRoutingInfo()->getNodeBy(flat::selectNodeByType(flat::NodeType::STORAGE) &&
                                                         flat::selectActiveNode());
  for (auto &node : nodes) {
    auto req = storageClient_->querySpaceInfo(storage::NodeId(node.app.nodeId)).semi();
    reqs.push_back(std::move(req));
  }

  uint64_t cap = 0;
  uint64_t free = 0;
  auto results = co_await folly::coro::collectAllRange(std::move(reqs));
  std::map<flat::NodeId, size_t> nodesFree;
  std::map<std::pair<flat::NodeId, std::string>, size_t> pathFree;
  for (size_t i = 0; i < nodes.size(); i++) {
    const auto &node = nodes.at(i);
    const auto &result = results.at(i);
    if (result.hasError()) {
      XLOGF(ERR, "FileHelper statFs: failed to querySpaceInfo of {}, error {}", node, result.error());
      continue;
    }

    const auto &rsp = *result;
    for (const auto &space : rsp.spaceInfos) {
      XLOGF(DBG,
            "FileHelper statFs: node {}, path {}, cap {:.1f}TiB, free {:.1f}TiB",
            node.app.nodeId,
            space.path,
            space.capacity / kTiB,
            space.free / kTiB);
      cap += space.capacity;
      free += std::min(space.free, space.capacity);
      nodesFree[node.app.nodeId] += space.free;
      pathFree[std::make_pair(node.app.nodeId, space.path)] = space.free;
    }
  }

  // log space info
  if (!pathFree.empty()) {
    auto status = cachedFsStatus_.rlock()->status_;
    if (status) {
      XLOGF(INFO,
            "FileHelper statFs: cap {:.1f}TiB, free {:.1f}TiB, prev free {:.1f}TiB, free diff {:.1f}TiB",
            cap / kTiB,
            free / kTiB,
            status->free / kTiB,
            ((int64_t)free - (int64_t)status->free) / kTiB);
    } else {
      XLOGF(INFO, "FileHelper statFs: cap {:.1f}TiB, free {:.1f}TiB", cap / kTiB, free / kTiB);
    }

    auto threshold = config_.statfs_space_imbalance_threshold();
    auto [minNode, maxNode] = findMinMax(nodesFree);
    auto avgNodeCap = cap / nodesFree.size();
    auto nodeDiff = (double)(maxNode->second - minNode->second) / avgNodeCap * 100.0;
    auto nodeMsg = fmt::format("{} {:.1f}TiB free, {} {:.1f}TiB free, avgNodeCap {:.1f}TiB, diff {:.3f}%",
                               minNode->first,
                               minNode->second / kTiB,
                               maxNode->first,
                               maxNode->second / kTiB,
                               avgNodeCap / kTiB,
                               nodeDiff);
    if (nodeDiff > threshold) {
      XLOGF(WARN, "FileHelper statFs: node space utilization imbalance, {}", nodeMsg);
    } else {
      XLOGF(INFO, "FileHelper statFs: {}", nodeMsg);
    }

    auto [minPath, maxPath] = findMinMax(pathFree);
    auto avgPathCap = cap / pathFree.size();
    auto pathDiff = (double)(maxPath->second - minPath->second) / avgPathCap * 100.0;
    auto pathMsg = fmt::format("{}:{} {:.1f}TiB free, {}:{} {:.1f}TiB free, avgPathCap {:.1f}TiB, diff {:.3f}%",
                               minPath->first.first,
                               minPath->first.second,
                               minPath->second / kTiB,
                               maxPath->first.first,
                               maxPath->first.second,
                               maxPath->second / kTiB,
                               avgPathCap / kTiB,
                               pathDiff);
    if (pathDiff > threshold) {
      XLOGF(WARN, "FileHelper statFs: disk space utilization imbalance, {}", pathMsg);
    } else {
      XLOGF(INFO, "FileHelper statFs: {}", pathMsg);
    }
  }

  auto guard = cachedFsStatus_.wlock();
  guard->update_ = RelativeTime::now();
  guard->status_ = FsStatus(cap, cap - free, free);

  co_return Void{};
}
}  // namespace hf3fs::meta::server
