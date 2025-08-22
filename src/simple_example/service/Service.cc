#include "Service.h"

#include "common/serde/CallContext.h"
#include "fbs/simple_example/SerdeService.h"

namespace hf3fs::simple_example::server {

#define DEFINE_SERVICE_METHOD(METHOD, REQ, RESP) \
  CoTryTask<RESP> SimpleExampleService::METHOD(serde::CallContext &, const REQ &req)

SimpleExampleService::SimpleExampleService() {}

DEFINE_SERVICE_METHOD(echo, SimpleExampleReq, SimpleExampleRsp) {
  SimpleExampleRsp resp;
  resp.message = req.message;
  co_return resp;
}

#undef DEFINE_SERVICE_METHOD

}  // namespace hf3fs::simple_example::server
