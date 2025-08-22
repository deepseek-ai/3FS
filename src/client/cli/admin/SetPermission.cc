#include "SetPermission.h"

#include <scn/scn.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "fbs/meta/Common.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("set-perm");
  parser.add_argument("-u", "--uid").scan<'u', uint32_t>();
  parser.add_argument("-g", "--gid").scan<'u', uint32_t>();
  parser.add_argument("-p", "--perm").scan<'o', uint32_t>();
  parser.add_argument("--iflags").scan<'i', uint32_t>();
  parser.add_argument("path");
  return parser;
}

template <typename T>
std::optional<T> to(auto val) {
  if (!val.has_value()) return std::nullopt;
  return T(*val);
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.get<std::string>("path");
  auto u = parser.present<uint32_t>("-u");
  auto g = parser.present<uint32_t>("-g");
  auto p = parser.present<uint32_t>("-p");
  auto iflags = parser.present<uint32_t>("--iflags");

  auto res = co_await env.metaClientGetter()->setPermission(env.userInfo,
                                                            env.currentDirId,
                                                            Path(path),
                                                            true,
                                                            to<flat::Uid>(u),
                                                            to<flat::Gid>(g),
                                                            to<meta::Permission>(p),
                                                            to<meta::IFlags>(iflags));
  CO_RETURN_ON_ERROR(res);
  co_return table;
}

}  // namespace

CoTryTask<void> registerSetPermissionHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
