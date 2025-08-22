#include "ServerEnv.h"

namespace hf3fs::core {
namespace {

template <typename Ptr>
void setPtr(Ptr &src, Ptr &dst) {
  if (src.get() != dst.get()) {
    src.reset();
    src = std::move(dst);
  }
}
}  // namespace

void ServerEnv::setAppInfo(flat::AppInfo appInfo) { appInfo_ = appInfo; }

void ServerEnv::setKvEngine(std::shared_ptr<kv::IKVEngine> engine) { setPtr(kvEngine_, engine); }

void ServerEnv::setMgmtdStubFactory(std::shared_ptr<MgmtdStubFactory> factory) { setPtr(mgmtdStubFactory_, factory); }

void ServerEnv::setUtcTimeGenerator(UtcTimeGenerator generator) { utcTimeGenerator_ = std::move(generator); }

void ServerEnv::setBackgroundExecutor(CPUExecutorGroup *executor) { backgroundExecutor_ = executor; }

void ServerEnv::setConfigUpdater(ConfigUpdater updater) { configUpdater_ = std::move(updater); }

void ServerEnv::setConfigValidater(ConfigValidater validater) { configValidater_ = std::move(validater); }

}  // namespace hf3fs::core
