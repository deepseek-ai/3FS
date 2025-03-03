#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bits/types/struct_iovec.h>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fmt/core.h>
#include <folly/CancellationToken.h>
#include <folly/Conv.h>
#include <folly/IPAddressV4.h>
#include <folly/Likely.h>
#include <folly/MPMCQueue.h>
#include <folly/Memory.h>
#include <folly/Optional.h>
#include <folly/Range.h>
#include <folly/String.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/experimental/StampedPtr.h>
#include <folly/experimental/coro/AsyncGenerator.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/experimental/coro/Promise.h>
#include <folly/experimental/coro/SharedMutex.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/fibers/BatchSemaphore.h>
#include <folly/io/coro/Transport.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>
#include <folly/net/NetworkSocket.h>
#include <functional>
#include <gtest/gtest_prod.h>
#include <infiniband/verbs.h>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/monitor/Sample.h"
#include "common/net/EventLoop.h"
#include "common/net/Network.h"
#include "common/net/Socket.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBConnect.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/RDMABuf.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/FdWrapper.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "common/utils/UtcTime.h"

namespace hf3fs {

namespace serde {
class ClientContext;
};

namespace net {
class IBConnectService;
class IBSocketManager;

class IBSocket : public Socket, folly::MoveOnly {
 public:
  struct Config : public ConfigBase<Config> {
    // for recorder
    CONFIG_HOT_UPDATED_ITEM(record_bytes_per_peer, false);
    CONFIG_HOT_UPDATED_ITEM(record_latency_per_peer, false);

    // connection param
    CONFIG_HOT_UPDATED_ITEM(pkey_index, std::optional<uint16_t>());
    CONFIG_HOT_UPDATED_ITEM(roce_pkey_index, std::optional<uint16_t>());  // RoCE pkey should be 0
    CONFIG_HOT_UPDATED_ITEM(sl, (uint8_t)0);
    CONFIG_HOT_UPDATED_ITEM(traffic_class, std::optional<uint8_t>());  // for RoCE
    // CONFIG_HOT_UPDATED_ITEM(gid_index, std::optional<uint8_t>());      // for RoCE

    CONFIG_HOT_UPDATED_ITEM(start_psn, (uint32_t)0);
    CONFIG_HOT_UPDATED_ITEM(min_rnr_timer, (uint8_t)1);
    CONFIG_HOT_UPDATED_ITEM(timeout, (uint8_t)14);  // ibv_qp_attr.timeout
    CONFIG_HOT_UPDATED_ITEM(retry_cnt, (uint8_t)7);
    CONFIG_HOT_UPDATED_ITEM(rnr_retry, (uint8_t)0);

