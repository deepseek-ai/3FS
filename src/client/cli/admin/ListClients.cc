#include "ListClients.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"

namespace hf3fs::client::cli {
namespace {

auto getParser() {
  argparse::ArgumentParser parser("list-clients");
  return parser;
}

void printClientSession(Dispatcher::OutputTable &table,
                        const flat::ClientSession &session,
                        const std::vector<flat::TagPair> &clientTags,
                        const flat::ConfigVersion *latestConfigVersion) {
  Dispatcher::OutputRow row;
  row.push_back(session.clientId);
  row.push_back(session.clientStart.YmdHMS());
  row.push_back(session.start.YmdHMS());
  row.push_back(session.lastExtend.YmdHMS());

  String configStatus = [&] {
    if (latestConfigVersion && session.configVersion == *latestConfigVersion) {
      switch (session.configStatus) {
        case ConfigStatus::NORMAL:
          return "UPTODATE";
        case ConfigStatus::DIRTY:
          return "DIRTY";
        case ConfigStatus::FAILED:
          return "FAILED";
        default:
          return "";
      }
    }
    return "";
  }();

  if (configStatus.empty()) {
    row.push_back(std::to_string(session.configVersion));
  } else {
    row.push_back(fmt::format("{}({})", session.configVersion.toUnderType(), configStatus));
  }
  row.push_back(session.universalId.empty() ? "N/A" : session.universalId);
  row.push_back(session.description.empty() ? "N/A" : session.description);
  row.push_back(fmt::format("[{}]", fmt::join(clientTags, ",")));
  row.push_back(fmt::format("{}", session.releaseVersion));

  table.push_back(std::move(row));
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto res = co_await env.mgmtdClientGetter()->listClientSessions();
  CO_RETURN_ON_ERROR(res);

  table.push_back({"ClientId",
                   "ClientStart",
                   "SessionStart",
                   "LastExtend",
                   "ConfigVersion",
                   "Hostname",
                   "Description",
                   "Tags",
                   "ReleaseVersion"});

  auto configVersionsRes = co_await env.mgmtdClientGetter()->getConfigVersions();
  const auto *latestConfigVersion =
      configVersionsRes && configVersionsRes->contains("CLIENT") ? &configVersionsRes->at("CLIENT") : nullptr;

  std::sort(res->sessions.begin(), res->sessions.end(), [](const flat::ClientSession &a, const flat::ClientSession &b) {
    if (a.universalId != b.universalId) {
      // clients without universalId have the lowest priority
      if (a.universalId.empty() || b.universalId.empty()) return b.universalId.empty();
      if (b.universalId.empty()) return true;
      return a.universalId < b.universalId;
    }
    if (a.description != b.description) {
      if (a.description.empty() || b.description.empty()) return b.description.empty();
      return a.description < b.description;
    }
    return a.clientId < b.clientId;
  });

  for (const auto &session : res->sessions) {
    printClientSession(table, session, res->referencedTags.at(session.universalId), latestConfigVersion);
  }

  if (res->bootstrapping) {
    // TODO: directly print via printer
    table.push_back({"Mgmtd Bootstrapping", "The client list may be incomplete"});
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerListClientsHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
