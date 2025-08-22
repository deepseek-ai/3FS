#pragma once

#include "common/app/AppInfo.h"
#include "common/kv/IKVEngine.h"
#include "common/utils/CPUExecutorGroup.h"
#include "stubs/common/IStubFactory.h"
#include "stubs/mgmtd/IMgmtdServiceStub.h"

namespace hf3fs::core {
class ServerEnv {
 public:
  const flat::AppInfo &appInfo() const { return appInfo_; }

  void setAppInfo(flat::AppInfo appInfo);

  const std::shared_ptr<kv::IKVEngine> &kvEngine() const { return kvEngine_; }

  void setKvEngine(std::shared_ptr<kv::IKVEngine> engine);

  using MgmtdStubFactory = stubs::IStubFactory<mgmtd::IMgmtdServiceStub>;
  const std::shared_ptr<MgmtdStubFactory> &mgmtdStubFactory() const { return mgmtdStubFactory_; }

  void setMgmtdStubFactory(std::shared_ptr<MgmtdStubFactory> factory);

  using UtcTimeGenerator = std::function<UtcTime()>;
  const UtcTimeGenerator &utcTimeGenerator() const { return utcTimeGenerator_; }

  void setUtcTimeGenerator(UtcTimeGenerator generator);

  CPUExecutorGroup *backgroundExecutor() const { return backgroundExecutor_; }

  void setBackgroundExecutor(CPUExecutorGroup *executor);

  using ConfigUpdater = std::function<Result<Void>(const String &, const String &)>;
  const ConfigUpdater &configUpdater() const { return configUpdater_; }

  void setConfigUpdater(ConfigUpdater updater);

  using ConfigValidater = std::function<Result<Void>(const String &, const String &)>;
  const ConfigValidater &configValidater() const { return configValidater_; }

  void setConfigValidater(ConfigValidater validater);

 private:
  flat::AppInfo appInfo_;
  std::shared_ptr<kv::IKVEngine> kvEngine_;
  std::shared_ptr<MgmtdStubFactory> mgmtdStubFactory_;
  UtcTimeGenerator utcTimeGenerator_ = &UtcClock::now;
  CPUExecutorGroup *backgroundExecutor_ = nullptr;
  ConfigUpdater configUpdater_;
  ConfigValidater configValidater_;
};
}  // namespace hf3fs::core
