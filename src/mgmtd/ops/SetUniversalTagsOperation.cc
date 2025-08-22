#include "SetUniversalTagsOperation.h"

#include "fbs/mgmtd/NodeConversion.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<SetUniversalTagsRsp> SetUniversalTagsOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  for (const auto &tp : req.tags) {
    if (tp.key.empty()) {
      CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kInvalidTag, "tag key is empty");
    }
  }

  auto handler = [&]() -> CoTryTask<SetUniversalTagsRsp> {
    auto writerLock = co_await state.coScopedLock<"SetUniversalTags">();

    std::vector<flat::TagPair> oldTags;
    {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &m = dataPtr->universalTagsMap;
      auto it = m.find(req.universalId);
      if (it != m.end()) oldTags = it->second;
    }

    auto updateRes = updateTags(*this, req.mode, oldTags, req.tags);
    CO_RETURN_ON_ERROR(updateRes);
    auto &newTags = *updateRes;

    if (oldTags == newTags) {
      co_return SetUniversalTagsRsp::create(std::move(oldTags));
    }

    CO_RETURN_ON_ERROR(
        co_await updateStoredRoutingInfo(state, *this, [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
          co_return co_await state.store_.storeUniversalTags(txn, req.universalId, newTags);
        }));

    LOG_OP_INFO(*this, "change tags from {} to {}", serde::toJsonString(oldTags), serde::toJsonString(newTags));

    {
      auto dataPtr = co_await state.data_.coLock();
      auto &m = dataPtr->universalTagsMap;
      m[req.universalId] = newTags;
    }

    co_return SetUniversalTagsRsp::create(std::move(newTags));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
