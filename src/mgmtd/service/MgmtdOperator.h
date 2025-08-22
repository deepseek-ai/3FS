#pragma once

#include "MgmtdState.h"
#include "common/net/PeerInfo.h"
#include "fbs/mgmtd/Rpc.h"
#include "mgmtd/background/MgmtdBackgroundRunner.h"

namespace hf3fs::mgmtd {
namespace testing {
class MgmtdTestHelper;
}

class MgmtdOperator : public folly::NonCopyableNonMovable {
 public:
  MgmtdOperator(std::shared_ptr<core::ServerEnv> env, const MgmtdConfig &config);

  ~MgmtdOperator();

  // start/stop background tasks
  void start();
  CoTask<void> stop();

#define DEFINE_SERDE_SERVICE_METHOD_FULL(Service, method, Method, Id, Req, Rsp) \
  CoTryTask<Rsp> method(Req req, const net::PeerInfo &peer);

#include "fbs/mgmtd/MgmtdServiceDef.h"

 private:
  MgmtdState state_;
  MgmtdBackgroundRunner backgroundRunner_;

  friend class testing::MgmtdTestHelper;
};
}  // namespace hf3fs::mgmtd
