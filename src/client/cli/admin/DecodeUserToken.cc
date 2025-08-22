#include "DecodeUserToken.h"

#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "core/user/UserToken.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("decode-user-token");
  parser.add_argument("token");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleDecodeUserToken(IEnv &,
                                                         const argparse::ArgumentParser &parser,
                                                         const Dispatcher::Args &args) {
  Dispatcher::OutputTable table;
  ENSURE_USAGE(args.empty());

  auto token = parser.get<std::string>("token");
  auto res = core::decodeUserToken(token);
  CO_RETURN_ON_ERROR(res);
  auto [uid, ts] = *res;
  table.push_back({"Uid", fmt::format("{}", uid)});
  table.push_back({"Timestamp", fmt::format("{}", ts)});

  co_return table;
}
}  // namespace

CoTryTask<void> registerDecodeUserTokenHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleDecodeUserToken);
}
}  // namespace hf3fs::client::cli
