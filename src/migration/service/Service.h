#pragma once

#include "common/serde/CallContext.h"
#include "fbs/migration/SerdeService.h"

namespace hf3fs::migration::server {

class MigrationService : public serde::ServiceWrapper<MigrationService, MigrationSerde> {
 public:
  MigrationService();

#define DECLARE_SERVICE_METHOD(METHOD, REQ, RESP) CoTryTask<RESP> METHOD(serde::CallContext &, const REQ &req)

  DECLARE_SERVICE_METHOD(start, StartJobReq, StartJobRsp);
  DECLARE_SERVICE_METHOD(stop, StopJobReq, StopJobRsp);
  DECLARE_SERVICE_METHOD(list, ListJobsReq, ListJobsRsp);

#undef DECLARE_SERVICE_METHOD

 private:
};

}  // namespace hf3fs::migration::server
