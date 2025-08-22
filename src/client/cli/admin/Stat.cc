#include "Stat.h"

#include <cstdlib>
#include <fmt/chrono.h>
#include <folly/Conv.h>
#include <folly/logging/xlog.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "client/storage/StorageClient.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/StringUtils.h"
#include "common/utils/Transform.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/FileOperation.h"
#include "fbs/meta/Schema.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "fmt/core.h"
#include "scn/scan/scan.h"
#include "scn/tuple_return/tuple_return.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("stat");
  parser.add_argument("path");
  parser.add_argument("-L", "--follow-link").default_value(false).implicit_value(true);
  parser.add_argument("--display-layout").default_value(false).implicit_value(true);
  parser.add_argument("--display-chain-list").default_value(false).implicit_value(true);
  parser.add_argument("--display-chunks").default_value(false).implicit_value(true);
  parser.add_argument("--display-all-chunks").default_value(false).implicit_value(true);
  parser.add_argument("--inode").default_value(false).implicit_value(true);
  return parser;
}

auto format(auto v) { return fmt::format("{}", v); }

Result<Void> printLayout(Dispatcher::OutputTable &table,
                         const flat::RoutingInfo &ri,
                         const meta::Layout &layout,
                         bool displayChainList) {
  table.push_back({"Layout-ChainTable", std::to_string(layout.tableId)});
  table.push_back({"Layout-ChainTableVersion", std::to_string(layout.tableVersion)});
  table.push_back({"Layout-ChunkSize", std::to_string(layout.chunkSize)});
  table.push_back({"Layout-StripeSize", std::to_string(layout.stripeSize)});
  table.push_back({"Layout-Type", std::string(magic_enum::enum_name(layout.type()))});
  if (!displayChainList) {
    table.push_back({"Layout-ChainCount", std::to_string(layout.getChainIndexList().size())});
  } else {
    const auto &chainIndexes = layout.getChainIndexList();
    std::vector<String> chains;
    for (auto index : chainIndexes) {
      auto cid = ri.getChainId({layout.tableId, layout.tableVersion, index});
      if (!cid) {
        return makeError(MgmtdClientCode::kRoutingInfoNotReady);
      }
      chains.push_back(fmt::format("{}", *cid));
    }
    table.push_back({"Layout-Chains", fmt::format("[{}]", fmt::join(chains, ", "))});
  }
  return Void{};
}

Result<Void> printLayoutAndChunks(Dispatcher::OutputTable &table,
                                  const flat::RoutingInfo &ri,
                                  const meta::Layout &layout,
                                  std::map<flat::ChainId, meta::FileOperation::QueryResult> chunks,
                                  bool displayAllChunks) {
  table.push_back({"Layout-ChainTable", std::to_string(layout.tableId)});
  table.push_back({"Layout-ChainTableVersion", std::to_string(layout.tableVersion)});
  table.push_back({"Layout-ChunkSize", std::to_string(layout.chunkSize)});
  table.push_back({"Layout-StripeSize", std::to_string(layout.stripeSize)});
  table.push_back({"Layout-Type", std::string(magic_enum::enum_name(layout.type()))});

  size_t totalLength = 0;
  size_t totalChunkLength = 0;
  table.push_back({"Layout-Chains", "last chunk", "last chunk len", "last offset"});
  if (displayAllChunks) {
    table.push_back({"total chunks", "total chunks len"});
  }
  const auto &chainIndexes = layout.getChainIndexList();
  for (auto index : chainIndexes) {
    auto cid = ri.getChainId({layout.tableId, layout.tableVersion, index});
    if (!cid) {
      return makeError(MgmtdClientCode::kRoutingInfoNotReady);
    }
    if (chunks.contains(*cid)) {
      auto chunk = chunks.find(*cid)->second;
      table.push_back({format(*cid), format(chunk.lastChunk), format(chunk.lastChunkLen), format(chunk.length)});
      if (displayAllChunks) {
        table.push_back({format(chunk.totalNumChunks), format(chunk.totalChunkLen)});
      }
      totalChunkLength += chunk.totalChunkLen;
      totalLength = std::max(chunk.length, totalLength);
    } else {
      table.push_back({format(*cid), "0", "0", "0"});
      if (displayAllChunks) {
        table.push_back({"0", "0"});
      }
    }
  }

  if (displayAllChunks) {
    table.push_back({"total",
                     format(totalLength),
                     format(totalChunkLength),
                     format((int64_t)totalLength - (int64_t)totalChunkLength)});
  } else {
    table.push_back({"total", format(totalLength)});
  }

  return Void{};
}

