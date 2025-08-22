#pragma once

#include "common/app/ApplicationBase.h"
#include "common/serde/SerdeHelper.h"
#include "common/utils/RobinHoodUtils.h"

namespace hf3fs::core {
struct EchoMessage : public serde::SerdeHelper<EchoMessage> {
  SERDE_STRUCT_FIELD(str, std::string{});
};

using EchoReq = EchoMessage;
using EchoRsp = EchoMessage;

struct GetConfigReq : public serde::SerdeHelper<GetConfigReq> {
  SERDE_STRUCT_FIELD(configKey, std::string{});
};

struct GetConfigRsp : public serde::SerdeHelper<GetConfigRsp> {
  SERDE_STRUCT_FIELD(config, std::string{});
};

struct RenderConfigReq : public serde::SerdeHelper<RenderConfigReq> {
  SERDE_STRUCT_FIELD(configTemplate, std::string{});
  SERDE_STRUCT_FIELD(testUpdate, bool{true});
  SERDE_STRUCT_FIELD(isHotUpdate, bool{true});
};

struct RenderConfigRsp : public serde::SerdeHelper<RenderConfigRsp> {
  SERDE_STRUCT_FIELD(configAfterRender, std::string{});
  SERDE_STRUCT_FIELD(updateStatus, Status(StatusCode::kOK));
  SERDE_STRUCT_FIELD(configAfterUpdate, std::string{});
};

struct HotUpdateConfigReq : public serde::SerdeHelper<HotUpdateConfigReq> {
  SERDE_STRUCT_FIELD(update, String{});
  SERDE_STRUCT_FIELD(render, false);
};

struct HotUpdateConfigRsp : public serde::SerdeHelper<HotUpdateConfigRsp> {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

struct GetLastConfigUpdateRecordReq : public serde::SerdeHelper<GetLastConfigUpdateRecordReq> {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

struct GetLastConfigUpdateRecordRsp : public serde::SerdeHelper<GetLastConfigUpdateRecordRsp> {
  SERDE_STRUCT_FIELD(record, std::optional<app::ConfigUpdateRecord>{});
};

DEFINE_SERDE_HELPER_STRUCT(ShutdownReq) {
  //
  SERDE_STRUCT_FIELD(graceful, true);
};

DEFINE_SERDE_HELPER_STRUCT(ShutdownRsp) { SERDE_STRUCT_FIELD(dummy, Void{}); };
}  // namespace hf3fs::core
