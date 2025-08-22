#pragma once

#include "common/serde/Serde.h"
#include "common/serde/Service.h"

namespace hf3fs::simple_example {

struct SimpleExampleReq {
  SERDE_STRUCT_FIELD(message, String{});
};

struct SimpleExampleRsp {
  SERDE_STRUCT_FIELD(message, String{});
};

SERDE_SERVICE(SimpleExampleSerde, 0xF0) { SERDE_SERVICE_METHOD(echo, 1, SimpleExampleReq, SimpleExampleRsp); };

}  // namespace hf3fs::simple_example
