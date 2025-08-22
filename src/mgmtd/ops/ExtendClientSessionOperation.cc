#include "ExtendClientSessionOperation.h"

#include "common/utils/StringUtils.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<ExtendClientSessionRsp> ExtendClientSessionOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));

  const auto &clientId = req.clientId;
  const auto &sessionData = req.data;

  if (clientId.empty()) {
    CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Empty clientId");
  }

  if (auto uuidRes = Uuid::fromHexString(clientId); uuidRes.hasError()) {
    LOG_OP_ERR(*this, "ClientId not valid hex uuid. req: {}", serde::toJsonString(req));
    if (state.config_.only_accept_client_uuid()) {
      co_return makeError(StatusCode::kInvalidArg, "ClientId not valid hex uuid");
    }
  }

  auto handler = [&]() -> CoTryTask<ExtendClientSessionRsp> {
    std::optional<flat::ConfigInfo> config;
    std::vector<flat::TagPair> tags;
    auto nodeType = req.type;

    if (nodeType != flat::NodeType::CLIENT && nodeType != flat::NodeType::FUSE) {
      CO_RETURN_AND_LOG_OP_ERR(*this, StatusCode::kInvalidArg, "Invalid node type: {}", toStringView(nodeType));
    }

    {
      auto dataPtr = co_await state.data_.coSharedLock();
      CO_RETURN_ON_ERROR(dataPtr->checkConfigVersion(*this, nodeType, req.configVersion));
      config = dataPtr->getConfig(nodeType, req.configVersion, /*latest=*/true);
      if (dataPtr->universalTagsMap.contains(sessionData.universalId)) {
        tags = dataPtr->universalTagsMap.at(sessionData.universalId);
      }
    }

    {
      auto clientSessionMap = co_await state.clientSessionMap_.coLock();
      auto &sessionMap = *clientSessionMap;
      auto it = sessionMap.find(clientId);
      if (it == sessionMap.end()) {
        sessionMap[clientId].base() = flat::ClientSession(req);
      } else {
        auto &cur = it->second;
        auto &base = cur.base();
        if (cur.clientSessionVersion >= req.clientSessionVersion) {
          LOG_OP_ERR(*this,
                     "ClientSessoin version stale. clientId:{} client:{} server:{}",
                     clientId,
                     req.clientSessionVersion,
                     cur.clientSessionVersion);

          co_return makeError(MgmtdCode::kClientSessionVersionStale, std::to_string(cur.clientSessionVersion));
        }
        auto unexpectedChange = [&]() -> String {
          if (base.universalId != sessionData.universalId) {
            return fmt::format("Expected universalId: {}. UniversalId in request: {}",
                               base.universalId,
                               sessionData.universalId);
          }
          if (base.description != sessionData.description) {
            return fmt::format("Expected description: {}. Description in request: {}",
                               base.description,
                               sessionData.description);
          }
          if (base.serviceGroups != sessionData.serviceGroups) {
            return fmt::format("Expected serviceGroups: {}. ServiceGroups in request: {}",
                               serde::toJsonString(base.serviceGroups),
                               serde::toJsonString(sessionData.description));
          }
          if (base.releaseVersion != sessionData.releaseVersion) {
            return fmt::format("Expected releaseVersion: {}. ReleaseVersion in request: {}",
                               base.releaseVersion,
                               sessionData.releaseVersion);
          }
          if (base.type != nodeType) {
            return fmt::format("Expected type: {}. Type in request: {}",
                               toStringView(base.type),
                               toStringView(nodeType));
          }
          if (base.clientStart != req.clientStart) {
            return fmt::format("Expected clientStart: {}. Type in request: {}",
                               base.clientStart.YmdHMS(),
                               req.clientStart.YmdHMS());
          }
          return "";
        }();

        if (!unexpectedChange.empty()) {
          CO_RETURN_AND_LOG_OP_ERR(*this, MgmtdCode::kExtendClientSessionMismatch, "{}", unexpectedChange);
        }
        cur.clientSessionVersion = req.clientSessionVersion;
        cur.base().configVersion = req.configVersion;
        cur.base().configStatus = req.configStatus;
        cur.base().lastExtend = UtcClock::now();
        cur.updateTs();
      }
    }
    co_return ExtendClientSessionRsp::create(std::move(config), std::move(tags));
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}
}  // namespace hf3fs::mgmtd
