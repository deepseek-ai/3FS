#include "RecursiveChown.h"

#include <folly/experimental/coro/Collect.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

#define LOG_WITH_STDOUT(level, ...)      \
  do {                                   \
    auto msg = fmt::format(__VA_ARGS__); \
    XLOG(level, msg);                    \
    fmt::print("{}\n", msg);             \
  } while (0)

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("recursive-chown");
  parser.add_argument("path");
  parser.add_argument("uid").scan<'u', uint32_t>();
  parser.add_argument("gid").scan<'u', uint32_t>();
  parser.add_argument("-t", "--threads").default_value(uint32_t(16)).scan<'u', uint32_t>();
  parser.add_argument("-c", "--concurrency").default_value(uint32_t(2048)).scan<'u', uint32_t>();
  parser.add_argument("--debug").default_value(false).implicit_value(true);
  return parser;
}

struct TaskArg {
  meta::InodeId ino;
  Path path;
};

class RecursiveChownTask {
 public:
  RecursiveChownTask(AdminEnv &env,
                     Path path,
                     uint32_t uid,
                     uint32_t gid,
                     uint32_t threads_num,
                     uint32_t concurrency,
                     bool debug)
      : env_(env),
        path_(path),
        uid_(uid),
        gid_(gid),
        exec_(threads_num),
        listSemaphore_(concurrency),
        chownSemaphore_(concurrency),
        debug_(debug) {}

  ~RecursiveChownTask() { exec_.join(); }

  CoTryTask<Dispatcher::OutputTable> run() {
    auto res = co_await env_.metaClientGetter()->stat(env_.userInfo, env_.currentDirId, path_, false);
    CO_RETURN_ON_ERROR(res);
    auto ino = res->id;

    ++listTasks_;
    co_await listSemaphore_.co_wait();
    listDir(TaskArg{.ino = ino, .path = path_}).scheduleOn(&exec_).start();

    int64_t listTasksSuccess = 0;
    int64_t listTasksFailed = 0;
    int64_t listTasks = 0;

    int64_t chownTasksSuccess = 0;
    int64_t chownTasksFailed = 0;
    int64_t chownTasks = 0;

    for (;;) {
      listTasksSuccess = listTasksSuccess_.load(std::memory_order_acquire);
      listTasksFailed = listTasksFailed_.load(std::memory_order_acquire);
      listTasks = listTasks_.load(std::memory_order_acquire);

      chownTasksSuccess = chownTasksSuccess_.load(std::memory_order_acquire);
      chownTasksFailed = chownTasksFailed_.load(std::memory_order_acquire);
      chownTasks = chownTasks_.load(std::memory_order_acquire);

      if (listTasks == listTasksSuccess + listTasksFailed && chownTasks == chownTasksSuccess + chownTasksFailed) {
        break;
      }

      LOG_WITH_STDOUT(INFO,
                      "listTasks: total={} success={} failed={}. chownTasks: total={} success={} failed={}",
                      listTasks,
                      listTasksSuccess,
                      listTasksFailed,
                      chownTasks,
                      chownTasksSuccess,
                      chownTasksFailed);

      co_await folly::coro::sleep(std::chrono::seconds(1));
    }

    Dispatcher::OutputTable table;
    table.push_back(
        {"listTasks", "listTasksSuccess", "listTasksFailed", "chownTasks", "chownTasksSuccess", "chownTasksFailed"});
    table.push_back({
        std::to_string(listTasks),
        std::to_string(listTasksSuccess),
        std::to_string(listTasksFailed),
        std::to_string(chownTasks),
        std::to_string(chownTasksSuccess),
        std::to_string(chownTasksFailed),
    });
    co_return table;
  }

  CoTask<void> listDir(TaskArg arg) {
    SCOPE_EXIT { listSemaphore_.signal(); };
    std::list<TaskArg> args;
    co_await listDir(arg, args);
    co_await listDir(args);
  }

