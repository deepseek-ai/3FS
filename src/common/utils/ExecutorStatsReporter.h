#pragma once

#include "common/monitor/Recorder.h"

namespace hf3fs {

template <class T>
class ExecutorStatsReporter {
 public:
  ExecutorStatsReporter(const T &executor);

  void report();

 private:
  const T &executor_;
  monitor::Recorder::TagRef<monitor::CountRecorder> threadCountRecorder_;
  monitor::Recorder::TagRef<monitor::CountRecorder> idleThreadCountRecorder_;
  monitor::Recorder::TagRef<monitor::CountRecorder> activeThreadCountRecorder_;
  monitor::Recorder::TagRef<monitor::CountRecorder> pendingTaskCountRecorder_;
  monitor::Recorder::TagRef<monitor::CountRecorder> totalTaskCountRecorder_;
};

}  // namespace hf3fs
