#include "IBSocket.h"

#include <algorithm>
#include <array>
#include <asm-generic/errno.h>
#include <atomic>
#include <bits/types/struct_timespec.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <fmt/core.h>
#include <folly/IPAddressV4.h>
#include <folly/Likely.h>
#include <folly/Memory.h>
#include <folly/Random.h>
#include <folly/Range.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/IOExecutor.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/fibers/BatchSemaphore.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/coro/Transport.h>
#include <folly/logging/LogLevel.h>
#include <folly/logging/xlog.h>
#include <folly/net/NetworkSocket.h>
#include <folly/portability/Asm.h>
#include <folly/small_vector.h>
#include <infiniband/verbs.h>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/net/EventLoop.h"
#include "common/net/Network.h"
#include "common/net/Processor.h"
#include "common/net/Socket.h"
#include "common/net/WriteItem.h"
#include "common/net/ib/IBConnect.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/RDMABuf.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/Result.h"
#include "common/utils/UtcTime.h"

// IBDBG use to print logs on data path, disable it by default.
#ifndef ENABLE_IBDBG
#define ENABLE_IBDBG 0
#endif

#if ENABLE_IBDBG
#define IBDBG(...) XLOGF(DBG, __VA_ARGS__)
#define IBDBG_IF(...) XLOGF_IF(DBG, __VA_ARGS__)
#else
#define IBDBG(...) (void)0
#define IBDBG_IF(...) (void)0
#endif

FMT_BEGIN_NAMESPACE

template <>
struct formatter<ibv_wc> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const ibv_wc &wc, FormatContext &ctx) const {
    hf3fs::net::IBSocket::WRId wr(wc.wr_id);
    if (wc.status == IBV_WC_SUCCESS) {
      if ((wc.opcode == IBV_WC_RECV || wc.opcode == IBV_WC_RECV_RDMA_WITH_IMM) && wc.wc_flags & IBV_WC_WITH_IMM) {
        hf3fs::net::IBSocket::ImmData imm(wc.imm_data);
        return fmt::format_to(ctx.out(),
                              "{{WR: {}, opcode {}, immdata: {}, bytes: {}, status: SUCCESS}}",
                              wr.describe(),
                              magic_enum::enum_name(wc.opcode),
                              imm.describe(),
                              wc.byte_len);
      } else {
        return fmt::format_to(ctx.out(),
                              "{{WR: {}, opcode {}, bytes: {}, status: SUCCESS}}",
                              wr.describe(),
                              magic_enum::enum_name(wc.opcode),
                              wc.byte_len);
      }
    } else {
      return fmt::format_to(ctx.out(),
                            "{{WR: {}, error: {}, vendor_error: {}}}",
                            wr.describe(),
                            ibv_wc_status_str(wc.status),
                            wc.vendor_err);
    }
  }
};

FMT_END_NAMESPACE

