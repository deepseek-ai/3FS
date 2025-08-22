#pragma once

#include <atomic>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <limits>
#include <thread>

#include "common/net/MessageHeader.h"
#include "common/net/Network.h"
#include "common/net/Transport.h"
#include "common/net/Waiter.h"
#include "common/serde/CallContext.h"
#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "common/serde/Services.h"
#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/CoroutinesPool.h"
#include "common/utils/DynamicCoroutinesPool.h"
#include "common/utils/Uuid.h"
#include "common/utils/ZSTD.h"

namespace hf3fs::net {

class IOWorker;

class Processor {
 public:
  struct Config : public ConfigBase<Config> {
    // TODO: directly use CoroutinesPool::Config
    CONFIG_ITEM(max_processing_requests_num, 4096ul);
    CONFIG_ITEM(max_coroutines_num, 256ul);
    CONFIG_HOT_UPDATED_ITEM(enable_coroutines_pool, true);
    CONFIG_HOT_UPDATED_ITEM(response_compression_level, 1u);
    CONFIG_HOT_UPDATED_ITEM(response_compression_threshold, 128_KB);
  };

  struct Job {
    IOBufPtr buf;
    serde::MessagePacket<> packet;
    TransportPtr tr;
  };

  explicit Processor(serde::Services &serdeServices, CPUExecutorGroup &executor, const Config &config)
      : serdeServices_(serdeServices),
        executor_(executor),
        config_(config) {}

  void processMsg(MessageWrapper wrapper, TransportPtr tr) {
    auto num = processingRequestsNum(flags_.load(std::memory_order_acquire));
    if (UNLIKELY(num >= config_.max_processing_requests_num())) {
      XLOGF(WARN, "Too many processing requests: {}, unpack message in current thread.", num);
      unpackMsg(wrapper, tr);
    } else {
      executor_.pickNext().add(
          [this, wrapper = std::move(wrapper), tr = std::move(tr)]() mutable { unpackMsg(wrapper, tr); });
    }
  }

  void setCoroutinesPoolGetter(auto &&g) { coroutinesPoolGetter_ = std::forward<decltype(g)>(g); }

  Result<Void> start(std::string_view = {}) { return Void{}; }

  void stopAndJoin() {
    // Mark as stopped, and refuse new requests.
    auto flags = flags_.fetch_or(kStopFlag);
    while (processingRequestsNum(flags) != 0) {
      XLOGF(INFO, "Waiting for {} requests to finish...", processingRequestsNum(flags));
      std::this_thread::sleep_for(100ms);
      flags = flags_.load(std::memory_order_acquire);
    }
  }

  void setFrozen(bool frozen, bool isRDMA) {
    auto flags = (isRDMA ? kFrozenRDMA : kFrozenTCP);
    if (frozen) {
      flags_ |= flags;
    } else {
      flags_ &= ~flags;
    }
  }

 protected:
  void unpackMsg(MessageWrapper &wrapper, TransportPtr &tr) {
    while (wrapper.length()) {
      // 1. check the message is complete.
      if (UNLIKELY(!wrapper.headerComplete() || !wrapper.messageComplete())) {
        XLOGF(ERR,
              "Message is not complete! {} < {}, peer: {}",
              wrapper.length(),
              wrapper.headerComplete() ? wrapper.header().size : 0,
              tr->describe());
        tr->invalidate();
        return;
      }

      if (LIKELY(wrapper.isSerdeMessage())) {
        unpackSerdeMsg(wrapper.cloneMessage(), wrapper.header().checksum, tr);
        wrapper.next();
        continue;
      }

      XLOGF(ERR, "Message is invalid", tr->describe());
      tr->invalidate();
      return;
    }
  }

  Result<Void> decompressSerdeMsg(IOBufPtr &buf, TransportPtr &tr);

