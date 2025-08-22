#pragma once

#include <folly/ThreadLocal.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <mutex>

#include "common/utils/CPUExecutorGroup.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/FdWrapper.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/Shards.h"

namespace hf3fs::storage {

struct FileDescriptor {
  FdWrapper normal_;
  FdWrapper direct_;
  std::optional<uint32_t> index_{};
};

class GlobalFileStore {
 public:
  Result<FileDescriptor *> open(const Path &filePath, bool createFile = false);

  void collect(std::vector<int> &fds);

  Result<Void> clear(CPUExecutorGroup &executor);

 private:
  constexpr static auto kShardsNum = 256u;
  using FdMap = std::unordered_map<Path, FileDescriptor>;
  Shards<FdMap, kShardsNum> shards_;
};

}  // namespace hf3fs::storage
