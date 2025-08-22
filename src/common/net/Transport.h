#pragma once

#include <folly/Executor.h>
#include <folly/IPAddressV4.h>
#include <memory>

#include "common/net/EventLoop.h"
#include "common/net/MessageHeader.h"
#include "common/net/Socket.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBSocket.h"
#include "common/net/tcp/TcpSocket.h"
#include "common/utils/Address.h"
#include "common/utils/Duration.h"
#include "common/utils/Size.h"

namespace hf3fs::net {

class IOWorker;

// A wrapper of the underlying socket with read/write states.
class Transport : public EventLoop::EventHandler, public hf3fs::enable_shared_from_this<Transport> {
 protected:
  // not allowed to construct transport directly. use create instead.
  explicit Transport(std::unique_ptr<Socket> socket, IOWorker &io_worker, Address serverAddr);

 public:
  ~Transport() override;

  // create a transport from a socket with ownership.
  static std::shared_ptr<Transport> create(std::unique_ptr<Socket> socket, IOWorker &io_worker, Address::Type addrType);
  static std::shared_ptr<Transport> create(Address addr, IOWorker &io_worker);

  // connect.
  CoTryTask<void> connect(Address addr, Duration timeout);

  // return the address of the server.
  Address serverAddr() const { return serverAddr_; }

  // return IP of peer.
  folly::IPAddressV4 peerIP() const { return socket_->peerIP(); }

  // is TCP or RDMA connection.
  bool isTCP() const { return serverAddr_.isTCP(); }
  bool isRDMA() const { return serverAddr_.isRDMA(); }
  IBSocket *ibSocket() { return dynamic_cast<IBSocket *>(socket_.get()); }

  // return the address of the peer.
  std::string describe() const { return socket_->describe(); }

  // send a list asynchronously. return a list to be retried if send failed. [thread-safe]
  std::optional<WriteList> send(WriteList list);

  // invalidate this transport. [thread-safe]
  void invalidate(bool logError = true);

  // is invalidated or not.
  bool invalidated() const;

  // close IB socket.
  CoTask<void> closeIB();

  // check socket.
  Result<Void> check() { return socket_->check(); }

  // get credentials for UNIX domain socket.
  Result<Void> getPeerCredentials();

  // return credentials for UNIX domain socket.
  auto &credentials() const { return credentials_; }

  // is expired or not.
  bool expired(Duration expiredTime) const {
    return expiredTime != Duration::zero() && RelativeTime::now() >= lastUsedTime_.load() + expiredTime;
  }

 protected:
  enum class Action { OK, Retry, Suspend, Fail };
  template <uint32_t CheckAndRemove, uint32_t WantToRemove, MethodName Name>
  Action tryToSuspend();

  // there is and at most one read task is being executed.
  void doRead(bool error, bool logError = true);

  // there is and at most one write task is being executed.
  void doWrite(bool error, bool logError = true);
  Action writeAll();

  // try to clean up the state of the socket.
  void tryToCleanUp(bool isWrite);

  // file descriptor monitored by epoll. [EventHandler]
  int fd() const final { return socket_->fd(); }
  // handle the events notified by epoll. [EventHandler]
  void handleEvents(uint32_t epollEvents) final;

  // wake up when read/write/poll finished.
  void wakeUpAfterLastReadAndWriteFinished(uint32_t flags);

 private:
  friend class IOWorker;

  // the inner socket, TCP or RDMA.
  std::unique_ptr<Socket> socket_;
  // the object that actually handles the io work.
  IOWorker &ioWorker_;
  // connection executor
  std::weak_ptr<folly::Executor::KeepAlive<>> connExecutor_;
  // address of the remote server.
  Address serverAddr_;

  // protect the read and write for RDMA connection.
  std::mutex mutex_;

  // the inner state that supports atomic changes.
  alignas(folly::hardware_destructive_interference_size) std::atomic<uint32_t> flags_{0};

  // last used time.
  std::atomic<RelativeTime> lastUsedTime_ = RelativeTime::now();

  // for read.
  IOBufPtr readBuff_ = MessageWrapper::allocate(kMessageReadBufferSize);

  // for UNIX domain socket.
  std::optional<struct ucred> credentials_;

  // the linked list in writing.
  WriteListWithProgress inWritingList_;
  // the linked list to be written.
  MPSCWriteList mpscWriteList_;

  // post when last read and write are finished.
  folly::coro::Baton lastReadAndWriteFinished_;
};
using TransportPtr = std::shared_ptr<Transport>;

}  // namespace hf3fs::net
