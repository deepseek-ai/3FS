#include "common/net/Transport.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include <iostream>
#include <memory>
#include <random>
#include <thread>

#include "common/monitor/Recorder.h"
#include "common/net/IOWorker.h"
#include "common/net/MessageHeader.h"
#include "common/net/Socket.h"
#include "common/net/Waiter.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/IBSocket.h"
#include "common/net/tcp/TcpSocket.h"
#include "common/serde/ClientContext.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/Size.h"

namespace hf3fs::net {
namespace {

constexpr uint32_t kInvalidatedFlag = 1u << 0u;     // Bit 0
constexpr uint32_t kReadAvailableFlag = 1u << 1u;   // Bit 1
constexpr uint32_t kReadNewWakedFlag = 1u << 2u;    // Bit 2
constexpr uint32_t kWriteAvailableFlag = 1u << 3u;  // Bit 3
constexpr uint32_t kWriteNewWakedFlag = 1u << 4u;   // Bit 4
constexpr uint32_t kWriteHasMsgFlag = 1u << 5u;     // Bit 5
constexpr uint32_t kWriteNewMsgFlag = 1u << 6u;     // Bit 6
constexpr uint32_t kLastReadFinished = 1u << 7u;    // Bit 7
constexpr uint32_t kLastWriteFinished = 1u << 8u;   // Bit 8

monitor::CountRecorder readCPUTime{"common_net_read_cpu_time"};
monitor::CountRecorder doWriteCPUTime{"common_net_do_write_cpu_time"};
monitor::CountRecorder writeAllCPUTime{"common_net_write_all_cpu_time"};
monitor::CountRecorder readBytes{"common_net_read_bytes"};
monitor::CountRecorder writeBytes{"common_net_write_bytes"};
monitor::DistributionRecorder batchReadSize{"common_net_batch_read_size"};
monitor::DistributionRecorder batchWriteSize{"common_net_batch_write_size"};

}  // namespace

Transport::Transport(std::unique_ptr<Socket> socket, IOWorker &io_worker, Address serverAddr)
    : socket_(std::move(socket)),
      ioWorker_(io_worker),
      connExecutor_(io_worker.connExecutorWeak()),
      serverAddr_(serverAddr) {}

Transport::~Transport() {
  if (auto eventLoop = eventLoop_.lock()) {
    eventLoop->remove(this);
  }
  if (isRDMA()) {
    IBManager::close(IBSocket::Ptr(dynamic_cast<IBSocket *>(socket_.release())));
    auto exec = connExecutor_.lock();
  } else {
    dynamic_cast<TcpSocket *>(socket_.get())->close();
  }
}

TransportPtr Transport::create(std::unique_ptr<Socket> socket, IOWorker &io_worker, Address::Type addrType) {
  return enable_shared_from_this::create(std::move(socket), io_worker, Address{0, 0, addrType});
}

std::shared_ptr<Transport> Transport::create(Address addr, IOWorker &io_worker) {
  if (addr.isTCP()) {
    return enable_shared_from_this::create(std::make_unique<TcpSocket>(), io_worker, addr);
  } else if (addr.isRDMA()) {
    return enable_shared_from_this::create(std::make_unique<IBSocket>(io_worker.config_.ibsocket()), io_worker, addr);
  }
  return nullptr;
}

CoTryTask<void> Transport::connect(Address addr, Duration timeout) {
  if (addr.isRDMA()) {
    auto tcpAddr = Address(addr.ip, addr.port, Address::TCP);
    static const folly::atomic_shared_ptr<const CoreRequestOptions> connectOptions{
        std::make_shared<CoreRequestOptions>()};
    auto tcpCtx = serde::ClientContext(ioWorker_, tcpAddr, connectOptions);
    auto ibSocket = dynamic_cast<IBSocket *>(socket_.get());
    co_return co_await ibSocket->connect(tcpCtx, timeout);
  } else if (addr.isTCP()) {
    auto tcpSocket = dynamic_cast<TcpSocket *>(socket_.get());
    co_return co_await tcpSocket->connect(addr, timeout);
  } else {
    co_return makeError(StatusCode::kNotImplemented);
  }
}

std::optional<WriteList> Transport::send(WriteList list) {
  list.setTransport(shared_from_this());
  if (list.empty()) {
    return std::nullopt;
  }
  mpscWriteList_.add(std::move(list));

  // This is the first write item. Try to start a write task.
  auto flags = flags_.fetch_or(kWriteHasMsgFlag | kWriteNewMsgFlag);
  if ((flags & (kInvalidatedFlag | kWriteHasMsgFlag)) == 0 && (flags & kWriteAvailableFlag) != 0) {
    ioWorker_.startWriteTask(this, false);
  } else if (UNLIKELY(flags & kInvalidatedFlag)) {
    return mpscWriteList_.takeOut().extractForRetry();
  }
  return std::nullopt;
}

void Transport::invalidate(bool logError /* = true */) {
  // mask a invalidated flag and try to start a write task with error mark.
  auto flags = flags_.fetch_or(kInvalidatedFlag | kReadAvailableFlag | kWriteAvailableFlag | kWriteHasMsgFlag);
  if ((flags & kInvalidatedFlag) == 0) {
    ioWorker_.remove(shared_from_this());
  }
  // if no read task is executing, start one.
  if ((flags & kReadAvailableFlag) == 0) {
    ioWorker_.startReadTask(this, true, logError);
  }
  // if no write task is executing, start one.
  if ((flags & kWriteAvailableFlag) == 0 || (flags & kWriteHasMsgFlag) == 0) {
    ioWorker_.startWriteTask(this, true, logError);
  }
}

bool Transport::invalidated() const { return flags_ & kInvalidatedFlag; }

CoTask<void> Transport::closeIB() {
  if (isRDMA()) {
    invalidate();
    XLOGF(DBG, "Wait ib socket {} last read/write finished begin", fmt::ptr(socket_.get()));
    co_await lastReadAndWriteFinished_;
    XLOGF(DBG, "Wait ib socket {} last read/write finished end", fmt::ptr(socket_.get()));
    co_await dynamic_cast<IBSocket *>(socket_.get())->close();
  }
}

Result<Void> Transport::getPeerCredentials() {
  if (!serverAddr_.isUNIX()) {
    // skip if it is not a UNIX domain socket.
    return Void{};
  }

  struct ucred credentials {};
  socklen_t len = sizeof(credentials);
  int ret = ::getsockopt(socket_->fd(), SOL_SOCKET, SO_PEERCRED, &credentials, &len);
  if (UNLIKELY(ret == -1)) {
    auto msg = fmt::format("::getsockopt({}) failed: errno {}", socket_->fd(), errno);
    XLOGF(ERR, msg);
    return makeError(RPCCode::kSocketError, std::move(msg));
  }
  credentials_ = credentials;
  return Void{};
}

template <uint32_t CheckAndRemove, uint32_t WantToRemove, MethodName Name>
Transport::Action Transport::tryToSuspend() {
  auto flags = flags_.load(std::memory_order_acquire);
  while (true) {
    if (UNLIKELY(flags & kInvalidatedFlag)) {
      return Action::Fail;
    } else if ((flags & CheckAndRemove)) {
      flags_ &= ~CheckAndRemove;
      static monitor::CountRecorder recorder(fmt::format("common_{}_retry", Name.str()));
      recorder.addSample(1);
      return Action::Retry;
    }
    auto newFlags = flags & ~WantToRemove;
    if (LIKELY(flags_.compare_exchange_strong(flags, newFlags))) {
      // wait next epoll.
      static monitor::CountRecorder recorder(fmt::format("common_{}_suspend", Name.str()));
      recorder.addSample(1);
      return Action::Suspend;
    }
  }
}

void Transport::doRead(bool error, bool logError /* = true */) {
  lastUsedTime_ = RelativeTime::now();

  auto guard = folly::makeGuard([startTime = std::chrono::steady_clock::now()] {
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    readCPUTime.addSample(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  });

  if (UNLIKELY(error)) {
    XLOGF_IF(WARNING, logError, "transport {} read error notified by epoll", describe());
    return tryToCleanUp(false);
  }

  flags_ &= ~kReadNewWakedFlag;
  while (true) {
    auto result = socket_->recv(folly::MutableByteRange{readBuff_->writableTail(), readBuff_->tailroom()});
    if (UNLIKELY(result.hasError())) {
      XLOGF(WARNING, "transport {} receive failed: {}", describe(), result.error());
      return tryToCleanUp(false);
    }

    auto readSize = result.value();
    if (readSize == 0) {
      auto action = tryToSuspend<kReadNewWakedFlag, kReadAvailableFlag, "read_available">();
      if (action == Action::Suspend) {
        return;
      } else if (action == Action::Retry) {
        continue;
      } else if (action == Action::Fail) {
        XLOGF(WARNING, "transport {} ready to delete in read", describe());
        return tryToCleanUp(false);
      }
    }

    readBytes.addSample(readSize);
    batchReadSize.addSample(readSize);
    readBuff_->append(readSize);

    MessageWrapper msgWrapper(std::move(readBuff_));
    while (msgWrapper.headerComplete()) {
      // check message size.
      auto size = msgWrapper.header().size;
      if (UNLIKELY(size >= kMessageMaxSize)) {
        XLOGF(ERR, "transport {} receive a message with too large size: {}", describe(), size);
        return tryToCleanUp(false);
      }
      if (msgWrapper.messageComplete()) {
        // read a full message.
        msgWrapper.next();
      } else {
        break;
      }
    }

    if (msgWrapper.hasMessages()) {
      // allocate a new read buffer with remaining incomplete message.
      readBuff_ = msgWrapper.createFromRemain(kMessageReadBufferSize);
      // seek to beginning of the buffer and process this batch of messages.
      msgWrapper.seekBegin();
      ioWorker_.processMsg(std::move(msgWrapper), shared_from_this());
    } else if (msgWrapper.headerComplete() && msgWrapper.messageLength() > msgWrapper.capacity()) {
      readBuff_ = msgWrapper.createFromRemain(kMessageReadBufferSize);
    } else {
      // message is not complete, and the tail spase is enough to store the remaining part.
      readBuff_ = msgWrapper.detach();
    }
  }
}

void Transport::doWrite(bool error, bool logError /* = true */) {
  auto guard = folly::makeGuard([startTime = std::chrono::steady_clock::now()] {
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    doWriteCPUTime.addSample(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  });

  if (UNLIKELY(error)) {
    XLOGF_IF(WARNING, logError, "transport {} write error notified by epoll", describe());
    return tryToCleanUp(true);
  }

  flags_ &= ~(kWriteNewMsgFlag | kWriteNewWakedFlag);
  while (true) {
    // 1. collect items to write.
    auto newList = mpscWriteList_.takeOut();
    if (newList.empty() && inWritingList_.empty()) {
      socket_->flush();
      auto action = tryToSuspend<kWriteNewMsgFlag, kWriteHasMsgFlag, "write_has_msg">();
      if (action == Action::Suspend) {
        return;
      } else if (action == Action::Retry) {
        continue;
      } else if (action == Action::Fail) {
        XLOGF(WARNING, "transport {} ready to delete in write", describe());
        return tryToCleanUp(true);
      }
    }

    // 2. concat write item list.
    if (!newList.empty()) {
      inWritingList_.concat(std::move(newList));
    }

    // 3. write remain data.
    if (!inWritingList_.empty()) {
      auto action = writeAll();
      if (action == Action::Suspend) {
        return;
      } else if (action == Action::Fail) {
        XLOGF(WARNING, "transport {} ready to delete in write", describe());
        return tryToCleanUp(true);
      }
    }
  }
}

Transport::Action Transport::writeAll() {
  auto guard = folly::makeGuard([startTime = std::chrono::steady_clock::now()] {
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    writeAllCPUTime.addSample(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  });

  constexpr uint32_t kMaxBatchSize = 64;
  struct iovec iov[kMaxBatchSize];

  while (!inWritingList_.empty()) {
    size_t expectedWriteSize = 0;
    uint32_t len = inWritingList_.toIOVec(iov, kMaxBatchSize, expectedWriteSize);
    auto result = socket_->send(iov, len);
    if (result.hasError()) {
      return Action::Fail;
    }

    writeBytes.addSample(result.value());
    batchWriteSize.addSample(result.value());
    inWritingList_.advance(result.value());
    if (result.value() < expectedWriteSize) {
      return tryToSuspend<kWriteNewWakedFlag, kWriteAvailableFlag, "write_available">();
    }
  }
  return Action::OK;
}

void Transport::tryToCleanUp(bool isWrite) {
  // try to start both read and write task with error flag.
  invalidate();

  if (isWrite) {
    // clean up write status.
    auto retryList = inWritingList_.extractForRetry();
    retryList.concat(mpscWriteList_.takeOut().extractForRetry());
    if (!retryList.empty()) {
      ioWorker_.retryAsync(serverAddr_, std::move(retryList));
    }
    wakeUpAfterLastReadAndWriteFinished(flags_ |= kLastWriteFinished);
  } else {
    wakeUpAfterLastReadAndWriteFinished(flags_ |= kLastReadFinished);
  }
}

void Transport::handleEvents(uint32_t epollEvents) {
  // 1. poll socket.
  auto results = socket_->poll(epollEvents);
  bool error = results.hasError();

  // 2. construct mask from events of poll.
  Socket::Events events = error ? (Socket::kEventReadableFlag | Socket::kEventWritableFlag) : results.value();
  bool doRead = events & Socket::kEventReadableFlag;
  bool doWrite = events & Socket::kEventWritableFlag;
  uint32_t mask = 0;
  if (doRead) {
    mask |= (kReadAvailableFlag | kReadNewWakedFlag);
  }
  if (doWrite) {
    mask |= (kWriteAvailableFlag | kWriteNewWakedFlag);
  }

  // 3. start read/write tasks based on flags.
  auto flags = flags_.fetch_or(mask);
  if (UNLIKELY(flags & kInvalidatedFlag)) {
    // broken connection. do nothing.
    return;
  }
  if (doRead && LIKELY((flags & kReadAvailableFlag) == 0)) {
    ioWorker_.startReadTask(this, error);
  }
  if (doWrite && LIKELY((flags & (kWriteAvailableFlag)) == 0 && (flags & kWriteHasMsgFlag) != 0)) {
    ioWorker_.startWriteTask(this, error);
  }
}

void Transport::wakeUpAfterLastReadAndWriteFinished(uint32_t flags) {
  constexpr auto kAllFinished = (kLastReadFinished | kLastWriteFinished);
  if ((flags & kAllFinished) == kAllFinished) {
    Waiter::instance().clearPendingRequestsOnTransportFailure(this);
    lastReadAndWriteFinished_.post();
  }
}

}  // namespace hf3fs::net
