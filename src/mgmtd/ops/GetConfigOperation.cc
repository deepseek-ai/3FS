#include "GetConfigOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<GetConfigRsp> GetConfigOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  auto nodeType = req.nodeType;
  auto version = req.configVersion;

  auto handler = [&]() -> CoTryTask<GetConfigRsp> {
    auto configInfo = (co_await state.data_.coSharedLock())->getConfig(nodeType, version, !req.exactVersion);
    co_return GetConfigRsp::create(std::move(configInfo));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
