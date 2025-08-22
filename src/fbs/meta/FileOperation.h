#pragma once

#include <functional>
#include <optional>
#include <string_view>

#include "client/storage/StorageClient.h"
#include "client/storage/TargetSelection.h"
#include "common/monitor/Recorder.h"
#include "common/utils/Coroutine.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::meta {

class FileOperation {
 public:
  struct Recorder {
    Recorder(std::string_view prefix)
        : removeChunksCount(fmt::format("{}.remove_chunks", prefix)),
          removeChunksSize(fmt::format("{}.remove_chunks_size", prefix)),
          queryChunksFailed(fmt::format("{}.query_chunks_failed", prefix)),
          removeChunksFailed(fmt::format("{}.remove_chunks_failed", prefix)),
          truncateChunkFailed(fmt::format("{}.truncate_chunk_failed", prefix)),
          queryLastChunkLatency(fmt::format("{}.query_last_chunk", prefix)),
          queryTotalChunkLatency(fmt::format("{}.query_total_chunks", prefix)),
          removeChunksLatency(fmt::format("{}.remove_chunks_latency", prefix)),
          truncateChunkLatency(fmt::format("{}.truncate_latency", prefix)) {}

    monitor::CountRecorder removeChunksCount;
    monitor::CountRecorder removeChunksSize;
    monitor::CountRecorder queryChunksFailed;
    monitor::CountRecorder removeChunksFailed;
    monitor::CountRecorder truncateChunkFailed;
    monitor::LatencyRecorder queryLastChunkLatency;
    monitor::LatencyRecorder queryTotalChunkLatency;
    monitor::LatencyRecorder removeChunksLatency;
    monitor::LatencyRecorder truncateChunkLatency;
  };

  FileOperation(storage::client::StorageClient &storage,
                const flat::RoutingInfo &routing,
                const flat::UserInfo &userInfo,
                const meta::Inode &inode,
                std::optional<std::reference_wrapper<Recorder>> recorder = {})
      : storage_(storage),
        routing_(routing),
        userInfo_(userInfo),
        inode_(inode),
        recorder_(recorder) {}

  struct QueryResult {
    uint64_t length = 0;
    uint64_t lastChunk = 0;
    uint64_t lastChunkLen = 0;
    uint64_t totalChunkLen = 0;
    uint64_t totalNumChunks = 0;
  };

  CoTryTask<QueryResult> queryChunks(bool queryTotalChunk, bool dynStripe);

  CoTryTask<std::map<flat::ChainId, QueryResult>> queryChunksByChain(bool queryTotalChunk, bool dynStripe);

  CoTryTask<std::pair<uint32_t, bool>> removeChunks(size_t targetLength,
                                                    size_t removeChunksBatchSize,
                                                    bool dynStripe,
                                                    storage::client::RetryOptions retry);

  CoTryTask<void> truncateChunk(size_t targetLength);

  CoTryTask<std::map<uint32_t, uint32_t>> queryAllChunks(uint64_t begin, uint64_t end);

 private:
  storage::client::StorageClient &storage_;
  const flat::RoutingInfo &routing_;
  const flat::UserInfo &userInfo_;
  const meta::Inode &inode_;
  std::optional<std::reference_wrapper<Recorder>> recorder_;
};

}  // namespace hf3fs::meta