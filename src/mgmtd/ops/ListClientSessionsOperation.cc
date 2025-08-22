#include "ListClientSessionsOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<ListClientSessionsRsp> ListClientSessionsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  const auto bootstrappingLength = state.config_.bootstrapping_length().asUs();
  ListClientSessionsRsp rsp;
  auto handler = [&]() -> CoTryTask<void> {
    auto dataPtr = co_await state.data_.coSharedLock();
    auto clientSessionMap = co_await state.clientSessionMap_.coSharedLock();

    rsp.bootstrapping = dataPtr->leaseStartTs + bootstrappingLength > SteadyClock::now();
    for (const auto &[clientId, session] : *clientSessionMap) {
      if (auto uuidRes = Uuid::fromHexString(clientId); uuidRes.hasError()) {
        LOG_OP_WARN(*this,
                    "ClientId not valid hex uuid. id: {}. session: {}",
                    clientId,
                    serde::toJsonString(session.base()));
      }

      rsp.sessions.emplace_back(session.base());

      const auto &universalId = session.base().universalId;
      if (!rsp.referencedTags.contains(universalId)) {
        rsp.referencedTags[universalId] = dataPtr->universalTagsMap.contains(universalId)
                                              ? dataPtr->universalTagsMap.at(universalId)
                                              : std::vector<flat::TagPair>{};
      }
    }
    co_return Void{};
  };
  CO_RETURN_ON_ERROR(co_await doAsPrimary(state, std::move(handler)));
  co_return rsp;
}
}  // namespace hf3fs::mgmtd
