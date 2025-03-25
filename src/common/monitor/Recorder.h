#pragma once

#include <atomic>
#include <folly/Memory.h>
#include <folly/ThreadLocal.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/container/HeterogeneousAccess.h>
#include <folly/stats/DigestBuilder.h>
#include <folly/stats/TDigest.h>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "DigestBuilder.h"
#include "common/monitor/Sample.h"
#include "common/utils/Address.h"
#include "common/utils/Duration.h"
#include "common/utils/RobinHood.h"
#include "common/utils/Size.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::monitor {

class MonitorInstance;
class Collector;

class Recorder {
 public:
  Recorder(std::string_view name, std::optional<TagSet> tag, MonitorInstance &monitor)
      : register_(false),
        monitor_(monitor) {
    if (tag.has_value()) {
      name_ = name;
      tag_ = tag.value();
    } else {
      name_ = name;
    }
  }
  virtual ~Recorder();

  const std::string &name() { return name_; }

  virtual void collectAndClean(std::vector<Sample> &samples, bool cleanInactive);

  virtual void collect(std::vector<Sample> &samples) = 0;

  static void setHostname(std::string hostname, std::string podname);

 public:
  using ConcurrentMap = folly::ConcurrentHashMap<TagSet,
                                                 std::unique_ptr<Recorder>,
                                                 folly::HeterogeneousAccessHash<TagSet>,
                                                 folly::HeterogeneousAccessEqualTo<TagSet>>;

  template <typename T>
  class TagRef : public ConcurrentMap::ConstIterator {
   public:
    TagRef(ConcurrentMap::ConstIterator &&iter)
        : ConcurrentMap::ConstIterator(std::move(iter)) {}

    T *operator->() const {
      auto ptr = ConcurrentMap::ConstIterator::operator->()->second.get();
      return static_cast<T *>(ptr);
    }
  };

  // Expose recorder associated with a tag set
  template <typename T>
  TagRef<T> getRecorderWithTag(const TagSet &tag);

 protected:
  friend class Collector;
  friend bool checkRecorderHasTag(const Recorder &rec, const TagSet &tag);

 protected:
  void registerRecorder();
  void unregisterRecorder();

 protected:
  std::string name_;
  TagSet tag_;
  bool register_;
  bool logPer30s_ = true;
  UtcTime lastLogTime_;
  bool active_ = true;

  ConcurrentMap map_;
  MonitorInstance &monitor_;
};

template <class ThreadLocalTag>
class CountRecorderWithTLSTag final : public Recorder {
 public:
  CountRecorderWithTLSTag(std::string_view name, bool resetWhenCollect)
      : CountRecorderWithTLSTag(name, std::nullopt, resetWhenCollect) {}

  CountRecorderWithTLSTag(std::string_view name,
                          std::optional<TagSet> tag = std::nullopt,
                          bool resetWhenCollect = true);
  CountRecorderWithTLSTag(MonitorInstance &monitor,
                          std::string_view name,
                          std::optional<TagSet> tag = std::nullopt,
                          bool resetWhenCollect = true)
      : CountRecorderWithTLSTag(monitor, name, tag, resetWhenCollect, true) {}
  CountRecorderWithTLSTag(CountRecorderWithTLSTag &parent, const TagSet &tag)
      : CountRecorderWithTLSTag(parent.monitor_, parent.name(), tag, parent.resetWhenCollect_, false) {}
  ~CountRecorderWithTLSTag() final { unregisterRecorder(); }

  void collectAndClean(std::vector<Sample> &samples, bool cleanInactive) final;
  void collect(std::vector<Sample> &samples) final;
  void addSample(int64_t val) { tls_->addSample(val); }
  void addSample(int64_t val, const TagSet &tag);

  TagRef<CountRecorderWithTLSTag> getRecorderWithTag(const TagSet &tag) {
    return Recorder::getRecorderWithTag<CountRecorderWithTLSTag>(tag);
  }

 private:
  CountRecorderWithTLSTag(MonitorInstance &monitor,
                          std::string_view name,
                          std::optional<TagSet> tag,
                          bool resetWhenCollect,
                          bool needRegister)
      : Recorder(name, tag, monitor),
        resetWhenCollect_(resetWhenCollect) {
    if (needRegister) {
      registerRecorder();
    }
  }

  struct TLS {
    TLS(CountRecorderWithTLSTag *parent)
        : parent_(parent) {}
    ~TLS() { parent_->sum_ += exchange(); }
    void addSample(int64_t val) { val_ += val; }
    int64_t exchange() { return val_.exchange(0); }
    int64_t load() { return val_.load(std::memory_order_acquire); }

   private:
    CountRecorderWithTLSTag *parent_ = nullptr;
    std::atomic<int64_t> val_{0};
  };

  std::atomic<int64_t> sum_{0};
  folly::ThreadLocal<TLS, ThreadLocalTag> tls_{[this] { return new TLS(this); }};

  bool resetWhenCollect_;
  int64_t cumulativeVal_ = 0;
};

