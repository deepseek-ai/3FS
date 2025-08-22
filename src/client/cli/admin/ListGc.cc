#include "ListGc.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/Coroutine.h"
#include "common/utils/MagicEnum.hpp"
#include "common/utils/String.h"
#include "meta/components/GcManager.h"

namespace hf3fs::client::cli {

namespace {
auto getParser() {
  argparse::ArgumentParser parser("gc-list");
  parser.add_argument("-l", "--limit").default_value(256).scan<'i', int>();
  parser.add_argument("-p", "--prev");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleListGc(IEnv &ienv,
                                                const argparse::ArgumentParser &parser,
                                                const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  Dispatcher::OutputTable table;

  ENSURE_USAGE(args.size() <= 1);

  auto limit = parser.get<int>("-l");
  auto prev = parser.present<std::string>("-p");
  auto path = args.empty() ? "." : args[0];
  auto res = co_await env.metaClientGetter()
                 ->list(env.userInfo, meta::InodeId::gcRoot(), Path(path), prev.value_or(""), limit, false);
  CO_RETURN_ON_ERROR(res);
  const auto &lsp = res.value();
  for (const auto &e : lsp.entries) {
    auto parsed = meta::server::GcManager::parseGcEntry(e.name);
    if (parsed.hasValue()) {
      table.push_back({fmt::format("{}", parsed->first),
                       fmt::format("{}", parsed->second),
                       e.name,
                       e.id.toHexString(),
                       std::string(magic_enum::enum_name(e.type))});
    } else {
      table.push_back({String(e.name), e.id.toHexString()});
    }
  }
  if (lsp.more) {
    table.push_back({"..."});
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerListGcHandler(Dispatcher &dispatcher) {
  constexpr auto usage = "Usage: gc-list [path] [-l limit] [-p prev]";
  co_return co_await dispatcher.registerHandler(usage, usage, getParser, handleListGc);
}

}  // namespace hf3fs::client::cli