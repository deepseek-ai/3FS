#pragma once

#include <vector>

#include "IEnv.h"
#include "common/utils/ArgParse.h"
#include "common/utils/Coroutine.h"

namespace hf3fs::client::cli {
class Dispatcher {
 public:
  using Args = std::vector<String>;
  using OutputRow = std::vector<String>;
  using OutputTable = std::vector<OutputRow>;
  using ParserGetter = std::function<argparse::ArgumentParser()>;
  using Handler = std::function<CoTryTask<OutputTable>(IEnv &, const argparse::ArgumentParser &, const Args &)>;

  Dispatcher();
  ~Dispatcher();
  CoTryTask<OutputTable> run(IEnv &env, const Args &args) const;

  CoTryTask<void> registerHandler(String usage,
                                  String help,
                                  ParserGetter parserGetter,
                                  Handler handler,
                                  bool replace = false);
  CoTryTask<void> registerHandler(ParserGetter parserGetter, Handler handler, bool replace = false);

  template <typename Handler>
  CoTryTask<void> registerHandler() {
    co_return co_await registerHandler(&Handler::getParser, &Handler::handle);
  }

  std::map<String, String, std::less<>> getUsages() const;

  CoTryTask<void> run(IEnv &env,
                      const std::function<String()> &promptGetter,
                      std::string_view cmd,
                      bool verbose,
                      bool profile,
                      bool breakMultiLineCommandOnFailure);

 private:
  struct HandlerInfo;

  CoTryTask<const HandlerInfo *> findMethod(const String &name) const;
  CoTryTask<OutputTable> handleHelp(String method) const;

  std::map<String, std::unique_ptr<HandlerInfo>> handlers_;
};
}  // namespace hf3fs::client::cli
