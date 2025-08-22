#pragma once

#include <folly/Random.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <future>

#include "SerdeObjectWriter.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/ScopedMetricsWriter.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Path.h"
#include "common/utils/SysResource.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::analytics {

template <serde::SerdeType SerdeType>
class StructuredTraceLog : public folly::MoveOnly {
  struct TraceMeta {
    SERDE_STRUCT_FIELD(timestamp, std::time_t{});
    SERDE_STRUCT_FIELD(hostname, String{});
  };

  struct StructuredTrace {
    SERDE_STRUCT_FIELD(trace_meta, TraceMeta{});
    SERDE_STRUCT_FIELD(_, SerdeType{});
  };

  using WriterType = SerdeObjectWriter<StructuredTrace>;
  using WriterPtr = std::shared_ptr<WriterType>;

 public:
  class Config : public hf3fs::ConfigBase<Config> {
   public:
    CONFIG_ITEM(trace_file_dir, Path{"."});
#ifndef NDEBUG
    CONFIG_HOT_UPDATED_ITEM(enabled, false);
    CONFIG_HOT_UPDATED_ITEM(dump_interval, 60_min);
#else
    CONFIG_HOT_UPDATED_ITEM(enabled, true);
    CONFIG_HOT_UPDATED_ITEM(dump_interval, 30_s);
#endif
    CONFIG_HOT_UPDATED_ITEM(max_num_writers, size_t{1}, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(max_row_group_length, size_t{100'000});
  };

 public:
  StructuredTraceLog(const Config &config)
      : config_(config),
        enabled_(config.enabled()),
        typename_(nameof::nameof_short_type<SerdeType>()),
        hostname_(SysResource::hostname().value_or("unknown_host")),
        latencyTagSet_({{"tag", typename_}, {"instance", fmt::to_string(fmt::ptr(this))}}),
        createLatency_("trace_log.create_latency", latencyTagSet_),
        appendLatency_("trace_log.append_latency", latencyTagSet_),
        flushLatency_("trace_log.flush_latency", latencyTagSet_),
        maxNumWriters_(config.max_num_writers()) {
    onConfigUpdated_ = config_.addCallbackGuard([this]() {
      bool enabled = config_.enabled();
      if (enabled_ != enabled) {
        enableTraceLog(enabled);
        if (!enabled) flush(false /*async*/);
      }

      if (maxNumWriters_ != config_.max_num_writers()) {
        updateMaxNumWriters(config_.max_num_writers());
      }
    });

    uint64_t secsUntilFirstDump =
        folly::Random::rand64(config_.dump_interval().asSec().count() / 2, config_.dump_interval().asSec().count());
    nextDumpTime_ = microsecondsSinceEpoch(UtcClock::now() + std::chrono::seconds{secsUntilFirstDump});
  }

  ~StructuredTraceLog() { close(); }

  bool open() {
    auto writer = getOrCreateWriter();
    if (!writer) return false;
    writerPool_.enqueue(writer);
    return true;
  }

  std::shared_ptr<SerdeType> newEntry(const SerdeType &init = SerdeType{}) {
    auto ptr = new SerdeType(init);
    return std::shared_ptr<SerdeType>(ptr, [this](SerdeType *ptr) {
      this->append(*ptr);
      delete ptr;
    });
  }

  void append(const SerdeType &msg) {
    if (!enabled_) return;

    {
      monitor::ScopedLatencyWriter appendLatency(appendLatency_);
      StructuredTrace trace{
          .trace_meta = TraceMeta{.timestamp = UtcClock::secondsSinceEpoch(), .hostname = hostname_},
          ._ = msg,
      };

      WriterPtr writer = getOrCreateWriter();

      if (UNLIKELY(writer == nullptr)) {
        XLOGF(CRITICAL, "Cannot get a writer of {} trace log in directory {}", typename_, config_.trace_file_dir());
        enableTraceLog(false);
        return;
      }

      *writer << trace;
      auto writerOk = writer->ok();
      writerPool_.enqueue(std::move(writer));
      if (UNLIKELY(!writerOk)) enableTraceLog(false);
    }

    auto currentTime = microsecondsSinceEpoch(UtcClock::now());

    if (UNLIKELY(currentTime >= nextDumpTime_)) {
      nextDumpTime_ = currentTime + config_.dump_interval().asUs().count();
      flush(true /*async*/);
    }
  }

  void flush(bool async, bool shutdown = false) {
    auto running = dumpingTrace_.test_and_set();
    if (running) return;

    monitor::ScopedLatencyWriter flushLatency(flushLatency_);
    if (asyncFlush_.valid()) asyncFlush_.wait();

    asyncFlush_ = std::async(
        std::launch::async,
        [this](bool shutdown) {
          size_t numWritersToClose = numWriters_.load();
          auto now = UtcClock::now();

          XLOGF(INFO,
                "Flushing {} {} log writers in directory {}",
                numWritersToClose,
                typename_,
                config_.trace_file_dir());

          for (size_t i = 0; numWritersToClose > 0; i++) {
            // give up flushing old writers after trying for too many loops
            if (i >= 10 * maxNumWriters_) {
              break;
            }

            auto writer = writerPool_.dequeue();
            if (!writer) continue;

            if (writer->createTime() > now) {
              writerPool_.enqueue(writer);
              continue;
            }

            if (writer->ok()) {
              // add an empty trace at the end of log
              *writer << StructuredTrace{
                  .trace_meta = {.timestamp = UtcClock::secondsSinceEpoch(), .hostname = hostname_}};
            }

            try {
              writer.reset();
            } catch (const std::exception &ex) {
              XLOGF(ERR,
                    "Failed to close {} log writer in directory {}, error: {}",
                    typename_,
                    config_.trace_file_dir(),
                    ex.what());
            }

            if (shutdown)
              numWriters_--;
            else
              writerPool_.enqueue(createNewWriter());

            numWritersToClose--;
          }

          if (numWritersToClose > 0) {
            XLOGF(WARN,
                  "Still have {} {} log writers not closed in directory {}",
                  numWritersToClose,
                  typename_,
                  config_.trace_file_dir());
          } else {
            XLOGF(INFO, "Flushed {} trace log in directory {}", typename_, config_.trace_file_dir());
          }
        },
        shutdown);

    if (!async) asyncFlush_.wait();
    dumpingTrace_.clear();
  }

  void close() {
    enableTraceLog(false);
    flush(false /*async*/, true /*shutdown*/);
    XLOGF(INFO, "Closed {} trace log in directory {}", typename_, config_.trace_file_dir());
  }

 private:
  uint64_t microsecondsSinceEpoch(const UtcTime &time) const {
    return std::chrono::duration_cast<std::chrono::microseconds>((time).time_since_epoch()).count();
  }

  WriterPtr getOrCreateWriter() {
    WriterPtr writer;
    if (writerPool_.try_dequeue(writer)) return writer;

    auto currentNumWriters = numWriters_.load();
    if (currentNumWriters < maxNumWriters_) {
      bool create = numWriters_.compare_exchange_strong(currentNumWriters, currentNumWriters + 1);
      if (create) return createNewWriter();
    }

    return writerPool_.dequeue();
  }

  WriterPtr createNewWriter() {
    monitor::ScopedLatencyWriter createLatency(createLatency_);
    auto timestamp = fmt::localtime(UtcClock::to_time_t(UtcClock::now()));
    Path logfilePath =
        config_.trace_file_dir() / Path{fmt::format("{:%Y-%m-%d}", timestamp)} / Path{hostname_} /
        Path{
            fmt::format("{}.{}.{:%Y-%m-%d-%H-%M-%S}.{}.parquet", typename_, hostname_, timestamp, nextLogFileIndex_++)};

    if (!boost::filesystem::exists(logfilePath.parent_path())) {
      boost::system::error_code err{};
      boost::filesystem::create_directories(logfilePath.parent_path(), err);
      if (UNLIKELY(err.failed())) {
        XLOGF(CRITICAL, "Failed to create directory {}, error: {}", logfilePath.parent_path(), err.message());
        return nullptr;
      }
    }

    XLOGF(INFO, "Opening {} trace log: {}", typename_, logfilePath);
    return WriterType::open(logfilePath, false /*append*/, config_.max_row_group_length());
  }

  void enableTraceLog(bool enable) {
    enabled_ = enable;
    XLOGF(INFO,
          "{} {} trace log in directory {}",
          enable ? "Enabled" : "Disabled",
          typename_,
          config_.trace_file_dir());
  }

  void updateMaxNumWriters(size_t newMaxNumWriters) {
    XLOGF(INFO,
          "Update max num of writers from {} to {} for {} trace log in directory {}",
          maxNumWriters_.load(),
          newMaxNumWriters,
          typename_,
          config_.trace_file_dir());
    bool doFlush = maxNumWriters_ > newMaxNumWriters;
    maxNumWriters_ = newMaxNumWriters;
    if (doFlush) flush(false /*async*/);
  }

 private:
  const Config &config_;
  bool enabled_ = false;
  const std::string typename_;
  const std::string hostname_;

  const monitor::TagSet latencyTagSet_;
  monitor::LatencyRecorder createLatency_;
  monitor::LatencyRecorder appendLatency_;
  monitor::LatencyRecorder flushLatency_;

  std::unique_ptr<ConfigCallbackGuard> onConfigUpdated_;
  std::atomic_size_t maxNumWriters_;
  std::atomic_size_t numWriters_ = 0;
  std::atomic_size_t nextLogFileIndex_ = 1;
  folly::UnboundedQueue<WriterPtr, false, false, true> writerPool_;

  std::atomic_uint64_t nextDumpTime_;
  std::atomic_flag dumpingTrace_;
  std::future<void> asyncFlush_;
};

}  // namespace hf3fs::analytics
