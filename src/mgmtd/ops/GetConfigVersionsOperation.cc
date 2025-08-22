#include "GetConfigVersionsOperation.h"

#include "common/utils/StringUtils.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<GetConfigVersionsRsp> GetConfigVersionsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  auto handler = [&]() -> CoTryTask<GetConfigVersionsRsp> {
    auto dataPtr = co_await state.data_.coSharedLock();
    RHStringHashMap<flat::ConfigVersion> versions;
    for (const auto &[k, vm] : dataPtr->configMap) {
      auto key = hf3fs::toString(k);
      XLOGF_IF(FATAL, vm.empty(), "Config version map empty! key: {}", key);
      auto vit = vm.rbegin();
      versions[key] = vit->first;
    }
    co_return GetConfigVersionsRsp::create(std::move(versions));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
