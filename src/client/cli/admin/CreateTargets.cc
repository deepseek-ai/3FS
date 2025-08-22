#include "CreateTargets.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/RapidCsv.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("create-targets");
  parser.add_argument("--node-id").scan<'u', flat::NodeId::UnderlyingType>().required();
  parser.add_argument("--disk-index").scan<'u', uint32_t>().nargs(argparse::nargs_pattern::at_least_one);
  parser.add_argument("--allow-existing-target").default_value(false).implicit_value(true);
  parser.add_argument("--add-chunk-size").default_value(false).implicit_value(true);
  parser.add_argument("--use-new-chunk-engine").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleCreateTarget(IEnv &ienv,
                                                      const argparse::ArgumentParser &parser,
                                                      const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto nodeId = flat::NodeId(parser.get<flat::NodeId::UnderlyingType>("--node-id"));
  auto diskIndexes = parser.get<std::vector<uint32_t>>("--disk-index");
  auto diskIndexSet = std::set<uint32_t>(diskIndexes.begin(), diskIndexes.end());
  auto allowExistingTarget = parser.get<bool>("--allow-existing-target");
  auto addChunkSize = parser.get<bool>("--add-chunk-size");
  auto onlyChunkEngine = parser.get<bool>("--use-new-chunk-engine");

  CO_RETURN_AND_LOG_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  auto rawRoutingInfo = routingInfo->raw();
  if (routingInfo == nullptr || rawRoutingInfo == nullptr) {
    co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }
  for (auto &[targetId, targetInfo] : rawRoutingInfo->targets) {
    if (targetInfo.nodeId == nodeId && targetInfo.diskIndex && diskIndexSet.contains(*targetInfo.diskIndex)) {
      if (targetInfo.publicState == flat::PublicTargetState::LASTSRV) {
        auto chainInfo = routingInfo->getChain(targetInfo.chainId);
        if (chainInfo->targets.size() > 1) {
          std::cout << fmt::format("creation of this target is prohibited {}, skip\n", targetInfo);
          continue;
        }
      }

      storage::CreateTargetReq req;
      req.targetId = targetId;
      req.chainId = targetInfo.chainId;
      req.diskIndex = *targetInfo.diskIndex;
      req.allowExistingTarget = allowExistingTarget;
      req.addChunkSize = addChunkSize;
      req.onlyChunkEngine = onlyChunkEngine;
      auto client = env.storageClientGetter();
      auto res = co_await client->createTarget(nodeId, req);
      CO_RETURN_ON_ERROR(res);

      std::cout << fmt::format("Create target {} on disk {} of {} succeeded\n",
                               req.targetId.toUnderType(),
                               req.diskIndex,
                               nodeId.toUnderType());
    }
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerCreateTargetsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleCreateTarget);
}

}  // namespace hf3fs::client::cli