  CoTask<void> listDir(std::list<TaskArg> &args) {
    while (!args.empty()) {
      auto &arg = args.front();
      if (listSemaphore_.try_wait()) {
        listDir(std::move(arg)).scheduleOn(&exec_).start();
      } else {
        co_await listDir(arg, args);
      }
      args.pop_front();
    }
    co_return;
  }

  CoTask<void> listDir(const TaskArg &arg, std::list<TaskArg> &args) {
    auto metaClient = env_.metaClientGetter();
    if (debug_) {
      LOG_WITH_STDOUT(INFO, "Listing {} {}", arg.ino, arg.path);
    }
    String prev;
    while (true) {
      auto res = co_await metaClient->list(env_.userInfo, arg.ino, std::nullopt, prev, 0, false);
      if (res.hasError()) {
        LOG_WITH_STDOUT(CRITICAL, "List inode {} {} failed, {}", arg.ino, arg.path, res.error());
        ++listTasksFailed_;
        co_return;
      }

      for (const auto &entry : res->entries) {
        auto nextPath = arg.path / entry.name;
        if (entry.isDirectory()) {
          ++listTasks_;
          if (listSemaphore_.try_wait()) {
            listDir(TaskArg{.ino = entry.id, .path = nextPath}).scheduleOn(&exec_).start();
          } else {
            args.push_back(TaskArg{.ino = entry.id, .path = nextPath});
          }
        } else {
          ++chownTasks_;
          co_await chownSemaphore_.co_wait();
          chown(TaskArg{.ino = entry.id, .path = nextPath}).scheduleOn(&exec_).start();
        }
      }
      if (res->more) {
        prev = res->entries.back().name;
      } else {
        break;
      }
    }
    ++listTasksSuccess_;
    co_return;
  }

  CoTask<void> chown(TaskArg arg) {
    SCOPE_EXIT { chownSemaphore_.signal(); };
    auto metaClient = env_.metaClientGetter();
    if (debug_) {
      LOG_WITH_STDOUT(INFO, "Chowning {} {}", arg.ino, arg.path);
    }
    auto res = co_await metaClient->setPermission(env_.userInfo,
                                                  arg.ino,
                                                  std::nullopt,
                                                  false,
                                                  flat::Uid(uid_),
                                                  flat::Gid(gid_),
                                                  std::nullopt,
                                                  std::nullopt);
    if (res.hasError()) {
      LOG_WITH_STDOUT(CRITICAL, "Chown inode {} {} failed, {}", arg.ino, arg.path, res.error());
      ++chownTasksFailed_;
      co_return;
    }
    ++chownTasksSuccess_;
  }

 private:
  AdminEnv &env_;
  Path path_;
  uint32_t uid_;
  uint32_t gid_;

  folly::CPUThreadPoolExecutor exec_;
  folly::fibers::Semaphore listSemaphore_;
  folly::fibers::Semaphore chownSemaphore_;

  std::atomic<int64_t> listTasks_ = 0;
  std::atomic<int64_t> listTasksSuccess_ = 0;
  std::atomic<int64_t> listTasksFailed_ = 0;

  std::atomic<int64_t> chownTasks_ = 0;
  std::atomic<int64_t> chownTasksSuccess_ = 0;
  std::atomic<int64_t> chownTasksFailed_ = 0;

  bool debug_;
};

CoTryTask<Dispatcher::OutputTable> handleRecursiveChown(IEnv &ienv,
                                                        const argparse::ArgumentParser &parser,
                                                        const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());

  Path path{parser.get<std::string>("path")};
  auto uid = parser.get<uint32_t>("uid");
  auto gid = parser.get<uint32_t>("gid");
  auto threads_num = parser.get<uint32_t>("-t");
  auto concurrency = parser.get<uint32_t>("-c");
  auto debug = parser.get<bool>("--debug");

  auto task = std::make_unique<RecursiveChownTask>(env, path, uid, gid, threads_num, concurrency, debug);
  co_return co_await task->run();
}
}  // namespace

CoTryTask<void> registerRecursiveChownHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleRecursiveChown);
}
}  // namespace hf3fs::client::cli
