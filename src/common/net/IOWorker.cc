#include "common/net/IOWorker.h"

#include <folly/Executor.h>
#include <folly/experimental/coro/Sleep.h>
#include <memory>

#include "common/net/Transport.h"
#include "common/net/tcp/TcpSocket.h"
#include "common/utils/Address.h"

namespace hf3fs::net {

Result<Void> IOWorker::start(const std::string &name) { return eventLoopPool_.start(name + "EL"); }

void IOWorker::stopAndJoin() {
  auto flags = flags_.fetch_or(kStopFlag);
  while (flags > kStopFlag) {
    XLOGF(INFO, "Waiting for {} tasks to finish...", (flags >> 1));
    std::this_thread::sleep_for(100ms);
    flags = flags_.load(std::memory_order_acquire);
  }

  eventLoopPool_.stopAndJoin();
  dropConnections(true, true);
  connExecutorShared_.store(std::shared_ptr<folly::Executor::KeepAlive<>>());
}

Result<TransportPtr> IOWorker::addTcpSocket(folly::NetworkSocket sock, bool isDomainSocket /* = false */) {
  auto tcp = std::make_unique<TcpSocket>(sock.toFd());
  RETURN_AND_LOG_ON_ERROR(tcp->init(isDomainSocket));

  auto type = tcp->peerAddr().type;
  auto transport = Transport::create(std::move(tcp), *this, type);
  RETURN_AND_LOG_ON_ERROR(transport->getPeerCredentials());
  pool_.add(transport);

  auto result = eventLoopPool_.add(transport, (EPOLLIN | EPOLLOUT | EPOLLET));
  if (UNLIKELY(!result)) {
    pool_.remove(transport);
    RETURN_AND_LOG_ON_ERROR(result);
  }

  return Result<TransportPtr>(std::move(transport));
}

Result<TransportPtr> IOWorker::addIBSocket(std::unique_ptr<IBSocket> sock) {
  auto transport = Transport::create(std::move(sock), *this, Address::RDMA);

  pool_.add(transport);

  auto result = eventLoopPool_.add(transport, (EPOLLIN | EPOLLOUT | EPOLLET));
  if (UNLIKELY(!result)) {
    pool_.remove(transport);
    RETURN_AND_LOG_ON_ERROR(result);
  }

  return Result<TransportPtr>(std::move(transport));
}

void IOWorker::sendAsync(Address addr, WriteList list) {
  auto transport = getTransport(addr);
  auto failList = transport->send(std::move(list));
  if (failList.has_value()) {
    remove(transport);
    if (!failList.value().empty()) {
      retryAsync(addr, std::move(failList.value()));
    }
  }
}

void IOWorker::retryAsync(Address addr, WriteList list) {
  auto flags = flags_ += kCountInc;
  if (flags & kStopFlag) {
    flags_ -= kCountInc;
  } else {
    waitAndRetry(addr, std::move(list)).scheduleOn(&connExecutor_).start();
  }
}

TransportPtr IOWorker::getTransport(Address addr) {
  auto [transport, doConnect] = pool_.get(addr, *this);
  if (doConnect) {
    // connect asynchronous.
    auto flags = flags_ += kCountInc;
    if (flags & kStopFlag) {
      flags_ -= kCountInc;
    } else {
      startConnect(transport, addr).scheduleOn(&connExecutor_).start();
    }
  }
  return transport;
}

CoTryTask<void> IOWorker::startConnect(TransportPtr transport, Address addr) {
  auto flagsGuard = folly::makeGuard([this] { flags_ -= kCountInc; });
  auto guard = folly::makeGuard([transport] { transport->invalidate(false); });

  if (transport->isRDMA()) {
    auto guard = co_await connectConcurrencyLimiter_.lock(addr);
    CO_RETURN_AND_LOG_ON_ERROR(co_await transport->connect(addr, config_.rdma_connect_timeout()));
  } else {
    CO_RETURN_AND_LOG_ON_ERROR(co_await transport->connect(addr, config_.tcp_connect_timeout()));
  }
  CO_RETURN_AND_LOG_ON_ERROR(eventLoopPool_.add(transport, (EPOLLIN | EPOLLOUT | EPOLLET)));

  // connect successfully.
  guard.dismiss();
  co_return Void{};
}

CoTryTask<void> IOWorker::waitAndRetry(Address addr, WriteList list) {
  auto flagsGuard = folly::makeGuard([this] { flags_ -= kCountInc; });
  co_await folly::coro::sleep(config_.wait_to_retry_send().asMs());
  sendAsync(addr, std::move(list));
  co_return Void{};
}

void IOWorker::startReadTask(Transport *transport, bool error, bool logError /* = true */) {
  if ((transport->isTCP() && config_.read_write_tcp_in_event_thread()) ||
      (transport->isRDMA() && config_.read_write_rdma_in_event_thread())) {
    transport->doRead(error, logError);
  } else {
    executor_.pickNextFree().add(
        [tr = transport->shared_from_this(), error, logError] { tr->doRead(error, logError); });
  }
}

void IOWorker::startWriteTask(Transport *transport, bool error, bool logError /* = true */) {
  if ((transport->isTCP() && config_.read_write_tcp_in_event_thread()) ||
      (transport->isRDMA() && config_.read_write_rdma_in_event_thread())) {
    transport->doWrite(error, logError);
  } else {
    executor_.pickNextFree().add(
        [tr = transport->shared_from_this(), error, logError] { tr->doWrite(error, logError); });
  }
}

}  // namespace hf3fs::net
