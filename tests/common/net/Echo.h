#pragma once

#include "common/net/ib/RDMABuf.h"
#include "common/serde/Service.h"

namespace hf3fs::net::test {

struct EchoReq {
  SERDE_STRUCT_FIELD(val, std::string{});
  SERDE_STRUCT_FIELD(rdma_bufs, std::vector<RDMARemoteBuf>{});
};

struct EchoRsp {
  SERDE_STRUCT_FIELD(val, std::string{});
};

struct HelloReq {
  SERDE_STRUCT_FIELD(val, std::string{});
};

struct HelloRsp {
  SERDE_STRUCT_FIELD(val, std::string{});
  SERDE_STRUCT_FIELD(idx, uint32_t{});
};

SERDE_SERVICE(Echo, 86) {
  SERDE_SERVICE_METHOD(echo, 1, EchoReq, EchoRsp);
  SERDE_SERVICE_METHOD(hello, 2, HelloReq, HelloRsp);
  SERDE_SERVICE_METHOD(fail, 3, HelloReq, HelloRsp);
};

}  // namespace hf3fs::net::test
