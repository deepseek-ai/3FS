#pragma once

#include "common/monitor/Reporter.h"
#include "common/monitor/Sample.h"
#include "common/utils/ConfigBase.h"

namespace clickhouse {
class Client;
}  // namespace clickhouse

namespace hf3fs::monitor {

class ClickHouseClient : public Reporter {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(host, "");
    CONFIG_ITEM(port, "");
    CONFIG_ITEM(user, "");
    CONFIG_ITEM(passwd, "");
    CONFIG_ITEM(db, "");
  };

  ClickHouseClient(const Config &config);
  ~ClickHouseClient() override;

  Result<Void> init() final;

  Result<Void> commit(const std::vector<Sample> &samples) final;

 private:
  const Config &config_;
  std::unique_ptr<clickhouse::Client> client_;
  bool errorHappened_{false};
};

}  // namespace hf3fs::monitor
