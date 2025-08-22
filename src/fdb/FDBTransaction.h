#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Status.h"
#include "fdb/FDB.h"

namespace hf3fs::kv {

class FDBTransaction : public IReadWriteTransaction {
 public:
  FDBTransaction(fdb::Transaction &&tr)
      : tr_(std::move(tr)),
        errcode_(0) {}

  // Read operations
  CoTryTask<std::optional<String>> snapshotGet(std::string_view key) override;
  CoTryTask<GetRangeResult> snapshotGetRange(const KeySelector &begin, const KeySelector &end, int32_t limit) override;
  CoTryTask<std::optional<String>> get(std::string_view key) override;
  CoTryTask<GetRangeResult> getRange(const KeySelector &begin, const KeySelector &end, int32_t limit) override;

  CoTryTask<void> cancel() override;

  CoTryTask<void> addReadConflict(std::string_view key) override;
  CoTryTask<void> addReadConflictRange(std::string_view begin, std::string_view end) override;

  // Write operations
  CoTryTask<void> set(std::string_view key, std::string_view value) override;
  CoTryTask<void> clear(std::string_view key) override;

  CoTryTask<void> setVersionstampedKey(std::string_view key, uint32_t offset, std::string_view value) override;
  CoTryTask<void> setVersionstampedValue(std::string_view key, std::string_view value, uint32_t offset) override;

  CoTryTask<void> clearRange(std::string_view begin, std::string_view end);  // only used in test.

  CoTryTask<void> commit() override;
  void reset() override;

  Result<Void> setOption(FDBTransactionOption option, std::string_view value = {});
  CoTask<bool> onError(fdb_error_t errcode);

  CoTryTask<int64_t> getReadVersion();
  int64_t getCommittedVersion() override;
  void setReadVersion(int64_t version) override;

  fdb_error_t errcode() const { return errcode_; }

 private:
  friend class TestFDBTransaction;
  static Status testFDBError(int errCode, bool commit);

  fdb::Transaction tr_;
  std::atomic<fdb_error_t> errcode_;
};

}  // namespace hf3fs::kv
