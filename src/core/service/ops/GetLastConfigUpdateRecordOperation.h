#pragma once

#include "common/app/ApplicationBase.h"
#include "core/utils/ServiceOperation.h"
#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

struct GetLastConfigUpdateRecordOperation
    : ServiceOperationWithMetric<"CoreService", "GetLastConfigUpdateRecord", "op"> {
  GetLastConfigUpdateRecordReq req;

  explicit GetLastConfigUpdateRecordOperation(GetLastConfigUpdateRecordReq r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "GetLastConfigUpdateRecord"; }

  CoTryTask<GetLastConfigUpdateRecordRsp> handle() {
    auto res = ApplicationBase::getLastConfigUpdateRecord();
    co_return GetLastConfigUpdateRecordRsp::create(std::move(res));
  }
};

}  // namespace hf3fs::core
