#include "storage/store/GlobalFileStore.h"

#include <fcntl.h>
#include <folly/logging/xlog.h>

#include "common/utils/Duration.h"
#include "common/utils/Size.h"

namespace hf3fs::storage {

Result<FileDescriptor *> GlobalFileStore::open(const Path &filePath, bool createFile /* = false */) {
  return shards_.withLock(
      [&](FdMap &map) -> Result<FileDescriptor *> {
        auto &innerFile = map[filePath];
        if (innerFile.normal_.valid()) {
          return &innerFile;
        }

        // 3. open file.
        FileDescriptor file;
        {
          // open in normal mode.
          auto flags = O_RDWR | O_SYNC;
          int ret = createFile ? ::open(filePath.c_str(), O_CREAT | flags, 0644) : ::open(filePath.c_str(), flags);
          if (UNLIKELY(ret == -1)) {
            auto msg = fmt::format("chunk store open file {} failed: errno {}", filePath, errno);
            XLOG(ERR, msg);
            return makeError(StorageCode::kChunkOpenFailed, std::move(msg));
          }
          file.normal_ = ret;
        }

        {
          // open in direct mode.
          auto flags = O_RDWR | O_DIRECT;
          int ret = ::open(filePath.c_str(), flags);
          if (UNLIKELY(ret == -1)) {
            auto msg = fmt::format("chunk store open file {} failed: errno {}", filePath, errno);
            XLOG(ERR, msg);
            return makeError(StorageCode::kChunkOpenFailed, std::move(msg));
          }
          file.direct_ = ret;
        }

        innerFile = std::move(file);
        return &innerFile;
      },
      filePath);
}

void GlobalFileStore::collect(std::vector<int> &fds) {
  fds.clear();
  fds.reserve(128_KB);
  shards_.iterate([&](FdMap &map) {
    for (auto &[path, fd] : map) {
      fd.index_ = fds.size();
      fds.push_back(fd.direct_);
    }
  });
}

Result<Void> GlobalFileStore::clear(CPUExecutorGroup &executor) {
  std::atomic<uint32_t> finished = 0;
  shards_.iterate([&](FdMap &map) {
    executor.pickNext().add([&, m = std::move(map)]() mutable {
      m.clear();
      ++finished;
    });
  });
  for (int i = 0; finished != kShardsNum; ++i) {
    XLOGF_IF(INFO, i % 5 == 0, "Waiting for clear fd finished...");
    std::this_thread::sleep_for(50_ms);
  }
  return Void{};
}

}  // namespace hf3fs::storage
