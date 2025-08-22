#pragma once

#include <map>
#include <string>

#include "common/utils/ConfigBase.h"

namespace hf3fs::kv::fdb {
struct FDBConfig : public ConfigBase<FDBConfig> {
  CONFIG_ITEM(clusterFile, "");
  CONFIG_ITEM(enableMultipleClient, false);
  CONFIG_ITEM(externalClientDir, "");
  CONFIG_ITEM(externalClientPath, "");
  CONFIG_ITEM(multipleClientThreadNum, 4L, ConfigCheckers::checkPositive);
  CONFIG_ITEM(trace_file, "");
  CONFIG_ITEM(trace_format, "json");
  CONFIG_ITEM(casual_read_risky, false);
  CONFIG_ITEM(default_backoff, 0);
  CONFIG_ITEM(readonly, false);
};

}  // namespace hf3fs::kv::fdb
