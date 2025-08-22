#include "Chdir.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "scn/tuple_return/tuple_return.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("cd");
  parser.add_argument("-L").default_value(false).implicit_value(true);
  parser.add_argument("--inode").default_value(false).implicit_value(true);
  parser.add_argument("path");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleChdir(IEnv &ienv,
                                               const argparse::ArgumentParser &parser,
                                               const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  // TODO: now cd can't correctly handle many cases e.g.:
  // - path contains symlink.
  // - and so on.
  Dispatcher::OutputTable table;
  auto pathstr = parser.get<std::string>("path");
  Path path(pathstr);
  auto inodeid = env.currentDirId;
  auto metaClient = env.metaClientGetter();
  bool followLastSymlink = parser.get<bool>("-L");
  bool byinode = parser.get<bool>("--inode");

  Result<meta::Inode> res = makeError(MetaCode::kFoundBug);
  if (!byinode) {
    inodeid = meta::InodeId::root();
    if (!path.is_absolute()) {
      inodeid = env.currentDirId;
    }
    res = co_await metaClient->stat(env.userInfo, inodeid, path, true);
  } else {
    auto [r, v] = scn::scan_tuple<uint64_t>(pathstr, "{:i}");
    if (!r) {
      co_return makeError(StatusCode::kInvalidArg, "invalid inodeid {}", path);
    }
    inodeid = meta::InodeId(v);
    path = ".";
    res = co_await metaClient->stat(env.userInfo, inodeid, std::nullopt, true);
  }

  CO_RETURN_ON_ERROR(res);
  const auto &inode = res.value();
  if (inode.getType() != meta::InodeType::Directory) {
    co_return makeError(MetaCode::kNotDirectory);
  }

  if (followLastSymlink || byinode) {
    auto getRealPathRes = co_await metaClient->getRealPath(env.userInfo, inodeid, path, true);
    CO_RETURN_ON_ERROR(getRealPathRes);
    path = *getRealPathRes;
  }

  env.currentDirId = inode.id;
  env.currentDir = path.native();
  co_return table;
}

}  // namespace

CoTryTask<void> registerChdirHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleChdir);
}
}  // namespace hf3fs::client::cli
