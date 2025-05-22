#include "meta/event/Scan.h"

#include <algorithm>
#include <chrono>
#include <fmt/format.h>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/experimental/coro/AsyncGenerator.h>
#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/CurrentExecutor.h>
#include <folly/experimental/coro/Invoke.h>
#include <folly/futures/Future.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/kv/KeyPrefix.h"
#include "common/logging/LogInit.h"
#include "common/utils/Coroutine.h"
#include "common/utils/ExponentialBackoffRetry.h"
#include "common/utils/Result.h"
#include "fdb/FDB.h"
#include "fdb/FDBKVEngine.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"

namespace hf3fs::meta::server {

static ExponentialBackoffRetry createBackoff(const MetaScan::Options &options) {
  return ExponentialBackoffRetry(std::chrono::milliseconds((int)(options.backoff_min_wait * 1000)),
                                 std::chrono::milliseconds((int)(options.backoff_max_wait * 1000)),
                                 std::chrono::milliseconds((int)(options.backoff_total_wait * 1000)));
}

CoTryTask<kv::IReadOnlyTransaction::GetRangeResult> MetaScan::KeyRange::snapshotGetRange(kv::IReadOnlyTransaction &txn,
                                                                                         int32_t limit) {
  XLOGF(DBG, "MetaScan snapshotGetRange {}", describe());
  auto result = co_await txn.snapshotGetRange({begin, true}, {end, false}, limit);
  CO_RETURN_ON_ERROR(result);
  hasMore = result->hasMore;
  if (!result->kvs.empty()) {
    begin = kv::TransactionHelper::keyAfter(result->kvs.rbegin()->key);
  }
  co_return result;
}

std::vector<MetaScan::KeyRange> MetaScan::KeyRange::split(std::string prefix) {
  std::vector<KeyRange> ranges;
  unsigned char c = 0;
  do {
    std::string begin = prefix + (char)c;
    std::string end;
    if (c != 0xff) {
      end = prefix + (char)(c + 1);
    } else {
      end = kv::TransactionHelper::prefixListEndKey(prefix);
    }
    ranges.push_back({begin, end});

    c += 1;
  } while (c != 0);

  return ranges;
}

MetaScan::MetaScan(Options options, std::shared_ptr<kv::IKVEngine> kvEngine)
    : options_(options),
      kvEngine_(kvEngine),
      exec_(std::pair<size_t, size_t>{options.threads, options.threads},
            std::make_shared<folly::NamedThreadFactory>("Scan")) {
  if (options_.threads < 0 || options_.coroutines < 0) {
    throw std::runtime_error("Invalid options, thread < 0 or coroutines < 0");
  }
  if (!kvEngine) {
    throw std::runtime_error("kvEngine is required");
  }
  if (!options_.logging.empty()) {
    XLOGF(INFO, "Setup log: {}", options_.logging);
    logging::initOrDie(options_.logging);
  }
}

MetaScan::~MetaScan() {
  if (scanInodeTask_.has_value()) {
    scanInodeTask_->cancel.requestCancellation();
  }
  if (scanDirEntryTask_.has_value()) {
    scanDirEntryTask_->cancel.requestCancellation();
  }
  exec_.stop();
}

std::vector<Inode> MetaScan::getInodes() {
  std::scoped_lock<std::mutex> lock(mutex_);

  if (!scanInodeTask_) {
    scanInodeTask_.emplace(256);
    scanInodeTask_->future = scanInode(*scanInodeTask_).scheduleOn(&exec_).start();
  }
  return waitResult(scanInodeTask_);
}

std::vector<DirEntry> MetaScan::getDirEntries() {
  std::scoped_lock<std::mutex> lock(mutex_);

  if (!scanDirEntryTask_) {
    scanDirEntryTask_.emplace(256);
    scanDirEntryTask_->future = scanDirEntry(*scanDirEntryTask_).scheduleOn(&exec_).start();
  }
  return waitResult(scanDirEntryTask_);
}

template <typename T>
std::vector<T> MetaScan::waitResult(std::optional<BackgroundTask<T>> &task) {
  std::vector<T> vec;
  while (true) {
    // dequeue
    while (true) {
      auto result = task->queue.try_dequeue();
      if (!result.has_value()) {
        break;
      }
      if (vec.empty()) {
        vec = std::move(*result);
      } else {
        vec.insert(vec.end(), std::make_move_iterator(result->begin()), std::make_move_iterator(result->end()));
      }
    }

    // return items
    if (vec.size() > 64) {
      return vec;
    }
    if (task->future.isReady()) {
      if (!vec.empty()) {
        return vec;
      }
      if (task->future.valid()) {
        auto &result = task->future.result();
        if (result.value().hasError()) {
          throw std::runtime_error(result.value().error().describe());
        }
      }
      return {};
    }
    std::this_thread::sleep_for(10_ms);
  }
}

template <typename T>
CoTryTask<void> MetaScan::scanRange(KeyRange range, BackgroundTask<T> &task) {
  auto originRange = range;
  XLOGF(INFO, "Worker scan range {}", originRange.describe());

  size_t total = 0;

  auto txn = kvEngine_->createReadonlyTransaction();
  auto txnCreateTime = RelativeTime::now();
  while (range.hasMore) {
    auto retry = createBackoff(options_);
    while (true) {
      // todo: tune FDB transaction get range mode
      auto result = co_await range.snapshotGetRange(*txn, options_.items_per_getrange);

      // handle error
      if (result.hasError()) {
        auto wait = retry.getWaitTime();
        if (wait.count() == 0) {
          XLOGF(ERR, "Failed to get range {} after retry {}ms", range.describe(), retry.getElapsedTime().count());
          CO_RETURN_ERROR(result);
        }
        if (result.error().code() == TransactionCode::kTooOld) {
          txnCreateTime = RelativeTime::now();
          txn->reset();
        }
        continue;
      }

      // deserialize and continue
      std::vector<T> items;
      items.reserve(result->kvs.size());
      for (auto [key, value] : result->kvs) {
        auto unpacked = T::newUnpacked(key, value);
        if (unpacked.hasError()) {
          // todo: maybe should return error here, or give caller a statistic data?
          XLOGF(FATAL, "Failed to deserialize key {:02x}, value {:02x}", fmt::join(key, ""), fmt::join(value, ""));
        } else {
          items.emplace_back(std::move(*unpacked));
        }
      }
      if (!items.empty()) {
        total += items.size();
        co_await folly::coro::co_withCancellation(task.cancel.getToken(), task.queue.enqueue(std::move(items)));
      }

      // reset transaction to avoid transaction too old
      if (RelativeTime::now() - txnCreateTime > std::chrono::seconds(3)) {
        txnCreateTime = RelativeTime::now();
        txn->reset();
      }
      break;
    }
  }

  XLOGF(INFO, "Worker finished scan range {}, found {} kvs", originRange.describe(), total);

  co_return Void{};
}

template <typename T>
CoTryTask<void> MetaScan::scan(kv::KeyPrefix prefix, BackgroundTask<T> &task) {
  auto prefixStr = std::string(kv::toStr(prefix));
  auto ranges = KeyRange::split(prefixStr);
  folly::Synchronized<std::queue<KeyRange>, std::mutex> taskQueue;
  for (auto &range : ranges) {
    taskQueue.lock()->push(range);
  }

  auto exec = co_await folly::coro::co_current_executor;
  std::vector<folly::SemiFuture<Result<Void>>> workers;
  for (auto i = 0; i < options_.coroutines; i++) {
    auto worker = folly::coro::co_invoke([&]() -> CoTryTask<void> {
      KeyRange range;
      while (true) {
        {
          auto guard = taskQueue.lock();
          if (guard->empty()) {
            co_return Void{};
          }
          range = guard->front();
          guard->pop();
        }
        auto result = co_await scanRange(range, task);
        CO_RETURN_ON_ERROR(result);
      }
      co_return Void{};
    });
    workers.push_back(std::move(worker).scheduleOn(exec).start());
  }

  auto results = co_await folly::coro::collectAllRange(std::move(workers));
  for (auto result : results) {
    CO_RETURN_ON_ERROR(result);
  }

  co_return Void{};
}

CoTryTask<void> MetaScan::scanInode(BackgroundTask<Inode> &task) {
  co_return co_await scan(kv::KeyPrefix::Inode, task);
}

CoTryTask<void> MetaScan::scanDirEntry(BackgroundTask<DirEntry> &task) {
  co_return co_await scan(kv::KeyPrefix::Dentry, task);
}

}  // namespace hf3fs::meta::server