#pragma once

#include <algorithm>
#include <cmath>
#include <exception>
#include <fmt/core.h>
#include <folly/CancellationToken.h>
#include <folly/Function.h>
#include <folly/MPMCQueue.h>
#include <folly/concurrency/UnboundedQueue.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BoundedQueue.h>
#include <folly/futures/Future.h>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include "common/kv/IKVEngine.h"
#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Result.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {

class MetaScan {
 public:
  struct Options {
    // scan options
    int threads = 4;
    int coroutines = 8;
    int items_per_getrange = -1;
    double backoff_min_wait = 0.1;   // 100ms
    double backoff_max_wait = 5;     // 5s
    double backoff_total_wait = 60;  // 60s
    // log level
    std::string logging;
  };

  MetaScan(Options options, std::shared_ptr<kv::IKVEngine> kvEngine);
  ~MetaScan();

  std::vector<Inode> getInodes();
  std::vector<DirEntry> getDirEntries();

  kv::IKVEngine &kvEngine() { return *kvEngine_; }

 private:
  struct KeyRange {
    std::string begin;
    std::string end;
    bool hasMore;

    KeyRange()
        : begin(),
          end(),
          hasMore(false) {}
    KeyRange(std::string begin, std::string end)
        : begin(std::move(begin)),
          end(std::move(end)),
          hasMore(true) {}

    static std::vector<KeyRange> split(std::string prefix);

    CoTryTask<kv::IReadOnlyTransaction::GetRangeResult> snapshotGetRange(kv::IReadOnlyTransaction &txn, int32_t limit);

    std::string describe() const {
      return fmt::format("[begin {:02x}, end {:02x}, hasMore {}]", fmt::join(begin, ""), fmt::join(end, ""), hasMore);
    }
  };

  template <typename T>
  struct BackgroundTask {
    folly::coro::BoundedQueue<std::vector<T>> queue;
    folly::SemiFuture<Result<Void>> future;
    folly::CancellationSource cancel;

    BackgroundTask(size_t cap)
        : queue(cap),
          future(folly::SemiFuture<Result<Void>>::makeEmpty()),
          cancel() {}
  };

  void createKVEngine();

  template <typename T>
  std::vector<T> waitResult(std::optional<BackgroundTask<T>> &task);

  CoTryTask<void> scanInode(BackgroundTask<Inode> &task);
  CoTryTask<void> scanDirEntry(BackgroundTask<DirEntry> &task);

  template <typename T>
  CoTryTask<void> scan(kv::KeyPrefix prefix, BackgroundTask<T> &task);

  template <typename T>
  CoTryTask<void> scanRange(KeyRange range, BackgroundTask<T> &task);

  std::mutex mutex_;
  Options options_;
  std::shared_ptr<kv::IKVEngine> kvEngine_;
  folly::CPUThreadPoolExecutor exec_;
  std::optional<BackgroundTask<Inode>> scanInodeTask_;
  std::optional<BackgroundTask<DirEntry>> scanDirEntryTask_;
};

}  // namespace hf3fs::meta::server