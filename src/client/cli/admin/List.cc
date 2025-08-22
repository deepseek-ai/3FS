#include <fmt/chrono.h>
#include <string>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/MagicEnum.hpp"
#include "fmt/core.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("ls");
  parser.add_argument("-l", "--limit").scan<'i', int>();
  parser.add_argument("-s", "--status").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleList(IEnv &ienv,
                                              const argparse::ArgumentParser &parser,
                                              const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  Dispatcher::OutputTable table;

  ENSURE_USAGE(args.size() <= 1);

  auto path = args.empty() ? "." : args[0];
  auto limit = parser.present<int>("-l");
  auto status = parser.get<bool>("-s");

  String prev;
  for (;;) {
    auto res = co_await env.metaClientGetter()
                   ->list(env.userInfo, env.currentDirId, Path(path), prev, limit.value_or(0), status);
    CO_RETURN_ON_ERROR(res);
    const auto &lsp = res.value();
    for (size_t i = 0; i < lsp.entries.size(); ++i) {
      const auto &entry = lsp.entries.at(i);
      if (!status) {
        table.push_back({String(entry.name), String(magic_enum::enum_name(entry.type)), entry.id.toHexString()});
      } else {
        const auto &inode = lsp.inodes.at(i);
        table.push_back({String(entry.name),
                         String(magic_enum::enum_name(entry.type)),
                         entry.id.toHexString(),
                         fmt::format("{}", inode.data().mtime),
                         inode.isFile() ? fmt::format("{}", inode.asFile().getVersionedLength())
                                        : (inode.isDirectory() ? std::string("-") : inode.asSymlink().target.string()),
                         fmt::format("{:04o}", inode.acl.perm.toUnderType())});
      }
    }
    if (limit.has_value()) {
      limit.value() -= lsp.entries.size();
    }
    if (lsp.more && limit.value_or(1) > 0) {
      prev = lsp.entries.at(lsp.entries.size() - 1).name;
    } else {
      break;
    }
  }
  co_return table;
}
}  // namespace

CoTryTask<void> registerListHandler(Dispatcher &dispatcher) {
  constexpr auto usage = "Usage: ls [path] [-l limit] [-s]";
  co_return co_await dispatcher.registerHandler(usage, usage, getParser, handleList);
}
}  // namespace hf3fs::client::cli
