#include "MgmtdOperator.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "core/utils/runOp.h"
#include "mgmtd/ops/Include.h"

#define DEFINE_SERDE_SERVICE_METHOD_FULL(svc, method, Method, Id, Req, Rsp)  \
  CoTryTask<Rsp> svc##Operator::method(Req req, const net::PeerInfo &peer) { \
    Method##Operation op(std::move(req));                                    \
    CO_INVOKE_OP_INFO(op, peer.str, state_);                                 \
  }

namespace hf3fs::mgmtd {

MgmtdOperator::MgmtdOperator(std::shared_ptr<core::ServerEnv> env, const MgmtdConfig &config)
    : state_(env, config),
      backgroundRunner_(state_) {}

void MgmtdOperator::start() { backgroundRunner_.start(); }

CoTask<void> MgmtdOperator::stop() { co_await backgroundRunner_.stop(); }

MgmtdOperator::~MgmtdOperator() { folly::coro::blockingWait(stop()); }

#include "fbs/mgmtd/MgmtdServiceDef.h"
}  // namespace hf3fs::mgmtd
