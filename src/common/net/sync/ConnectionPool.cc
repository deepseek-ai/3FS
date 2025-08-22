#include "common/net/sync/ConnectionPool.h"

#include "common/utils/Duration.h"
#include "common/utils/FdWrapper.h"

namespace hf3fs::net::sync {

Result<std::unique_ptr<TcpSocket>> ConnectionPool::acquire(Address addr) {
  auto startTime = RelativeTime::now();
  auto deadlineTime = startTime + config_.tcp_connect_timeout();
  auto connections = getConnections(addr);

  // acquire from existing connections.
  std::unique_ptr<TcpSocket> socket;
  if (LIKELY(connections->try_dequeue(socket) && socket->valid())) {
    return std::move(socket);
  }

  // establish a new connection.
  socket = std::make_unique<TcpSocket>(::socket(addr.isUNIX() ? AF_UNIX : AF_INET, SOCK_STREAM, 0));
  if (UNLIKELY(!socket->valid())) {
    XLOGF(ERR, "Create socket failed, addr {}, errno {}", addr, errno);
    return makeError(RPCCode::kSocketError);
  }
  folly::NetworkSocket sock(socket->fd());
  int ret = folly::netops::set_socket_non_blocking(sock);
  if (UNLIKELY(ret == -1)) {
    XLOGF(ERR, "EpollWorker set_socket_non_blocking({}) failed: errno {}", sock.toFd(), errno);
    return makeError(RPCCode::kSocketError);
  }
  ret = folly::netops::set_socket_close_on_exec(sock);
  if (UNLIKELY(ret == -1)) {
    XLOGF(ERR, "EpollWorker set_socket_close_on_exec({}) failed: errno {}", sock.toFd(), errno);
    return makeError(RPCCode::kSocketError);
  }

  if (addr.isUNIX()) {
    struct sockaddr_un destAddr;
    memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sun_family = AF_UNIX;
    auto path = addr.domainSocketPath();
    ::strncpy(destAddr.sun_path, path->c_str(), sizeof(destAddr.sun_path) - 1);
    socklen_t destAddrLen = sizeof(destAddr);
    ret = ::connect(socket->fd(), (struct sockaddr *)&destAddr, destAddrLen);
  } else {
    struct sockaddr_in destAddr;
    memset(&destAddr, 0, sizeof(destAddr));
    destAddr.sin_family = AF_INET;
    destAddr.sin_addr.s_addr = addr.ip;
    destAddr.sin_port = htons(addr.port);
    socklen_t destAddrLen = sizeof(destAddr);
    ret = ::connect(socket->fd(), (struct sockaddr *)&destAddr, destAddrLen);
  }
  if (UNLIKELY(ret != 0 && errno != EINPROGRESS)) {
    XLOGF(ERR, "Socket connect failed, errno {}", errno);
    return makeError(RPCCode::kConnectFailed);
  }

  while (true) {
    auto now = RelativeTime::now();
    if (now >= deadlineTime) {
      auto msg = fmt::format("RPC connect timeout {}", (now - startTime).asMs());
      XLOG(ERR, msg);
      return makeError(RPCCode::kTimeout, std::move(msg));
    }
    auto result = socket->pollWithTimeout(POLLOUT | POLLERR | POLLHUP, deadlineTime - now);
    RETURN_AND_LOG_ON_ERROR(result);
    if (result.value() & Socket::kEventWritableFlag) {
      break;
    }
  }
  return std::move(socket);
}

void ConnectionPool::restore(Address addr, std::unique_ptr<TcpSocket> socket) {
  auto connections = getConnections(addr);
  connections->try_enqueue(std::move(socket));
}

ConnectionPool::Connections *ConnectionPool::getConnections(Address addr) {
  auto &cached = (*tlsCache_)[addr];
  if (LIKELY(cached != nullptr)) {
    return cached;
  }

  auto lock = std::unique_lock(mutex_);
  auto &connections = map_[addr];
  if (connections == nullptr) {
    connections = std::make_unique<Connections>(config_.max_connection_num());
  }
  return cached = connections.get();
}

}  // namespace hf3fs::net::sync
