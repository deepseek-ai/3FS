#pragma once

#include <folly/Likely.h>
#include <gtest/gtest_prod.h>

#include "FDB.h"
#include "FDBTransaction.h"
#include "common/kv/IKVEngine.h"

namespace hf3fs {

namespace meta {
template <typename KV>
class MetaTestBase;
}

namespace kv {
class FDBKVEngine : public IKVEngine {
 public:
  FDBKVEngine(fdb::DB db)
      : db_(std::move(db)) {}

  std::unique_ptr<IReadOnlyTransaction> createReadonlyTransaction() override { return createReadWriteTransaction(); }

  std::unique_ptr<IReadWriteTransaction> createReadWriteTransaction() override {
    fdb::Transaction tr(db_);
    if (UNLIKELY(tr.error())) {
      return nullptr;
    }
    return std::make_unique<FDBTransaction>(std::move(tr));
  }

 private:
  template <typename KV>
  friend class hf3fs::meta::MetaTestBase;

  FRIEND_TEST(TestFDBTransaction, Readonly);

  void setReadonly(bool rdonly) { db_.readonly_ = rdonly; }

  fdb::DB db_;
};
}  // namespace kv

}  // namespace hf3fs
