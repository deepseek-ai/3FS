#include "FileOperation.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <folly/Math.h>
#include <folly/logging/xlog.h>
#include <limits>
#include <type_traits>

#include "common/utils/Result.h"
#include "fmt/core.h"

#define RECORD_LATENCY(name)                                                                              \
  auto FB_ANONYMOUS_VARIABLE(guard) = folly::makeGuard([this, begin = std::chrono::steady_clock::now()] { \
    if (recorder_.has_value()) {                                                                          \
      recorder_->get().name.addSample(std::chrono::steady_clock::now() - begin);                          \
    }                                                                                                     \
  })

#define ADD_SAMPLE(name, count)             \
  if (recorder_.has_value()) {              \
    recorder_->get().name.addSample(count); \
  }

#define ADD_FAILED(name, code)                          \
  if (recorder_.has_value()) {                          \
    auto tag = folly::to<std::string>(code);            \
    recorder_->get().name.addSample(1, {{"tag", tag}}); \
  }

#define CHECK_FILE(inode)                                                                                            \
  do {                                                                                                               \
    if (!inode.isFile()) {                                                                                           \
      co_return makeError(MetaCode::kNotFile, "Not file");                                                           \
    }                                                                                                                \
    if (auto valid = inode.asFile().layout.valid(false); valid.hasError()) {                                         \
      XLOGF(CRITICAL, "File {} has a invalid layout {}, error {}!", inode.id, inode.asFile().layout, valid.error()); \
      CO_RETURN_ERROR(valid);                                                                                        \
    }                                                                                                                \
  } while (0)

