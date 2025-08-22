#include "common/serde/CallContext.h"

#include "common/monitor/Recorder.h"
#include "common/serde/ClientContext.h"

namespace hf3fs::serde {
namespace {

monitor::CountRecorder deserilizeFails{"common.rpc.deserilize.fails"};
monitor::OperationRecorder applyRDMATransmission{"common.apply_rdma_transmission"};
monitor::CountRecorder applyRDMATransmissionTimeout{"common.apply_rdma_transmission.timeout"};

}  // namespace

CoTask<void> CallContext::RDMATransmission::applyTransmission(Duration timeout) {
  auto recordGuard = applyRDMATransmission.record();
  net::UserRequestOptions options{timeout};
  net::RDMATransmissionReq req{ctx_.packet().uuid};
  serde::ClientContext clientCtx(ctx_.transport());
  auto applyResult = co_await net::RDMAControl<>::apply(clientCtx, req, &options);
  if (UNLIKELY(!applyResult)) {
    XLOGF(DBG, "apply transmission error: {}", applyResult.error());
    applyRDMATransmissionTimeout.addSample(1);
  }
  recordGuard.succ();
}

void CallContext::onDeserializeFailed() {
  packet_.payload = std::string_view{};
  XLOGF(ERR, "deserialize request failed: {}, peer: {}", packet_, peer());
  deserilizeFails.addSample(1);
  onError(makeError(RPCCode::kVerifyRequestFailed));
}

}  // namespace hf3fs::serde
