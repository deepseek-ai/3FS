#pragma once

#include <string>

#include "common/utils/AtomicSharedPtrTable.h"
#include "fbs/meta/Schema.h"
#include "lib/common/Shm.h"

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

 public:
  std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
  listIovs(const meta::UserInfo &ui);

 public:
  std::string mountName;
  std::shared_mutex shmLock;
  robin_hood::unordered_map<Uuid, int> shmsById;
  std::unique_ptr<AtomicSharedPtrTable<lib::ShmBuf>> iovs;

 private:
  mutable std::shared_mutex iovdLock_;
  robin_hood::unordered_map<std::string, int> iovds_;
};
}  // namespace hf3fs::fuse
