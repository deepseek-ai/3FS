#pragma once

#include "common/net/IOWorker.h"
#include "common/net/Transport.h"
#include "common/net/Waiter.h"
#include "common/net/WriteItem.h"
#include "common/net/sync/ConnectionPool.h"
#include "common/serde/ClientContext.h"
#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "common/serde/Service.h"
#include "common/utils/Duration.h"

namespace hf3fs::serde {

class ClientContext {
 public:
  ClientContext(net::IOWorker &ioWorker,
                net::Address destAddr,
                const folly::atomic_shared_ptr<const net::CoreRequestOptions> &options)
      : connectionSource_(&ioWorker),
        destAddr_(destAddr),
        options_(options) {}

  ClientContext(net::sync::ConnectionPool &connectionPool,
                net::Address destAddr,
                const folly::atomic_shared_ptr<const net::CoreRequestOptions> &options)
      : connectionSource_(&connectionPool),
        destAddr_(destAddr),
        options_(options) {}

  ClientContext(const net::TransportPtr &tr);

  template <NameWrapper kServiceName,
            NameWrapper kMethodName,
            class Req,
            class Rsp,
            uint16_t ServiceID,
            uint16_t MethodID>
  CoTryTask<Rsp> call(const Req &req,
                      const net::UserRequestOptions *customOptions = nullptr,
                      Timestamp *timestamp = nullptr) {
    auto options = *options_.load(std::memory_order_acquire);
    if (customOptions != nullptr) {
      options.merge(*customOptions);
    }

    net::Waiter::Item item;
    uint64_t uuid = net::Waiter::instance().bind(item);

    Timestamp ts;
    if ((options.logLongRunningThreshold != 0_ms || options.reportMetrics) && timestamp == nullptr) {
      timestamp = &ts;
    }

    MessagePacket packet(req);
    packet.uuid = uuid;
    packet.serviceId = ServiceID;
    packet.methodId = MethodID;
    packet.flags = EssentialFlags::IsReq;
    if (options.compression) {
      packet.flags |= EssentialFlags::UseCompress;
    }
    if (options.enableRDMAControl) {
      packet.flags |= EssentialFlags::ControlRDMA;
    }
    if (timestamp != nullptr) {
      packet.timestamp = Timestamp{UtcClock::now().time_since_epoch().count()};
    }

    auto writeItem = net::WriteItem::createMessage(packet, options);
    writeItem->uuid = uuid;
    auto requestLength = writeItem->buf->length();
    if (LIKELY(std::holds_alternative<net::IOWorker *>(connectionSource_))) {
      std::get<net::IOWorker *>(connectionSource_)->sendAsync(destAddr_, net::WriteList(std::move(writeItem)));
    } else if (std::holds_alternative<net::Transport *>(connectionSource_)) {
      std::get<net::Transport *>(connectionSource_)->send(net::WriteList(std::move(writeItem)));
    } else {
      co_return MAKE_ERROR_F(StatusCode::kFoundBug,
                             "Sync client call async method: service id {}, method id {}",
                             ServiceID,
                             MethodID);
    }

    net::Waiter::instance().schedule(uuid, options.timeout);
    co_await item.baton;

    if (UNLIKELY(!item.status)) {
      if (item.status.code() == RPCCode::kTimeout && std::holds_alternative<net::IOWorker *>(connectionSource_)) {
        if (item.transport) {
          XLOGF(INFO, "req timeout and close transport {}", fmt::ptr(item.transport.get()));
          co_await item.transport->closeIB();
        } else {
          XLOGF(INFO, "req timeout but no transport");
        }
        std::get<net::IOWorker *>(connectionSource_)->checkConnections(destAddr_, Duration::zero());
      }
      co_return makeError(std::move(item.status));
    }

    Result<Rsp> rsp = makeError(StatusCode::kUnknown);
    auto deserializeResult = serde::deserialize(rsp, item.packet.payload);
    if (UNLIKELY(!deserializeResult)) {
      XLOGF(ERR, "deserialize rsp error: {}", deserializeResult.error());
      if (item.transport) {
        item.transport->invalidate();
      }
      co_return makeError(std::move(deserializeResult.error()));
    }
    if (timestamp != nullptr && item.packet.timestamp.has_value()) {
      *timestamp = *item.packet.timestamp;
      timestamp->clientWaked = UtcClock::now().time_since_epoch().count();
    }
    if (options.logLongRunningThreshold != 0_ms && timestamp->totalLatency() >= options.logLongRunningThreshold) {
      XLOGF(WARNING,
            "req takes too long, total {}, server {}, network {}, queue {}\ndetails {}",
            timestamp->totalLatency(),
            timestamp->serverLatency(),
            timestamp->networkLatency(),
            timestamp->queueLatency(),
            serde::toJsonString(*timestamp));
    }
    if (timestamp && options.reportMetrics) {
      static const auto methodName = fmt::format("{}::{}",
                                                 static_cast<std::string_view>(kServiceName),
                                                 static_cast<std::string_view>(kMethodName));
      static const auto tags = monitor::TagSet::create("instance", methodName);
      reportMetrics(tags, timestamp, requestLength, item.buf->length());
    }
    co_return rsp;
  }

