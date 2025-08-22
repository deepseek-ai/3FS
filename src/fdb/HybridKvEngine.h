#pragma once

#include "common/kv/IKVEngine.h"

namespace hf3fs::kv::fdb {
struct FDBConfig;
class FDBContext;
}  // namespace hf3fs::kv::fdb

namespace hf3fs::kv {
struct HybridKvEngineConfig;
class HybridKvEngine : public kv::IKVEngine {
 public:
  static std::shared_ptr<HybridKvEngine> fromMem();
  static std::shared_ptr<HybridKvEngine> fromFdb(const kv::fdb::FDBConfig &config);
  static std::shared_ptr<HybridKvEngine> from(bool useMemKV, const kv::fdb::FDBConfig &config);
  static std::shared_ptr<HybridKvEngine> from(const HybridKvEngineConfig &config);

  // use `config` if explicitly set, else fallback to use `useMemKV` and `fdbConfig`.
  static std::shared_ptr<HybridKvEngine> from(const HybridKvEngineConfig &config,
                                              bool useMemKV,
                                              const kv::fdb::FDBConfig &fdbConfig);

  // config contains:
  // 1. use_memkv (fallback mode)
  // 2. fdb (fallback mode)
  // 3. kv_engine (new)
  static std::shared_ptr<HybridKvEngine> fromSuperConfig(const auto &cfg) {
    return from(cfg.kv_engine(), cfg.use_memkv(), cfg.fdb());
  }

  ~HybridKvEngine();

  std::unique_ptr<kv::IReadOnlyTransaction> createReadonlyTransaction() override;
  std::unique_ptr<kv::IReadWriteTransaction> createReadWriteTransaction() override;

 private:
  HybridKvEngine();

  kv::IKVEngine &pick() const;

  std::shared_ptr<kv::fdb::FDBContext> fdbContext_;
  std::vector<std::unique_ptr<kv::IKVEngine>> kvEngines_;
};

}  // namespace hf3fs::kv
