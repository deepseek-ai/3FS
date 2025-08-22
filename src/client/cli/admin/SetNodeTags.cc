#include "SetNodeTags.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "Utils.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

const std::map<String, flat::SetTagMode> modeMap = {{"replace", flat::SetTagMode::REPLACE},
                                                    {"update", flat::SetTagMode::UPSERT},
                                                    {"remove", flat::SetTagMode::REMOVE}};

constexpr auto knownKeys = std::array{flat::kDisabledTagKey, flat::kTrafficZoneTagKey};

const auto modeChoices = [] {
  std::set<String> choices;
  for (const auto &[k, _] : modeMap) choices.insert(k);
  return choices;
}();

auto getParser() {
  argparse::ArgumentParser parser("set-node-tags");
  parser.add_description(fmt::format("known tag keys: {}", fmt::join(knownKeys, " | ")));
  parser.add_argument("-n", "--node-id").scan<'u', uint32_t>();
  parser.add_argument("-u", "--client-hostname");
  parser.add_argument("mode")
      .help(fmt::format("choices: {}", fmt::join(modeChoices, " | ")))
      .action([](const String &mode) {
        ENSURE_USAGE(modeChoices.contains(mode), fmt::format("invalid mode: {}", mode));
        return mode;
      });
  parser.add_argument("tags").remaining();
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleSetNodeTag(IEnv &ienv,
                                                    const argparse::ArgumentParser &parser,
                                                    const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto nodeId = parser.present<uint32_t>("-n");
  auto clientHostname = parser.present<String>("-u");
  auto mode = parser.get<String>("mode");
  auto tagStrs = parser.get<std::vector<String>>("tags");

  ENSURE_USAGE(nodeId.has_value() + clientHostname.has_value() == 1, "should set one of -n and -u");

  std::vector<flat::TagPair> tags;
  for (const auto &s : tagStrs) {
    flat::TagPair tp;
    auto p = s.find('=');
    if (p == String::npos) {
      tp.key = s;
    } else {
      tp.key = s.substr(0, p);
      tp.value = s.substr(p + 1);
    }
    tags.push_back(std::move(tp));
  }

  if (nodeId) {
    auto res =
        co_await env.mgmtdClientGetter()->setNodeTags(env.userInfo, flat::NodeId(*nodeId), tags, modeMap.at(mode));
    CO_RETURN_ON_ERROR(res);
    statNode(table, *res);
  } else if (clientHostname) {
    auto res =
        co_await env.mgmtdClientGetter()->setUniversalTags(env.userInfo, *clientHostname, tags, modeMap.at(mode));
    CO_RETURN_ON_ERROR(res);
    table.push_back({"Hostname", *clientHostname});
    table.push_back({"NewTags", fmt::format("[{}]", fmt::join(*res, ","))});
  }
  co_return table;
}
}  // namespace
CoTryTask<void> registerSetNodeTagsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleSetNodeTag);
}
}  // namespace hf3fs::client::cli
