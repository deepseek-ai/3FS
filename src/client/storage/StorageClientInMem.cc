#include "StorageClientInMem.h"

#include <algorithm>
#include <boost/core/ignore_unused.hpp>
#include <boost/range/adaptor/reversed.hpp>
#include <cassert>
#include <chrono>
#include <folly/Random.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <iterator>
#include <random>
#include <ranges>

#include "TargetSelection.h"
#include "common/utils/ExponentialBackoffRetry.h"
#include "common/utils/FaultInjection.h"
#include "common/utils/Result.h"
#include "fbs/mgmtd/TargetInfo.h"
#include "fbs/storage/Common.h"
#include "src/common/logging/LogInit.h"

#define GET_CHAIN(chainId)                                                     \
  auto [chain, guard] = co_await getChain(chainId);                            \
  if (chain->error.hasError()) {                                               \
    XLOGF(WARN, "Inject error {} on chain {}", chain->error.error(), chainId); \
    co_return makeError(chain->error.error().code(), "fault injection");       \
  }

namespace hf3fs::storage::client {

template <typename T>
static std::vector<T *> randomShuffle(std::span<T> span) {
  std::vector<T *> vec;
  vec.reserve(span.size());
  for (auto &t : span) {
    vec.push_back(&t);
  }

  std::shuffle(vec.begin(), vec.end(), std::mt19937(std::random_device()()));
  return vec;
}

StorageClientInMem::StorageClientInMem(ClientId clientId,
                                       const Config &config,
                                       hf3fs::client::ICommonMgmtdClient &mgmtdClient)
    : StorageClient(clientId, config),
      mgmtdClient_(mgmtdClient) {}

CoTryTask<void> StorageClientInMem::batchRead(std::span<ReadIO> readIOs,
                                              const flat::UserInfo &userInfo,
                                              const ReadOptions &options,
                                              std::vector<ReadIO *> *failedIOs) {
  boost::ignore_unused(userInfo, options);

  XLOGF(DBG, "reads {}", readIOs.size());

  for (auto &readIO : readIOs) {
    XLOGF(DBG, "to read from chunk off {} bytes {}", readIO.offset, readIO.length);

    GET_CHAIN(readIO.routingTarget.chainId);
    auto key = readIO.chunkId;
    if (chain->chunks.count(key) == 0) {
      readIO.result = StorageClientCode::kChunkNotFound;
      if (failedIOs) failedIOs->push_back(&readIO);
      XLOGF(DBG, "chunk not found");
      continue;
    }

    auto &chunkData = chain->chunks[key];
    auto readSize = chunkData.content.size() < readIO.offset
                        ? 0
                        : std::min(readIO.length, (uint32_t)chunkData.content.size() - readIO.offset);
    std::memcpy(readIO.data, &chunkData.content[readIO.offset], readSize);
    readIO.result = IOResult{readSize, ChunkVer(chunkData.version), ChunkVer(chunkData.version)};

    XLOGF(DBG, "read size {}", readSize);
  }

  co_return Void{};
}

CoTryTask<void> StorageClientInMem::batchWrite(std::span<WriteIO> writeIOs,
                                               const flat::UserInfo &userInfo,
                                               const WriteOptions &options,
                                               std::vector<WriteIO *> *failedIOs) {
  boost::ignore_unused(userInfo, options);

  for (auto writeIO : randomShuffle(writeIOs)) {
    co_await write(*writeIO, userInfo, options);
    if (failedIOs != nullptr && !writeIO->result.lengthInfo) {
      failedIOs->push_back(writeIO);
    }
  }

  co_return Void{};
}

CoTryTask<void> StorageClientInMem::read(ReadIO &readIO, const flat::UserInfo &userInfo, const ReadOptions &options) {
  return batchRead(std::span(&readIO, 1), userInfo, options, nullptr);
}

CoTryTask<void> StorageClientInMem::write(WriteIO &writeIO,
                                          const flat::UserInfo &userInfo,
                                          const WriteOptions &options) {
  boost::ignore_unused(userInfo, options);

  if (writeIO.offset + writeIO.length > writeIO.chunkSize) {
    writeIO.result = StorageClientCode::kInvalidArg;
    co_return Void{};
  }

  GET_CHAIN(writeIO.routingTarget.chainId);

  auto key = writeIO.chunkId;
  bool newChunk = chain->chunks.count(key) == 0;
  auto &chunkData = chain->chunks[key];

  if (newChunk) {
    chunkData.capacity = writeIO.chunkSize;
  } else if (chunkData.capacity != writeIO.chunkSize) {
    writeIO.result = StorageClientCode::kInvalidArg;
    co_return Void{};
  }

  if (writeIO.offset + writeIO.length > chunkData.content.size()) {
    chunkData.content.resize(writeIO.offset + writeIO.length);
  }
  std::memcpy(&chunkData.content[writeIO.offset], writeIO.data, writeIO.length);
  chunkData.version++;

  writeIO.result = IOResult{writeIO.length, ChunkVer(chunkData.version), ChunkVer(chunkData.version)};

  co_return Void{};
}

CoTryTask<std::vector<std::pair<ChunkId, StorageClientInMem::ChunkData>>> StorageClientInMem::doQuery(
    const ChainId &chainId,
    const ChunkIdRange &range) {
  GET_CHAIN(chainId);
  std::vector<std::pair<ChunkId, StorageClientInMem::ChunkData>> queryResult;

  for (auto iter = chain->chunks.crbegin();
       iter != chain->chunks.crend() && queryResult.size() < range.maxNumChunkIdsToProcess;
       iter++) {
    const auto &[chunkId, chunkData] = *iter;

    if (chunkId >= range.end) {  // [begin, end)
      continue;
    }

    if (chunkId < range.begin) {
      break;
    }

    queryResult.emplace_back(chunkId, chunkData);
  }

  co_return queryResult;
}

CoTryTask<uint32_t> StorageClientInMem::processQueryResults(const ChainId chainId,
                                                            const ChunkIdRange &range,
                                                            ChunkDataProcessor processor,
                                                            bool &moreChunksInRange) {
  if (FAULT_INJECTION()) {
    XLOGF(WARN, "Inject fault on processQueryResults");
    co_return makeError(StorageClientCode::kCommError, "Inject fault");
  }

  const uint32_t numChunksToProcess =
      range.maxNumChunkIdsToProcess ? std::min(range.maxNumChunkIdsToProcess, UINT32_MAX - 1) : (UINT32_MAX - 1);
  const uint32_t maxNumResultsPerQuery = 3U;
  ChunkIdRange currentRange = {range.begin, range.end, 0};
  uint32_t numQueryResults = 0;
  Status status(StatusCode::kOK);

  moreChunksInRange = true;

  while (true) {
    currentRange.maxNumChunkIdsToProcess = std::min(numChunksToProcess - numQueryResults + 1, maxNumResultsPerQuery);

    auto queryResult = co_await doQuery(chainId, currentRange);

    if (UNLIKELY(queryResult.hasError())) {
      status = queryResult.error();
      goto exit;
    }

    for (const auto &[chunkId, chunkData] : *queryResult) {
      if (numQueryResults < numChunksToProcess) {
        auto result = co_await processor(chunkId, chunkData);

        if (UNLIKELY(result.hasError())) {
          status = result.error();
          goto exit;
        }
      }

      numQueryResults++;

      if (numQueryResults >= numChunksToProcess + 1) {
        goto exit;
      }
    }

    if (queryResult->size() < currentRange.maxNumChunkIdsToProcess) {
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
          range,
          status,
          numQueryResults);
    co_return makeError(status);
  }

  moreChunksInRange = numQueryResults > numChunksToProcess;

  XLOGF(DBG3, "Processed metadata of {} chunks in range: {}", numQueryResults, range);
  co_return numQueryResults;
}

CoTryTask<void> StorageClientInMem::queryLastChunk(std::span<QueryLastChunkOp> ops,
                                                   const flat::UserInfo &userInfo,
                                                   const ReadOptions &options,
                                                   std::vector<QueryLastChunkOp *> *failedOps) {
  boost::ignore_unused(userInfo, options);

  FAULT_INJECTION_SET_FACTOR(ops.size());
  for (auto op : randomShuffle(ops)) {
    QueryLastChunkResult queryResult{
        Void{},
        ChunkId(), /*lastChunkId*/
        0 /*lastChunkLen*/,
        0 /*totalChunkLen*/,
        0 /*totalNumChunks*/,
        false /*moreChunksInRange*/,
    };

    auto processChunkData = [&queryResult](const ChunkId &chunkId, const ChunkData &chunkData) -> CoTryTask<void> {
      if (queryResult.lastChunkId.data().empty() || queryResult.lastChunkId < chunkId) {
        queryResult.lastChunkId = chunkId;
        queryResult.lastChunkLen = chunkData.content.size();
      }

      queryResult.totalChunkLen += chunkData.content.size();
      queryResult.totalNumChunks++;
      co_return Void{};
    };

    auto processResult = co_await processQueryResults(op->routingTarget.chainId,
                                                      op->chunkRange(),
                                                      processChunkData,
                                                      queryResult.moreChunksInRange);

    if (UNLIKELY(processResult.hasError())) {
      queryResult.statusCode = makeError(processResult.error());
    }

    op->result = queryResult;
  }

  co_return Void{};
}

CoTryTask<void> StorageClientInMem::removeChunks(std::span<RemoveChunksOp> ops,
                                                 const flat::UserInfo &userInfo,
                                                 const WriteOptions &options,
                                                 std::vector<RemoveChunksOp *> *failedOps) {
  boost::ignore_unused(userInfo, options, failedOps);

  FAULT_INJECTION_SET_FACTOR(ops.size());
  for (auto op : randomShuffle(ops)) {
    RemoveChunksResult removeRes{Void{}, 0 /*numChunksRemoved*/, false /*moreChunksInRange*/};

    auto processChunkData = [op, &removeRes, this](const ChunkId &chunkId,
                                                   const ChunkData &chunkData) -> CoTryTask<void> {
      GET_CHAIN(op->routingTarget.chainId);
      chain->chunks.erase(chunkId);
      removeRes.numChunksRemoved++;
      co_return Void{};
    };

    auto processResult = co_await processQueryResults(op->routingTarget.chainId,
                                                      op->chunkRange(),
                                                      processChunkData,
                                                      removeRes.moreChunksInRange);

    if (UNLIKELY(processResult.hasError())) {
      removeRes.statusCode = makeError(processResult.error());
    }

    op->result = removeRes;
  }

  co_return Void{};
}

CoTryTask<void> StorageClientInMem::truncateChunks(std::span<TruncateChunkOp> ops,
                                                   const flat::UserInfo &userInfo,
                                                   const WriteOptions &options,
                                                   std::vector<TruncateChunkOp *> *failedOps) {
  boost::ignore_unused(userInfo, options);

  for (auto op : randomShuffle(ops)) {
    if (op->chunkLen > op->chunkSize) {
      op->result = StorageClientCode::kInvalidArg;
      if (failedOps) failedOps->push_back(op);
      continue;
    }

    GET_CHAIN(op->routingTarget.chainId);
    auto key = op->chunkId;
    bool newChunk = chain->chunks.count(key) == 0;
    auto &chunkData = chain->chunks[key];

    if (newChunk) {
      chunkData.capacity = op->chunkSize;
    } else if (chunkData.capacity != op->chunkSize) {
      op->result = StorageClientCode::kInvalidArg;
      if (failedOps) failedOps->push_back(op);
      continue;
    }

    size_t contentSize = op->onlyExtendChunk && op->chunkLen <= chunkData.content.size()
                             ? contentSize = chunkData.content.size()
                             : op->chunkLen;
    chunkData.content.resize(contentSize);
    chunkData.version++;

    op->result = IOResult{chunkData.content.size(), ChunkVer(chunkData.version), ChunkVer(chunkData.version)};
  }

  co_return Void{};
}

CoTryTask<SpaceInfoRsp> StorageClientInMem::querySpaceInfo(NodeId node) {
  SpaceInfoRsp rsp;
  rsp.spaceInfos.push_back(
      SpaceInfo{"/disk1", 10ULL << 30, 2ULL << 30, 1ULL << 30, std::vector<flat::TargetId>{}, "intel"});
  rsp.spaceInfos.push_back(
      SpaceInfo{"/disk2", 10ULL << 30, 8ULL << 30, 8ULL << 30, std::vector<flat::TargetId>{}, "micron"});

  co_return rsp;
}

CoTryTask<CreateTargetRsp> StorageClientInMem::createTarget(NodeId nodeId, const CreateTargetReq &req) {
  co_return CreateTargetRsp{};
}

CoTryTask<OfflineTargetRsp> StorageClientInMem::offlineTarget(NodeId nodeId, const OfflineTargetReq &req) {
  co_return OfflineTargetRsp{};
}

CoTryTask<RemoveTargetRsp> StorageClientInMem::removeTarget(NodeId nodeId, const RemoveTargetReq &req) {
  co_return RemoveTargetRsp{};
}

CoTryTask<std::vector<Result<QueryChunkRsp>>> StorageClientInMem::queryChunk(const QueryChunkReq &req) {
  co_return std::vector<Result<QueryChunkRsp>>{};
}

CoTryTask<ChunkMetaVector> StorageClientInMem::getAllChunkMetadata(const ChainId &chainId, const TargetId &targetId) {
  ChunkMetaVector chunkMetaVec;
  auto [chain, guard] = co_await getChain(chainId);

  for (const auto &[chunkId, chunkData] : chain->chunks) {
    chunkMetaVec.push_back({chunkId,
                            ChunkVer{chunkData.version},
                            ChunkVer{chunkData.version},
                            ChainVer{},
                            ChunkState::COMMIT,
                            ChecksumInfo{},
                            static_cast<uint32_t>(chunkData.content.size())});
  }

  co_return chunkMetaVec;
}

CoTask<void> StorageClientInMem::injectErrorOnChain(ChainId chainId, Result<Void> error) {
  auto [chain, guard] = co_await getChain(chainId);
  chain->error = error;
  co_return;
}

}  // namespace hf3fs::storage::client
