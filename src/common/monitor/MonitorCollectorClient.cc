#include "common/monitor/MonitorCollectorClient.h"

#include <folly/experimental/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "common/utils/Address.h"
#include "fbs/monitor_collector/MonitorCollectorService.h"

namespace hf3fs::monitor {

Result<Void> MonitorCollectorClient::init() {
  auto serverAddr = net::Address::fromString(config_.remote_ip(), net::Address::TCP);
  client_ = std::make_unique<net::Client>(config_.client(), "MCC");
  client_->start("MCC");
  ctx_ = std::make_unique<serde::ClientContext>(client_->serdeCtx(serverAddr));
  return Void{};
}

void MonitorCollectorClient::stop() {}

Result<Void> MonitorCollectorClient::commit(const std::vector<Sample> &samples) {
  MonitorCollector client;
  folly::coro::blockingWait(client.write(*ctx_, samples));
  return Void{};
}

}  // namespace hf3fs::monitor
