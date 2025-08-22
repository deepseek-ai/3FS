#pragma once

#include <folly/fibers/Baton.h>
#include <memory>

namespace hf3fs {
class DestructionGuard : public std::enable_shared_from_this<DestructionGuard> {
 public:
  folly::fibers::Baton destructed;

  static std::shared_ptr<DestructionGuard> create() {
    struct X : DestructionGuard {};
    auto ptr = std::make_shared<X>();
    return std::shared_ptr<DestructionGuard>(std::move(ptr));
  }

 protected:
  DestructionGuard() = default;
};
}  // namespace hf3fs
