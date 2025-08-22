#include "common/net/tcp/TcpSocket.h"

#include <folly/Likely.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/io/async/AsyncSocket.h>
#include <folly/io/coro/Transport.h>
#include <folly/logging/xlog.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>

#include "common/net/Network.h"
#include "common/utils/Duration.h"

namespace hf3fs::net {

static folly::ConcurrentHashMap<Address, RelativeTime> lastLogErrorTime;

CoTryTask<void> TcpSocket::connect(Address addr, Duration timeout) {
  auto executor = co_await folly::coro::co_current_executor;
  auto ioExecutor = dynamic_cast<folly::IOThreadPoolExecutor *>(executor);
  if (UNLIKELY(ioExecutor == nullptr)) {
    XLOGF(ERR, "this is not IOThreadPoolExecutor!");
    co_return makeError(StatusCode::kInvalidArg);
  }

  auto result = co_await co_awaitTry(
      folly::coro::Transport::newConnectedSocket(ioExecutor->getEventBase(), addr.toFollyAddress(), timeout.asMs()));
  if (UNLIKELY(result.hasException())) {
    auto now = RelativeTime::now();
    if (now - lastLogErrorTime[addr] >= 10_s) {
      XLOGF(ERR, "connect {} failed: {}", addr.str(), result.exception().what());
      lastLogErrorTime.insert_or_assign(addr, now);
    }
    co_return makeError(RPCCode::kConnectFailed, fmt::format("error: {}", result.exception().what()));
  }

  auto &tr = result.value();
  fd_ = tr.getTransport()->getUnderlyingTransport<folly::AsyncSocket>()->detachNetworkSocket().toFd();
  CO_RETURN_AND_LOG_ON_ERROR(init());
  co_return Void{};
}

Result<TcpSocket::Events> TcpSocket::poll(uint32_t events) {
  if (events & (EPOLLERR | EPOLLHUP)) {
    return makeError(RPCCode::kSocketError);
  }
  TcpSocket::Events out{};
  if (events & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
    out |= kEventReadableFlag;
  }
  if (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
    out |= kEventWritableFlag;
  }
  return out;
}

Result<size_t> TcpSocket::recv(folly::MutableByteRange buf) {
  size_t readSize = 0;
  while (!buf.empty()) {
    int ret = ::read(fd_, buf.data(), buf.size());
    if (LIKELY(ret > 0)) {
      readSize += ret;
      buf.advance(ret);
      continue;
    } else if (ret == 0) {
      XLOGF(INFO, "connection is closed: fd {}, peer {}", fd_, describe());
      return makeError(RPCCode::kSocketClosed);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return readSize;
    } else if (errno == EINTR) {
      continue;
    } else {
      XLOGF(INFO, "read error: fd {}, peer {}, errno {}", fd_, describe(), errno);
      return makeError(RPCCode::kSocketError);
    }
  }
  return readSize;
}

static void updateIOVec(struct iovec iov[], uint32_t len, uint32_t &begin, size_t written) {
  while (begin < len) {
    auto &current = iov[begin];
    if (written < current.iov_len) {
      current.iov_base = reinterpret_cast<uint8_t *>(current.iov_base) + written;
      current.iov_len -= written;
      break;
    } else {
      written -= current.iov_len;
      ++begin;
    }
  }
}

Result<size_t> TcpSocket::send(struct iovec iov[], uint32_t len) {
  size_t writeSize = 0;
  uint32_t begin = 0;

  struct msghdr msghdr;
  memset(&msghdr, 0, sizeof(msghdr));

  while (begin < len) {
    msghdr.msg_iov = &iov[begin];
    msghdr.msg_iovlen = len - begin;
    int ret = ::sendmsg(fd_, &msghdr, MSG_NOSIGNAL);

    if (LIKELY(ret > 0)) {
      writeSize += ret;
      updateIOVec(iov, len, begin, ret);
      continue;
    } else if (ret == 0) {
      XLOGF(INFO, "connection is closed: fd {}, peer {}", fd_, describe());
      return makeError(RPCCode::kSocketClosed);
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return writeSize;
    } else if (errno == EINTR) {
      continue;
    } else {
      XLOGF(INFO, "write error: fd {}, peer {}, errno {}", fd_, describe(), errno);
      return makeError(RPCCode::kSocketError);
    }
  }
  return writeSize;
}

Result<Void> TcpSocket::init(bool isDomainSocket /* = false */) {
  folly::NetworkSocket sock(fd_);
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

  if (isDomainSocket) {
    peerAddr_.ip = 0;
    peerAddr_.port = 0;
    peerAddr_.type = Address::UNIX;
    return Void{};
  }

  struct sockaddr_in addr;
  socklen_t addrlen = sizeof(addr);
  ret = ::getpeername(fd_, (struct sockaddr *)&addr, &addrlen);
  if (UNLIKELY(ret == -1)) {
    XLOGF(ERR, "::getpeername({}) failed: errno {}", sock.toFd(), errno);
    return makeError(RPCCode::kSocketError, fmt::format("::getpeername({}) failed: errno {}", sock.toFd(), errno));
  }
  peerAddr_.ip = addr.sin_addr.s_addr;
  peerAddr_.port = ntohs(addr.sin_port);

  return Void{};
}

Result<Socket::Events> TcpSocket::pollWithTimeout(short events, Duration timeout) {
  struct pollfd fds[1];
  fds[0].fd = fd_;
  fds[0].events = events;
  fds[0].revents = 0;
  int ret = ::poll(fds, 1, timeout.asMs().count());
  if (UNLIKELY(ret < 0 && (errno != EAGAIN && errno != EINTR))) {
    auto msg = fmt::format("Poll connection failed, errno {}", errno);
    XLOG(ERR, msg);
    return makeError(RPCCode::kConnectFailed, std::move(msg));
  }
  if (ret <= 0) {
    return Events{};
  }
  if (UNLIKELY(fds[0].revents & (POLLERR | POLLHUP))) {
    XLOGF(ERR, "Poll connection failed, events {:X}", fds[0].revents);
    return makeError(RPCCode::kConnectFailed);
  }
  return ((fds[0].revents & POLLIN) ? kEventReadableFlag : 0) | ((fds[0].revents & POLLOUT) ? kEventWritableFlag : 0);
}

Result<Void> TcpSocket::sendSync(uint8_t *buff, uint32_t size, RelativeTime startTime, Duration timeout) {
  uint32_t written = 0;
  auto deadlineTime = startTime + timeout;
  while (written < size) {
    struct iovec iov;
    iov.iov_base = buff + written;
    iov.iov_len = size - written;
    auto result = this->send(&iov, 1);
    RETURN_AND_LOG_ON_ERROR(result);
    written += result.value();
    if (LIKELY(written >= size)) {
      break;
    }

    auto now = RelativeTime::now();
    if (now >= deadlineTime) {
      auto msg = fmt::format("RPC send timeout {}", (now - startTime).asMs());
      XLOG(ERR, msg);
      return makeError(RPCCode::kTimeout, std::move(msg));
    }
    RETURN_AND_LOG_ON_ERROR(pollWithTimeout(POLLOUT | POLLERR | POLLHUP, deadlineTime - now));
  }
  return Void{};
}

Result<Void> TcpSocket::recvSync(folly::MutableByteRange buf, RelativeTime startTime, Duration timeout) {
  auto deadlineTime = startTime + timeout;
  while (!buf.empty()) {
    auto result = this->recv(buf);
    RETURN_AND_LOG_ON_ERROR(result);
    buf.advance(result.value());
    if (LIKELY(buf.empty())) {
      break;
    }

    auto now = RelativeTime::now();
    if (now >= deadlineTime) {
      auto msg = fmt::format("RPC recv timeout {}", (now - startTime).asMs());
      XLOG(ERR, msg);
      return makeError(RPCCode::kTimeout, std::move(msg));
    }
    RETURN_AND_LOG_ON_ERROR(pollWithTimeout(POLLIN | POLLERR | POLLHUP, deadlineTime - now));
  }
  return Void{};
}

}  // namespace hf3fs::net
