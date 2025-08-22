#include "GetRoutingInfoOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<GetRoutingInfoRsp> GetRoutingInfoOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  auto handler = [&]() -> CoTryTask<GetRoutingInfoRsp> {
    auto dataPtr = co_await state.data_.coSharedLock();
    CO_RETURN_ON_ERROR(dataPtr->checkRoutingInfoVersion(*this, req.routingInfoVersion));
    auto rsp = GetRoutingInfoRsp::create(dataPtr->getRoutingInfo(req.routingInfoVersion, state.config_));
    if (rsp.info) {
      LOG_OP_DBG(*this, "return {}", rsp.info->routingInfoVersion);
    } else {
      LOG_OP_DBG(*this, "return nullopt");
    }
    co_return rsp;
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
