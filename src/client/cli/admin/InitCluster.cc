#include "InitCluster.h"

#include <fmt/core.h>
#include <folly/Conv.h>
#include <folly/logging/xlog.h>
#include <memory>

#include "AdminEnv.h"
#include "Utils.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/FileUtils.h"
#include "common/utils/Result.h"
#include "common/utils/StringUtils.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fdb/FDBRetryStrategy.h"
#include "fuse/FuseConfig.h"
#include "meta/components/ChainAllocator.h"
#include "meta/service/MetaServer.h"
#include "meta/store/MetaStore.h"
#include "mgmtd/service/MgmtdConfig.h"
#include "mgmtd/store/MgmtdStore.h"
#include "storage/service/StorageServer.h"

namespace hf3fs::client::cli {
namespace {
using namespace std::chrono_literals;
kv::FDBRetryStrategy getRetryStrategy() { return kv::FDBRetryStrategy({1_s, 10, false}); }

auto getParser() {
  argparse::ArgumentParser parser("init-cluster");
  parser.add_argument("chaintableid").scan<'u', uint32_t>();
  parser.add_argument("chunksize").scan<'u', uint32_t>();
  parser.add_argument("stripesize").scan<'u', uint32_t>();
  parser.add_argument("--mgmtd", "--mgmtd-config-path");
  parser.add_argument("--meta", "--meta-config-path");
  parser.add_argument("--storage", "--storage-config-path");
  parser.add_argument("--fuse", "--fuse-config-path");
  parser.add_argument("--allow-config-existed").default_value(false).implicit_value(true);
  parser.add_argument("--skip-config-check").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<void> handleInitFileSystem(AdminEnv &env,
                                     const argparse::ArgumentParser &parser,
                                     Dispatcher::OutputTable &table) {
  auto tableId = flat::ChainTableId(parser.get<uint32_t>("chaintableid"));
  auto chunksize = parser.get<uint32_t>("chunksize");
  auto stripesize = parser.get<uint32_t>("stripesize");

  auto chainAlloc = std::make_unique<meta::server::ChainAllocator>(nullptr);
  auto rootLayout = meta::Layout::newEmpty(tableId, chunksize, stripesize);
  auto op = meta::server::MetaStore::initFileSystem(*chainAlloc, rootLayout);
  auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> { co_return co_await op->run(txn); };

  auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                       .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
  if (commitRes.hasError()) {
    co_return makeError(commitRes.error().code(), fmt::format("Failed to init filesystem: {}", commitRes.error()));
  };

  table.push_back({fmt::format("Init filesystem, root directory layout: chain table {}, chunksize {}, stripesize {}\n",
                               tableId,
                               chunksize,
                               stripesize)});

  co_return Void{};
}

CoTryTask<void> handleInitConfig(AdminEnv &env,
                                 Dispatcher::OutputTable &table,
                                 flat::NodeType nodeType,
                                 config::IConfig &&cfg [[maybe_unused]],
                                 mgmtd::MgmtdStore &store,
                                 const String &filePath,
                                 bool allowConfigExisted,
                                 bool skipConfigCheck) {
  auto configStr = loadFile(filePath);
  CO_RETURN_ON_ERROR(configStr);
  if (!skipConfigCheck) {
    auto validateRes = cfg.update(std::string_view(*configStr), /*isHotUpdate=*/false);
    if (validateRes.hasError()) {
      co_return makeError(validateRes.error().code(),
                          fmt::format("Validate config for {} failed: {}", toString(nodeType), validateRes.error()));
    }
  }

  auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<flat::ConfigVersion> {
    auto configInfo = co_await store.loadLatestConfig(txn, nodeType);
    CO_RETURN_ON_ERROR(configInfo);
    flat::ConfigVersion ver(1);
    if (configInfo->has_value()) {
      if (!allowConfigExisted) {
        co_return makeError(StatusCode::kUnknown,
                            fmt::format("Config for {} existed, version {}",
                                        toString(nodeType),
                                        configInfo->value().configVersion.toUnderType()));
      }
      ver = flat::ConfigVersion(configInfo->value().configVersion + 1);
    }
    auto writeRes = co_await store.storeConfig(txn, nodeType, flat::ConfigInfo::create(ver, std::move(*configStr)));
    CO_RETURN_ON_ERROR(writeRes);
    co_return ver;
  };

  auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                       .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
  if (commitRes.hasError()) {
    co_return makeError(commitRes.error().code(),
                        fmt::format("Failed to set config for {}: {}", toString(nodeType), commitRes.error()));
  };

  table.push_back({fmt::format("Init config for {} version {}", toString(nodeType), commitRes->toUnderType())});
  co_return Void{};
}

CoTryTask<Dispatcher::OutputTable> handleInitCluster(IEnv &ienv,
                                                     const argparse::ArgumentParser &parser,
                                                     const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  CO_RETURN_ON_ERROR(co_await handleInitFileSystem(env, parser, table));

  auto allowConfigExisted = parser.get<bool>("--allow-config-existed");
  // TODO: consider how to check
  // auto skipConfigCheck = parser.get<bool>("skip-config-check");
  auto skipConfigCheck = true;
  mgmtd::MgmtdStore store;
  if (auto mgmtdConfigPath = parser.present<String>("--mgmtd"); mgmtdConfigPath) {
    CO_RETURN_ON_ERROR(co_await handleInitConfig(env,
                                                 table,
                                                 flat::NodeType::MGMTD,
                                                 mgmtd::MgmtdConfig{},
                                                 store,
                                                 *mgmtdConfigPath,
                                                 allowConfigExisted,
                                                 skipConfigCheck));
  }
  if (auto configPath = parser.present<String>("--meta"); configPath) {
    CO_RETURN_ON_ERROR(co_await handleInitConfig(env,
                                                 table,
                                                 flat::NodeType::META,
                                                 meta::server::MetaServer::Config{},
                                                 store,
                                                 *configPath,
                                                 allowConfigExisted,
                                                 skipConfigCheck));
  }
  if (auto configPath = parser.present<String>("--storage"); configPath) {
    CO_RETURN_ON_ERROR(co_await handleInitConfig(env,
                                                 table,
                                                 flat::NodeType::STORAGE,
                                                 storage::StorageServer::Config{},
                                                 store,
                                                 *configPath,
                                                 allowConfigExisted,
                                                 skipConfigCheck));
  }
  if (auto configPath = parser.present<String>("--fuse"); configPath) {
    CO_RETURN_ON_ERROR(co_await handleInitConfig(env,
                                                 table,
                                                 flat::NodeType::FUSE,
                                                 fuse::FuseConfig{},
                                                 store,
                                                 *configPath,
                                                 allowConfigExisted,
                                                 skipConfigCheck));
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerInitClusterHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleInitCluster);
}
}  // namespace hf3fs::client::cli