CoTryTask<Dispatcher::OutputTable> handleStat(IEnv &ienv,
                                              const argparse::ArgumentParser &parser,
                                              const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto path = parser.get<std::string>("path");
  auto followLastSymlink = parser.get<bool>("-L");
  auto displayLayout = parser.get<bool>("--display-layout");
  auto displayChainList = parser.get<bool>("--display-chain-list");
  auto displayChunks = parser.get<bool>("--display-chunks");
  auto displayAllChunks = parser.get<bool>("--display-all-chunks");
  auto inode = parser.get<bool>("--inode");

  if (displayChainList) {
    displayLayout = true;
  }

  Result<meta::Inode> res = makeError(StatusCode::kInvalidArg);
  if (inode) {
    auto [r, v] = scn::scan_tuple<uint64_t>(path, "{:i}");
    if (!r) co_return makeError(StatusCode::kInvalidArg, fmt::format("invalid inodeid: {}", r.error().msg()));
    res = co_await env.metaClientGetter()->stat(env.userInfo, meta::InodeId(v), std::nullopt, followLastSymlink);
  } else {
    res = co_await env.metaClientGetter()->stat(env.userInfo, env.currentDirId, Path(path), followLastSymlink);
  }
  CO_RETURN_ON_ERROR(res);

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();

  const auto &o = res.value();
  // table.push_back({"Path", path});
  table.push_back({String("Type"), String(magic_enum::enum_name(o.getType()))});
  table.push_back({"Inode", o.id.toHexString()});
  table.push_back({"NLink", folly::to<std::string>(o.nlink)});
  table.push_back({"Permission", fmt::format("{:04o}", o.acl.perm.toUnderType())});
  table.push_back({"IFlags", fmt::format("0x{:08x}", o.acl.iflags.toUnderType())});
  table.push_back({"Uid", std::to_string(o.acl.uid)});
  table.push_back({"Gid", std::to_string(o.acl.gid)});
  table.push_back({"Access", o.atime.YmdHMS()});
  table.push_back({"Change", o.ctime.YmdHMS()});
  table.push_back({"Modified", o.mtime.YmdHMS()});
  switch (o.getType()) {
    case meta::InodeType::File: {
      table.push_back({"Length", fmt::format("{}@{}", o.asFile().length, o.asFile().truncateVer)});
      table.push_back({"DynamicStripe", fmt::format("{}", o.asFile().dynStripe)});
      if (displayChunks || displayAllChunks) {
        meta::FileOperation fop(*env.storageClientGetter(), *routingInfo->raw(), env.userInfo, o);
        auto result = co_await fop.queryChunksByChain(displayAllChunks, false);
        CO_RETURN_ON_ERROR(result);
        CO_RETURN_ON_ERROR(
            printLayoutAndChunks(table, *routingInfo->raw(), o.asFile().layout, *result, displayAllChunks));
      } else if (displayLayout) {
        CO_RETURN_ON_ERROR(printLayout(table, *routingInfo->raw(), o.asFile().layout, displayChainList));
      }
      break;
    }
    case meta::InodeType::Directory:
      table.push_back({"ParentInode", o.asDirectory().parent.toHexString()});
      if (displayLayout) {
        const auto &layout = o.asDirectory().layout;
        printLayout(table, *routingInfo->raw(), layout, displayChainList);
      }
      break;
    case meta::InodeType::Symlink:
      table.push_back({"Target", o.asSymlink().target.native()});
      break;
  }
  co_return table;
}
}  // namespace

CoTryTask<void> registerStatHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleStat);
}
}  // namespace hf3fs::client::cli
