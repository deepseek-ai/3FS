#pragma once

#include <memory>

#include "common/kv/ITransaction.h"

namespace hf3fs::kv {
class IKVEngine {
 public:
  virtual ~IKVEngine() = default;

  virtual std::unique_ptr<IReadOnlyTransaction> createReadonlyTransaction() = 0;
  virtual std::unique_ptr<IReadWriteTransaction> createReadWriteTransaction() = 0;
};
}  // namespace hf3fs::kv