namespace hf3fs::meta {

CoTryTask<FileOperation::QueryResult> FileOperation::queryChunks(bool queryTotalChunk, bool dynStripe) {
  auto chains = co_await queryChunksByChain(queryTotalChunk, dynStripe);
  CO_RETURN_ON_ERROR(chains);
  QueryResult result;
  for (auto iter = chains->begin(); iter != chains->end(); iter++) {
    auto cresult = iter->second;
    XLOGF_IF(DFATAL,
             (result.lastChunk == cresult.lastChunk && result.lastChunk != 0),
             "inode {}, {} == {}",
             inode_.id,
             result.lastChunk,
             cresult.lastChunk);
    XLOGF(DBG, "queryChunks for {}, chain {}, length {}", inode_.id, iter->first, iter->second.length);
    if (iter == chains->begin() || result.length < cresult.length) {
      result.length = cresult.length;
      result.lastChunk = cresult.lastChunk;
      result.lastChunkLen = cresult.lastChunkLen;
    }
    result.totalChunkLen += cresult.totalChunkLen;
    result.totalNumChunks += cresult.totalNumChunks;
  }

  auto length = result.lastChunk * inode_.asFile().layout.chunkSize + result.lastChunkLen;
  static_assert(std::is_same_v<decltype(length), uint64_t>);
  XLOGF_IF(DFATAL, length != result.length, "inode {}, {} != {}", inode_.id, length, result.length);
  if (length != result.length) {
    co_return makeError(MetaCode::kFoundBug,
                        fmt::format("inode {}, length {} != {}", inode_.id, length, result.length));
  }

  co_return result;
}

CoTryTask<std::map<flat::ChainId, FileOperation::QueryResult>> FileOperation::queryChunksByChain(bool queryTotalChunk,
                                                                                                 bool dynStripe) {
  CHECK_FILE(inode_);

  XLOGF(DBG, "query length of {}", inode_.id);

  auto chunkRange = ChunkId::range(inode_.id);
  CO_RETURN_AND_LOG_ON_ERROR(chunkRange);
  auto [begin, end] = *chunkRange;
  std::vector<storage::client::QueryLastChunkOp> queries;
  auto maxStripe = inode_.asFile().layout.stripeSize;
  if (dynStripe && inode_.asFile().dynStripe) {
    maxStripe = std::min(inode_.asFile().dynStripe, maxStripe);
    XLOGF(DBG, "file {}, dynStripe {}", inode_, inode_.asFile().dynStripe);
  }
  queries.reserve(maxStripe);

  const auto &layout = inode_.asFile().layout;
  auto chainIndexList = layout.getChainIndexList();
  for (size_t stripe = 0; stripe < maxStripe; stripe++) {
    auto chainIndex = chainIndexList[stripe];
    auto ref = flat::ChainRef{layout.tableId, layout.tableVersion, chainIndex};
    auto cid = routing_.getChainId(ref);
    if (!cid) {
      XLOGF(ERR, "Failed to get ChainId by {}", ref);
      co_return makeError(MgmtdClientCode::kRoutingInfoNotReady, fmt::format("Failed to get ChainId by {}", ref));
    }

    auto query = storage_.createQueryOp(storage::ChainId(*cid),
                                        storage::ChunkId(begin),
                                        storage::ChunkId(end),
                                        queryTotalChunk ? UINT32_MAX : 1);
    queries.push_back(std::move(query));
  }
  assert(queries.size() <= layout.stripeSize);

  auto guard = folly::makeGuard([this, queryTotalChunk, begin = std::chrono::steady_clock::now()] {
    if (recorder_.has_value()) {
      auto duration = std::chrono::steady_clock::now() - begin;
      if (queryTotalChunk) {
        recorder_->get().queryTotalChunkLatency.addSample(duration);
      } else {
        recorder_->get().queryLastChunkLatency.addSample(duration);
      }
    }
  });
  auto queryResult = co_await storage_.queryLastChunk(queries, userInfo_);

  std::map<flat::ChainId, FileOperation::QueryResult> chunks;
  for (auto &query : queries) {
    auto &result = query.result;
    if (UNLIKELY(result.statusCode.hasError())) {
      if (result.statusCode.error().code() == StorageClientCode::kChunkNotFound) {
        continue;
      }

      XLOGF(ERR,
            "Failed to queryLastChunk from chain {}, err {}",
            flat::ChainId(query.routingTarget.chainId),
            result.statusCode.error());
      ADD_FAILED(queryChunksFailed, result.statusCode.error().code());
      co_return makeError(result.statusCode.error());
    }

    if (result.totalNumChunks == 0) {
      continue;
    }

    auto chunkId = ChunkId::unpack(result.lastChunkId.data());
    if (!chunkId) {
      XLOGF(CRITICAL,
            "Failed to unpack chunkId queried from storage, {} {}.",
            result.lastChunkId.describe(),
            result.lastChunkLen);
      co_return makeError(StatusCode::kDataCorruption, "Failed to unpack chunkId queried from storage");
    }
    if (chunkId.inode() != inode_.id) {
      XLOGF(CRITICAL,
            "InodeId of chunk {} queried from storage not match, {} != {}",
            result.lastChunkId.describe(),
            chunkId.inode(),
            inode_.id);
      co_return makeError(StatusCode::kDataCorruption, "InodeId of chunk queried from storage not match");
    }
    if (dynStripe && maxStripe < inode_.asFile().layout.stripeSize && chunkId.chunk() >= maxStripe) {
      // chunk >= maxStripe, this means that another process is writing file, let caller retry again
      ADD_FAILED(queryChunksFailed, MetaCode::kBusy);
      auto msg = fmt::format("inode {}, dynStripe {}, found chunk {}, retry", inode_, maxStripe, chunkId);
      XLOG(WARN, msg);
      co_return makeError(MetaCode::kBusy, std::move(msg));
    }

    chunks[query.routingTarget.chainId] = {
        .length = chunkId.chunk() * inode_.asFile().layout.chunkSize + result.lastChunkLen,
        .lastChunk = chunkId.chunk(),
        .lastChunkLen = result.lastChunkLen,
        .totalChunkLen = result.totalChunkLen,
        .totalNumChunks = result.totalNumChunks};
  }

  co_return chunks;
}

CoTryTask<std::pair<uint32_t, bool>> FileOperation::removeChunks(size_t targetLength,
                                                                 size_t removeChunksBatchSize,
                                                                 bool dynStripe,
                                                                 storage::client::RetryOptions retry) {
  CHECK_FILE(inode_);

  std::vector<storage::client::RemoveChunksOp> removeOps;
  auto maxStripe = inode_.asFile().layout.stripeSize;
  if (dynStripe && inode_.asFile().dynStripe) {
    maxStripe = std::min(inode_.asFile().dynStripe, maxStripe);
    XLOGF(DBG, "file {}, dynStripe {}", inode_, inode_.asFile().dynStripe);
  }
  removeOps.reserve(maxStripe);

  const auto &layout = inode_.asFile().layout;
  size_t chunksLeft = folly::divCeil(targetLength, layout.chunkSize.u64());
  auto chunkRange = ChunkId::range(inode_.id, chunksLeft);
  CO_RETURN_AND_LOG_ON_ERROR(chunkRange);
  auto [begin, end] = *chunkRange;
  auto chainIndexList = layout.getChainIndexList();
  for (size_t stripe = 0; stripe < maxStripe; stripe++) {
    auto chainIndex = chainIndexList[stripe];
    auto ref = flat::ChainRef{layout.tableId, layout.tableVersion, chainIndex};
    auto cid = routing_.getChainId(ref);
    if (!cid) {
      XLOGF(ERR, "Failed to get ChainId by {}", ref);
      co_return makeError(MgmtdClientCode::kRoutingInfoNotReady, fmt::format("Failed to get ChainId by {}", ref));
    }
    auto removeOp = storage_.createRemoveOp(storage::ChainId(*cid),
                                            storage::ChunkId(begin),
                                            storage::ChunkId(end),
                                            removeChunksBatchSize);
    removeOps.push_back(std::move(removeOp));
  }

  RECORD_LATENCY(removeChunksLatency);

  storage::client::WriteOptions options;
  options.retry() = retry;
  CO_RETURN_ON_ERROR(co_await storage_.removeChunks(removeOps, userInfo_, options));

  bool more = false;
  uint64_t totalRemovedChunks = 0;
  for (const auto &op : removeOps) {
    if (auto &status = op.result.statusCode; !status.hasError()) {
      auto removed = op.result.numChunksRemoved;
      auto chain = flat::ChainId(op.routingTarget.chainId);
      XLOGF(DBG, "Remove {} chunks in range {} - {} at chain {}, more {}", removed, begin, end, chain, more);
      totalRemovedChunks += removed;
      more |= op.result.moreChunksInRange;
    }
  }

  ADD_SAMPLE(removeChunksCount, totalRemovedChunks);
  ADD_SAMPLE(removeChunksSize, totalRemovedChunks * inode_.asFile().layout.chunkSize);
  XLOGF(DBG, "Remove {} chunks in range {}-{}, more {}", totalRemovedChunks, begin, end, more);

  for (const auto &op : removeOps) {
    if (auto &status = op.result.statusCode; status.hasError()) {
      auto chain = flat::ChainId(op.routingTarget.chainId);
      XLOGF(ERR, "Failed to remove chunk range {}-{} at chain {}, err {}", begin, end, chain, status.error());
      ADD_FAILED(removeChunksFailed, status.error().code());
      co_return makeError(status.error());
    }
  }

  co_return std::make_pair(totalRemovedChunks, more);
}

CoTryTask<void> FileOperation::truncateChunk(size_t targetLength) {
  CHECK_FILE(inode_);

  const auto &layout = inode_.asFile().layout;
  size_t chunkCnt = folly::divCeil(targetLength, layout.chunkSize.u64());
  if (chunkCnt == 0) {
    co_return Void{};
  }
  auto lastChunkIndex = chunkCnt - 1;
  if (lastChunkIndex > std::numeric_limits<uint32_t>::max()) {
    co_return MAKE_ERROR_F(MetaCode::kFileTooLarge, "length {} chunk id {}", targetLength, lastChunkIndex);
  }

  RECORD_LATENCY(truncateChunkLatency);
  auto lastChunkId = ChunkId(inode_.id, 0, lastChunkIndex);
  auto lastChunkLen = targetLength - lastChunkIndex * layout.chunkSize;
  auto lastChunkChainRef = layout.getChainOfChunk(inode_, lastChunkIndex);
  auto lastChunkChainId = routing_.getChainId(lastChunkChainRef);
  if (!lastChunkChainId) {
    XLOGF(ERR, "Failed to get ChainId by {}", lastChunkChainRef);
    co_return makeError(MgmtdClientCode::kRoutingInfoNotReady,
                        fmt::format("Failed to get ChainId by {}", lastChunkChainRef));
  }
  auto truncateOp = storage_.createTruncateOp(storage::ChainId(*lastChunkChainId),
                                              storage::ChunkId(lastChunkId),
                                              lastChunkLen,
                                              layout.chunkSize);
  XLOGF(INFO, "User {} truncate inode {} to targetLength {}", userInfo_.uid, inode_.id, targetLength);
  CO_RETURN_ON_ERROR(
      co_await storage_.truncateChunks(std::span<storage::client::TruncateChunkOp>(&truncateOp, 1), userInfo_));
  if (auto &status = truncateOp.result.lengthInfo; status.hasError()) {
    XLOGF(ERR,
          "Failed to truncate chunk {} to {}, {}, err {}",
          lastChunkId,
          lastChunkLen,
          layout.chunkSize,
          status.error());
    ADD_FAILED(truncateChunkFailed, status.error().code());
    co_return makeError(truncateOp.result.lengthInfo.error());
  }

  co_return Void{};
}

CoTryTask<std::map<uint32_t, uint32_t>> FileOperation::queryAllChunks(size_t begin, size_t end) {
  CHECK_FILE(inode_);

  std::vector<storage::client::QueryLastChunkOp> queries;
  for (size_t chunk = begin; chunk < end; chunk++) {
    auto ref = inode_.asFile().layout.getChainOfChunk(inode_, chunk);
    auto cid = routing_.getChainId(ref);
    if (!cid) {
      XLOGF(ERR, "Failed to get ChainId by {}", ref);
      co_return makeError(MgmtdClientCode::kRoutingInfoNotReady, fmt::format("Failed to get ChainId by {}", ref));
    }
    auto begin = meta::ChunkId(inode_.id, 0, chunk);
    auto end = meta::ChunkId(inode_.id, 0, chunk + 1);
    auto query = storage_.createQueryOp(storage::ChainId(*cid),
                                        storage::ChunkId(begin),
                                        storage::ChunkId(end),
                                        1,
                                        (void *)chunk);
    queries.push_back(std::move(query));
  }

  std::map<uint32_t, uint32_t> chunks;
  auto queryResult = co_await storage_.queryLastChunk(queries, userInfo_);
  for (auto &query : queries) {
    auto &result = query.result;
    auto begin = meta::ChunkId::unpack(query.range.begin.data());
    if (UNLIKELY(result.statusCode.hasError())) {
      if (result.statusCode.error().code() == StorageClientCode::kChunkNotFound) {
        continue;
      }
      co_return makeError(result.statusCode.error());
    }

    if (result.totalNumChunks == 0) {
      continue;
    }

    auto chunkId = meta::ChunkId::unpack(result.lastChunkId.data());
    if (!chunkId) {
      XLOGF(CRITICAL,
            "Failed to unpack chunkId queried from storage, {} {}.",
            result.lastChunkId.describe(),
            result.lastChunkLen);
      co_return makeError(StatusCode::kDataCorruption, "Failed to unpack chunkId queried from storage");
    }
    if (chunkId != begin) {
      XLOGF(CRITICAL, "{} != {}", chunkId, begin);
      co_return makeError(StatusCode::kDataCorruption, fmt::format("ChunkId {} != {}", chunkId, begin));
    }

    chunks[chunkId.chunk()] = result.lastChunkLen;
  }

  co_return chunks;
}

}  // namespace hf3fs::meta