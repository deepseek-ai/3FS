#include "GetClientSessionOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<GetClientSessionRsp> GetClientSessionOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  if (req.clientId.empty()) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty clientId");
  }

  const auto bootstrappingLength = state.config_.bootstrapping_length().asUs();
  GetClientSessionRsp rsp;
  auto handler = [&]() -> CoTryTask<void> {
    auto dataPtr = co_await state.data_.coSharedLock();
    auto clientSessionMap = co_await state.clientSessionMap_.coSharedLock();
    rsp.bootstrapping = dataPtr->leaseStartTs + bootstrappingLength > SteadyClock::now();
    auto it = clientSessionMap->find(req.clientId);
    if (it != clientSessionMap->end()) {
      rsp.session = it->second.base();

      const auto &universalId = rsp.session->universalId;
      rsp.referencedTags = dataPtr->universalTagsMap.contains(universalId) ? dataPtr->universalTagsMap.at(universalId)
                                                                           : std::vector<flat::TagPair>{};
    }
    co_return Void{};
  };
  CO_RETURN_ON_ERROR(co_await doAsPrimary(state, std::move(handler)));
  co_return rsp;
}
}  // namespace hf3fs::mgmtd
