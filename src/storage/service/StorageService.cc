#include "storage/service/StorageService.h"

#include "common/monitor/Recorder.h"

namespace hf3fs::storage {
namespace {

monitor::LatencyRecorder readQueueLatency{"storage.read.queue_latency"};
monitor::LatencyRecorder updateQueueLatency{"storage.update.queue_latency"};
monitor::LatencyRecorder defaultQueueLatency{"storage.default.queue_latency"};

}  // namespace

void StorageService::reportReadQueueLatency(serde::CallContext &ctx) {
  if (ctx.packet().timestamp) {
    readQueueLatency.addSample(ctx.packet().timestamp->queueLatency());
  }
}

void StorageService::reportUpdateQueueLatency(serde::CallContext &ctx) {
  if (ctx.packet().timestamp) {
    updateQueueLatency.addSample(ctx.packet().timestamp->queueLatency());
  }
}

void StorageService::reportDefaultQueueLatency(serde::CallContext &ctx) {
  if (ctx.packet().timestamp) {
    defaultQueueLatency.addSample(ctx.packet().timestamp->queueLatency());
  }
}

}  // namespace hf3fs::storage
