#pragma once

#include "core/utils/ServiceOperation.h"
#include "fbs/core/service/Rpc.h"

namespace hf3fs::core {

struct EchoOperation : ServiceOperationWithMetric<"CoreService", "Echo", "op"> {
  EchoMessage req;

  explicit EchoOperation(EchoMessage r)
      : req(std::move(r)) {}

  String toStringImpl() const final { return "Echo"; }

  CoTryTask<EchoMessage> handle() { co_return req; }
};

}  // namespace hf3fs::core
