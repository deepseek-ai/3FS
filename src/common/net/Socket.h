#pragma once

#include <chrono>
#include <folly/Range.h>
#include <memory>
#include <sys/uio.h>

#include "common/net/Network.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"

namespace hf3fs::net {

class Socket {
 public:
  virtual ~Socket() = default;

  // describe this socket.
  virtual std::string describe() = 0;
  virtual folly::IPAddressV4 peerIP() = 0;

  // file descriptor monitored by epoll.
  virtual int fd() const = 0;

  using Events = uint32_t;
  constexpr static auto kEventReadableFlag = (1u << 0);
  constexpr static auto kEventWritableFlag = (1u << 1);
  // poll read and/or write events for this socket.
  virtual Result<Events> poll(uint32_t events) = 0;

  // receive a piece of buffer. return the received size.
  virtual Result<size_t> recv(folly::MutableByteRange buf) = 0;
  // send a batch of buffers. return the send size.
  virtual Result<size_t> send(struct iovec iov[], uint32_t len) = 0;
  // flush the write buffers.
  virtual Result<Void> flush() = 0;
  // check the liveness of the socket.
  virtual Result<Void> check() = 0;
};
using SocketPtr = std::shared_ptr<Socket>;

}  // namespace hf3fs::net
