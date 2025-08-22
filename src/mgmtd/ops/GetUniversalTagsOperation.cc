#include "GetUniversalTagsOperation.h"

#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<GetUniversalTagsRsp> GetUniversalTagsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  auto handler = [&]() -> CoTryTask<GetUniversalTagsRsp> {
    auto dataPtr = co_await state.data_.coSharedLock();
    const auto &m = dataPtr->universalTagsMap;
    auto it = m.find(req.universalId);
    if (it != m.end()) co_return GetUniversalTagsRsp::create(it->second);
    co_return GetUniversalTagsRsp::create();
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