    CONFIG_HOT_UPDATED_ITEM(max_sge, 16u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(max_rdma_wr, 128u, ConfigCheckers::checkPositive);  // max number of RDMA WRs in send queue
    CONFIG_HOT_UPDATED_ITEM(max_rdma_wr_per_post,
                            32u,
                            ConfigCheckers::checkPositive);  // max number of RDMA WRs per ibv_post_send
    CONFIG_HOT_UPDATED_ITEM(max_rd_atomic, 16u, ConfigCheckers::checkPositive);  // ibv_qp_attr.max_rd_atomic

    CONFIG_HOT_UPDATED_ITEM(buf_size, (16u * 1024), ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(send_buf_cnt, 32u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(buf_ack_batch, 8u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(buf_signal_batch, 8u, ConfigCheckers::checkPositive);
    CONFIG_HOT_UPDATED_ITEM(event_ack_batch, 128u, ConfigCheckers::checkPositive);

    CONFIG_HOT_UPDATED_ITEM(drain_timeout, 5_s);
    CONFIG_HOT_UPDATED_ITEM(drop_connections, 0u);

   public:
    mutable folly::Synchronized<std::unordered_map<Address, uint64_t>> roundRobin_;
    IBConnectConfig toIBConnectConfig(bool roce) const;
  };

  using Ptr = std::unique_ptr<IBSocket>;

  IBSocket(const Config &config, IBPort port = {});
  ~IBSocket() override;

  CoTryTask<void> connect(serde::ClientContext &ctx, Duration timeout);
  Result<Void> accept(folly::IPAddressV4 ip, const IBConnectReq &req, Duration acceptTimeout = 15_s);

  std::string describe() override { return *describe_.rlock(); };
  std::string describe() const { return *describe_.rlock(); }
  folly::IPAddressV4 peerIP() override { return peerIP_; };

  int fd() const override { return channel_->fd; }

  Result<Events> poll(uint32_t events) override;

  Result<size_t> send(folly::ByteRange buf);
  Result<size_t> send(struct iovec *iov, uint32_t cnt) override;
  Result<Void> flush() override;
  Result<size_t> recv(folly::MutableByteRange buf) override;

  CoTask<void> close();

  // Send a empty message to check the liveness of the socket.
  // - if socket is known to be broken, return corresponding error.
  // - else send check message and return immediately
  //   - if the message cannot be delivered, an error will be returned in future during poll.
  Result<Void> check() override;

  // Socket should already turn from ACCEPTED state to READY or ERROR state
  bool checkConnectFinished() const;

  /** RDMA operations. */
  CoTryTask<void> rdmaRead(const RDMARemoteBuf &remoteBuf, RDMABuf &localBuf) {
    co_return co_await rdmaRead(remoteBuf, std::span(&localBuf, 1));
  }
  CoTryTask<void> rdmaRead(const RDMARemoteBuf &remoteBuf, std::span<RDMABuf> localBufs) {
    co_return co_await rdma(IBV_WR_RDMA_READ, remoteBuf, localBufs);
  }

  CoTryTask<void> rdmaWrite(const RDMARemoteBuf &remoteBuf, RDMABuf &localBuf) {
    co_return co_await rdmaWrite(remoteBuf, std::span(&localBuf, 1));
  }
  CoTryTask<void> rdmaWrite(const RDMARemoteBuf &remoteBuf, std::span<RDMABuf> localBufs) {
    co_return co_await rdma(IBV_WR_RDMA_WRITE, remoteBuf, localBufs);
  }

 private:
  struct RDMAReq {
    uint64_t raddr;
    uint32_t rkey;
    uint32_t localBufFirst;
    uint32_t localBufCnt;

    RDMAReq()
        : RDMAReq(0, 0, 0, 0) {}
    RDMAReq(uint64_t raddr, uint32_t rkey, uint32_t localBufFirst, uint32_t localBufCnt)
        : raddr(raddr),
          rkey(rkey),
          localBufFirst(localBufFirst),
          localBufCnt(localBufCnt) {}
  };

 public:
  class RDMAReqBatch {
   public:
    RDMAReqBatch()
        : RDMAReqBatch(nullptr, ibv_wr_opcode(-1)) {}
    RDMAReqBatch(IBSocket *socket, ibv_wr_opcode opcode)
        : socket_(socket),
          opcode_(opcode),
          reqs_(),
          localBufs_() {}

    Result<Void> add(const RDMARemoteBuf &remoteBuf, RDMABuf localBuf);
    Result<Void> add(RDMARemoteBuf remoteBuf, std::span<RDMABuf> localBufs);

    void reserve(size_t numReqs, size_t numLocalBufs) {
      reqs_.reserve(numReqs);
      localBufs_.reserve(numLocalBufs);
    }

    void clear() {
      reqs_.clear();
      localBufs_.clear();
      waitLatency_ = std::chrono::nanoseconds(0);
      transferLatency_ = std::chrono::nanoseconds(0);
    }

    ibv_wr_opcode opcode() const { return opcode_; }
    CoTryTask<void> post() {
      co_return co_await socket_->rdmaBatch(opcode_, reqs_, localBufs_, waitLatency_, transferLatency_);
    }

    std::chrono::nanoseconds waitLatency() const { return waitLatency_; };
    std::chrono::nanoseconds transferLatency() const { return transferLatency_; };

   private:
    IBSocket *socket_;
    ibv_wr_opcode opcode_;
    std::vector<RDMAReq> reqs_;
    std::vector<RDMABuf> localBufs_;
    std::chrono::nanoseconds waitLatency_;
    std::chrono::nanoseconds transferLatency_;
  };

  RDMAReqBatch rdmaReadBatch() { return RDMAReqBatch(this, IBV_WR_RDMA_READ); }
  RDMAReqBatch rdmaWriteBatch() { return RDMAReqBatch(this, IBV_WR_RDMA_WRITE); }

  const IBDevice *device() const { return port_.dev(); }

 private:
  static constexpr size_t kRDMAPostBatch = 8;

  enum class State {
    INIT,
    CONNECTING,
    ACCEPTED,
    READY,
    CLOSE,
    ERROR,
  };

  struct RDMAPostCtx {
    std::optional<std::reference_wrapper<folly::fibers::BatchSemaphore>> sem;
    std::optional<folly::fibers::BatchSemaphore::Waiter> waiter;

    ibv_wr_opcode opcode;
    std::span<const RDMAReq> reqs;
    std::span<RDMABuf> localBufs;
    size_t bytes = 0;

    folly::coro::Baton baton;
    ibv_wc_status status = IBV_WC_SUCCESS;
    std::chrono::steady_clock::time_point postBegin;
    std::chrono::steady_clock::time_point postEnd;

    CoTask<void> waitSem() {
      if (waiter.has_value()) {
        co_await waiter->baton;
      }
    }

    bool setError(ibv_wc_status error) {
      if (status == IBV_WC_SUCCESS) {
        status = error;
        return true;
      }
      return false;
    }

    __attribute__((no_sanitize("thread"))) void finish() {
      postEnd = std::chrono::steady_clock::now();
      baton.post();
    }
  };

  enum class WRType : uint16_t { SEND, RECV, ACK, RDMA, RDMA_LAST, CLOSE, CHECK };

 public:  // note: public to formatter
  struct WRId : private folly::StampedPtr<void> {
    using Base = folly::StampedPtr<void>;

    static uint64_t send(uint32_t signalCount) { return WRId::pack(signalCount, WRType::SEND); }
    static uint64_t recv(uint32_t bufIndex) { return WRId::pack(bufIndex, WRType::RECV); }
    static uint64_t ack() { return WRId::pack(nullptr, WRType::ACK); }
    static uint64_t rdma(RDMAPostCtx *ptr, bool last) {
      return WRId::pack(ptr, last ? WRType::RDMA_LAST : WRType::RDMA);
    }
    static uint64_t close() { return WRId::pack(nullptr, WRType::CLOSE); }
    static uint64_t check() { return WRId::pack(nullptr, WRType::CHECK); }

    static uint64_t pack(uint32_t val, WRType type) { return Base::pack((void *)(uint64_t)val, (uint16_t)type); }
    static uint64_t pack(void *ptr, WRType type) { return Base::pack(ptr, (uint16_t)type); }

    WRId(uint64_t raw)
        : Base{raw} {}

    WRType type() const { return static_cast<WRType>(stamp()); }

    uint32_t sendSignalCount() const {
      assert(type() == WRType::SEND);
      return (uint32_t)(uint64_t)ptr();
    }
    uint32_t recvBufIndex() const {
      assert(type() == WRType::RECV);
      return (uint32_t)(uint64_t)ptr();
    }
    RDMAPostCtx *rdmaPostCtx() const {
      assert(type() == WRType::RDMA || type() == WRType::RDMA_LAST);
      return (RDMAPostCtx *)ptr();
    }

    std::string describe() const {
      switch (type()) {
        case WRType::SEND:
          return fmt::format("[WRType::SEND, signal {}]", sendSignalCount());
        case WRType::RECV:
          return fmt::format("[WRType::RECV, bufIndex {}]", recvBufIndex());
        case WRType::ACK:
          return fmt::format("[WRType::ACK]");
        case WRType::RDMA:
          return fmt::format("[WRType::RDMA, ctx {}]", ptr());
        case WRType::RDMA_LAST:
          return fmt::format("[WRTpe::RDMA_LAST, ctx {}]", ptr());
        case WRType::CLOSE:
          return fmt::format("[WRType::CLOSE]");
        case WRType::CHECK:
          return fmt::format("[WRType::CHECK]");
      }
      assert(false);
      return fmt::format("[INVALID 0x{:016x}]", raw);
    }

    bool operator==(const WRId &other) const { return raw == other.raw; }
  };

  class ImmData {
    static constexpr size_t kTypeOffset = 24;
    static constexpr uint32_t kValMax = (1 << kTypeOffset) - 1;
    static constexpr uint32_t kValMask = kValMax;
    uint32_t val;

   public:
    enum class Type : uint8_t {
      ACK,
      CLOSE,
    };

    ImmData(__be32 val = 0)
        : val(folly::Endian::big32(val)) {}

    static ImmData ack(uint32_t count) { return create(Type::ACK, count); }
    static ImmData close() { return create(Type::CLOSE, 0); }
    static ImmData create(Type type, uint32_t val) {
      assert(type == Type::ACK || type == Type::CLOSE);
      assert(val <= kValMax);
      return folly::Endian::big32(((uint8_t)type << kTypeOffset) | val);
    }

    Type type() const { return Type(val >> kTypeOffset); }
    uint32_t data() const { return val & kValMask; }

    std::string describe() const {
      switch (type()) {
        case Type::ACK:
          return fmt::format("[ACK, count {}]", data());
        case Type::CLOSE:
          return fmt::format("[CLOSE]");
        default:
          return fmt::format("[INVALID 0x{:08x}]", val);
      }
    }

    operator __be32() const { return folly::Endian::big(val); }
  };
  static_assert(sizeof(ImmData) == sizeof(__be32));

 private:
  struct BufferMem : folly::MoveOnly {
    const IBDevice *dev = nullptr;
    int64_t total = 0;
    uint8_t *ptr = nullptr;
    ibv_mr *mr = nullptr;

    static Result<BufferMem> create(const IBDevice *dev, size_t bufSize, size_t bufCnt, unsigned int flags);
    BufferMem() = default;
    BufferMem(BufferMem &&o)
        : dev(std::exchange(o.dev, nullptr)),
          total(std::exchange(o.total, 0)),
          ptr(std::exchange(o.ptr, nullptr)),
          mr(std::exchange(o.mr, nullptr)) {}
    ~BufferMem();
  };

  class Buffers {
   public:
    void init(uint8_t *ptr, ibv_mr *mr, size_t bufSize, size_t bufCnt) {
      ptr_ = ptr;
      mr_ = mr;
      bufSize_ = bufSize;
      bufCnt_ = bufCnt;
    }

    uint8_t *getBuf(size_t idx) { return ptr_ + idx * bufSize_; }
    ibv_mr *getMr() { return mr_; }
    size_t getBufCnt() const { return bufCnt_; }
    size_t getBufSize() const { return bufSize_; }

   protected:
    uint8_t *ptr_ = nullptr;
    ibv_mr *mr_ = nullptr;
    size_t bufSize_ = 0;
    size_t bufCnt_ = 0;
  };

  class SendBuffers : public Buffers {
   public:
    void init(uint8_t *ptr, ibv_mr *mr, size_t bufSize, size_t bufCnt);
    void push(size_t cnts);
    bool empty();
    std::pair<size_t, folly::MutableByteRange &> front();
    void pop();

   private:
    alignas(folly::hardware_destructive_interference_size) std::atomic<uint64_t> tailIdx_{0};
    alignas(folly::hardware_destructive_interference_size) std::atomic<uint64_t> frontIdx_{0};
    std::pair<size_t, folly::MutableByteRange> front_;
  };

  class RecvBuffers : public Buffers {
   public:
    void init(uint8_t *ptr, ibv_mr *mr, size_t bufSize, size_t bufCnt);
    void push(uint32_t idx, uint32_t len);
    bool empty();
    std::pair<size_t, folly::ByteRange &> front();
    bool pop();

   private:
    folly::MPMCQueue<std::pair<uint32_t, uint32_t>> queue_;
    std::optional<std::pair<size_t, folly::ByteRange>> front_;
  };

  friend class IBConnectService;

  int checkConfig() const;
  Result<Void> checkPort() const;
  Result<IBConnectInfo> getConnectInfo() const;
  void setPeerInfo(folly::IPAddressV4 ip, const IBConnectInfo &info);

  // ibv_create_qp & ibv_modify_qp
  [[nodiscard]] int qpCreate();
  [[nodiscard]] int qpInit();
  [[nodiscard]] int qpReadyToRecv();
  [[nodiscard]] int qpReadyToSend();
  void qpError();

  Result<Void> checkState();

  void cqPoll(Events &events);
  int cqGetEvent();
  int cqRequestNotify();
  void wcError(const ibv_wc &wc);
  int wcSuccess(const ibv_wc &wc, Events &events);
  int onSended(const ibv_wc &wc, Events &events);
  int onRecved(const ibv_wc &wc, Events &events);
  int onAckSended(const ibv_wc &wc, Events &events);
  int onRDMAFinished(const ibv_wc &wc, Events &events);
  int onImmData(ImmData imm, Events &events);

  int initBufs();
  int postSend(uint32_t idx, size_t len, uint32_t flags = 0);
  int postRecv(uint32_t idx);
  int postAck();

  friend class IBSocketManager;
  class Drainer : public EventLoop::EventHandler, public std::enable_shared_from_this<Drainer> {
   public:
    using Ptr = std::shared_ptr<Drainer>;
    using WeakPtr = std::weak_ptr<Drainer>;

    static Ptr create(IBSocket::Ptr socket, std::weak_ptr<IBSocketManager> manager);

    Drainer(IBSocket::Ptr socket, std::weak_ptr<IBSocketManager> manager);
    ~Drainer() override;

    std::string describe() const { return socket_->describe(); }

    int fd() const override { return socket_->fd(); }
    void handleEvents(uint32_t /* epollEvents */) override;

   private:
    IBSocket::Ptr socket_;
    std::weak_ptr<IBSocketManager> manager_;
  };
  bool closeGracefully();
  bool drain();

  CoTryTask<void> rdma(ibv_wr_opcode opcode, const RDMARemoteBuf &remoteBuf, std::span<RDMABuf> localBufs) {
    RDMAReqBatch batch(this, opcode);
    CO_RETURN_ON_ERROR(batch.add(remoteBuf, localBufs));
    co_return co_await batch.post();
  }

  CoTryTask<void> rdmaBatch(ibv_wr_opcode opcode,
                            const std::span<const RDMAReq> reqs,
                            const std::span<RDMABuf> localBufs,
                            std::chrono::nanoseconds &waitLatency,
                            std::chrono::nanoseconds &transferLatency);
  CoTryTask<void> rdmaPost(RDMAPostCtx &ctx);
  int rdmaPostWR(RDMAPostCtx &ctx);

  const Config &config_;
  IBConnectConfig connectConfig_;

  IBPort port_;
  std::unique_ptr<ibv_comp_channel, IBDevice::Deleter> channel_;
  std::unique_ptr<ibv_cq, IBDevice::Deleter> cq_;
  std::unique_ptr<ibv_qp, IBDevice::Deleter> qp_;

  folly::IPAddressV4 peerIP_;  // IP of TCP socket to setup connection
  IBConnectInfo peerInfo_;
  folly::Synchronized<std::string> describe_;

  monitor::TagSet tag_;
  monitor::TagSet peerTag_;

  SteadyTime acceptTimeout_;
  std::atomic<State> state_ = State::INIT;
  std::atomic<bool> closed_ = false;
  std::atomic<bool> closeMsgSended_ = false;
  size_t unackedEvents_ = 0;

  // memory
  std::optional<BufferMem> mem_;

  // socket send
  SendBuffers sendBufs_;
  size_t sendNotSignaled_ = 0;
  size_t sendSignaled_ = 0;
  size_t sendAcked_ = 0;
  std::atomic<uint64_t> sendWaitBufBegin_ = 0;

  // socket recv
  RecvBuffers recvBufs_;
  uint32_t recvNotAcked_ = 0;
  std::atomic<int32_t> ackBufAvailable_ = 0;

  // RDMA
  folly::fibers::BatchSemaphore rdmaSem_{0};

  // check
  std::atomic<bool> checkMsgSended_ = false;
};

class IBSocketManager : public EventLoop::EventHandler, public std::enable_shared_from_this<IBSocketManager> {
 public:
  static std::shared_ptr<IBSocketManager> create();

  IBSocketManager(int fd)
      : timer_(fd) {}

  void stopAndJoin();

  void close(IBSocket::Ptr socket);

  int fd() const override { return timer_; }
  void handleEvents(uint32_t /* events */) override;

 private:
  friend class IBSocket::Drainer;
  void remove(const IBSocket::Drainer::Ptr &drainer) { drainers_.lock()->erase(drainer); }

  FdWrapper timer_;
  folly::Synchronized<std::set<IBSocket::Drainer::Ptr>, std::mutex> drainers_;
  folly::Synchronized<std::multimap<SteadyTime, IBSocket::Drainer::WeakPtr>, std::mutex> deadlines_;
};

}  // namespace net
}  // namespace hf3fs
