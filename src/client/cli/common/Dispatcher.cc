#include "client/cli/common/Dispatcher.h"

#include <boost/algorithm/string.hpp>
#include <chrono>
#include <iostream>

#include "Parser.h"
#include "Printer.h"
#include "Utils.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/Linenoise.h"

namespace hf3fs::client::cli {
namespace {
using Clock = RelativeTime;

CoTryTask<Dispatcher::OutputTable> runLine(IEnv &env, Dispatcher &dispatcher, std::string_view line) {
  ArgsParser parser(line);

  std::vector<String> args;
  {
    auto res = parser.parseAll();
    CO_RETURN_ON_ERROR(res);
    args = std::move(res.value());
  }

  if (args.empty()) {
    co_return Dispatcher::OutputTable{};
  }

  try {
    co_return co_await dispatcher.run(env, args);
  } catch (const std::exception &e) {
    co_return makeError(StatusCode::kUnknown, e.what());
  } catch (...) {
    co_return makeError(StatusCode::kUnknown);
  }
}

void print(Printer &printer, const Result<Dispatcher::OutputTable> &res) {
  if (res.hasError()) {
    auto buf = fmt::format("Encounter error: {}({})", res.error().code(), StatusCode::toString(res.error().code()));
    if (!res.error().message().empty()) {
      buf += fmt::format("\n{}", res.error().message());
    }
    printer.print(buf);
  } else if (!res.value().empty()) {
    printer.print(res.value());
  }
}
}  // namespace
using Args = Dispatcher::Args;
using Handler = Dispatcher::Handler;
using OutputTable = Dispatcher::OutputTable;

struct Dispatcher::HandlerInfo {
  String usage;
  String help;
  ParserGetter parserGetter;
  Handler handler;

  HandlerInfo(String u, String hp, ParserGetter p, Handler h)
      : usage(std::move(u)),
        help(std::move(hp)),
        parserGetter(std::move(p)),
        handler(std::move(h)) {}
};

Dispatcher::Dispatcher() {}

Dispatcher::~Dispatcher() {}

CoTryTask<void> Dispatcher::registerHandler(ParserGetter parserGetter, Handler handler, bool replace) {
  if (!parserGetter) {
    co_return makeError(StatusCode::kInvalidArg, "Empty parserGetter");
  }
  auto parser = parserGetter();
  co_return co_await registerHandler(parser.usage(),
                                     parser.help().str(),
                                     std::move(parserGetter),
                                     std::move(handler),
                                     replace);
}

CoTryTask<void> Dispatcher::registerHandler(String usage,
                                            String help,
                                            ParserGetter parserGetter,
                                            Handler handler,
                                            bool replace) {
  if (usage.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "Empty usage");
  }
  if (help.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "Empty help");
  }
  if (!parserGetter) {
    co_return makeError(StatusCode::kInvalidArg, "Empty parserGetter");
  }
  if (!handler) {
    co_return makeError(StatusCode::kInvalidArg, "Empty handler");
  }

  auto parser = parserGetter();
  if (parser.program_name().empty()) {
    co_return makeError(StatusCode::kInvalidArg, "Empty command name");
  }
  auto handlerInfo =
      std::make_unique<HandlerInfo>(std::move(usage), std::move(help), std::move(parserGetter), std::move(handler));
  if (replace) {
    handlers_[parser.program_name()] = std::move(handlerInfo);
  } else {
    auto res = handlers_.emplace(parser.program_name(), std::move(handlerInfo));
    if (!res.second) {
      co_return makeError(StatusCode::kInvalidArg, fmt::format("Duplicated name: {}", parser.program_name()));
    }
  }
  co_return Void();
}

namespace {
auto wrongUsage(std::string_view usage, std::string_view err) {
  if (!err.empty()) {
    return makeError(CliCode::kWrongUsage, fmt::format("{}\n{}", err, usage));
  } else {
    return makeError(CliCode::kWrongUsage, usage);
  }
}
}  // namespace

CoTryTask<OutputTable> Dispatcher::run(IEnv &env, const Args &args) const {
  if (args.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "empty args");
  }
  const auto &methodName = args[0];
  if (methodName == "help") {
    co_return co_await handleHelp(args.size() > 1 ? args[1] : "");
  }

  auto res = co_await findMethod(methodName);
  CO_RETURN_ON_ERROR(res);
  const auto &info = *res.value();

  auto parser = info.parserGetter();
  Args unknownArgs;
  try {
    unknownArgs = parser.parse_known_args(args);
  } catch (const std::exception &e) {
    co_return wrongUsage(info.usage, fmt::format("parse failed: {}", e.what()));
  }

  try {
    co_return co_await info.handler(env, parser, unknownArgs);
  } catch (const StatusException &e) {
    if (e.get().code() == CliCode::kWrongUsage) co_return wrongUsage(info.usage, e.get().message());
    co_return makeError(std::move(e.get()));
  }
}