namespace hf3fs::net {

namespace {
monitor::CountRecorder connections("common.ib.connections", {}, false);  // total connections
monitor::CountRecorder newConnections("common.ib.new_connections");
monitor::CountRecorder connectSendFailed("common.ib.connect_send_failed");  // failed to send first msg
monitor::CountRecorder acceptTimeout("common.ib.accept_timeout");           // wait client first msg timeout
monitor::CountRecorder draining("common.ib.draining", {}, false);           // draining connections

monitor::CountRecorder sendFailed("common.ib.send_failed");
monitor::CountRecorder rdmaFailed("common.ib.rdma_failed");
monitor::CountRecorder checkFailed("common.ib.check_failed");

monitor::CountRecorder memused("common.ib.memused", {}, false);
monitor::CountRecorder pollTime{"common.ib.poll_cpu_time"};
monitor::CountRecorder sendBytes("common.ib.send_bytes");
monitor::CountRecorder recvBytes("common.ib.recv_bytes");
monitor::CountRecorder pollCqFailed("common.ib.poll_cq_failed");
monitor::LatencyRecorder waitSendBufLatency("common.ib.wait_send_buf");
// RDMA performance counter
monitor::CountRecorder rdmaReadBytes("common.ib.rdma_read_bytes");
monitor::CountRecorder rdmaWriteBytes("common.ib.rdma_write_bytes");
monitor::LatencyRecorder rdmaWaitLatency("common.ib.rdma_wait_latency");
monitor::LatencyRecorder rdmaReadLatency("common.ib.rdma_read_latency");
monitor::LatencyRecorder rdmaWriteLatency("common.ib.rdma_write_latency");

}  // namespace

IBConnectConfig IBSocket::Config::toIBConnectConfig(bool roce) const {
  auto default_pkey_index = roce ? IBManager::instance().config().default_roce_pkey_index()
                                 : IBManager::instance().config().default_pkey_index();
  return IBConnectConfig{
      .sl = sl(),
      .traffic_class = traffic_class().value_or(IBManager::instance().config().default_traffic_class()),
      .gid_index = 0,  // gid_index().value_or(IBManager::instance().config().default_gid_index()),
      .pkey_index = pkey_index().value_or(default_pkey_index),
      .start_psn = start_psn(),
      .min_rnr_timer = min_rnr_timer(),
      .timeout = timeout(),
      .retry_cnt = retry_cnt(),
      .rnr_retry = rnr_retry(),
      .max_sge = max_sge(),
      .max_rdma_wr = max_rdma_wr(),
      .max_rdma_wr_per_post = max_rdma_wr_per_post(),
      .max_rd_atomic = max_rd_atomic(),
      .buf_size = buf_size(),
      .send_buf_cnt = send_buf_cnt(),
      .buf_ack_batch = buf_ack_batch(),
      .buf_signal_batch = buf_signal_batch(),
      .event_ack_batch = event_ack_batch(),
      .record_latency_per_peer = record_latency_per_peer(),
  };
}

/** IBSocket::Bufs */
Result<IBSocket::BufferMem> IBSocket::BufferMem::create(const IBDevice *dev,
                                                        size_t bufSize,
                                                        size_t bufCnt,
                                                        unsigned int flags) {
  static size_t kPageSize = sysconf(_SC_PAGESIZE);
  IBSocket::BufferMem mem;
  mem.dev = dev;
  mem.total = bufSize * bufCnt;
  mem.ptr = ((uint8_t *)hf3fs::memory::memalign(kPageSize, mem.total));
  if (!mem.ptr) {
    XLOGF(CRITICAL, "IBSocket failed to allocate buffer.");
    return makeError(StatusCode::kNotEnoughMemory);
  }
  memused.addSample(mem.total);
  mem.mr = dev->regMemory(mem.ptr, mem.total, flags);
  if (!mem.mr) {
    XLOGF(ERR, "IBSocket failed to register buffer.");
    return makeError(RPCCode::kRDMAError);
  }

  return mem;
}

IBSocket::BufferMem::~BufferMem() {
  if (mr) {
    assert(dev);
    dev->deregMemory(mr);
    mr = nullptr;
  }
  if (ptr) {
    hf3fs::memory::deallocate(ptr);
    ptr = nullptr;
    memused.addSample(-total);
  }
}

/** IBSocket::SendBuffers */
void IBSocket::SendBuffers::init(uint8_t *ptr, ibv_mr *mr, size_t bufSize, size_t bufCnt) {
  IBSocket::Buffers::init(ptr, mr, bufSize, bufCnt);
  front_ = std::make_pair(0, folly::MutableByteRange(getBuf(0), getBufSize()));
}

void IBSocket::SendBuffers::push(size_t cnts) { tailIdx_.fetch_add(cnts, std::memory_order_relaxed); }

bool IBSocket::SendBuffers::empty() {
  assert(frontIdx_.load(std::memory_order_seq_cst) <= tailIdx_.load(std::memory_order_seq_cst));
  return frontIdx_.load(std::memory_order_relaxed) == tailIdx_.load(std::memory_order_relaxed);
}

std::pair<size_t, folly::MutableByteRange &> IBSocket::SendBuffers::front() { return {front_.first, front_.second}; }

void IBSocket::SendBuffers::pop() {
  assert(front_.second.size() <= getBufSize());
  auto nextIdx = frontIdx_.fetch_add(1, std::memory_order_relaxed) + 1;
  auto nextBufIdx = nextIdx % getBufCnt();
  front_ = std::make_pair(nextBufIdx, folly::MutableByteRange(getBuf(nextBufIdx), getBufSize()));
}

/** IBSocket::RecvBuffers */
void IBSocket::RecvBuffers::init(uint8_t *ptr, ibv_mr *mr, size_t bufSize, size_t bufCnt) {
  IBSocket::Buffers::init(ptr, mr, bufSize, bufCnt);
  queue_ = folly::MPMCQueue<std::pair<uint32_t, uint32_t>>(bufCnt);
}

void IBSocket::RecvBuffers::push(uint32_t idx, uint32_t len) {
  recvBytes.addSample(len);
  bool succ __attribute__((unused)) = queue_.write(std::make_pair(idx, len));
  assert(succ);
}

bool IBSocket::RecvBuffers::empty() { return !front_.has_value() && !pop(); }

std::pair<size_t, folly::ByteRange &> IBSocket::RecvBuffers::front() {
  auto &f = front_.value();
  return {f.first, f.second};
}

bool IBSocket::RecvBuffers::pop() {
  std::pair<uint32_t, uint32_t> next;
  if (queue_.read(next)) {
    front_ = std::make_pair(next.first, folly::MutableByteRange(getBuf(next.first), next.second));
    return true;
  }
  front_ = std::nullopt;

  return false;
}

/** IBSocket::RDMAReqBatch */
Result<Void> IBSocket::RDMAReqBatch::add(const RDMARemoteBuf &remoteBuf, RDMABuf localBuf) {
  auto raddr = remoteBuf.addr();
  auto rkey = remoteBuf.getRkey(socket_->peerInfo_.dev);
  if (UNLIKELY(!localBuf || !rkey.has_value() || localBuf.size() > remoteBuf.size() ||
               localBuf.size() > socket_->port_.attr().max_msg_sz)) {
    XLOGF(ERR,
          "RDMAReqBatch::add invalid args, {} {} {} {} {}",
          localBuf.valid(),
          rkey.has_value(),
          localBuf.size(),
          remoteBuf.size(),
          socket_->port_.attr().max_msg_sz);
    return makeError(StatusCode::kInvalidArg);
  }
  localBufs_.emplace_back(std::move(localBuf));
  reqs_.emplace_back(raddr, rkey.value(), localBufs_.size() - 1, 1);
  return Void{};
}

Result<Void> IBSocket::RDMAReqBatch::add(RDMARemoteBuf remoteBuf, std::span<RDMABuf> localBufs) {
  auto rkey = remoteBuf.getRkey(socket_->peerInfo_.dev);
  if (UNLIKELY(!rkey.has_value())) {
    return makeError(StatusCode::kInvalidArg);
  }
  while (!localBufs.empty()) {
    auto cnt = std::min((size_t)socket_->connectConfig_.max_sge, localBufs.size());
    auto lbufs = localBufs.first(cnt);
    localBufs = localBufs.subspan(cnt);
    size_t total = 0;
    for (auto &buf : lbufs) {
      if (UNLIKELY(!buf)) {
        XLOGF(ERR, "RDMAReqBatch::add found invalid localbuf");
        return makeError(StatusCode::kInvalidArg);
      }
      total += buf.size();
    }
    auto raddr = remoteBuf.addr();
    auto advance = remoteBuf.advance(total);
    if (UNLIKELY(!advance || total > socket_->port_.attr().max_msg_sz)) {
      XLOGF(ERR, "RDMAReqBatch::add invalid args, {} {} {}", advance, total, socket_->port_.attr().max_msg_sz);
      return makeError(StatusCode::kInvalidArg);
    }
    localBufs_.insert(localBufs_.end(), lbufs.begin(), lbufs.end());
    reqs_.emplace_back(raddr, rkey.value(), localBufs_.size() - lbufs.size(), lbufs.size());
  }

  return Void{};
}

/** IBSocket */
IBSocket::IBSocket(const IBSocket::Config &config, IBPort port)
    : config_(config),
      port_(port) {
  connections.addSample(1);
  newConnections.addSample(1);
}

IBSocket::~IBSocket() {
  connections.addSample(-1);
  XLOGF(DBG, "IBSocket destructor {}", describe());
  if (qp_) {
    ibv_qp_attr attr{.qp_state = IBV_QPS_ERR};
    int ret = ibv_modify_qp(qp_.get(), &attr, IBV_QP_STATE);
    XLOGF_IF(CRITICAL, ret != 0, "IBSocket {} failed to modify QP to ERR, ret {}", describe(), ret);
  }
  if (cq_ && unackedEvents_) {
    ibv_ack_cq_events(cq_.get(), unackedEvents_);
    unackedEvents_ = 0;
  }
}

Result<IBSocket::Events> IBSocket::poll(uint32_t /* events */) {
  auto guard = folly::makeGuard([startTime = std::chrono::steady_clock::now()] {
    auto elapsed = std::chrono::steady_clock::now() - startTime;
    pollTime.addSample(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count());
  });

  // poll CQ
  Events events = 0;
  if (UNLIKELY(cqGetEvent())) {
    return makeError(RPCCode::kSocketError);
  }
  cqPoll(events);
  if (UNLIKELY(cqRequestNotify())) {
    return makeError(RPCCode::kSocketError);
  }
  cqPoll(events);

  // check send buffer available
  auto sendAvailable = std::min(sendAcked_, sendSignaled_);
  if (sendAvailable) {
    sendBufs_.push(sendAvailable);
    sendAcked_ -= sendAvailable;
    sendSignaled_ -= sendAvailable;
    events |= kEventWritableFlag;
  }

  // check QP error.
  if (auto state = state_.load(std::memory_order_seq_cst); UNLIKELY(state == State::ERROR)) {
    return makeError(RPCCode::kSocketError);
  }

  IBDBG("IBSocket {} poll return {}", describe(), events);
  return events;
}

void IBSocket::cqPoll(Events &events) {
  static constexpr int kPollCQBatch = 16;
  std::array<ibv_wc, kPollCQBatch> wcArr;

  int ret = 0;
  while ((ret = ibv_poll_cq(cq_.get(), kPollCQBatch, &wcArr[0])) > 0) {
    IBDBG("IBSocket {} get {} WCs", describe(), ret);
    for (int i = 0; i < ret; i++) {
      const ibv_wc &wc = wcArr[i];
      if (UNLIKELY(wc.status != IBV_WC_SUCCESS)) {
        wcError(wc);
        state_ = State::ERROR;
      } else if (UNLIKELY(wcSuccess(wc, events) != 0)) {
        state_ = State::ERROR;
      }
    }

    // break out of the loop if the CQ is empty
    if (ret < kPollCQBatch) break;
  }
  // NOTE: We pre-allocate enough size for CQ, so CQ overrun shouldn't happen!
  if (UNLIKELY(ret < 0)) {
    XLOGF(DFATAL, "IBSocket {} failed to poll CQ, errno {}", describe(), ret);
    pollCqFailed.addSample(1, {{"tag", folly::to<std::string>(ret)}});
  }
}

int IBSocket::cqGetEvent() {
  IBDBG("IBSocket {} get CQ event.", describe());
  ibv_cq *cq = nullptr;
  void *context = nullptr;
  int ret = ibv_get_cq_event(channel_.get(), &cq, &context);
  if (UNLIKELY(ret < 0)) {
    if (UNLIKELY(errno != EAGAIN && errno != EWOULDBLOCK)) {
      XLOGF(CRITICAL, "IBSocket {} failed to get comp_channel event, errno {}", describe(), errno);
      return -1;
    }
    return 0;
  }

  unackedEvents_++;
  if (unackedEvents_ >= connectConfig_.event_ack_batch) {
    ibv_ack_cq_events(cq_.get(), unackedEvents_);
    unackedEvents_ = 0;
  }
  return 0;
}

int IBSocket::cqRequestNotify() {
  IBDBG("IBSocket {} request notify CQ.", describe());
  int ret = ibv_req_notify_cq(cq_.get(), 0);
  if (UNLIKELY(ret)) {
    XLOGF(ERR, "IBSocket {} failed to request notify CQ, errno {}", describe(), ret);
    return -1;
  }
  return 0;
}

void IBSocket::wcError(const ibv_wc &wc) {
  WRId wr(wc.wr_id);
  if (UNLIKELY(state_.load() == State::CONNECTING)) {
    XLOGF(ERR,
          "IBSocket {} failed to send first msg, WC {}, port {}, closed {}",
          describe(),
          wc,
          port_,
          closed_.load());
    connectSendFailed.addSample(1);
  }

  bool skipLog = wc.status == IBV_WC_WR_FLUSH_ERR;
  if (wr.type() == WRType::RDMA || wr.type() == WRType::RDMA_LAST) {
    if (wr.rdmaPostCtx()->setError(wc.status)) {
      skipLog = false;
    }
    if (wr.type() == WRType::RDMA_LAST) {
      wr.rdmaPostCtx()->finish();
    }
  }

  XLOGF_IF(DBG, skipLog, "IBSocket {} get failed WC {}", describe(), wc);
  if (!skipLog) {
    switch (wr.type()) {
      case WRType::SEND:
      case WRType::ACK:
      case WRType::CLOSE:
        sendFailed.addSample(1);
        break;
      case WRType::RDMA:
      case WRType::RDMA_LAST:
        rdmaFailed.addSample(1);
        break;
      case WRType::CHECK:
        checkFailed.addSample(1);
        break;
      case WRType::RECV:
        break;
    }
    XLOGF_IF(ERR,
             wr.type() != WRType::CHECK,
             "IBSocket {} get failed WC {}, closed {}",
             describe(),
             wc,
             closed_.load());
    XLOGF_IF(WARN,
             wr.type() == WRType::CHECK,
             "IBSocket {} get failed WC {}, closed {}",
             describe(),
             wc,
             closed_.load());
  }
}

int IBSocket::wcSuccess(const ibv_wc &wc, Events &events) {
  assert(wc.status == IBV_WC_SUCCESS);
  IBDBG("IBSocket {} get success WC {}", describe(), wc);

  WRId wr(wc.wr_id);
  switch (wr.type()) {
    case WRType::SEND:
      return onSended(wc, events);
    case WRType::RECV:
      return onRecved(wc, events);
    case WRType::ACK:
      return onAckSended(wc, events);
    case WRType::RDMA_LAST:
      return onRDMAFinished(wc, events);
    case WRType::CLOSE:
      XLOGF(INFO, "IBSocket {} closed.", describe());
      events |= kEventReadableFlag;
      state_ = State::CLOSE;
      return 0;
    case WRType::CHECK:
      checkMsgSended_ = false;
      return 0;
    case WRType::RDMA:
    default:
      XLOGF(FATAL, "IBSocket {} get unexpected wc op type {}", describe(), magic_enum::enum_name(wr.type()));
  }
}

int IBSocket::onSended(const ibv_wc &wc, Events &events) {
  WRId wr(wc.wr_id);
  if (UNLIKELY(state_.load(std::memory_order_seq_cst) == State::CONNECTING)) {
    // turn QP to READY when first msg sended
    XLOGF(INFO, "IBSocket {} turn to READY from CONNECTING.", describe());
    state_ = State::READY;
    events |= kEventWritableFlag;
  }
  sendSignaled_ += wr.sendSignalCount();
  return 0;
}

int IBSocket::onRecved(const ibv_wc &wc, Events &events) {
  WRId wr(wc.wr_id);

  if (UNLIKELY(state_.load(std::memory_order_seq_cst) == State::ACCEPTED)) {
    // turn QP from ACCEPTED to RTS when receive first msg from peer.
    XLOGF(INFO, "IBSocket {} turn to READY from ACCEPTED.", describe());
    if (qpReadyToSend() != 0) {
      return -1;
    }
    state_ = State::READY;
    events |= kEventWritableFlag;
  }

  auto recvBufIdx = wr.recvBufIndex();
  if (wc.wc_flags & IBV_WC_WITH_IMM) {
    // special WR with immediate date
    auto imm = ImmData(wc.imm_data);
    if (onImmData(imm, events) != 0) {
      return -1;
    }
    assert(wc.byte_len == 0);  // shouldn't contains data if contains immdata
    return postRecv(recvBufIdx);
  }

  XLOGF_IF(FATAL, wc.byte_len > recvBufs_.getBufSize(), "{} > {}", wc.byte_len, recvBufs_.getBufSize());
  recvBufs_.push(recvBufIdx, wc.byte_len);
  events |= kEventReadableFlag;

  return 0;
}

int IBSocket::onAckSended(const ibv_wc &, Events &) {
  if (UNLIKELY(ackBufAvailable_.fetch_add(1, std::memory_order_relaxed) < 0)) {
    // there is some pending ACK msg, send it
    XLOGF(DBG, "IBSocket {} ACK buf available, send ack", describe());
    return postAck();
  }
  return 0;
}

int IBSocket::onRDMAFinished(const ibv_wc &wc, Events &) {
  WRId wr(wc.wr_id);
  wr.rdmaPostCtx()->finish();
  return 0;
}

int IBSocket::onImmData(ImmData imm, Events &events) {
  switch (imm.type()) {
    case ImmData::Type::ACK:
      sendAcked_ += imm.data();
      return 0;
    case ImmData::Type::CLOSE:
      // change state to CLOSE
      if (state_ != State::ERROR) {
        XLOGF(INFO, "IBSocket {} closed.", describe());
        state_ = State::CLOSE;
        events |= kEventReadableFlag;
      }
      return 0;
    default:
      assert(false);
      return -1;
  }
}

Result<Void> IBSocket::checkState() {
  switch (state_.load(std::memory_order_seq_cst)) {
    case State::READY:
      return Void{};
    case State::CLOSE:
      return makeError(RPCCode::kSocketClosed);
    default:
      return makeError(RPCCode::kSocketError);
  }
}

Result<size_t> IBSocket::send(struct iovec *iov, uint32_t cnt) {
  ssize_t total = 0;
  for (size_t i = 0; i < cnt; i++) {
    auto result = send(folly::ByteRange((uint8_t *)iov[i].iov_base, iov[i].iov_len));
    RETURN_ON_ERROR(result);

    total += result.value();
    if ((size_t)result.value() < iov[i].iov_len) {
      break;
    }
  }

  return total;
}

Result<size_t> IBSocket::send(folly::ByteRange buf) {
  RETURN_ON_ERROR(checkState());

  if (sendWaitBufBegin_ && !sendBufs_.empty()) {
    // record wait send buf latency
    auto begin = sendWaitBufBegin_.exchange(0);
    auto dur = SteadyClock::now().time_since_epoch().count() - begin;
    waitSendBufLatency.addSample(std::chrono::nanoseconds(dur));
  }

  size_t total = 0;
  size_t wanted = buf.size();
  while (!sendBufs_.empty()) {
    if (buf.empty()) {
      break;
    }

    auto [sendBufIdx, sendBuf] = sendBufs_.front();
    size_t wsize = std::min(buf.size(), sendBuf.size());
    memcpy(sendBuf.data(), buf.data(), wsize);

    sendBuf.advance(wsize);
    buf.advance(wsize);
    total += wsize;

    if (sendBuf.empty()) {
      sendBufs_.pop();
      if (UNLIKELY(postSend(sendBufIdx, sendBufs_.getBufSize()))) {
        return makeError(RPCCode::kSendFailed, "IBSocket failed to post send.");
      }
    }
  }

  if (UNLIKELY(total < wanted)) {
    // no enough send buf, need wait send buf available
    auto zero = (uint64_t)0;
    auto now = SteadyClock::now().time_since_epoch().count();
    sendWaitBufBegin_.compare_exchange_strong(zero, now);
  }

  return total;
}

Result<Void> IBSocket::flush() {
  if (!sendBufs_.empty() && sendBufs_.front().second.size() != sendBufs_.getBufSize()) {
    auto [sendBufIdx, sendBuf] = sendBufs_.front();
    auto size = sendBufs_.getBufSize() - sendBuf.size();
    sendBufs_.pop();
    if (UNLIKELY(postSend(sendBufIdx, size))) {
      return makeError(RPCCode::kSendFailed, "IBSocket failed to post send.");
    }
  }
  return Void{};
}

Result<size_t> IBSocket::recv(folly::MutableByteRange buf) {
  size_t total = 0;
  while (!recvBufs_.empty()) {
    if (buf.empty()) {
      break;
    }

    auto [recvedBufIdx, recvedBuf] = recvBufs_.front();
    size_t rsize = std::min(recvedBuf.size(), buf.size());
    memcpy(buf.data(), recvedBuf.data(), rsize);

    recvedBuf.advance(rsize);
    buf.advance(rsize);
    total += rsize;

    if (recvedBuf.empty()) {
      recvBufs_.pop();
      if (UNLIKELY(postRecv(recvedBufIdx))) {
        return makeError(RPCCode::kSocketError, "IBSocket failed to post recv.");
      }

      recvNotAcked_++;
      if (recvNotAcked_ == connectConfig_.buf_ack_batch) {
        recvNotAcked_ = 0;
        auto ackBufAvailable = ackBufAvailable_.fetch_sub(1, std::memory_order_relaxed) >= 1;
        if (UNLIKELY(!ackBufAvailable)) {
          XLOGF(DBG, "IBSocket {} ACK buf not available, wait ACK WR signaled.", describe());
        } else if (UNLIKELY(postAck())) {
          return makeError(RPCCode::kSocketError, "IBSocket failed to post ACK.");
        }
      }
    }
  }

  if (LIKELY(total)) {
    return total;
  }

  RETURN_ON_ERROR(checkState());
  return 0;
}

CoTask<void> IBSocket::close() {
  XLOGF(INFO, "IBSocket {} close", describe());

  // set a flags to notify we are going to close this socket
  closed_ = true;
  if (state_.load(std::memory_order_seq_cst) == State::INIT) {
    co_return;
  }
  XLOGF_IF(FATAL,
           !qp_,
           "IBSocket {} state is {}, but QP is nullptr!!",
           describe(),
           magic_enum::enum_name(state_.load()));

  static constexpr auto kTimeout = 200_ms;
  auto begin = SteadyClock::now();
  if (closeGracefully()) {
    while ((state_.load() == State::READY || state_.load() == State::CONNECTING) &&
           SteadyClock::now() - begin < kTimeout) {
      co_await folly::coro::sleep(std::chrono::milliseconds(50));
    }
  }

  // modify QP to ERROR state
  XLOGF(DBG, "IBSocket {} modify QP to ERROR", describe());

  ibv_qp_attr attr{.qp_state = IBV_QPS_ERR};
  int ret = ibv_modify_qp(qp_.get(), &attr, IBV_QP_STATE);
  XLOGF_IF(CRITICAL, ret != 0, "IBSocket {} failed to modify QP to ERR, ret {}", describe(), ret);
}

bool IBSocket::checkConnectFinished() const {
  if (UNLIKELY(state_ == State::ACCEPTED)) {
    XLOGF(CRITICAL, "IBSocket {} accept timeout, port {}", describe(), port_);
    acceptTimeout.addSample(1);
    return false;
  }
  return true;
}

Result<Void> IBSocket::check() {
  switch (state_.load(std::memory_order_seq_cst)) {
    case State::CLOSE:
      return makeError(RPCCode::kSocketClosed);
    case State::ERROR:
      return makeError(RPCCode::kSocketError);
    case State::READY:
      break;
    case State::ACCEPTED:
      if (SteadyClock::now() > acceptTimeout_) {
        XLOGF(CRITICAL, "IBSocket {} accept timeout, port {}", describe(), port_);
        acceptTimeout.addSample(1);
        return makeError(RPCCode::kSocketError);
      }
      return Void{};
    case State::INIT:
    case State::CONNECTING:
    default:
      // socket is connecting
      return Void{};
  }

  if (checkMsgSended_.exchange(true)) {
    return Void{};
  }

  // post a empty RDMA msg to check liveness
  RDMAPostCtx ctx;
  ibv_send_wr wr{
      .wr_id = WRId::check(),
      .next = nullptr,
      .sg_list = nullptr,
      .num_sge = 0,
      .opcode = IBV_WR_RDMA_WRITE,
      .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE,
      .imm_data = 0,
  };
  ibv_send_wr *badWr = nullptr;
  int ret = ibv_post_send(qp_.get(), &wr, &badWr);
  if (UNLIKELY(ret)) {
    checkMsgSended_ = false;
    auto msg =
        fmt::format("IBSocket {} failed to post check msg, closed {}, errno {}", describe(), closed_.load(), ret);
    XLOG(CRITICAL, msg);
    return makeError(RPCCode::kSocketError, std::move(msg));
  }

  return Void{};
}

// disconnect gracefully
bool IBSocket::closeGracefully() {
  closed_ = true;
  if (closeMsgSended_.exchange(true)) {
    // close message already sended
    return false;
  }

  auto state = state_.load(std::memory_order_seq_cst);
  if (state != State::READY && state != State::CONNECTING) {
    // QP is not ready to send, can't close gracefully
    return false;
  }

  XLOGF(DBG, "IBSocket {} send close msg.", describe());

  // post close msg, a empty RDMA write with ImmData::close(),
  // this also notify peer this socket has been closed.
  RDMAPostCtx ctx;
  ibv_send_wr wr{
      .wr_id = WRId::close(),
      .next = nullptr,
      .sg_list = nullptr,
      .num_sge = 0,
      .opcode = IBV_WR_RDMA_WRITE_WITH_IMM,
      .send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE,
      .imm_data = ImmData::close(),
  };
  ibv_send_wr *badWr = nullptr;
  int ret = ibv_post_send(qp_.get(), &wr, &badWr);
  if (UNLIKELY(ret)) {
    XLOGF_IF(WARN, state == State::READY, "IBSocket {} failed to post close msg, errno {}", describe(), ret);
    return false;
  }

  return true;
}

int IBSocket::postSend(uint32_t idx, size_t len, uint32_t flags) {
  IBDBG("IBSocket {} post send buffer, idx {}, len {}, flags {}.", describe(), idx, len, flags);
  sendBytes.addSample(len);

  ibv_sge sge{
      .addr = (uint64_t)sendBufs_.getBuf(idx),
      .length = (uint32_t)len,
      .lkey = sendBufs_.getMr()->lkey,
  };

  uint32_t signal = 0;
  if (++sendNotSignaled_ >= connectConfig_.buf_signal_batch) {
    signal = sendNotSignaled_;
    sendNotSignaled_ = 0;
    flags |= IBV_SEND_SIGNALED;
  }
  ibv_send_wr wr{
      .wr_id = WRId::send(signal),
      .next = nullptr,
      .sg_list = &sge,
      .num_sge = 1,
      .opcode = IBV_WR_SEND,
      .send_flags = flags,
  };
  ibv_send_wr *badWr = nullptr;
  int ret = ibv_post_send(qp_.get(), &wr, &badWr);
  XLOGF_IF(CRITICAL, ret != 0, "IBSocket {} failed to post send, closed {}, errno {}", describe(), closed_.load(), ret);
  return ret;
}

int IBSocket::postAck() {
  IBDBG("IBSocket {} post ACK.", describe());

  ibv_send_wr wr{
      .wr_id = WRId::ack(),
      .next = nullptr,
      .sg_list = nullptr,
      .num_sge = 0,
      .opcode = IBV_WR_SEND_WITH_IMM,
      .send_flags = IBV_SEND_SIGNALED,
      .imm_data = ImmData::ack(connectConfig_.buf_ack_batch),
  };
  ibv_send_wr *badWr = nullptr;
  int ret = ibv_post_send(qp_.get(), &wr, &badWr);
  XLOGF_IF(CRITICAL,
           ret != 0,
           "IBSocket {} failed to post send for ACK, closed {}, errno {}",
           describe(),
           closed_.load(),
           ret);
  return ret;
}

int IBSocket::postRecv(uint32_t idx) {
  IBDBG("IBSocket {} post recv buffer {}.", describe(), idx);

  ibv_sge sge{
      .addr = (uint64_t)recvBufs_.getBuf(idx),
      .length = (uint32_t)recvBufs_.getBufSize(),
      .lkey = recvBufs_.getMr()->lkey,
  };
  ibv_recv_wr wr{
      .wr_id = WRId::recv(idx),
      .next = nullptr,
      .sg_list = &sge,
      .num_sge = 1,
  };
  ibv_recv_wr *badWr = nullptr;
  int ret = ibv_post_recv(qp_.get(), &wr, &badWr);
  XLOGF_IF(CRITICAL, ret != 0, "IBSocket {} failed to post recv, closed {}, errno {}", describe(), closed_.load(), ret);
  return ret;
}

CoTryTask<void> IBSocket::rdmaBatch(ibv_wr_opcode opcode,
                                    const std::span<const RDMAReq> reqs,
                                    const std::span<RDMABuf> localBufs,
                                    std::chrono::nanoseconds &waitLatency,
                                    std::chrono::nanoseconds &transferLatency) {
  IBDBG("IBSocket {} rdmaBatch: opcode {}, {} reqs, {} localBufs",
        describe(),
        magic_enum::enum_name(opcode),
        reqs.size(),
        localBufs.size());

  if (UNLIKELY(opcode != IBV_WR_RDMA_WRITE && opcode != IBV_WR_RDMA_READ)) {
    XLOGF(ERR, "IBSocket {} postRdma with invalid opcode {}", describe(), magic_enum::enum_name(opcode));
    co_return makeError(StatusCode::kInvalidArg, "Invalid RDMA opcode.");
  }

  auto begin = std::chrono::steady_clock::now();
  size_t wrsPerPost = connectConfig_.max_rdma_wr_per_post;
  size_t numPosts = (reqs.size() + wrsPerPost - 1) / wrsPerPost;
  std::vector<RDMAPostCtx> posts(numPosts);

  if (UNLIKELY(numPosts == 0)) {
    co_return Void{};
  }

  /* setup post info and wait on semaphore */
  for (size_t i = 0; i < numPosts; i++) {
    auto &post = posts[i];
    post.opcode = opcode;
    post.reqs = reqs.subspan(i * wrsPerPost, std::min(reqs.size() - i * wrsPerPost, wrsPerPost));
    post.localBufs = localBufs;
    post.waiter.emplace(post.reqs.size());
    if (rdmaSem_.try_wait(post.waiter.value(), post.reqs.size())) {
      post.waiter = std::nullopt;
    }
  }

  if (posts.size() == 1) {
    auto result = co_await rdmaPost(posts[0]);
    CO_RETURN_ON_ERROR(result);
  } else {
    std::vector<CoTryTask<void>> tasks;
    tasks.reserve(numPosts);
    for (size_t i = 0; i < numPosts; i++) {
      tasks.emplace_back(rdmaPost(posts[i]));
    }

    auto results = co_await folly::coro::collectAllRange(std::move(tasks));
    for (auto &result : results) {
      CO_RETURN_ON_ERROR(result);
    }
  }

  auto bytes = 0;
  auto transferBegin = posts[0].postBegin;
  auto transferEnd = posts[0].postEnd;
  for (auto &post : posts) {
    transferBegin = std::min(post.postBegin, transferBegin);
    transferEnd = std::max(post.postEnd, transferEnd);
    bytes += post.bytes;
  }

  waitLatency = transferBegin - begin;
  transferLatency = transferEnd - transferBegin;

  const auto &bytesTag = config_.record_bytes_per_peer() ? peerTag_ : tag_;
  const auto &latencyTag =
      (config_.record_latency_per_peer() || connectConfig_.record_latency_per_peer) ? peerTag_ : tag_;
  rdmaWaitLatency.addSample(waitLatency, latencyTag);
  if (opcode == IBV_WR_RDMA_READ) {
    rdmaReadBytes.addSample(bytes, bytesTag);
    rdmaReadLatency.addSample(transferLatency, latencyTag);
  } else {
    rdmaWriteBytes.addSample(bytes, bytesTag);
    rdmaWriteLatency.addSample(transferLatency, latencyTag);
  }

  co_return Void{};
}

CoTryTask<void> IBSocket::rdmaPost(RDMAPostCtx &ctx) {
  IBDBG("IBSocket {} postRdma: opcode {}, {} reqs", describe(), magic_enum::enum_name(ctx.opcode), ctx.reqs.size());

  if (ctx.waiter.has_value()) {
    co_await ctx.waiter->baton;
  }
  auto guard = folly::makeGuard([&]() { rdmaSem_.signal(ctx.reqs.size()); });
  CO_RETURN_ON_ERROR(checkState());

  if (auto ret = rdmaPostWR(ctx); UNLIKELY(ret != 0)) {
    co_return makeError(RPCCode::kRDMAPostFailed);
  }
  co_await ctx.baton;
  if (ctx.status != IBV_WC_SUCCESS) {
    XLOGF(DBG, "IBSocket {} RDMA failed, error {}", describe(), ibv_wc_status_str(ctx.status));
    co_return makeError(RPCCode::kRDMAError);
  }

  co_return Void{};
}

int IBSocket::rdmaPostWR(RDMAPostCtx &ctx) {
  static thread_local folly::small_vector<ibv_send_wr, 256> wrs;
  static thread_local folly::small_vector<ibv_sge, 2048> sges;
  wrs.clear();
  sges.clear();

  XLOGF_IF(FATAL, ctx.reqs.empty(), "Empty reqs");

  size_t numSges = 0;
  for (auto &req : ctx.reqs) {
    numSges += req.localBufCnt;
  }
  wrs.resize(ctx.reqs.size());
  sges.resize(numSges);

  size_t wrId = 0, sgeId = 0;
  ctx.bytes = 0;
  for (auto &req : ctx.reqs) {
    assert(req.localBufCnt > 0);

    auto &wr = wrs[wrId++];
    wr.wr_id = WRId::rdma(&ctx, false);
    wr.next = &wr + 1;
    wr.sg_list = &sges[sgeId];
    wr.num_sge = req.localBufCnt;
    wr.opcode = ctx.opcode;
    wr.send_flags = 0;
    wr.wr.rdma.remote_addr = req.raddr;
    wr.wr.rdma.rkey = req.rkey;

    for (auto &buf : ctx.localBufs.subspan(req.localBufFirst, req.localBufCnt)) {
      auto mr = buf.getMR(port_.dev()->id());
      if (UNLIKELY(!mr)) {
        XLOGF(ERR, "IBSocket {} failed to get RDMABufMR", describe());
        return -1;
      }

      auto &sge = sges[sgeId++];
      sge.addr = (uint64_t)buf.ptr();
      sge.length = (uint32_t)buf.size();
      sge.lkey = mr->lkey;
      ctx.bytes += sge.length;
    }
  }

  wrs.rbegin()->next = nullptr;
  wrs.rbegin()->wr_id = WRId::rdma(&ctx, true);
  wrs.rbegin()->send_flags |= IBV_SEND_SIGNALED;

  IBDBG("IBSocket {} post opcode {}, {} WRs, {} bytes.",
        describe(),
        magic_enum::enum_name(ctx.opcode),
        wrs.size(),
        ctx.bytes);

  ctx.postBegin = std::chrono::steady_clock::now();

  ibv_send_wr *bad = nullptr;
  const int ret = ibv_post_send(qp_.get(), &wrs[0], &bad);
  if (LIKELY(ret == 0)) {
    // success
    return 0;
  }

  // ibv_post_send fails when the work request is invalid, this should not happen
  if (bad == &wrs[0]) {
    XLOGF(CRITICAL, "IBSocket {} failed to post RDMA, closed {}, errno {}", describe(), closed_.load(), ret);
  } else {
    // Only a subset of RDMA work requests were successfully posted. As only the final WR has the IBV_SEND_SIGNALED flag
    // set, there is no way to track when the posted RDMA WRs will complete. We need set the QP to error state.
    XLOGF(DFATAL,
          "IBSocket {} failed to post RDMA, closed {}, errno {}, wr {}, badwr {}",
          describe(),
          closed_.load(),
          ret,
          (void *)&wrs[0],
          (void *)bad);
    ibv_qp_attr attr{.qp_state = IBV_QPS_ERR};
    int ret = ibv_modify_qp(qp_.get(), &attr, IBV_QP_STATE);
    XLOGF_IF(FATAL, ret != 0, "IBSocket {} failed to modify QP to ERR, ret {}", describe(), ret);
  }

  return ret;
}

IBSocket::Drainer::Ptr IBSocket::Drainer::create(IBSocket::Ptr socket, std::weak_ptr<IBSocketManager> manager) {
  if (!socket || !socket->closeGracefully()) {
    return {};
  }

  ibv_qp_attr attr;
  ibv_qp_init_attr init_attr;
  auto ret = ibv_query_qp(socket->qp_.get(), &attr, IBV_QP_STATE, &init_attr);
  if (ret != 0 || attr.qp_state != IBV_QPS_RTS) {
    XLOGF_IF(CRITICAL, ret != 0, "ibv_query_qp failed, errno {}", ret);
    return {};
  }

  return std::make_shared<Drainer>(std::move(socket), manager);
}

IBSocket::Drainer::Drainer(IBSocket::Ptr socket, std::weak_ptr<IBSocketManager> manager)
    : socket_(std::move(socket)),
      manager_(std::move(manager)) {
  draining.addSample(1);
}

IBSocket::Drainer::~Drainer() { draining.addSample(-1); }

void IBSocket::Drainer::handleEvents(uint32_t /* epollEvents */) {
  auto finished = socket_->drain();
  if (finished) {
    auto eventloop = eventLoop_.lock();
    eventloop->remove(this);
    auto manager = manager_.lock();
    if (manager) {
      manager->remove(shared_from_this());
    }
  }
}

// drain CQ on close.
bool IBSocket::drain() {
  XLOGF(DBG, "IBSocket {} drain get event", describe());

  if (cqGetEvent() != 0 || cqRequestNotify() != 0) {
    return true;
  }

  ibv_wc wc;
  int ret = 0;
  while ((ret = ibv_poll_cq(cq_.get(), 1, &wc)) != 0) {
    if (ret < 0) {
      // CQ overrun shouldn't happen.
      XLOGF(CRITICAL, "IBSocket {} failed to poll CQ while draining, errno {}", describe(), ret);
      return true;
    }

    auto wr = WRId(wc.wr_id);
    XLOGF(DBG, "IBSocket drain {} get WC {}", describe(), wc);
    if (wc.status != IBV_WC_SUCCESS) {
      XLOGF(WARN, "IBSocket {} get error {}, stop drain.", describe(), ibv_wc_status_str(wc.status));
      return true;
    }
    auto localClose = wr.type() == WRType::CLOSE;
    auto remoteClose =
        wr.type() == WRType::RECV && (wc.wc_flags & IBV_WC_WITH_IMM) && ImmData(wc.imm_data) == ImmData::close();
    if (localClose || remoteClose) {
      XLOGF(INFO, "IBSocket {} closed.", describe());
      return true;
    }
    XLOGF_IF(FATAL, wr.type() == WRType::RDMA_LAST, "IBSocket {} get RDMA_LAST while draining.", describe());
  }

  return false;
}

std::shared_ptr<IBSocketManager> IBSocketManager::create() {
  auto fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
  if (fd < 0) {
    XLOGF(ERR, "Failed to create timer fd, errno {}", errno);
    return {};
  }

  itimerspec spec{{0, 200000000}, {0, 200000000}};
  auto ret = timerfd_settime(fd, 0, &spec, nullptr);
  XLOGF_IF(FATAL, ret != 0, "timerfd_settime failed, errno {}", errno);
  return std::make_shared<IBSocketManager>(fd);
}

void IBSocketManager::stopAndJoin() {
  static constexpr auto kDrainTimeout = 500ms;
  XLOGF(INFO, "IBSocketManager::stopAndJoin");
  auto begin = SteadyClock::now();
  while (SteadyClock::now() < begin + kDrainTimeout) {
    if (auto draining = drainers_.lock()->size(); draining != 0) {
      XLOGF(INFO, "IBSocketManager wait {} sockets draining.", draining);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    } else {
      break;
    }
  }

  auto drainers = drainers_.lock();
  XLOGF_IF(CRITICAL, !drainers->empty(), "IBSocketManager {} sockets drain timeout", drainers->size());
  drainers->clear();
  XLOGF(INFO, "IBSocketManager stopped!");
}

void IBSocketManager::close(IBSocket::Ptr socket) {
  if (UNLIKELY(!socket)) {
    return;
  }
  auto manager = shared_from_this();
  auto timeout = socket->config_.drain_timeout();
  auto drainer = IBSocket::Drainer::create(std::move(socket), manager);
  if (drainer) {
    drainers_.lock()->insert(drainer);
    deadlines_.lock()->emplace(SteadyClock::now() + timeout, drainer);
    IBManager::instance().eventLoop_->add(drainer, (EPOLLIN | EPOLLOUT | EPOLLET));
  }
}

void IBSocketManager::handleEvents(uint32_t /* events */) {
  std::array<char, 64> buf;
  while (true) {
    auto readed = ::read(timer_, buf.data(), buf.size());
    if (readed < (int)buf.size()) {
      break;
    }
  }

  XLOGF(DBG, "IBSocketManager handleEvents running");
  auto now = SteadyClock::now();
  auto guard = deadlines_.lock();
  auto iter = guard->begin();
  while (iter != guard->end()) {
    if (iter->first > now) {
      break;
    }
    auto drainer = std::dynamic_pointer_cast<IBSocket::Drainer>(iter->second.lock());
    if (drainer) {
      XLOGF(WARN, "IBSocket {} drain timeout.", drainer->describe());
      IBManager::instance().eventLoop_->remove(drainer.get());
      remove(drainer);
    }
    iter = guard->erase(iter);
  }
}

}  // namespace hf3fs::net
