#include "common/net/ServiceGroup.h"

#include <chrono>
#include <folly/Random.h>
#include <folly/experimental/coro/Sleep.h>

namespace hf3fs::net {

ServiceGroup::ServiceGroup(const Config &config, ThreadPoolGroup &tpg)
    : config_(config),
      tpg_(tpg),
      processor_(serdeServices_, tpg.procThreadPool(), config_.processor()),
      ioWorker_(processor_, tpg.ioThreadPool(), tpg.connThreadPool(), config_.io_worker()),
      listener_(config_.listener(),
                config_.io_worker().ibsocket(),
                ioWorker_,
                tpg.connThreadPool(),
                config_.network_type()) {}

ServiceGroup::~ServiceGroup() {
  cancel_.requestCancellation();
  future_.wait();
}

Result<Void> ServiceGroup::setup() {
  RETURN_AND_LOG_ON_ERROR(listener_.setup());
  return Void{};
}

Result<Void> ServiceGroup::start() {
  auto name = config_.services().empty() ? "default" : *config_.services().begin();
  RETURN_AND_LOG_ON_ERROR(processor_.start(name));
  RETURN_AND_LOG_ON_ERROR(ioWorker_.start("Svr"));
  future_ = co_withCancellation(cancel_.getToken(), checkConnectionsRegularly())
                .scheduleOn(&tpg_.bgThreadPool().randomPick())
                .start();
  return listener_.start(*this);
}

void ServiceGroup::stopAndJoin() {
  cancel_.requestCancellation();
  future_.wait();
  listener_.stopAndJoin();
  processor_.stopAndJoin();
  ioWorker_.stopAndJoin();
}

CoTask<void> ServiceGroup::checkConnectionsRegularly() {
  while (true) {
    XLOGF(DBG9, "server@{} check connections", fmt::ptr(this));
    ioWorker().checkConnections(Address{0, 0, Address::RDMA}, config_.connection_expiration_time());
    co_await folly::coro::sleep(config_.check_connections_interval().asMs());
  }
}

}  // namespace hf3fs::net
