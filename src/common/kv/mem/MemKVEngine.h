#pragma once

#include "MemKV.h"
#include "MemTransaction.h"
#include "common/kv/IKVEngine.h"

namespace hf3fs::kv {

class MemKVEngine : public IKVEngine {
 public:
  MemKVEngine() = default;

  std::unique_ptr<IReadOnlyTransaction> createReadonlyTransaction() override { return createReadWriteTransaction(); }

  std::unique_ptr<IReadWriteTransaction> createReadWriteTransaction() override {
    return std::make_unique<MemTransaction>(mem_);
  }

  mem::MemKV &getKV() { return mem_; }

 private:
  mem::MemKV mem_;
};

}  // namespace hf3fs::kv
