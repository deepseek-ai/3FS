#pragma once

#include <folly/logging/xlog.h>
#include <functional>
#include <memory>
#include <type_traits>
#include <variant>

#include "client/mgmtd/ICommonMgmtdClient.h"
#include "client/mgmtd/MgmtdClientForServer.h"
#include "common/app/NodeId.h"
#include "common/net/Client.h"
#include "common/net/RequestOptions.h"
#include "common/serde/CallContext.h"
#include "common/serde/ClientContext.h"
#include "common/serde/ClientMockContext.h"
#include "common/serde/MessagePacket.h"
#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StatusCode.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "fmt/core.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {

class Forward {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(debug, true);
    CONFIG_HOT_UPDATED_ITEM(addr_type, net::Address::Type::RDMA);
    CONFIG_HOT_UPDATED_ITEM(timeout, 10_s);
  };

  using NetClient = std::reference_wrapper<net::Client>;
  using MockClient = std::reference_wrapper<std::map<flat::NodeId, serde::ClientMockContext>>;

  Forward(const Config &config,
          flat::NodeId nodeId,
          std::variant<NetClient, MockClient> client,
          std::shared_ptr<::hf3fs::client::ICommonMgmtdClient> mgmtdClient)
      : config_(config),
        nodeId_(nodeId),
        client_(client),
        mgmtdClient_(mgmtdClient) {
    XLOGF_IF(FATAL, !nodeId_, "invalid nodeId {}", nodeId_);
  }

  template <typename Req, typename Rsp>
  CoTryTask<Rsp> forward(flat::NodeId node, Req req) {
    OperationRecorder::Guard record(OperationRecorder::server(), "forward", req.user.uid);
    auto result = co_await forwardImpl<Req, Rsp>(node, std::move(req));
    record.finish(result);
    co_return result;
  }

 private:
  template <typename Req, typename Rsp, typename Context>
  struct ForwardMethod {};

  template <typename Context>
  struct ForwardMethod<SyncReq, SyncRsp, Context> {
    static constexpr auto rpcMethod = MetaSerde<>::sync<Context>;
  };

  template <typename Context>
  struct ForwardMethod<CloseReq, CloseRsp, Context> {
    static constexpr auto rpcMethod = MetaSerde<>::close<Context>;
  };

  template <typename Context>
  struct ForwardMethod<SetAttrReq, SetAttrRsp, Context> {
    static constexpr auto rpcMethod = MetaSerde<>::setAttr<Context>;
  };

  template <typename Context>
  struct ForwardMethod<CreateReq, CreateRsp, Context> {
    static constexpr auto rpcMethod = MetaSerde<>::create<Context>;
  };

  template <typename Req>
  Result<Void> check(flat::NodeId node, Req &req) {
    if (!node) {
      XLOGF(WARN, "request {}, unknown corresponding server, need retry", req);
      return makeError(MetaCode::kForwardFailed, "unknown corresponding server");
    }
    if (req.forward) {
      XLOGF_IF(INFO, config_.debug(), "request is forward from {}, can't forward again, req {}.", req.forward, req);
      return makeError(MetaCode::kForwardFailed, "double forward, retry");
    }
    req.forward = nodeId_;

    XLOGF_IF(INFO, config_.debug(), "forward req {} to {}", req, node);
    XLOGF_IF(DBG, !config_.debug(), "forward req {} to {}", req, node);
    XLOGF_IF(FATAL, nodeId_ == node, "forward to self, {} == {}", nodeId_, node);

    return Void{};
  }

  CoTryTask<net::Address> getAddress(flat::NodeId node) {
    auto routing = mgmtdClient_->getRoutingInfo();
    if (!routing) {
      co_return makeError(MetaCode::kForwardFailed, "routing info not ready, need retry");
    }

    auto *nodeInfo = routing->raw()->getNode(node);
    if (!nodeInfo) {
      auto msg = fmt::format("req forward: routing info doesn't contains node {}", node);
      XLOG(WARN, msg);
      co_return makeError(MetaCode::kForwardFailed, std::move(msg));
    }
    auto addrs = nodeInfo->extractAddresses("MetaSerde", config_.addr_type());
    if (addrs.empty()) {
      auto msg =
          fmt::format("req forward: node {} doesn't have {} addr.", node, magic_enum::enum_name(config_.addr_type()));
      XLOG(WARN, msg);
      co_return makeError(MetaCode::kForwardFailed, std::move(msg));
    }
    co_return addrs.front();
  }

  template <typename Req, typename Rsp>
  CoTryTask<Rsp> forwardImpl(flat::NodeId node, Req req) {
    CO_RETURN_ON_ERROR(check(node, req));

    auto opts = net::UserRequestOptions();
    opts.timeout = config_.timeout();
    opts.sendRetryTimes = 3;
    opts.compression = std::nullopt;

    Result<Rsp> result = makeError(MetaCode::kFoundBug);
    if (std::holds_alternative<NetClient>(client_)) {
      auto &client = std::get<NetClient>(client_);
      auto addr = co_await getAddress(node);
      CO_RETURN_ON_ERROR(addr);
      auto ctx = client.get().serdeCtx(*addr);
      result = co_await ForwardMethod<Req, Rsp, serde::ClientContext>::rpcMethod(ctx, req, &opts, nullptr);
    } else {
      auto &client = std::get<MockClient>(client_);
      if (!client.get().contains(node)) {
        co_return makeError(MetaCode::kForwardFailed, fmt::format("{} not found", node));
      }
      auto &ctx = client.get()[node];
      result = co_await ForwardMethod<Req, Rsp, serde::ClientMockContext>::rpcMethod(ctx, req, &opts, nullptr);
    }

    if (result.hasError() && StatusCode::typeOf(result.error().code()) == StatusCodeType::RPC) {
      XLOGF(ERR, "failed to forward req to {}, error {}", node, result.error());
      co_return makeError(MetaCode::kForwardTimeout,
                          fmt::format("failed to forward req to {}, error {}", node, result.error()));
    }

    if (result.hasError()) {
      XLOGF_IF(INFO, config_.debug(), "forward req {} to {}, rsp {}", req, node, result.error());
    } else {
      XLOGF_IF(INFO, config_.debug(), "forward req {} to {}, rsp {}", req, node, result.value());
    }

    co_return result;
  }

  const Config &config_;
  flat::NodeId nodeId_;
  std::variant<NetClient, MockClient> client_;
  std::shared_ptr<::hf3fs::client::ICommonMgmtdClient> mgmtdClient_;
};

}  // namespace hf3fs::meta::server