#include "DropUserCache.h"

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/StringUtils.h"
#include "stubs/MetaService/MetaServiceStub.h"

namespace hf3fs::client::cli {
namespace {
auto getParser() {
  argparse::ArgumentParser parser("drop-user-cache");
  parser.add_argument("-u", "--uid").scan<'u', uint32_t>();
  parser.add_argument("-a", "--all").default_value(false).implicit_value(true);
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto u = parser.present<uint32_t>("-u");
  auto all = parser.get<bool>("-a");

  std::optional<flat::Uid> uid;
  if (u) uid = flat::Uid(*u);

  CO_RETURN_ON_ERROR(co_await env.mgmtdClientGetter()->refreshRoutingInfo(/*force=*/true));
  auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo()->raw();

  auto req = meta::DropUserCacheReq(uid, all);

  table.push_back({"NodeId", "Status", "Desc"});

  auto client = env.clientGetter();
  for (const auto &[nid, ni] : routingInfo->nodes) {
    if (ni.type != flat::NodeType::META) continue;
    if (ni.status != flat::NodeStatus::HEARTBEAT_CONNECTED) {
      table.push_back({std::to_string(nid), "Skip", fmt::format("Status is {}", toStringView(ni.status))});
      continue;
    }
    auto addrs = ni.extractAddresses("MetaSerde");
    if (addrs.empty()) {
      table.push_back({std::to_string(nid), "Skip", "Address of MetaSerde not found"});
      continue;
    }
    auto result = co_await [&]() -> CoTask<std::vector<Status>> {
      std::vector<Status> rets;
      for (auto addr : addrs) {
        auto ctx = client->serdeCtx(addr);
        auto stub = meta::MetaServiceStub<serde::ClientContext>(ctx);
        auto ret = co_await stub.dropUserCache(req, {});
        if (!ret.hasError()) co_return std::vector<Status>{};
        rets.push_back(std::move(ret.error()));
      }
      co_return rets;
    }();

    if (result.empty()) {
      table.push_back({std::to_string(nid), "Succeed", ""});
    } else {
      table.push_back({std::to_string(nid), "Failed", fmt::format("[{}]", fmt::join(result, ", "))});
    }
  }

  co_return table;
}

}  // namespace

CoTryTask<void> registerDropUserCacheHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