struct SharedThreadLocalTag {};
struct AllocatedMemoryCounterTag {};

using CountRecorder = CountRecorderWithTLSTag<SharedThreadLocalTag>;

class DistributionRecorder : public Recorder {
 public:
  DistributionRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt);
  DistributionRecorder(MonitorInstance &monitor, std::string_view name, std::optional<TagSet> tag = std::nullopt)
      : DistributionRecorder(monitor, name, tag, true) {}
  DistributionRecorder(DistributionRecorder &parent, const TagSet &tag)
      : DistributionRecorder(parent.monitor_, parent.name(), tag, false) {}
  ~DistributionRecorder() override { unregisterRecorder(); }

  void collect(std::vector<Sample> &samples) override;
  void addSample(double value) { tdigest_.append(value); }
  void addSample(double val, const TagSet &tag);
  virtual void logPer30s(const folly::TDigest &digest);

  TagRef<DistributionRecorder> getRecorderWithTag(const TagSet &tag) {
    return Recorder::getRecorderWithTag<DistributionRecorder>(tag);
  }

 protected:
  DistributionRecorder(MonitorInstance &monitor, std::string_view name, std::optional<TagSet> tag, bool needRegister)
      : Recorder(name, tag, monitor) {
    if (needRegister) {
      registerRecorder();
    }
  }

  static constexpr size_t kDigestMaxSize = 512;
  static constexpr size_t kDigestBufferSize = 128_KB;
  folly::DigestBuilder<folly::TDigest> tdigest_{kDigestBufferSize, kDigestMaxSize};
  std::vector<folly::TDigest> cumulativeDigests_;
};

class LatencyRecorder : public DistributionRecorder {
 public:
  LatencyRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt);
  LatencyRecorder(LatencyRecorder &parent, const TagSet &tag);
  ~LatencyRecorder() override { unregisterRecorder(); }

  void addSample(double value) = delete;
  void addSample(uint64_t val, const TagSet &tag) = delete;

  void addSample(std::chrono::nanoseconds duration) { tdigest_.append(duration.count()); }
  void addSample(std::chrono::nanoseconds duration, const TagSet &tag);
  void logPer30s(const folly::TDigest &digest) override;

  TagRef<LatencyRecorder> getRecorderWithTag(const TagSet &tag) {
    return Recorder::getRecorderWithTag<LatencyRecorder>(tag);
  }
};

class SimpleDistributionRecorder : public Recorder {
 public:
  SimpleDistributionRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt);
  SimpleDistributionRecorder(MonitorInstance &monitor, std::string_view name, std::optional<TagSet> tag = std::nullopt)
      : SimpleDistributionRecorder(monitor, name, tag, true) {}
  SimpleDistributionRecorder(SimpleDistributionRecorder &parent, const TagSet &tag)
      : SimpleDistributionRecorder(parent.monitor_, parent.name(), tag, false) {}
  ~SimpleDistributionRecorder() override { unregisterRecorder(); }

  void collect(std::vector<Sample> &samples) override;
  void addSample(int64_t val) { tls_->addSample(val); }
  void addSample(int64_t val, const TagSet &tag);

  void addSample(std::chrono::nanoseconds duration) { addSample(duration.count()); }
  void addSample(std::chrono::nanoseconds duration, const TagSet &tag) { addSample(duration.count(), tag); }

  TagRef<SimpleDistributionRecorder> getRecorderWithTag(const TagSet &tag) {
    return Recorder::getRecorderWithTag<SimpleDistributionRecorder>(tag);
  }

 protected:
  SimpleDistributionRecorder(MonitorInstance &monitor,
                             std::string_view name,
                             std::optional<TagSet> tag,
                             bool needRegister)
      : Recorder(name, tag, monitor) {
    if (needRegister) {
      registerRecorder();
    }
  }

  struct Tls {
    Tls(SimpleDistributionRecorder *parent)
        : parent_(parent) {}

    ~Tls() {
      parent_->sum_.fetch_add(exchangeSum(), std::memory_order_relaxed);
      parent_->count_.fetch_add(exchangeCount(), std::memory_order_relaxed);
      parent_->min_.store(std::min(parent_->min_.load(std::memory_order_relaxed), exchangeMin()),
                          std::memory_order_relaxed);
      parent_->max_.store(std::max(parent_->max_.load(std::memory_order_relaxed), exchangeMax()),
                          std::memory_order_relaxed);
    }

    void addSample(int64_t val) {
      sum_.fetch_add(val, std::memory_order_relaxed);
      count_.fetch_add(1, std::memory_order_relaxed);
      min_.store(std::min(min_.load(std::memory_order_relaxed), val), std::memory_order_relaxed);
      max_.store(std::max(max_.load(std::memory_order_relaxed), val), std::memory_order_relaxed);
    }

    int64_t exchangeSum() { return sum_.exchange(0); }
    int64_t exchangeCount() { return count_.exchange(0); }
    int64_t exchangeMin() { return min_.exchange(std::numeric_limits<int64_t>::max()); }
    int64_t exchangeMax() { return max_.exchange(0); }

   private:
    SimpleDistributionRecorder *parent_ = nullptr;
    std::atomic<int64_t> sum_{0};
    std::atomic<int64_t> count_{0};
    std::atomic<int64_t> min_{std::numeric_limits<int64_t>::max()};
    std::atomic<int64_t> max_{0};
  };

  struct TlsTag {};

  std::atomic<int64_t> sum_{0};
  std::atomic<int64_t> count_{0};
  std::atomic<int64_t> min_{std::numeric_limits<int64_t>::max()};
  std::atomic<int64_t> max_{0};
  folly::ThreadLocal<Tls, TlsTag> tls_{[this] { return new Tls(this); }};
};

