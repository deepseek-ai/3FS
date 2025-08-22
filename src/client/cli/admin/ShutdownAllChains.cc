#include "ShutdownAllChains.h"

#include <fmt/chrono.h>
#include <string>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/ArgParse.h"
#include "common/utils/Result.h"
#include "common/utils/StringUtils.h"
#include "common/utils/Uuid.h"
#include "fdb/FDBRetryStrategy.h"
#include "mgmtd/store/MgmtdStore.h"

namespace hf3fs::client::cli {
namespace {
kv::FDBRetryStrategy getRetryStrategy() { return kv::FDBRetryStrategy({1_s, 10, false}); }

auto getParser() {
  argparse::ArgumentParser parser("shutdown-all-chains");
  parser.add_argument("-c", "--check-offline").default_value(false).implicit_value(true);
  parser.add_argument("--display-chains").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  bool checkClusterOfflined = parser.get<bool>("-c");
  if (checkClusterOfflined) {
    auto mgmtdClient = env.unsafeMgmtdClientGetter();
    auto res = co_await mgmtdClient->refreshRoutingInfo(/*force=*/true);
    if (!res.hasError() || res.error().code() != MgmtdClientCode::kPrimaryMgmtdNotFound) {
      co_return makeError(
          StatusCode::kInvalidArg,
          fmt::format(
              "Cluster not offlined. expected status: MgmtdClientCode::kPrimaryMgmtdNotFound. actual status: {}",
              res.hasError() ? res.error().describe() : "OK"));
    }
  }

  mgmtd::MgmtdStore store;
  auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
    co_return co_await store.shutdownAllChains(txn);
  };
  auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                       .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
  CO_RETURN_ON_ERROR(commitRes);
  table.push_back({"Shutdown succeeded"});

  if (parser.get<bool>("--display-chains")) {
    auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<void> {
      auto chainsRes = co_await store.loadAllChains(txn);
      if (chainsRes.hasError()) {
        table.push_back({"Error when display chains", chainsRes.error().describe()});
      } else {
        size_t maxTargets = 0;
        for (const auto &ci : *chainsRes) {
          maxTargets = std::max(maxTargets, ci.targets.size());
        }
        Dispatcher::OutputRow head = {"ChainId", "ChainVersion"};
        for (size_t i = 0; i < maxTargets; ++i) {
          head.push_back("Target");
        }
        table.push_back(std::move(head));
        for (const auto &ci : *chainsRes) {
          Dispatcher::OutputRow row = {std::to_string(ci.chainId), std::to_string(ci.chainVersion)};
          for (const auto &cti : ci.targets) {
            row.push_back(fmt::format("{}({})", cti.targetId, toStringView(cti.publicState)));
          }
          table.push_back(std::move(row));
        }
      }
      co_return Void{};
    };

    auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                         .run(env.kvEngineGetter()->createReadonlyTransaction(), std::move(handler));
    if (commitRes.hasError()) {
      table.push_back({"Error when display chains", commitRes.error().describe()});
    }
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerShutdownAllChainsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
