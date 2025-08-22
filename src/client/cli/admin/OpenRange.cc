#include "OpenRange.h"

#include <folly/experimental/coro/Collect.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("open-range");
  parser.add_argument("prefix");
  parser.add_argument("inclusive_start").scan<'i', int64_t>();
  parser.add_argument("exclusive_end").scan<'i', int64_t>();
  parser.add_argument("-c", "--concurrency").default_value(1).scan<'i', int>();
  parser.add_argument("-r", "--round").default_value(1).scan<'i', int>();
  return parser;
}

CoTask<Dispatcher::OutputTable> handleOpenSubRange(AdminEnv &env, const String &prefix, int64_t start, int64_t n) {
  Dispatcher::OutputTable table;
  for (auto i = 0; i < n; ++i) {
    auto path = fmt::format("{}{}", prefix, start + i);
    auto res = co_await env.metaClientGetter()->open(env.userInfo, env.currentDirId, Path(path), std::nullopt, 0);
    if (res.hasError()) {
      table.push_back({fmt::format("Error at {}", start + i), res.error().describe()});
    }
  }
  co_return std::move(table);
}

CoTryTask<Dispatcher::OutputTable> handleOpenRange(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());

  auto prefix = parser.get<std::string>("prefix");
  auto inclusiveStart = parser.get<int64_t>("inclusive_start");
  auto exclusiveEnd = parser.get<int64_t>("exclusive_end");
  auto concurrency = parser.get<int>("-c");
  auto round = parser.get<int>("-r");

  Dispatcher::OutputTable table;
  auto total = exclusiveEnd - inclusiveStart;
  auto succeeded = total * round;
  for (auto r = 0; r < round; ++r) {
    auto every = total / concurrency;
    auto remain = total % concurrency;

    std::vector<CoTask<Dispatcher::OutputTable>> tasks;
    auto start = inclusiveStart;
    for (auto i = 0; i < remain; ++i) {
      tasks.push_back(handleOpenSubRange(env, prefix, start, every + 1));
      start += every + 1;
    }
    for (auto i = remain; i < concurrency; ++i) {
      tasks.push_back(handleOpenSubRange(env, prefix, start, every));
      start += every;
    }
    auto res = co_await folly::coro::collectAllRange(std::move(tasks));

    for (auto &t : res) {
      for (auto &r : t) {
        table.push_back(std::move(r));
      }
      succeeded -= t.size();
    }
  }
  table.push_back({"Succeeded", std::to_string(succeeded)});
  table.push_back({"Failed", std::to_string(total * round - succeeded)});

  co_return table;
}

}  // namespace

CoTryTask<void> registerOpenRangeHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleOpenRange);
}
}  // namespace hf3fs::client::cli
