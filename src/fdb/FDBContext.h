#pragma once

#include <memory>

#include "FDB.h"
#include "FDBConfig.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::kv::fdb {
// FDBContext encapsulates the setup and cleanup of FoundationDB.
// It is designed to be used in the main function.
class FDBContext {
 public:
  static std::shared_ptr<FDBContext> create(const FDBConfig &config);

  ~FDBContext();

  DB getDB() const;

  int64_t maxDbCount() const { return config_.enableMultipleClient() ? config_.multipleClientThreadNum() : 1; }

 private:
  explicit FDBContext(const FDBConfig &config);
  FDBConfig config_;  // don't support hot update config
  std::thread networkThread;
};
}  // namespace hf3fs::kv::fdb
