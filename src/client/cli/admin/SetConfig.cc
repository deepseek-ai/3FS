#include <folly/Conv.h>

#include "AdminEnv.h"
#include "ListNodes.h"
#include "Utils.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"

namespace hf3fs::client::cli {
namespace {

const std::map<String, flat::NodeType> typeMappings = {
    {"MGMTD", flat::NodeType::MGMTD},
    {"META", flat::NodeType::META},
    {"STORAGE", flat::NodeType::STORAGE},
    {"CLIENT", flat::NodeType::CLIENT},
    {"CLIENT_AGENT", flat::NodeType::CLIENT},
    {"FUSE", flat::NodeType::FUSE},
};

const std::set<String> &typeChoices() {
  static auto choices = [] {
    std::set<String> s;
    for (const auto &[k, _] : typeMappings) {
      s.insert(k);
    }
    return s;
  }();
  return choices;
}

auto getParser() {
  argparse::ArgumentParser parser("set-config");
  parser.add_argument("-t", "--type").help(fmt::format("choices : {}", fmt::join(typeChoices(), " | ")));
  parser.add_argument("-f", "--file");
  parser.add_argument("--desc");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto type = parser.present<String>("-t");
  auto path = parser.present<String>("-f");
  auto desc = parser.present<String>("--desc").value_or("");

  ENSURE_USAGE(type.has_value() && path.has_value(), "must specify -t and -f");

  ENSURE_USAGE(typeMappings.contains(*type), fmt::format("invalid type: {}", *type));
  auto t = typeMappings.at(*type);

  auto loadFileRes = loadFile(*path);
  CO_RETURN_ON_ERROR(loadFileRes);

  auto res = co_await env.mgmtdClientGetter()->setConfig(env.userInfo, t, *loadFileRes, desc);
  CO_RETURN_ON_ERROR(res);

  table.push_back({"Succeed"});
  table.push_back({"ConfigVersion", std::to_string(*res)});

  co_return table;
}
}  // namespace
CoTryTask<void> registerSetConfigHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
