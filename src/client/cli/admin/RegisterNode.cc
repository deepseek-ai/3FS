#include "RegisterNode.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/StringUtils.h"

namespace hf3fs::client::cli {
namespace {
const std::set<String> typeChoices(magic_enum::enum_names<flat::NodeType>().begin(),
                                   magic_enum::enum_names<flat::NodeType>().end());

auto getParser() {
  argparse::ArgumentParser parser("register-node");
  parser.add_argument("nodeId").scan<'u', uint32_t>();
  parser.add_argument("type").help(fmt::format("choices : {}", fmt::join(typeChoices, " | ")));
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto nodeId = flat::NodeId(parser.get<uint32_t>("nodeId"));
  auto type = magic_enum::enum_cast<flat::NodeType>(parser.get<String>("type"));
  ENSURE_USAGE(type.has_value(), fmt::format("invalid type: {}", parser.get<String>("type")));

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->registerNode(env.userInfo, nodeId, *type));

  co_return table;
}
}  // namespace
CoTryTask<void> registerRegisterNodesHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
