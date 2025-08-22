#include "common/utils/SysResource.h"

#include <dirent.h>
#include <filesystem>
#include <folly/logging/xlog.h>
#include <fstream>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "common/serde/Serde.h"
#include "common/utils/Result.h"
#include "scn/scan/scan.h"

namespace hf3fs {
namespace {

struct Device {
  SERDE_STRUCT_FIELD(mountpoint, std::optional<std::string>{});
  SERDE_STRUCT_FIELD(model, std::optional<std::string>{});
};
struct Devices {
  SERDE_STRUCT_FIELD(blockdevices, std::vector<Device>{});
};

}  // namespace

Result<std::string> SysResource::hostname(bool physicalMachineName) {
  char buffer[1024];
  if (physicalMachineName) {
    const char *nodename = std::getenv("NODE_NAME");
    if (nodename) {
      return std::string{nodename};
    }
  }
  if (const char *name = std::getenv("HOSTNAME"); name) {
    return std::string(name);
  }
  int ret = gethostname(buffer, sizeof(buffer));
  if (UNLIKELY(ret == -1)) {
    return makeError(StatusCode::kUnknown, fmt::format("get host name failed: {}", errno));
  }
  return std::string{buffer};
}

uint32_t SysResource::pid() { return static_cast<uint32_t>(getpid()); }

Result<Void> SysResource::increaseProcessFDLimit(uint64_t limit) {
  struct rlimit oldLimit;
  int getLimitRes = ::getrlimit(RLIMIT_NOFILE, &oldLimit);
  if (UNLIKELY(getLimitRes == -1)) {
    XLOGF(ERR, "Unable to get rlimit for number of files. errno: {}", errno);
    return makeError(StatusCode::kUnknown);
  }

  if (limit <= oldLimit.rlim_cur) {
    XLOGF(INFO, "rlimit for number of files has already been met. {} <= {}", limit, oldLimit.rlim_cur);
    return Void{};
  }

  struct rlimit newLimit = oldLimit;
  newLimit.rlim_cur = std::max(limit, oldLimit.rlim_cur);
  newLimit.rlim_max = std::max(limit, oldLimit.rlim_max);
  int setLimitRes = ::setrlimit(RLIMIT_NOFILE, &newLimit);
  if (UNLIKELY(setLimitRes == -1)) {
    XLOGF(ERR, "Unable to set rlimit for number of files. errno: {}", errno);
    return makeError(StatusCode::kUnknown);
  }

  XLOGF(INFO, "Set rlimit for number of files successfully. soft {}, hard {}", newLimit.rlim_cur, newLimit.rlim_max);
  return Void{};
}

Result<std::string> SysResource::exec(std::string_view cmd) {
  std::unique_ptr<::FILE, decltype(&::pclose)> pipe(::popen(cmd.data(), "r"), ::pclose);
  if (!pipe) {
    return makeError(StatusCode::kInvalidArg, fmt::format("cmd: {}", cmd));
  }
  std::string result;
  std::array<char, 128> buffer;
  while (::fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
    result += buffer.data();
  }
  return result;
}

static Result<std::map<unsigned long, std::string>> getFileSystemUUIDOnce() {
  auto diskInfoResult = SysResource::scanDiskInfo();
  RETURN_AND_LOG_ON_ERROR(diskInfoResult);
  std::map<unsigned long, std::string> deviceIdToDevicePath;
  for (auto &info : *diskInfoResult) {
    deviceIdToDevicePath[info.deviceId] = info.uuid;
  }
  return deviceIdToDevicePath;
}

Result<std::map<unsigned long, std::string>> SysResource::fileSystemUUID(bool force) {
  if (force) {
    return getFileSystemUUIDOnce();
  }
  static auto result = getFileSystemUUIDOnce();
  return result;
}

Result<std::vector<SysResource::DiskInfo>> SysResource::scanDiskInfo() {
  // 1. get device->uuid map.
  std::unordered_map<std::string, std::string> deviceToUUID;
  auto blkidResult = SysResource::exec("blkid -s UUID");
  RETURN_AND_LOG_ON_ERROR(blkidResult);
  std::istringstream stream(*blkidResult);
  for (std::string line; std::getline(stream, line);) {
    std::string device;
    std::string uuid;
    if (scn::scan(line, "{} {}", device, uuid) && !device.empty() && uuid.size() >= 7) {
      deviceToUUID[device.substr(0, device.size() - 1)] = uuid.substr(6, uuid.size() - 7);
    }
  }

  // 2. scan mount info.
  constexpr auto mountInfoPath = "/proc/self/mountinfo";
  std::ifstream mountInfo(mountInfoPath);
  if (!mountInfo) {
    XLOGF(ERR, "Open {} failed", mountInfoPath);
    return makeError(StatusCode::kUnknown);
  }

  std::vector<SysResource::DiskInfo> infos;
  for (std::string line; std::getline(mountInfo, line);) {
    std::istringstream is(line);
    std::string dummy;
    std::string deviceIdStr;
    std::string mountPath;
    std::string devicePath;
    is >> dummy >> dummy >> deviceIdStr >> dummy >> mountPath;
    while((is >> dummy) && dummy != "-")
      ;
    if (is >> dummy >> devicePath) {
      unsigned long maj, min;
      auto parseResult = scn::scan(deviceIdStr, "{}:{}", maj, min);
      if (!parseResult) {
        XLOGF(ERR, "Parse deviceIdStr {} failed", deviceIdStr);
        return makeError(StatusCode::kUnknown);
      }

      if (deviceToUUID.count(devicePath)) {
        infos.emplace_back();
        infos.back().deviceId = makedev(maj, min);
        infos.back().uuid = deviceToUUID[devicePath];
        infos.back().devicePath = devicePath;
        infos.back().mountPath = mountPath;
      }
    }
  }

  // 3. get manufacturer
  auto lsblkResult = SysResource::exec("lsblk -o MOUNTPOINT,MODEL --json");
  RETURN_AND_LOG_ON_ERROR(lsblkResult);
  Devices devices;
  RETURN_AND_LOG_ON_ERROR(serde::fromJsonString(devices, *lsblkResult));
  std::unordered_map<std::string, std::string> mountPathToManufacturer;
  for (auto &device : devices.blockdevices) {
    if (device.mountpoint.has_value() && device.model.has_value()) {
      mountPathToManufacturer[*device.mountpoint] = *device.model;
    }
  }
  for (auto &info : infos) {
    info.manufacturer = mountPathToManufacturer[info.mountPath.string()];
  }
  return infos;
}

std::map<String, String> SysResource::getAllEnvs(std::string_view prefix) {
  std::map<String, String> m;
  for (char **env = environ; *env; ++env) {
    std::string_view sv(*env);
    auto pos = sv.find('=');
    if (pos == std::string_view::npos) {
      XLOGF(ERR, "Parse environment variable failed: {}", sv);
    } else {
      auto key = sv.substr(0, pos);
      auto value = sv.substr(pos + 1);
      if (prefix.empty() || key.starts_with(prefix)) {
        m.try_emplace(String(key), String(value));
      }
    }
  }
  return m;
}

Result<rlimit> SysResource::getRlimit(__rlimit_resource_t resource) {
  struct rlimit limit;
  if (auto r = ::getrlimit(resource, &limit); r < 0) {
    return MAKE_ERROR_F(StatusCode::kOSError,
                        "Fail to call getrlimit({}): {} {}",
                        resource,
                        errno,
                        std::strerror(errno));
  } else {
    return limit;
  }
}

int64_t SysResource::getOpenedFDCount() {
  int64_t fd_count = 0;
  DIR *dirp = ::opendir("/proc/self/fd");
  SCOPE_EXIT { ::closedir(dirp); };

  if (dirp != nullptr) {
    while (::readdir(dirp) != nullptr) {
      ++fd_count;
    }
  }

  return fd_count;
}

}  // namespace hf3fs
