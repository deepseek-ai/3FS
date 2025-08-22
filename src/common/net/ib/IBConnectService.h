#pragma once

#include <atomic>
#include <folly/IPAddressV4.h>
#include <gtest/gtest_prod.h>
#include <map>

#include "common/net/ib/IBConnect.h"
#include "common/net/ib/IBDevice.h"
#include "common/net/ib/IBSocket.h"
#include "common/serde/CallContext.h"
#include "common/serde/Service.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"

namespace hf3fs::net {

SERDE_SERVICE(IBConnect, 11) {
  SERDE_SERVICE_METHOD(query, 1, IBQueryReq, IBQueryRsp);
  SERDE_SERVICE_METHOD(connect, 2, IBConnectReq, IBConnectRsp);
};

class IBConnectService : public serde::ServiceWrapper<IBConnectService, IBConnect> {
 public:
  using AcceptFn = std::function<void(std::unique_ptr<IBSocket>)>;

  IBConnectService(const IBSocket::Config &config, AcceptFn accept, std::function<Duration()> acceptTimeout)
      : config_(config),
        accept_(std::move(accept)),
        acceptTimeout_(acceptTimeout) {}

  CoTryTask<IBQueryRsp> query(serde::CallContext &ctx, const IBQueryReq &req);
  CoTryTask<IBConnectRsp> connect(serde::CallContext &ctx, const IBConnectReq &req);

 private:
  FRIEND_TEST(TestRDMA, SlowConnect);
  FRIEND_TEST(TestRDMA, ConnectitonLost);
  friend class IBSocket;

  static std::atomic<uint64_t> &delayMs() {
    static std::atomic<uint64_t> delay{0};
    return delay;
  }

  static std::atomic<bool> &connectionLost() {
    static std::atomic<bool> val{false};
    return val;
  }

  const IBSocket::Config &config_;
  AcceptFn accept_;
  std::function<Duration()> acceptTimeout_;
};

}  // namespace hf3fs::net