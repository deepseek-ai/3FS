#pragma once

#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

#include "common/utils/AtomicSharedPtrTable.h"
#include "fbs/meta/Schema.h"
#include "lib/common/Shm.h"
#ifdef HF3FS_GDR_ENABLED
#include "lib/common/GpuShm.h"
#endif

namespace hf3fs::fuse {
class IovTable {
 public:
  IovTable() = default;
  void init(const Path &mount, int cap);
  Result<std::pair<meta::Inode, std::shared_ptr<lib::ShmBuf>>> addIov(const char *key,
                                                                      const Path &shmPath,
                                                                      pid_t pid,
                                                                      const meta::UserInfo &ui,
                                                                      folly::Executor::KeepAlive<> exec,
                                                                      storage::client::StorageClient &sc);
  Result<std::shared_ptr<lib::ShmBuf>> rmIov(const char *key, const meta::UserInfo &ui);
  Result<meta::Inode> lookupIov(const char *key, const meta::UserInfo &ui);
  std::optional<int> iovDesc(meta::InodeId iid);
  Result<meta::Inode> statIov(int key, const meta::UserInfo &ui);
  std::vector<int> removeIovsByPid(pid_t pid);

 public:
  std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
  listIovs(const meta::UserInfo &ui);

 public:
  std::string mountName;
  std::shared_mutex shmLock;
  robin_hood::unordered_map<Uuid, int> shmsById;
  std::unique_ptr<AtomicSharedPtrTable<lib::ShmBuf>> iovs;

#ifdef HF3FS_GDR_ENABLED
  // GPU iov storage — separate from host iovs since GpuShmBuf != ShmBuf
  std::mutex gpuShmLock;
  robin_hood::unordered_map<Uuid, std::shared_ptr<lib::GpuShmBuf>> gpuShmsById;

  // Per-iovd GPU metadata for statIov/listIovs (GPU iovs have null ShmBuf slot)
  struct GpuIovMeta {
    std::string key;
    Path target;  // the gdr:// URI
    meta::Uid user{0};
    meta::Gid gid{0};
    pid_t pid = 0;
  };
  robin_hood::unordered_map<int, GpuIovMeta> gpuIovMetaByIovd;
#endif

 private:
  mutable std::shared_mutex iovdLock_;
  robin_hood::unordered_map<std::string, int> iovds_;
};
}  // namespace hf3fs::fuse
