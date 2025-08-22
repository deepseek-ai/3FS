#include "SetConfigOperation.h"

#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
CoTryTask<SetConfigRsp> SetConfigOperation::handle(MgmtdState &state) {
  CO_RETURN_ON_ERROR(state.validateClusterId(*this, req.clusterId));
  CO_RETURN_ON_ERROR(co_await state.validateAdmin(*this, req.user));

  auto nodeType = req.nodeType;
  auto content = req.content;

  auto handler = [&]() -> CoTryTask<SetConfigRsp> {
    auto writerLock = co_await state.coScopedLock<"SetConfig">();

    auto oldVersionRes = co_await [&]() -> CoTryTask<flat::ConfigVersion> {
      auto dataPtr = co_await state.data_.coSharedLock();
      const auto &cm = dataPtr->configMap;
      auto it = cm.find(nodeType);
      if (it == cm.end()) {
        CO_RETURN_AND_LOG_OP_ERR(*this,
                                 StatusCode::kInvalidArg,
                                 "Unknown NodeType {}",
                                 magic_enum::enum_name(nodeType));
      }
      co_return it->second.rbegin()->first;
    }();
    CO_RETURN_ON_ERROR(oldVersionRes);

    auto newVersion = nextVersion(*oldVersionRes);
    auto newConfigInfo = flat::ConfigInfo::create(newVersion, content, req.desc);
    auto storeHandler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
      co_return co_await state.store_.storeConfig(txn, nodeType, newConfigInfo);
    };
    CO_RETURN_ON_ERROR(co_await withReadWriteTxn(state, std::move(storeHandler)));

    auto dataPtr = co_await state.data_.coLock();
    dataPtr->configMap[nodeType][newVersion] = std::move(newConfigInfo);
    if (nodeType == flat::NodeType::MGMTD) {
      const auto &updater = state.env_->configUpdater();
      if (!updater || updater(content, newConfigInfo.genUpdateDesc())) {
        if (!updater) {
          LOG_OP_WARN(*this, "blindly promote to {} since no updater found", newVersion);
        }
        state.selfNodeInfo_.configVersion = newVersion;
        XLOGF_IF(FATAL, !dataPtr->routingInfo.nodeMap.contains(state.selfId()), "Self not found in state");
        auto &self = dataPtr->routingInfo.nodeMap[state.selfId()];
        XLOGF_IF(FATAL,
                 self.base().configVersion >= newVersion,
                 "Self's ConfigVersion {} is larger than newest {}",
                 self.base().configVersion,
                 newVersion);
        self.base().configVersion = newVersion;
      }
    }
    co_return SetConfigRsp::create(newVersion);
  };
  co_return co_await doAsPrimary(state, std::move(handler));
}

}  // namespace hf3fs::mgmtd
