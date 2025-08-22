#include <atomic>
#include <cstdlib>
#include <fmt/chrono.h>
#include <folly/CancellationToken.h>
#include <folly/Conv.h>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/Unit.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/Sleep.h>
#include <folly/experimental/coro/Task.h>
#include <folly/experimental/coro/UnboundedQueue.h>
#include <folly/experimental/coro/ViaIfAsync.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include "AdminEnv.h"
#include "Stat.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/meta/MetaClient.h"
#include "client/storage/StorageClient.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "common/utils/StringUtils.h"
#include "common/utils/Transform.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/MgmtdTypes.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("scan-tree");
  parser.add_argument("--inode").scan<'u', uint64_t>();
  parser.add_argument("--path");
  parser.add_argument("--output");
  return parser;
}
}  // namespace

class ScanTree {
 public:
  struct BrokenPath {
    Path path;
    Status error;
  };

  ScanTree(flat::UserInfo user, meta::client::MetaClient &meta, meta::PathAt path)
      : user_(user),
        meta_(meta),
        path_(path) {}

  CoTryTask<std::vector<BrokenPath>> run(size_t threads, size_t coroutines) {
    auto result = co_await meta_.stat(user_, path_.parent, path_.path, false);
    CO_RETURN_AND_LOG_ON_ERROR(result);
    enqueue({"", {}, *result});

    auto exec = std::make_unique<folly::CPUThreadPoolExecutor>(threads);
    std::vector<folly::SemiFuture<folly::Unit>> workers;
    while (workers.size() < coroutines) {
      workers.push_back(worker().scheduleOn(exec.get()).start());
    }

    while (pending_) {
      co_await folly::coro::sleep(std::chrono::milliseconds(100));
    }
    cancel_.requestCancellation();
    for (auto &worker : workers) {
      worker.wait();
    }

    co_return broken_.withWLock([](auto &broken) { return std::exchange(broken, {}); });
  }

 private:
  struct Task {
    Path path;
    meta::DirEntry entry;
    std::optional<meta::Inode> inode;
  };
  using TaskPtr = std::unique_ptr<Task>;

  void enqueue(Task task) {
    pending_++;
    queue_.enqueue(std::make_unique<Task>(std::move(task)));
  }

  CoTask<void> worker() {
    while (true) {
      auto dequeue =
          co_await folly::coro::co_awaitTry(folly::coro::co_withCancellation(cancel_.getToken(), queue_.dequeue()));
      if (UNLIKELY(dequeue.hasException())) {
        break;
      }

      auto &task = **dequeue;
      auto result = co_await handle(task);
      if (result.hasError()) {
        broken_.wlock()->push_back({task.path, result.error()});
      }
      pending_--;
      processed_++;
      XLOGF_EVERY_MS(INFO, 1000, "scan-tree processed {} tasks", processed_.load());
    }
  }

  CoTryTask<void> handle(Task &task) {
    if (!task.inode.has_value()) {
      auto result = co_await meta_.stat(user_, task.entry.id, std::nullopt, false);
      CO_RETURN_ON_ERROR(result);
      task.inode = std::move(*result);
    }

    auto &inode = *task.inode;
    CO_RETURN_AND_LOG_ON_ERROR(inode.valid());
    if (!inode.isDirectory()) {
      co_return Void{};
    }

    std::string prev;
    while (true) {
      auto list = co_await meta_.list(user_, inode.id, std::nullopt, prev, 512, true);
      CO_RETURN_AND_LOG_ON_ERROR(list);
      for (size_t i = 0; i < list->entries.size(); i++) {
        auto &entry = list->entries.at(i);
        auto inode = list->inodes.empty() ? std::nullopt : std::optional(list->inodes.at(i));
        prev = entry.name;
        enqueue({task.path / entry.name, entry, inode});
      }
      if (!list->more) {
        break;
      }
    }

    co_return Void{};
  }

  flat::UserInfo user_;
  meta::client::MetaClient &meta_;

  meta::PathAt path_;
  folly::CancellationSource cancel_;
  folly::coro::UnboundedQueue<TaskPtr> queue_;
  std::atomic<uint64_t> pending_ = 0;
  std::atomic<uint64_t> processed_ = 0;

  folly::Synchronized<std::vector<BrokenPath>> broken_;
};

CoTryTask<Dispatcher::OutputTable> scanTree(IEnv &ienv,
                                            const argparse::ArgumentParser &parser,
                                            const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.present<std::string>("--path");
  auto inodeId = parser.present<uint64_t>("--inode");
  auto output = parser.present<std::string>("--output");

  ENSURE_USAGE(inodeId.has_value() || path.has_value(), "set inodeId or path");

  ScanTree scan(env.userInfo,
                *env.metaClientGetter(),
                meta::PathAt{meta::InodeId(inodeId.value_or(env.currentDirId.u64())), path});
  auto result = co_await scan.run(8, 128);
  CO_RETURN_AND_LOG_ON_ERROR(result);

  table.push_back({"Path", "Status"});
  for (const auto &path : *result) {
    table.push_back({path.path.native(), path.error.describe()});
  }

  co_return table;
}

CoTryTask<void> registerScanTree(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, scanTree);
}

}  // namespace hf3fs::client::cli