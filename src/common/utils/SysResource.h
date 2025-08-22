#pragma once

#include <map>
#include <string>
#include <sys/resource.h>

#include "common/serde/Serde.h"
#include "common/utils/Result.h"

namespace hf3fs {

struct SysResource {
  static Result<std::string> hostname(bool physicalMachineName = true);

  static uint32_t pid();

  static Result<Void> increaseProcessFDLimit(uint64_t limit);

  static Result<std::string> exec(std::string_view cmd);

  struct DiskInfo {
    SERDE_STRUCT_FIELD(deviceId, uint32_t{});
    SERDE_STRUCT_FIELD(uuid, std::string{});
    SERDE_STRUCT_FIELD(manufacturer, std::string{});
    SERDE_STRUCT_FIELD(devicePath, Path{});
    SERDE_STRUCT_FIELD(mountPath, Path{});
  };
  static Result<std::vector<DiskInfo>> scanDiskInfo();

  static Result<std::map<unsigned long, std::string>> fileSystemUUID(bool force = false);

  static std::map<String, String> getAllEnvs(std::string_view prefix = {});

  static Result<rlimit> getRlimit(__rlimit_resource_t resource);

  static int64_t getOpenedFDCount();
};

}  // namespace hf3fs
