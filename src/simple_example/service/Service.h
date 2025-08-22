#pragma once

#include "common/serde/CallContext.h"
#include "fbs/simple_example/SerdeService.h"

namespace hf3fs::simple_example::server {

class SimpleExampleService : public serde::ServiceWrapper<SimpleExampleService, SimpleExampleSerde> {
 public:
  SimpleExampleService();

#define DECLARE_SERVICE_METHOD(METHOD, REQ, RESP) CoTryTask<RESP> METHOD(serde::CallContext &, const REQ &req)

  DECLARE_SERVICE_METHOD(echo, SimpleExampleReq, SimpleExampleRsp);

#undef DECLARE_SERVICE_METHOD

 private:
};

}  // namespace hf3fs::simple_example::server