CoTryTask<const Dispatcher::HandlerInfo *> Dispatcher::findMethod(const String &name) const {
  if (name.empty()) {
    co_return makeError(StatusCode::kInvalidArg, "Empty method name");
  }
  auto it = handlers_.find(name);
  if (it == handlers_.end()) {
    co_return makeError(StatusCode::kInvalidArg, fmt::format("Unknown method name: {}", name));
  }
  co_return it->second.get();
}

CoTryTask<OutputTable> Dispatcher::handleHelp(String method) const {
  OutputTable table;
  if (method.empty()) {
    for (const auto &[method, info] : handlers_) {
      table.push_back({method, info->usage});
    }
  } else {
    auto res = co_await findMethod(method);
    CO_RETURN_ON_ERROR(res);
    table.push_back({res.value()->help});
  }
  co_return table;
}

std::map<String, String, std::less<>> Dispatcher::getUsages() const {
  std::map<String, String, std::less<>> ret;
  for (const auto &[k, v] : handlers_) {
    ret[k] = v->usage;
  }
  return ret;
}

static std::map<String, String, std::less<>> methodToUsages;

static void completion(const char *buf, linenoiseCompletions *lc) {
  for (const auto &[k, _] : methodToUsages) {
    if (k.starts_with(buf)) {
      linenoiseAddCompletion(lc, k.c_str());
    }
  }
}

static char *usageHint(const char *buf, [[maybe_unused]] int *color, [[maybe_unused]] int *bold) {
  auto sv = std::string_view{buf};
  if (auto it = methodToUsages.find(sv); it != methodToUsages.end()) {
    *bold = 1;
    auto usage = std::string_view{it->second}.substr(7 /*"Usage: "*/ + sv.size());
    auto *hint = new char[usage.size() + 1];
    std::memcpy(hint, usage.data(), usage.size());
    hint[usage.size()] = '\0';
    return hint;
  }
  return nullptr;
}

static void freeUsageHint(void *hint) { delete[] reinterpret_cast<char *>(hint); }

CoTryTask<void> Dispatcher::run(IEnv &env,
                                const std::function<String()> &promptGetter,
                                std::string_view cmd,
                                bool verbose,
                                bool profile,
                                bool breakMultiLineCommandOnFailure) {
  Printer printer([](const String &s) { std::cout << s << std::endl; },
                  [](const String &s) { std::cerr << s << std::endl; });

  methodToUsages = getUsages();
  linenoiseSetCompletionCallback(completion);
  linenoiseSetHintsCallback(usageHint);
  linenoiseSetFreeHintsCallback(freeUsageHint);

  SCOPE_EXIT {
    linenoiseSetCompletionCallback(nullptr);
    linenoiseSetHintsCallback(nullptr);
    linenoiseSetFreeHintsCallback(nullptr);
  };

  if (cmd.empty()) {
    std::string line;
    for (;;) {
      auto input = linenoise(fmt::format("{} > ", promptGetter()).c_str());
      if (!input) {
        break;
      }
      linenoiseHistoryAdd(input);
      line = input;
      linenoiseFree(input);

      if (verbose) printer.print(fmt::format("> Execute {}", line));

      auto start = Clock::now();
      auto res = co_await runLine(env, *this, line);
      auto duration = Clock::now() - start;

      print(printer, res);
      if (profile) printer.print(fmt::format("> Time: {}", duration));
    }
  } else {
    while (!cmd.empty()) {
      auto it = cmd.find_first_of(';');
      if (it == std::string_view::npos) {
        if (verbose) printer.print(fmt::format("> Execute {}", cmd));
        auto start = Clock::now();
        auto res = co_await runLine(env, *this, cmd);
        auto duration = Clock::now() - start;

        print(printer, res);
        cmd = {};

        if (profile) printer.print(fmt::format("> Time: {}", duration));

        if (res.hasError()) {
          co_return makeError(std::move(res.error()));
        }
      } else {
        auto line = cmd.substr(0, it);
        if (verbose) printer.print(fmt::format("> Execute {}", line));

        auto start = Clock::now();
        auto res = co_await runLine(env, *this, line);
        auto duration = Clock::now() - start;

        print(printer, res);
        cmd = cmd.substr(it + 1);

        if (profile) printer.print(fmt::format("> Time: {}", duration));

        if (breakMultiLineCommandOnFailure && res.hasError()) {
          break;
        }
      }
    }
  }
  co_return Void{};
}
}  // namespace hf3fs::client::cli
