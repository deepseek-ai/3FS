#include "Service.h"

#include "common/serde/CallContext.h"
#include "fbs/migration/SerdeService.h"

namespace hf3fs::migration::server {

#define DEFINE_SERVICE_METHOD(METHOD, REQ, RESP) \
  CoTryTask<RESP> MigrationService::METHOD(serde::CallContext &, const REQ &req)

MigrationService::MigrationService() {}

DEFINE_SERVICE_METHOD(start, StartJobReq, StartJobRsp) { co_return makeError(StatusCode::kNotImplemented); }
DEFINE_SERVICE_METHOD(stop, StopJobReq, StopJobRsp) { co_return makeError(StatusCode::kNotImplemented); }
DEFINE_SERVICE_METHOD(list, ListJobsReq, ListJobsRsp) { co_return makeError(StatusCode::kNotImplemented); }

#undef DEFINE_SERVICE_METHOD

}  // namespace hf3fs::migration::server