  void unpackSerdeMsg(IOBufPtr buf, uint32_t checksumIn, TransportPtr tr) {
    // 1. check checksum. (without timestamp part)
    bool isCompressed = MessageHeader::isCompressed(checksumIn);
    auto checksum = Checksum::calcSerde(buf->data(), buf->length(), isCompressed);
    if (UNLIKELY(checksumIn != checksum)) {
      XLOGF(ERR, "checksum is not matched! {:X} != {:X}, peer: {}", checksumIn, checksum, tr->describe());
      tr->invalidate();
      return;
    }
    // message is verified by checksum and serde deserialize.

    if (isCompressed) {
      auto result = decompressSerdeMsg(buf, tr);
      if (UNLIKELY(!result)) {
        return;
      }
    }

    auto view = std::string_view{reinterpret_cast<const char *>(buf->data()), buf->length()};
    serde::MessagePacket<> packet;
    auto deserializeResult = serde::deserialize(packet, view);
    if (UNLIKELY(!deserializeResult)) {
      XLOGF(ERR, "Message buffer deserialize failed {}!, peer: {}", deserializeResult.error(), tr->describe());
      tr->invalidate();
      return;
    }

    // 2. check this message is request or response.
    if (packet.timestamp) {
      if (packet.isRequest()) {
        packet.timestamp->serverReceived = UtcClock::now().time_since_epoch().count();
      } else {
        packet.timestamp->clientReceived = UtcClock::now().time_since_epoch().count();
      }
    }
    if (packet.flags & serde::EssentialFlags::IsReq) {
      // is request.
      XLOGF(DBG, "receive request {}:{}", packet.serviceId, packet.methodId);
      tryToProcessSerdeRequest(std::move(buf), packet, std::move(tr));
    } else {
      // is response.
      XLOGF(DBG, "receive response {}:{}", packet.serviceId, packet.methodId);
      Waiter::instance().post(packet, std::move(buf));
    }
  }

  CoTask<void> processSerdeRequest(IOBufPtr buf, serde::MessagePacket<> packet, TransportPtr tr) {
    // decrease the count in any case.
    auto guard = folly::makeGuard([&] { flags_ -= kCountInc; });

    (void)buf;  // keep alive.
    auto &service = serdeServices_.getServiceById(packet.serviceId, tr->isRDMA());
    serde::CallContext ctx(packet, std::move(tr), service);
    if (packet.useCompress()) {
      ctx.responseOptions().compression = {config_.response_compression_level(),
                                           config_.response_compression_threshold()};
    }
    co_await ctx.handle();
  }

  void tryToProcessSerdeRequest(IOBufPtr buff, serde::MessagePacket<> &packet, TransportPtr tr) {
    auto flags = flags_.load(std::memory_order_acquire);
    if (UNLIKELY(needRefuse(flags))) {
      refuseSerdeRequest(packet, std::move(tr), flags);
      return;
    }
    if (UNLIKELY(isFrozen(flags, tr->isRDMA()))) {
      // do nothing and return.
      return;
    }

    flags = flags_.fetch_add(kCountInc);
    if (UNLIKELY(needRefuse(flags))) {
      refuseSerdeRequest(packet, std::move(tr), flags);
      flags_ -= kCountInc;  // resume count.
      return;
    }

    if (coroutinesPoolGetter_) {
      coroutinesPoolGetter_(packet).enqueue(processSerdeRequest(std::move(buff), packet, std::move(tr)));
    } else {
      processSerdeRequest(std::move(buff), packet, std::move(tr)).scheduleOn(&executor_.pickNext()).start();
    }
  }

  void refuseSerdeRequest(serde::MessagePacket<> &packet, TransportPtr tr, size_t flags) {
    auto msg =
        fmt::format("Refuse requests, stopped: {}, processing: {}", isStopped(flags), processingRequestsNum(flags));
    XLOG(WARN, msg);
    auto &service = serdeServices_.getServiceById(packet.serviceId, tr->isRDMA());
    serde::CallContext ctx(packet, std::move(tr), service);
    ctx.onError(makeError(RPCCode::kRequestRefused, msg));
  }

  inline bool needRefuse(size_t flags) const {
    return isStopped(flags) || processingRequestsNum(flags) >= config_.max_processing_requests_num();
  }
  inline bool isStopped(size_t flags) const { return flags & kStopFlag; }
  inline bool isFrozen(size_t flags, bool isRDMA) const { return flags & (isRDMA ? kFrozenRDMA : kFrozenTCP); }
  inline size_t processingRequestsNum(size_t flags) const { return flags / kCountInc; }

 private:
  serde::Services &serdeServices_;
  CPUExecutorGroup &executor_;
  const Config &config_;
  using CoroutinesPoolGetter = std::function<DynamicCoroutinesPool &(const serde::MessagePacket<> &)>;
  CoroutinesPoolGetter coroutinesPoolGetter_{};

  constexpr static size_t kStopFlag = 1;
  constexpr static size_t kFrozenTCP = 2;
  constexpr static size_t kFrozenRDMA = 4;
  constexpr static size_t kCountInc = 8;
  std::atomic<size_t> flags_{0};
};

}  // namespace hf3fs::net
