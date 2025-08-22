#include "common/serde/ClientContext.h"

#include "common/net/RequestOptions.h"

namespace hf3fs::serde {
namespace {
monitor::LatencyRecorder &getServerLatencyRecorder() {
  static monitor::LatencyRecorder recorder("NetClient.server_latency");
  return recorder;
}

monitor::LatencyRecorder &getTotalLatencyRecorder() {
  static monitor::LatencyRecorder recorder("NetClient.total_latency");
  return recorder;
}

monitor::LatencyRecorder &getNetworkLatencyRecorder() {
  static monitor::LatencyRecorder recorder("NetClient.network_latency");
  return recorder;
}

monitor::LatencyRecorder &getQueueLatencyRecorder() {
  static monitor::LatencyRecorder recorder("NetClient.queue_latency");
  return recorder;
}

monitor::CountRecorder &getRequestThroughputRecorder() {
  static monitor::CountRecorder recorder("NetClient.request_throughput");
  return recorder;
}

monitor::CountRecorder &getResponseThroughputRecorder() {
  static monitor::CountRecorder recorder("NetClient.response_throughput");
  return recorder;
}

monitor::LatencyRecorder &getClientPreSendLatencyRecorder() {
  static monitor::LatencyRecorder recorder("NetClient.client_pre_send_latency");
  return recorder;
}

monitor::LatencyRecorder &getClientPostRecvLatencyRecorder() {
  static monitor::LatencyRecorder recorder("NetClient.client_post_recv_latency");
  return recorder;
}

const folly::atomic_shared_ptr<const net::CoreRequestOptions> kDefaultOptions{
    std::make_shared<net::CoreRequestOptions>()};

}  // namespace

ClientContext::ClientContext(const net::TransportPtr &tr)
    : connectionSource_(tr.get()),
      options_(kDefaultOptions) {}

void ClientContext::reportMetrics(const monitor::TagSet &tags,
                                  Timestamp *ts,
                                  uint64_t requestSize,
                                  uint64_t responseSize) {
  if (ts) {
    getTotalLatencyRecorder().addSample(ts->totalLatency(), tags);
    getServerLatencyRecorder().addSample(ts->serverLatency(), tags);
    getNetworkLatencyRecorder().addSample(ts->networkLatency(), tags);
    getQueueLatencyRecorder().addSample(ts->queueLatency(), tags);
    getClientPreSendLatencyRecorder().addSample(
        Duration(std::chrono::microseconds(ts->clientSerialized - ts->clientCalled)),
        tags);
    getClientPostRecvLatencyRecorder().addSample(
        Duration(std::chrono::microseconds(ts->clientWaked - ts->clientReceived)),
        tags);
  }

  getRequestThroughputRecorder().addSample(requestSize, tags);
  getResponseThroughputRecorder().addSample(responseSize, tags);
}

}  // namespace hf3fs::serde