template <typename LatencyRecorderT>
class OperationRecorderT {
 public:
  OperationRecorderT(std::string_view name, std::optional<TagSet> tag = std::nullopt, bool recordErrorCode = false);

  struct Guard {
    explicit Guard(OperationRecorderT &recorder)
        : recorder_(recorder) {}
    Guard(OperationRecorderT &recorder, const TagSet &tags)
        : recorder_(recorder),
          tags_(tags) {}
    ~Guard() { report(false); }

    void reportWithCode(status_code_t code);
    void report(bool success) {
      if (success) {
        reportWithCode(StatusCode::kOK);
      } else {
        reportWithCode(StatusCode::kUnknown);
      }
    }
    void succ() { success_ = true; }
    std::optional<Duration> latency() const { return latency_; }
    Duration ellipse() const { return RelativeTime::now() - startTime_; }
    void dismiss() {
      if (!std::exchange(reported_, true)) {
        if (tags_.has_value()) {
          recorder_.current_.addSample(-1, *tags_);
        } else {
          recorder_.current_.addSample(-1);
        }
      }
    }
    RelativeTime startTime() const { return startTime_; }

   private:
    OperationRecorderT &recorder_;
    RelativeTime startTime_ = RelativeTime::now();
    std::optional<TagSet> tags_;
    bool success_ = false;
    bool reported_ = false;
    std::optional<Duration> latency_;
  };

  [[nodiscard]] Guard record() {
    total_.addSample(1);
    current_.addSample(1);
    return Guard(*this);
  }
  [[nodiscard]] Guard record(const TagSet &tag) {
    total_.addSample(1, tag);
    current_.addSample(1, tag);
    return Guard(*this, tag);
  }

 private:
  CountRecorder total_;
  CountRecorder fails_;
  CountRecorder current_;
  LatencyRecorderT succ_latencies_;
  LatencyRecorderT fail_latencies_;
  bool recordErrorCode_;
};

using OperationRecorder = OperationRecorderT<LatencyRecorder>;
using SimpleOperationRecorder = OperationRecorderT<SimpleDistributionRecorder>;

class ValueRecorder : public Recorder {
 public:
  ValueRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt, bool resetWhenCollect = true);
  ValueRecorder(MonitorInstance &monitor,
                std::string_view name,
                std::optional<TagSet> tag = std::nullopt,
                bool resetWhenCollect = true)
      : ValueRecorder(monitor, name, tag, true, resetWhenCollect) {}
  ValueRecorder(ValueRecorder &parent, const TagSet &tag)
      : ValueRecorder(parent.monitor_, parent.name(), tag, false, parent.resetWhenCollect_) {}
  ~ValueRecorder() override { unregisterRecorder(); }

  void collectAndClean(std::vector<Sample> &samples, bool cleanInactive) override;
  void collect(std::vector<Sample> &samples) override;
  void set(int64_t val) { val_.store(val, std::memory_order_release); }
  void set(int64_t val, const TagSet &tag);
  auto value() { return val_.load(); }

  TagRef<ValueRecorder> getRecorderWithTag(const TagSet &tag) { return Recorder::getRecorderWithTag<ValueRecorder>(tag); }

 private:
  ValueRecorder(MonitorInstance &monitor,
                std::string_view name,
                std::optional<TagSet> tag,
                bool needRegister,
                bool resetWhenCollect)
      : Recorder(name, tag, monitor),
        resetWhenCollect_(resetWhenCollect) {
    if (needRegister) {
      registerRecorder();
    }
  }

  std::atomic<int64_t> val_{0};
  bool resetWhenCollect_;
};

class LambdaRecorder : public Recorder {
 public:
  LambdaRecorder(std::string_view name, std::optional<TagSet> tag = std::nullopt);
  ~LambdaRecorder() override {
    unregisterRecorder();
    reset();
  }

  void setLambda(auto &&f) {
    auto lock = std::unique_lock(mutex_);
    getter_ = std::forward<decltype(f)>(f);
  }

  void reset() {
    auto lock = std::unique_lock(mutex_);
    getter_ = [] { return 0; };
  }

  void collect(std::vector<Sample> &samples) override;

 private:
  std::mutex mutex_;
  std::function<int64_t()> getter_ = [] { return 0; };
};

}  // namespace hf3fs::monitor
