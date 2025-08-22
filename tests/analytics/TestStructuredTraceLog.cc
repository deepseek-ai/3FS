#include <folly/logging/xlog.h>

#include "analytics/StructuredTraceLog.h"
#include "common/monitor/ScopedMetricsWriter.h"
#include "fbs/storage/Common.h"
#include "meta/event/Event.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::analytics {

TEST(TestStructuredTraceLog, Open) {
  StructuredTraceLog<storage::StorageEventTrace>::Config config;
  config.set_enabled(true);
  StructuredTraceLog<storage::StorageEventTrace> traceLog(config);
  ASSERT_TRUE(traceLog.open());
}

TEST(TestStructuredTraceLog, Close) {
  StructuredTraceLog<storage::StorageEventTrace>::Config config;
  config.set_enabled(true);
  StructuredTraceLog<storage::StorageEventTrace> traceLog(config);
  ASSERT_TRUE(traceLog.open());
  traceLog.close();
}

TEST(TestStructuredTraceLog, MetaEventTrace) {
  StructuredTraceLog<meta::server::MetaEventTrace>::Config config;
  config.set_enabled(true);
  config.set_dump_interval(100_ms);
  StructuredTraceLog<meta::server::MetaEventTrace> traceLog(config);
  ASSERT_TRUE(traceLog.open());

  for (size_t loop = 0; loop < 100'000; loop++) {
    traceLog.append(meta::server::MetaEventTrace{
        .inodeId = meta::InodeId{loop},
        .entryName = std::to_string(loop),
        .client = ClientId::zero(),
    });
  }

  traceLog.close();
}

auto createStorageEventTrace(size_t id) {
  return storage::StorageEventTrace{
      .updateReq =
          storage::UpdateReq{
              .tag =
                  storage::MessageTag{
                      ClientId::zero(),
                      storage::RequestId{id},
                      storage::UpdateChannel{
                          .id = (storage::ChannelId)id,
                          .seqnum = storage::ChannelSeqNum{id},
                      },
                  },
          },
  };
}

TEST(TestStructuredTraceLog, StorageEventTrace) {
  StructuredTraceLog<storage::StorageEventTrace>::Config config;
  config.set_enabled(true);
  config.set_dump_interval(100_ms);
  StructuredTraceLog<storage::StorageEventTrace> traceLog(config);
  ASSERT_TRUE(traceLog.open());

  for (size_t loop = 0; loop < 100'000; loop++) {
    traceLog.append(createStorageEventTrace(loop));
  }

  traceLog.close();
}

TEST(TestStructuredTraceLog, MultiThreadsAppend) {
  auto concurrency = std::min(std::thread::hardware_concurrency(), 64U);
  StructuredTraceLog<storage::StorageEventTrace>::Config config;
  config.set_enabled(true);
  config.set_max_num_writers(concurrency / 2);
  config.set_dump_interval(1_s);
  StructuredTraceLog<storage::StorageEventTrace> traceLog(config);
  ASSERT_TRUE(traceLog.open());

  std::vector<std::future<void>> tasks;

  for (size_t i = 0; i < concurrency; i++) {
    tasks.push_back(std::async(std::launch::async, [&traceLog]() {
      for (size_t loop = 0; loop < 100'000; loop++) {
        traceLog.append(createStorageEventTrace(loop));
      }
    }));
  }

  for (auto &t : tasks) t.wait();
  traceLog.close();
}

TEST(TestStructuredTraceLog, MultiThreadsNewEntry) {
  auto concurrency = std::min(std::thread::hardware_concurrency(), 64U);
  StructuredTraceLog<storage::StorageEventTrace>::Config config;
  config.set_enabled(true);
  config.set_max_num_writers(concurrency);
  config.set_dump_interval(1_s);
  StructuredTraceLog<storage::StorageEventTrace> traceLog(config);
  ASSERT_TRUE(traceLog.open());

  std::vector<std::future<void>> tasks;
  monitor::LatencyRecorder latRecorder("TestStructuredTraceLog");

  for (size_t i = 0; i < concurrency; i++) {
    tasks.push_back(std::async(std::launch::async, [&traceLog, &latRecorder]() {
      for (size_t loop = 0; loop < 100; loop++) {
        auto entry = traceLog.newEntry(createStorageEventTrace(loop));
        entry.reset();
      }
      for (size_t loop = 0; loop < 100'000; loop++) {
        monitor::ScopedLatencyWriter latWriter(latRecorder);
        auto entry = traceLog.newEntry(createStorageEventTrace(loop));
        entry.reset();
      }
    }));
  }

  for (auto &t : tasks) t.wait();
  traceLog.close();

  std::vector<monitor::Sample> samples;
  latRecorder.collect(samples);
  for (const auto &s : samples) {
    XLOGF(WARN, "latency: {}", s.dist());
  }
}

}  // namespace hf3fs::analytics
