#pragma once

#include <memory>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <sys/types.h>
#include <variant>
#include <vector>

#include "common/utils/AtomicSharedPtrTable.h"
#include "fbs/meta/Schema.h"
#include "fuse/IovTypes.h"
#include "lib/common/Shm.h"
#ifdef HF3FS_GDR_ENABLED
#include "lib/common/GpuShm.h"
#endif

namespace hf3fs::fuse {

class IovTableTestHelper;

#ifdef HF3FS_GDR_ENABLED
using IovBuffer = std::variant<std::shared_ptr<lib::ShmBuf>, std::shared_ptr<lib::GpuShmBuf>>;
#else
using IovBuffer = std::variant<std::shared_ptr<lib::ShmBuf>>;
#endif

struct IovEntry {
  std::string key;
  Uuid id;
  Path target;
  meta::Uid user{0};
  meta::Gid gid{0};
  pid_t pid = 0;
  IovBuffer buffer;

  bool isGpu() const {
#ifdef HF3FS_GDR_ENABLED
    return std::holds_alternative<std::shared_ptr<lib::GpuShmBuf>>(buffer);
#else
    return false;
#endif
  }
};

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
  Result<std::shared_ptr<lib::ShmBuf>> rmIov(const char *key,
                                             const meta::UserInfo &ui,
                                             const std::shared_ptr<lib::ShmBuf> &expectedBuffer = {});
  Result<meta::Inode> lookupIov(const char *key, const meta::UserInfo &ui);
  std::optional<int> iovDesc(meta::InodeId iid);
  Result<meta::Inode> statIov(int key, const meta::UserInfo &ui);
  std::vector<int> removeIovsByPid(pid_t pid);
  Result<IoBufForIO> lookupBuf(const IovLookupRequest &request, meta::Uid requester) const;
  // Appends exactly one final result per request; existing output is untouched.
  void lookupBufs(std::vector<Result<IoBufForIO>> &output,
                  std::span<const IovLookupRequest> requests,
                  meta::Uid requester) const;
  std::shared_ptr<const IovEntry> entryAt(int iovd) const;
#ifdef HF3FS_GDR_ENABLED
  void clearGpuIovs();
#endif

  std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
  listIovs(const meta::UserInfo &ui);

 private:
  friend class IovTableTestHelper;

  Result<Void> publishEntry(int iovd, std::shared_ptr<IovEntry> entry);
  bool removeEntryLocked(int iovd, const std::shared_ptr<IovEntry> &expected);

  std::string mountName;
  mutable std::shared_mutex mutex_;
  robin_hood::unordered_map<std::string, int> iovdsByKey_;
  robin_hood::unordered_map<Uuid, int> iovdsById_;
  std::unique_ptr<AtomicSharedPtrTable<IovEntry>> entries_;
};
}  // namespace hf3fs::fuse
