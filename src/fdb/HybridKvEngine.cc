#include "HybridKvEngine.h"

#include <folly/Random.h>

#include "FDBContext.h"
#include "FDBKVEngine.h"
#include "HybridKvEngineConfig.h"
#include "common/kv/mem/MemKVEngine.h"

namespace hf3fs::kv {
HybridKvEngine::HybridKvEngine() = default;
HybridKvEngine::~HybridKvEngine() = default;

std::shared_ptr<HybridKvEngine> HybridKvEngine::fromMem() {
  std::shared_ptr<HybridKvEngine> p(new HybridKvEngine);
  p->kvEngines_.push_back(std::make_unique<kv::MemKVEngine>());
  return p;
}

std::shared_ptr<HybridKvEngine> HybridKvEngine::fromFdb(const kv::fdb::FDBConfig &config) {
  std::shared_ptr<HybridKvEngine> p(new HybridKvEngine);
  p->fdbContext_ = kv::fdb::FDBContext::create(config);
  p->kvEngines_.reserve(p->fdbContext_->maxDbCount());
  for (auto i = 0; i < p->fdbContext_->maxDbCount(); ++i) {
    p->kvEngines_.push_back(std::make_unique<kv::FDBKVEngine>(p->fdbContext_->getDB()));
  }
  return p;
}

std::shared_ptr<HybridKvEngine> HybridKvEngine::from(bool useMemKV, const kv::fdb::FDBConfig &config) {
  if (useMemKV) return fromMem();
  return fromFdb(config);
}

std::shared_ptr<HybridKvEngine> HybridKvEngine::from(const HybridKvEngineConfig &config) {
  return from(config.use_memkv(), config.fdb());
}

std::shared_ptr<HybridKvEngine> HybridKvEngine::from(const HybridKvEngineConfig &config,
                                                     bool useMemKV,
                                                     const kv::fdb::FDBConfig &fdbConfig) {
  static const HybridKvEngineConfig defaultHybridConfig;
  if (config == defaultHybridConfig) {
    return from(useMemKV, fdbConfig);
  } else {
    return from(config);
  }
}

kv::IKVEngine &HybridKvEngine::pick() const {
  assert(!kvEngines_.empty());
  auto idx = folly::Random::rand32() % kvEngines_.size();
  return *kvEngines_[idx];
}

std::unique_ptr<kv::IReadOnlyTransaction> HybridKvEngine::createReadonlyTransaction() {
  return pick().createReadonlyTransaction();
}

std::unique_ptr<kv::IReadWriteTransaction> HybridKvEngine::createReadWriteTransaction() {
  return pick().createReadWriteTransaction();
}
}  // namespace hf3fs::kv
