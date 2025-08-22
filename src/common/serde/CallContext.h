#pragma once

#include "common/net/Transport.h"
#include "common/net/ib/IBSocket.h"
#include "common/serde/MessagePacket.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Size.h"
#include "common/utils/Tracing.h"
#include "common/utils/VersionInfo.h"

namespace hf3fs::serde {

class CallContext {
 public:
  using MethodType = CoTask<void> (CallContext::*)();
  using OnErrorType = Status (CallContext::*)(Status);
  struct ServiceWrapper {
    using MethodGetter = MethodType (*)(uint16_t);
    MethodGetter getter = &CallContext::invalidServiceId;
    OnErrorType onError = &CallContext::serviceOnError;
    void *object = nullptr;
    std::shared_ptr<void *> alive = nullptr;
  };

  CallContext(MessagePacket<> &packet, net::TransportPtr tr, ServiceWrapper &service)
      : packet_(packet),
        tr_(std::move(tr)),
        service_(service) {}

  const auto &packet() { return packet_; }
  auto &transport() const { return tr_; }
  auto &responseOptions() { return responseOptions_; }
  std::string peer() { return LIKELY(tr_ != nullptr) ? tr_->peerIP().str() : std::string{"null"}; }

  CoTask<void> handle() {
    auto method = service_.getter(packet_.methodId);
    co_await (this->*method)();
  }

  CoTask<void> invalidId() {
    XLOGF(INFO, "method {}:{} not found!", packet_.serviceId, packet_.methodId);
    onError(makeError(RPCCode::kInvalidMethodID));
    co_return;
  }

  template <class F>
  CoTask<void> call() {
    // deserialize payload.
    if (packet_.timestamp) {
      packet_.timestamp->serverWaked = UtcClock::now().time_since_epoch().count();
    }
    typename F::ReqType req;
    auto deserializeResult = serde::deserialize(req, packet_.payload);
    if (UNLIKELY(!deserializeResult)) {
      onDeserializeFailed();
      co_return;
    }

    // call method.
    auto obj = reinterpret_cast<typename F::Object *>(service_.object);
    auto result = co_await folly::coro::co_awaitTry((obj->*F::method)(*this, req));
    if (UNLIKELY(result.hasException())) {
      XLOGF(FATAL,
            "Processor has exception: {}, request {}:{} {}",
            result.exception().what(),
            packet_.serviceId,
            packet_.methodId,
            serde::toJsonString(req));
      co_return;
    }
    if (packet_.timestamp) {
      packet_.timestamp->serverProcessed = UtcClock::now().time_since_epoch().count();
    }
    makeResponse(result.value());
    co_return;
  }

  void onError(folly::Unexpected<Status> &&status) {
    makeResponse(Result<Void>(makeError((this->*service_.onError)(std::move(status.error())))));
  }

  Status serviceOnError(Status status) { return status; }

  template <class Object, auto Method>
  Status customOnError(Status status) {
    auto obj = reinterpret_cast<Object *>(service_.object);
    return (obj->*Method)(std::move(status));
  }

  void onDeserializeFailed();

  tracing::Points &tracingPoints() { return tracingPoints_; }

  class RDMATransmission {
   public:
    RDMATransmission(CallContext &ctx, ibv_wr_opcode opcode)
        : ctx_(ctx),
          batch_(ctx_.tr_->ibSocket(), opcode) {}

    Result<Void> add(const net::RDMARemoteBuf &remoteBuf, net::RDMABuf localBuf) {
      return batch_.add(remoteBuf, localBuf);
    }

    CoTask<void> applyTransmission(Duration timeout);

    CoTryTask<void> post() { return batch_.post(); }

   private:
    CallContext &ctx_;
    net::IBSocket::RDMAReqBatch batch_;
  };

  RDMATransmission readTransmission() { return RDMATransmission{*this, IBV_WR_RDMA_READ}; }
  RDMATransmission writeTransmission() { return RDMATransmission{*this, IBV_WR_RDMA_WRITE}; }

 private:
  static MethodType invalidServiceId(uint16_t) { return &CallContext::invalidId; }

  void makeResponse(const auto &payload) {
    MessagePacket send(payload);
    send.uuid = packet_.uuid;
    send.serviceId = packet_.serviceId;
    send.methodId = packet_.methodId;
    send.flags = 0;
    send.version = packet_.version;
    send.timestamp = packet_.timestamp;
    tr_->send(net::WriteList(net::WriteItem::createMessage(send, responseOptions_)));
  }

 private:
  MessagePacket<> &packet_;
  net::TransportPtr tr_;
  ServiceWrapper &service_;

  net::CoreRequestOptions responseOptions_;
  tracing::Points tracingPoints_;
};

}  // namespace hf3fs::serde
