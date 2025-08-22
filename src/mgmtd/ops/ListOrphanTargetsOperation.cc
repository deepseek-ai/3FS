#include "ListOrphanTargetsOperation.h"

#include "common/utils/StringUtils.h"
#include "mgmtd/service/helpers.h"
#include "mgmtd/service/updateChain.h"

namespace hf3fs::mgmtd {
CoTryTask<ListOrphanTargetsRsp> ListOrphanTargetsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  auto handler = [&]() -> CoTryTask<ListOrphanTargetsRsp> {
    ListOrphanTargetsRsp rsp;
    auto dataPtr = co_await state.data_.coLock();
    auto &ri = dataPtr->routingInfo;
    for (const auto &[_, ti] : ri.orphanTargetsByTargetId) {
      rsp.targets.push_back(ti);
    }
    co_return rsp;
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
