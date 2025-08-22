#include "FDBContext.h"

#include <fmt/core.h>
#include <folly/logging/xlog.h>
#include <memory>

#include "common/utils/String.h"
#include "foundationdb/fdb_c_options.g.h"

namespace hf3fs::kv::fdb {

#define CHECK_FDB_COND(condition, ...) XLOGF_IF(FATAL, !(condition), "FoundationDB error. " __VA_ARGS__)

#define CHECK_FDB_ERR(err, msg, ...) \
  XLOGF_IF(FATAL, (err) != 0, "FoundationDB error: {}. " msg, DB::errorMsg(err), #__VA_ARGS__)

#define CHECK_FDB_OP(cmd, ...)        \
  do {                                \
    auto err = (cmd);                 \
    CHECK_FDB_ERR(err, #__VA_ARGS__); \
  } while (false)

std::shared_ptr<FDBContext> FDBContext::create(const FDBConfig &config) {
  auto context = std::shared_ptr<FDBContext>(new FDBContext(config));
  return context;
}

FDBContext::FDBContext(const FDBConfig &config)
    : config_(config) {
  CHECK_FDB_OP(DB::selectAPIVersion(FDB_API_VERSION), "Failed to set API Version = {}.", FDB_API_VERSION);

  if (config_.enableMultipleClient()) {
    XLOGF(INFO, "FoundationDB: enable externalClientDir");
    const String &externalDir = config_.externalClientDir();
    const String &externalPath = config_.externalClientPath();
    CHECK_FDB_COND((!externalDir.empty() || !externalPath.empty()),
                   "'externalClientDir' or 'externalClientPath' should not be empty when 'enableMultipleClient'.");

    // CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_DISABLE_LOCAL_CLIENT), "Failed to disable local client.");
    if (!externalPath.empty()) {
      CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_EXTERNAL_CLIENT_LIBRARY, externalPath),
                   "Failed to set FDB_NET_OPTION_EXTERNAL_CLIENT_LIBRARY = {}.",
                   externalPath);
    } else {
      CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_EXTERNAL_CLIENT_DIRECTORY, externalDir),
                   "Failed to set FDB_NET_OPTION_EXTERNAL_CLIENT_DIRECTORY = {}.",
                   externalDir);
    }

    auto threadCnt = config_.multipleClientThreadNum();
    auto threadCntOption = std::string_view{reinterpret_cast<const char *>(&threadCnt), sizeof(threadCnt)};
    CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_CLIENT_THREADS_PER_VERSION, threadCntOption),
                 "Failed to set FDB_NET_OPTION_CLIENT_THREADS_PER_VERSION = {}.",
                 threadCnt);

    CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_CALLBACKS_ON_EXTERNAL_THREADS),
                 "Failed to set FDB_NET_OPTION_CALLBACKS_ON_EXTERNAL_THREADS.");
  }

  if (!config_.trace_file().empty()) {
    CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_TRACE_ENABLE, config_.trace_file()),
                 "Failed to set FDB_NET_OPTION_TRACE_ENABLE to {}",
                 config_.trace_file());
    CHECK_FDB_OP(DB::setNetworkOption(FDB_NET_OPTION_TRACE_FORMAT, config_.trace_format()),
                 "Failed to set FDB_NET_OPTION_TRACE_FORMAT to {}",
                 config_.trace_format());
  }

  if (config_.default_backoff()) {
    CHECK_FDB_OP(
        DB::setNetworkOption(FDB_NET_OPTION_KNOB, fmt::format("DEFAULT_BACKOFF={}", config_.default_backoff())),
        "Failed to set DEFAULT_BACKOFF to {}",
        config_.default_backoff());
  }

  CHECK_FDB_OP(DB::setupNetwork(), "Failed to setup network thread.");

  networkThread = std::thread([] {
    pthread_setname_np(pthread_self(), "fdb_net");
    DB::runNetwork();
  });
}

FDBContext::~FDBContext() {
  if (networkThread.joinable()) {
    CHECK_FDB_OP(DB::stopNetwork(), "Failed to stop network thread.");

    networkThread.join();
  }
}

DB FDBContext::getDB() const {
  DB db(config_.clusterFile(), config_.readonly());
  CHECK_FDB_ERR(db.error(), "Failed to get fdb::DB instance.");
  if (config_.casual_read_risky()) {
    CHECK_FDB_ERR(db.setOption(FDB_DB_OPTION_TRANSACTION_CAUSAL_READ_RISKY),
                  "Failed to set FDB_DB_OPTION_TRANSACTION_CAUSAL_READ_RISKY");
  }
  return db;
}

#undef CHECK_FDB_OP
#undef CHECK_FDB_COND
}  // namespace hf3fs::kv::fdb
