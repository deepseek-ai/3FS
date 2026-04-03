#pragma once
#include "common/monitor/Reporter.h"
#include "common/monitor/Sample.h"
#include "common/utils/ConfigBase.h"
#include <folly/sorted_vector_types.h>
namespace prometheus {
  class Registry;
  class Exposer;
  template <typename T> class Family;
  class Gauge;
}  // namespace prometheus

namespace hf3fs::monitor {
class PromExporter: public Reporter {
 public:
  struct Config : ConfigBase<Config> {
    CONFIG_ITEM(port, "");
  };
  PromExporter(const Config &config);
  ~PromExporter() override;
  Result<Void> init() final;
  Result<Void> commit(const std::vector<Sample> &samples) final;
 private:
  const Config &config_;
  std::shared_ptr<prometheus::Registry> registry_;
  std::unique_ptr<prometheus::Exposer> exposer_;
  // Create families for 3fs' counter metrics and distribution metrics
  using DistMap = std::map<std::string, prometheus::Family<prometheus::Gauge>*>;
  folly::sorted_vector_map<std::string, prometheus::Family<prometheus::Gauge>*> _3fs_counter_families_;
  folly::sorted_vector_map<std::string, DistMap> _3fs_dis_families_;
  bool errorHappened_{false};
};
}  // namespace hf3fs::monitor