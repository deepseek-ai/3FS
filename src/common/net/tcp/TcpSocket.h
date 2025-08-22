#pragma once

#include <folly/IPAddressV4.h>

#include "common/net/Socket.h"
#include "common/utils/Address.h"
#include "common/utils/Duration.h"
#include "common/utils/FdWrapper.h"

namespace hf3fs::net {

class TcpSocket : public Socket {
 public:
  TcpSocket() = default;
  explicit TcpSocket(int fd)
      : fd_(fd) {}

  ~TcpSocket() override { close(); }

  // file descriptor monitored by epoll. [Socket]
  int fd() const final { return fd_; }

  // start the connection with timeout in the IO thread pool executor.
  CoTryTask<void> connect(Address addr, Duration timeout);

  // describe the address of the peer.
  std::string describe() final { return peerAddr_.str(); }

  // return the address of the peer.
  auto peerAddr() const { return peerAddr_; }

  folly::IPAddressV4 peerIP() final { return folly::IPAddressV4::fromLong(peerAddr_.ip); }

  // poll read and/or write events for this socket.
  Result<Events> poll(uint32_t events) final;

  // receive a piece of buffer. return the received size.
  Result<size_t> recv(folly::MutableByteRange buf) final;

  // send a batch of buffers. return the send size.
  Result<size_t> send(struct iovec iov[], uint32_t len) final;

  // flush the write buffers. ignore for TCP.
  Result<Void> flush() final { return Void{}; }

  // check the liveness of the socket.
  Result<Void> check() final { return Void{}; };

  // close this socket.
  void close() { fd_.close(); }

  // initialize this socket.
  Result<Void> init(bool isDomainSocket = false);

  // valid.
  bool valid() const { return fd_.valid(); }

  // ::poll with timeout.
  Result<Events> pollWithTimeout(short events, Duration timeout);

  // send a buffer in sync mode.
  Result<Void> sendSync(uint8_t *buff, uint32_t size, RelativeTime startTime, Duration timeout);

  // receive a piece of buffer in sync mode.
  Result<Void> recvSync(folly::MutableByteRange buf, RelativeTime startTime, Duration timeout);

 private:
  FdWrapper fd_;
  Address peerAddr_{0};
};

}  // namespace hf3fs::net
