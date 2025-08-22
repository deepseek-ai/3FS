#pragma once

#include <memory>

#include "common/utils/Address.h"

namespace hf3fs::stubs {
template <typename IStub>
class IStubFactory {
 public:
  using Stub = IStub;

  virtual ~IStubFactory() = default;
  virtual std::unique_ptr<IStub> create(net::Address addr) = 0;
};
}  // namespace hf3fs::stubs
