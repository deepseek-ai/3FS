#include "common/net/Listener.h"

#include <atomic>
#include <boost/filesystem/operations.hpp>
#include <chrono>
#include <fmt/format.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/AsyncGenerator.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/io/coro/ServerSocket.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <string_view>
#include <vector>

#include "common/net/IfAddrs.h"
#include "common/net/ServiceGroup.h"
#include "common/net/Transport.h"
#include "common/net/ib/IBConnect.h"
#include "common/net/ib/IBConnectService.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/IBSocket.h"
#include "common/utils/Address.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"

namespace hf3fs::net {

static bool checkNicType(std::string_view nic, Address::Type type, std::string_view tcp_nic_custom_prefix) {
  switch (type) {
    // RDMA networks rely on TCP networks to establish connections. Therefore, when the network type is set to RDMA, a
    // TCP network card is still required for listening and handshaking. Since it is impossible to exhaustively list all
    // TCP nic prefixes, a configurable prefix option is provided.
    case Address::TCP:
    case Address::RDMA:
      return nic.starts_with("en") || nic.starts_with("eth") || nic.starts_with("bond") || nic.starts_with("xgbe") ||
             (!tcp_nic_custom_prefix.empty() && nic.starts_with(tcp_nic_custom_prefix));
    case Address::IPoIB:
      return nic.starts_with("ib");
    case Address::LOCAL:
      return nic.starts_with("lo");
    default:
      return false;
  }
}

Listener::Listener(const Config &config,
                   const IBSocket::Config &ibconfig,
                   IOWorker &ioWorker,
                   folly::IOThreadPoolExecutor &connThreadPool,
                   Address::Type networkType)
    : config_(config),
      ibconfig_(ibconfig),
      ioWorker_(ioWorker),
      connThreadPool_(connThreadPool),
      networkType_(networkType) {}

Result<Void> Listener::setup() {
  auto nics = IfAddrs::load();
  if (!nics) {
    XLOGF(ERR, "nic list is empty!");
    return makeError(RPCCode::kListenFailed);
  }

  auto &filters = config_.filter_list();
  for (auto [name, addr] : nics.value()) {
    if (addr.up && (filters.empty() || filters.count(name) != 0) &&
        checkNicType(name, networkType_, config_.custom_tcp_nic_prefix())) {
      addressList_.push_back(Address{addr.ip.toLong(),
                                     config_.listen_port(),
                                     networkType_ == Address::LOCAL ? Address::TCP : networkType_});
    }
  }

  if (networkType_ == Address::Type::UNIX) {
    auto port = config_.listen_port();
    if (port == 0) {
      port = folly::Random::rand32();
    }
    addressList_.push_back(Address{config_.domain_socket_index(), port, Address::Type::UNIX});
  }

  if (UNLIKELY(addressList_.empty())) {
    XLOGF(ERR,
          "No available address for listener with network type: {}, filter list: {}",
          magic_enum::enum_name(networkType_),
          fmt::join(filters, ","));
    return makeError(RPCCode::kListenFailed);
  }
  for (auto &address : addressList_) {
    // Create socket and get real port.
    auto evb = connThreadPool_.getEventBase();
    // must create socket in evb thread.
    auto createSocket = [&]() -> CoTask<folly::coro::ServerSocket> {
      co_return folly::coro::ServerSocket(folly::AsyncServerSocket::newSocket(evb),
                                          address.toFollyAddress(),
                                          config_.listen_queue_depth(),
                                          config_.reuse_port());
    };
    auto socket = folly::coro::blockingWait(folly::coro::co_awaitTry(createSocket().scheduleOn(evb)));
    if (socket.hasException()) {
      XLOGF(ERR, "create socket failed: {}", socket.exception().what());
      return makeError(RPCCode::kListenFailed);
    }
    if (networkType_ != Address::Type::UNIX) {
      address.port = socket->getAsyncServerSocket()->getAddress().getPort();
    }
    serverSockets_.push_back(std::move(*socket));
  }
  XLOGF(INFO, "Listener setup: {}", fmt::join(addressList_, ", "));
  return Void{};
}

Result<Void> Listener::start(ServiceGroup &group) {
  if (networkType_ == Address::RDMA) {
    if (!IBManager::initialized()) {
      XLOGF(CRITICAL, "Address::Type is RDMA, but IBDevice not initialized!");
      return makeError(RPCCode::kIBDeviceNotInitialized);
    }
    auto accept = [this](auto socket) { acceptRDMA(std::move(socket)); };
    auto service = std::make_unique<IBConnectService>(ibconfig_, accept, config_.rdma_accept_timeout_getter());
    group.addSerdeService(std::move(service), Address::Type::TCP);
  }

  if (UNLIKELY(addressList_.empty())) {
    XLOGF(ERR, "Listener address list is empty!");
    return makeError(RPCCode::kListenFailed, "Listener address list is empty");
  }
  if (UNLIKELY(addressList_.size() != serverSockets_.size())) {
    XLOGF(ERR, "address list size {} != server sockets size {}", addressList_.size(), serverSockets_.size());
    return makeError(RPCCode::kListenFailed);
  }

  for (auto &socket : serverSockets_) {
    ++running_;
    auto evb = socket.getAsyncServerSocket()->getEventBase();
    co_withCancellation(cancel_.getToken(), listen(std::move(socket))).scheduleOn(evb).start();
  }
  serverSockets_.clear();
  return Void{};
}

void Listener::stopAndJoin() {
  for (auto &socket : serverSockets_) {
    // folly ServerSocket's destructor must call on evb thread...
    auto evb = socket.getAsyncServerSocket()->getEventBase();
    running_++;
    release(std::move(socket)).scheduleOn(evb).start();
  }
  serverSockets_.clear();
  cancel_.requestCancellation();
  for (auto r = running_.load(); r != 0; r = running_.load()) {
    XLOGF(INFO, "Listener {} is waiting for {} tasks to finish...", fmt::ptr(this), r);
    std::this_thread::sleep_for(10ms);
  }
  for (auto &address : addressList_) {
    if (address.isUNIX()) {
      auto pathResult = address.domainSocketPath();
      if (bool(pathResult)) {
        boost::filesystem::remove(*pathResult);
      }
    }
  }
}

CoTask<void> Listener::listen(folly::coro::ServerSocket socket) {
  while (true) {
    auto result = co_await co_awaitTry(socket.accept());
    if (UNLIKELY(result.hasException())) {
      if (result.hasException<OperationCancelled>()) {
        XLOGF(DBG, "Listener::listen is cancelled.");
        break;
      }

      XLOGF(ERR, "Listener::listen has exception: {}", result.exception().what());
      break;
    }

    co_await acceptTCP(std::move(result.value()));
  }
  --running_;
}

CoTask<void> Listener::acceptTCP(std::unique_ptr<folly::coro::Transport> tr) {
  auto result =
      ioWorker_.addTcpSocket(tr->getTransport()->getUnderlyingTransport<folly::AsyncSocket>()->detachNetworkSocket(),
                             networkType_ == Address::UNIX);
  if (UNLIKELY(!result)) {
    XLOGF(ERR, "IOWorker::addTcpSocket failed: {}", result.error());
  }
  co_return;
}

void Listener::acceptRDMA(std::unique_ptr<IBSocket> socket) {
  auto result = ioWorker_.addIBSocket(std::move(socket));
  if (result.hasError()) {
    // log CRITICAL here
    XLOGF(CRITICAL, "failed to add IBSocket {}", result.error());
    return;
  }

  ++running_;
  co_withCancellation(cancel_.getToken(), checkRDMA(*result)).scheduleOn(&connThreadPool_).start();
}

CoTask<void> Listener::checkRDMA(std::weak_ptr<Transport> weak) {
  SCOPE_EXIT { --running_; };
  auto result = co_await co_awaitTry(folly::coro::sleep(config_.rdma_accept_timeout()));
  if (result.hasException()) {
    XLOGF(DBG, "checkRDMA is canceled");
    co_return;
  }
  auto transport = weak.lock();
  if (!transport) {
    co_return;
  }
  if (auto ib = transport->ibSocket(); ib && !ib->checkConnectFinished()) {
    XLOGF(ERR, "IBSocket {} still in ACCEPTED state after wait {}", ib->describe(), config_.rdma_accept_timeout());
    transport->invalidate();
    co_return;
  }
}

CoTask<void> Listener::release(folly::coro::ServerSocket /* socket */) {
  XLOGF(DBG, "release a unused TCP server socket");
  --running_;
  co_return;
}

}  // namespace hf3fs::net
