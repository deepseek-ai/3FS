#include "CreateTarget.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/RapidCsv.h"
#include "common/utils/Result.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("create-target");
  parser.add_argument("--node-id").scan<'u', flat::NodeId::UnderlyingType>().required();
  parser.add_argument("--disk-index").scan<'u', uint32_t>().required();
  parser.add_argument("--target-id").scan<'u', flat::TargetId::UnderlyingType>().required();
  parser.add_argument("--chain-id").scan<'u', flat::ChainId::UnderlyingType>().required();
  parser.add_argument("--add-chunk-size").default_value(false).implicit_value(true);
  parser.add_argument("--chunk-size").nargs(argparse::nargs_pattern::any);
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

  storage::CreateTargetReq req;
  req.targetId = flat::TargetId(parser.get<flat::TargetId::UnderlyingType>("--target-id"));
  req.chainId = flat::ChainId(parser.get<flat::ChainId::UnderlyingType>("--chain-id"));
  req.diskIndex = parser.get<uint32_t>("--disk-index");
  req.addChunkSize = parser.get<bool>("--add-chunk-size");
  req.onlyChunkEngine = parser.get<bool>("--use-new-chunk-engine");
  std::vector<Size> chunkSizeList;
  for (const auto &size : parser.get<std::vector<std::string>>("--chunk-size")) {
    auto sizeResult = Size::from(size);
    CO_RETURN_AND_LOG_ON_ERROR(sizeResult);
    chunkSizeList.push_back(*sizeResult);
  }
  if (!chunkSizeList.empty()) {
    req.chunkSizeList = std::move(chunkSizeList);
  }

  CO_RETURN_AND_LOG_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
  if (routingInfo == nullptr || routingInfo->raw() == nullptr) {
    co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
  }
  auto chainInfo = routingInfo->getChain(req.chainId);
  if (chainInfo && chainInfo->targets.size() > 1) {
    // chain info exists and the number of replicas is greater than one.
    for (auto target : chainInfo->targets) {
      if (target.targetId == req.targetId && target.publicState == flat::PublicTargetState::LASTSRV) {
        co_return makeError(StorageClientCode::kRoutingError,
                            fmt::format("creation of this target is prohibited {}", target));
      }
    }
  }

  auto client = env.storageClientGetter();
  auto res = co_await client->createTarget(nodeId, req);
  CO_RETURN_ON_ERROR(res);

  table.push_back({fmt::format("Create target {} on disk {} of {} succeeded",
                               req.targetId.toUnderType(),
                               req.diskIndex,
                               nodeId.toUnderType())});

  co_return table;
}

}  // namespace
CoTryTask<void> registerCreateTargetHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleCreateTarget);
}
}  // namespace hf3fs::client::cli
