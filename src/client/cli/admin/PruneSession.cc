#include "PruneSession.h"

#include <fmt/core.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/app/NodeId.h"
#include "common/utils/Result.h"
#include "meta/components/SessionManager.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("session-prune");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handlePruneSession(IEnv &ienv,
                                                      const argparse::ArgumentParser &parser,
                                                      const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  meta::server::SessionManager::Config config;
  config.set_sync_on_prune_session(false);
  meta::server::SessionManager manager(config,
                                       flat::NodeId{},
                                       env.kvEngineGetter(),
                                       env.mgmtdClientGetter(),
                                       {} /* todo: fix */);

  auto result = co_await manager.pruneManually();
  CO_RETURN_ON_ERROR(result);

  table.push_back({"num sessions", fmt::format("{}", *result)});
  co_return table;
}
}  // namespace

CoTryTask<void> registerPruneSessionHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handlePruneSession);
}

}  // namespace hf3fs::client::cli