#pragma once

#include "common/monitor/Reporter.h"
#include "common/monitor/Sample.h"
#include "common/utils/ConfigBase.h"

namespace hf3fs::monitor {

class TaosClient : public Reporter {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(host, "");
    CONFIG_ITEM(user, "");
    CONFIG_ITEM(passwd, "");
    CONFIG_ITEM(db, "");
    CONFIG_ITEM(cfg_dir, "/tmp");
    CONFIG_ITEM(log_dir, "/tmp");
  };

  TaosClient(const Config &config)
      : config_(config) {}
  ~TaosClient() override { stop(); }

  Result<Void> init() final;
  void stop();
  static void cleanUp();

  Result<Void> query(const std::string &sql);
  Result<Void> commit(const std::vector<Sample> &samples) final;

 protected:
  Result<Void> insert(std::string &buffer, std::vector<uintptr_t> &offsets);

 private:
  const Config &config_;
  void *taos_ = nullptr;
};

}  // namespace hf3fs::monitor
