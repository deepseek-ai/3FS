#pragma once

#include <variant>

#include "LocalTargetInfo.h"
#include "MgmtdTypes.h"
#include "common/app/AppInfo.h"
#include "common/app/ConfigStatus.h"

namespace hf3fs::flat {
class MetaHeartbeatInfo : public serde::SerdeHelper<MetaHeartbeatInfo> {
 public:
  static constexpr auto kTypeCode = NodeType::META;

  SERDE_STRUCT_FIELD(dummy, Void{});
};

class StorageHeartbeatInfo : public serde::SerdeHelper<StorageHeartbeatInfo> {
 public:
  static constexpr auto kTypeCode = NodeType::STORAGE;

  SERDE_STRUCT_FIELD(targets, std::vector<LocalTargetInfo>{});
};

class MgmtdHeartbeatInfo : public serde::SerdeHelper<MgmtdHeartbeatInfo> {
 public:
  static constexpr auto kTypeCode = NodeType::MGMTD;

  SERDE_STRUCT_FIELD(dummy, Void{});
};

class HeartbeatInfo : public serde::SerdeHelper<HeartbeatInfo> {
 public:
  HeartbeatInfo() = default;

  explicit HeartbeatInfo(FbsAppInfo appInfo)
      : app(std::move(appInfo)) {}

  HeartbeatInfo(FbsAppInfo appInfo, MetaHeartbeatInfo info)
      : HeartbeatInfo(std::move(appInfo)) {
    set(std::move(info));
  }

  HeartbeatInfo(FbsAppInfo appInfo, StorageHeartbeatInfo info)
      : HeartbeatInfo(std::move(appInfo)) {
    set(std::move(info));
  }

  HeartbeatInfo(FbsAppInfo appInfo, MgmtdHeartbeatInfo info)
      : HeartbeatInfo(std::move(appInfo)) {
    set(std::move(info));
  }

  NodeType type() const {
    return std::visit(
        [](auto &&v) {
          using T = std::decay_t<decltype(v)>;
          return T::kTypeCode;
        },
        info_);
  }

  MetaHeartbeatInfo &asMeta() { return std::get<MetaHeartbeatInfo>(info_); }

  const MetaHeartbeatInfo &asMeta() const { return std::get<MetaHeartbeatInfo>(info_); }

  StorageHeartbeatInfo &asStorage() { return std::get<StorageHeartbeatInfo>(info_); }

  const StorageHeartbeatInfo &asStorage() const { return std::get<StorageHeartbeatInfo>(info_); }

  MgmtdHeartbeatInfo &asMgmtd() { return std::get<MgmtdHeartbeatInfo>(info_); }

  const MgmtdHeartbeatInfo &asMgmtd() const { return std::get<MgmtdHeartbeatInfo>(info_); }

  void set(MetaHeartbeatInfo info) { info_ = std::move(info); }

  void set(StorageHeartbeatInfo info) { info_ = std::move(info); }

  void set(MgmtdHeartbeatInfo info) { info_ = std::move(info); }

  using Payload = std::variant<MetaHeartbeatInfo, StorageHeartbeatInfo, MgmtdHeartbeatInfo>;
  void set(Payload payload) { info_ = std::move(payload); }

  SERDE_CLASS_FIELD(info, Payload{});

  SERDE_STRUCT_FIELD(app, FbsAppInfo{});
  SERDE_STRUCT_FIELD(hbVersion,
                     HeartbeatVersion(1));  // HeartbeatVersion should be larger than server's so use 1 as default value
  SERDE_STRUCT_FIELD(
      configVersion,
      ConfigVersion(0));  // ConfigVersion should be smaller than or equal to server's so use 0 as default value
  SERDE_STRUCT_FIELD(configStatus, ConfigStatus::NORMAL);
};
}  // namespace hf3fs::flat
