#pragma once

#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/TargetInfo.h"

namespace hf3fs::mgmtd {
template <typename T>
class WithTimestamp {
 public:
  WithTimestamp()
      : base_(),
        ts_(SteadyClock::now()) {}

  explicit WithTimestamp(T info)
      : base_(std::move(info)),
        ts_(SteadyClock::now()) {}

  WithTimestamp(T info, SteadyTime ts)
      : base_(std::move(info)),
        ts_(ts) {}

  T &base() { return base_; }
  const T &base() const { return base_; }

  SteadyTime ts() const { return ts_; }
  void updateTs() { ts_ = SteadyClock::now(); }
  void updateTs(SteadyTime ts) { ts_ = ts; }

 private:
  T base_;
  SteadyTime ts_;
};
}  // namespace hf3fs::mgmtd