  template <NameWrapper kServiceName,
            NameWrapper kMethodName,
            class Req,
            class Rsp,
            uint16_t ServiceID,
            uint16_t MethodID>
  Result<Rsp> callSync(const Req &req,
                       const net::UserRequestOptions *customOptions = nullptr,
                       Timestamp *timestamp = nullptr) {
    auto options = *options_.load(std::memory_order_acquire);
    if (customOptions != nullptr) {
      options.merge(*customOptions);
    }
    auto startTime = RelativeTime::now();

    MessagePacket packet(req);
    packet.serviceId = ServiceID;
    packet.methodId = MethodID;
    packet.flags = EssentialFlags::IsReq;
    if (timestamp != nullptr) {
      packet.timestamp = Timestamp{UtcClock::now().time_since_epoch().count()};
    }
    auto buff = net::SerdeBuffer::create<MessagePacket<Req>, net::SystemAllocator<>>(packet, options);

    // acquire connection.
    if (UNLIKELY(!std::holds_alternative<net::sync::ConnectionPool *>(connectionSource_))) {
      return makeError(RPCCode::kConnectFailed);
    }
    auto connectionPool = std::get<net::sync::ConnectionPool *>(connectionSource_);
    auto acquireConnectionResult = connectionPool->acquire(destAddr_);
    if (UNLIKELY(!acquireConnectionResult)) {
      return makeError(RPCCode::kConnectFailed);
    }

    auto conn = std::move(acquireConnectionResult.value());
    RETURN_AND_LOG_ON_ERROR(conn->sendSync(buff->buff(), buff->length(), startTime, options.timeout));

    net::MessageHeader header;
    RETURN_AND_LOG_ON_ERROR(
        conn->recvSync(folly::MutableByteRange{reinterpret_cast<uint8_t *>(&header), net::kMessageHeaderSize},
                       startTime,
                       options.timeout));
    auto rspBuff = net::IOBuf::createCombined(header.size);
    rspBuff->append(header.size);
    RETURN_AND_LOG_ON_ERROR(
        conn->recvSync(folly::MutableByteRange{rspBuff->writableData(), header.size}, startTime, options.timeout));

    MessagePacket recv;
    RETURN_AND_LOG_ON_ERROR(
        serde::deserialize(recv, std::string_view(reinterpret_cast<const char *>(rspBuff->buffer()), header.size)));

    Result<Rsp> rsp = makeError(StatusCode::kUnknown);
    RETURN_AND_LOG_ON_ERROR(serde::deserialize(rsp, recv.payload));
    if (timestamp != nullptr && recv.timestamp.has_value()) {
      *timestamp = *recv.timestamp;
      timestamp->clientWaked = UtcClock::now().time_since_epoch().count();
    }
    connectionPool->restore(destAddr_, std::move(conn));
    return rsp;
  }

  net::Address addr() const { return destAddr_; }

 private:
  static void reportMetrics(const monitor::TagSet &tags, Timestamp *ts, uint64_t requestSize, uint64_t responseSize);

  std::variant<net::IOWorker *, net::sync::ConnectionPool *, net::Transport *> connectionSource_;
  net::Address destAddr_{};
  const folly::atomic_shared_ptr<const net::CoreRequestOptions> &options_;
};

}  // namespace hf3fs::serde
