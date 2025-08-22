#pragma once

#include <folly/Executor.h>
#include <folly/Synchronized.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/net/NetworkSocket.h>
#include <memory>

#include "common/net/EventLoop.h"
#include "common/net/Network.h"
#include "common/net/Processor.h"
#include "common/net/Transport.h"
#include "common/net/TransportPool.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBSocket.h"
#include "common/utils/ConcurrencyLimiter.h"
#include "common/utils/Duration.h"

namespace hf3fs::net {

// A worker that handles all I/O tasks.
class IOWorker {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(read_write_tcp_in_event_thread, false);
    CONFIG_HOT_UPDATED_ITEM(read_write_rdma_in_event_thread, false);
    CONFIG_HOT_UPDATED_ITEM(tcp_connect_timeout, 1_s);
    CONFIG_HOT_UPDATED_ITEM(rdma_connect_timeout, 5_s);
    CONFIG_HOT_UPDATED_ITEM(wait_to_retry_send, 100_ms);
    CONFIG_ITEM(num_event_loop, 1u);

    CONFIG_OBJ(ibsocket, IBSocket::Config);
    CONFIG_OBJ(transport_pool, TransportPool::Config);
    CONFIG_OBJ(connect_concurrency_limiter, ConcurrencyLimiterConfig, [](auto &c) { c.set_max_concurrency(4); });
  };

  IOWorker(Processor &processor,
           CPUExecutorGroup &executor,
           folly::IOThreadPoolExecutor &connExecutor,
           const Config &config)
      : processor_(processor),
        executor_(executor),
        connExecutor_(connExecutor),
        config_(config),
        connExecutorShared_(std::make_shared<folly::Executor::KeepAlive<>>(&connExecutor_)),
        pool_(config_.transport_pool()),
        eventLoopPool_(config_.num_event_loop()),
        ibsocketConfigGuard_(config.ibsocket().addCallbackGuard([this] {
          auto newVal = config_.ibsocket().drop_connections();
          if (dropConnections_.exchange(newVal) != newVal) {
            XLOGF(INFO, "ioworker@{} drop all connections", fmt::ptr(this));
            dropConnections();
          }
        })),
        connectConcurrencyLimiter_(config_.connect_concurrency_limiter()) {}
  ~IOWorker() { stopAndJoin(); }

  // start and stop.
  Result<Void> start(const std::string &name);
  void stopAndJoin();

  // add TCP socket with ownership into this IO worker. [thread-safe]
  Result<TransportPtr> addTcpSocket(folly::NetworkSocket sock, bool isDomainSocket = false);

  // add IBSocket with ownership into this IO worker. [thread-safe]
  Result<TransportPtr> addIBSocket(std::unique_ptr<IBSocket> sock);

  // send a batch of items asynchronously to specified address. [thread-safe]
  void sendAsync(Address addr, WriteList list);

  // retry a batch of items asynchronously to specified address. [thread-safe]
  void retryAsync(Address addr, WriteList list);

  // drop all connections.
  void dropConnections(bool dropAll = true, bool dropIncome = false) { pool_.dropConnections(dropAll, dropIncome); }

  // drop all connections to this addr
  void dropConnections(Address addr) { pool_.dropConnections(addr); }

  // drop connections to peer.
  void checkConnections(Address addr, Duration expiredTime) { return pool_.checkConnections(addr, expiredTime); }

 protected:
  friend class Transport;
  // get a valid transport.
  TransportPtr getTransport(Address addr);
  // remove a transport.
  void remove(TransportPtr transport) { pool_.remove(std::move(transport)); }

  // connect asynchronous.
  CoTryTask<void> startConnect(TransportPtr transport, Address addr);

  // wait and retry.
  CoTryTask<void> waitAndRetry(Address addr, WriteList list);

  // start a `Transport::doRead` task in current thread or thread pool.
  void startReadTask(Transport *transport, bool error, bool logError = true);
  // start a `Transport::doWrite` task in current thread or thread pool.
  void startWriteTask(Transport *transport, bool error, bool logError = true);

  // process a message received by transport.
  void processMsg(MessageWrapper wrapper, TransportPtr tr) { processor_.processMsg(std::move(wrapper), std::move(tr)); }

  std::weak_ptr<folly::Executor::KeepAlive<>> connExecutorWeak() { return connExecutorShared_.load(); }

 private:
  Processor &processor_;
  CPUExecutorGroup &executor_;
  folly::IOThreadPoolExecutor &connExecutor_;
  const Config &config_;
  std::atomic<uint32_t> dropConnections_{0};

  // export a weak keep alive to Transport
  // note: folly's executor has a weakRef method, but the implementation seems have bug and will cause memory leak.
  folly::atomic_shared_ptr<folly::Executor::KeepAlive<>> connExecutorShared_;

  // keep transports alive.
  TransportPool pool_;
  // monitor I/O events for all transports.
  EventLoopPool eventLoopPool_;

  std::unique_ptr<ConfigCallbackGuard> ibsocketConfigGuard_;

  ConcurrencyLimiter<Address> connectConcurrencyLimiter_;

  constexpr static size_t kStopFlag = 1;
  constexpr static size_t kCountInc = 2;
  std::atomic<size_t> flags_{0};
};

}  // namespace hf3fs::net
