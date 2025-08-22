#include "RemoveTarget.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/RapidCsv.h"
#include "common/utils/Result.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("remove-target");
  parser.add_argument("--node-id").scan<'u', flat::NodeId::UnderlyingType>().required();
  parser.add_argument("--target-id").scan<'u', flat::TargetId::UnderlyingType>().required();
  parser.add_argument("--force").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleRemoveTarget(IEnv &ienv,
                                                      const argparse::ArgumentParser &parser,
                                                      const Dispatcher::Args &args) {
  [[maybe_unused]] auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  storage::RemoveTargetReq req;
  req.targetId = flat::TargetId(parser.get<flat::TargetId::UnderlyingType>("--target-id"));
  req.force = parser.get<bool>("--force");

  flat::NodeId nodeId{};
  auto nodeIdResult = parser.present<flat::NodeId::UnderlyingType>("--node-id");
  if (nodeIdResult.has_value()) {
    nodeId = flat::NodeId{nodeIdResult.value()};
  } else {
    CO_RETURN_AND_LOG_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(true));
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    if (routingInfo == nullptr || routingInfo->raw() == nullptr) {
      co_return makeError(StorageClientCode::kRoutingError, "routing info is null");
    }
    auto targetInfo = routingInfo->getTarget(req.targetId);
    if (targetInfo.has_value() && targetInfo->nodeId.has_value()) {
      nodeId = targetInfo->nodeId.value();
    } else {
      co_return makeError(StorageClientCode::kRoutingError, "node id is unknown");
    }
  }

  auto client = env.storageClientGetter();
  auto res = co_await client->removeTarget(nodeId, req);
  CO_RETURN_ON_ERROR(res);

  table.push_back({fmt::format("Remove target {} of {} succeeded", req.targetId.toUnderType(), nodeId.toUnderType())});

  co_return table;
}

}  // namespace
CoTryTask<void> registerRemoveTargetHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleRemoveTarget);
}
}  // namespace hf3fs::client::cli
