#pragma once

#include <folly/experimental/coro/BlockingWait.h>
#include <memory>

#include "common/net/Client.h"
#include "common/net/Network.h"
#include "common/net/Server.h"
#include "common/serde/CallContext.h"
#include "common/serde/ClientContext.h"
#include "common/utils/Address.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"

namespace hf3fs::serde {

class ClientMockContext {
 public:
  enum IsMock : bool { value = true };

  template <class Service>
  static ClientMockContext create(std::unique_ptr<Service> service, net::Address::Type type = net::Address::TCP) {
    ClientMockContext ctx;
    ctx.setService(std::move(service), type);
    return ctx;
  }

  template <class Service>
  bool setService(std::unique_ptr<Service> service, net::Address::Type type = net::Address::TCP) {
    pack_ = std::make_shared<ServerPack>(type);
    auto setupResult = pack_->server_->setup();
    XLOGF_IF(FATAL, !setupResult, "setup server failed: {}", setupResult.error());

    auto addResult = pack_->server_->addSerdeService(std::move(service));
    XLOGF_IF(FATAL, !addResult, "add service failed: {}", addResult.error());

    auto startResult = pack_->server_->start();
    XLOGF_IF(FATAL, !startResult, "start server failed: {}", startResult.error());

    auto clientResult = pack_->client_->start();
    XLOGF_IF(FATAL, !clientResult, "start client failed: {}", clientResult.error());
    return true;
  }

  template <NameWrapper kServiceName,
            NameWrapper kMethodName,
            class Req,
            class Rsp,
            uint16_t ServiceID,
            uint16_t MethodID>
  CoTryTask<Rsp> call(const Req &req,
                      const net::UserRequestOptions *options = nullptr,
                      Timestamp *timestamp = nullptr) {
    auto ctx = pack_->client_->serdeCtx(pack_->server_->groups().front()->addressList().front());
    co_return co_await ctx.call<kServiceName, kMethodName, Req, Rsp, ServiceID, MethodID>(req, options, timestamp);
  }

  template <NameWrapper kServiceName,
            NameWrapper kMethodName,
            class Req,
            class Rsp,
            uint16_t ServiceID,
            uint16_t MethodID>
  Result<Rsp> callSync(const Req &req,
                       const net::UserRequestOptions *options = nullptr,
                       Timestamp *timestamp = nullptr) {
    return folly::coro::blockingWait(
        call<kServiceName, kMethodName, Req, Rsp, ServiceID, MethodID>(req, options, timestamp));
  }

 private:
  struct ServerPack {
    ServerPack(net::Address::Type type) {
      for (size_t i = 0; i < serverConfig_.groups_length(); i++) {
        serverConfig_.groups(i).set_network_type(type);
      }
      server_ = std::make_unique<net::Server>(serverConfig_);
      client_ = std::make_unique<net::Client>(clientConfig_);
    }
    ~ServerPack() {
      client_->stopAndJoin();
      server_->stopAndJoin();
    }
    net::Server::Config serverConfig_;
    std::unique_ptr<net::Server> server_;
    net::Client::Config clientConfig_;
    std::unique_ptr<net::Client> client_;
  };
  std::shared_ptr<ServerPack> pack_;
};

}  // namespace hf3fs::serde
