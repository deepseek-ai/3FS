#include "RemoteCall.h"

#include <folly/Conv.h>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/utils/FileUtils.h"
#include "common/utils/StringUtils.h"
#include "fbs/core/service/CoreServiceBase.h"
#include "fbs/meta/Service.h"
#include "fbs/mgmtd/MgmtdServiceBase.h"
#include "fbs/storage/Service.h"

namespace hf3fs::client::cli {
namespace {
using MgmtdBase = mgmtd::MgmtdServiceBase<>;
using CoreBase = core::CoreServiceBase<>;
using StorageBase = storage::StorageSerde<>;
using MetaBase = meta::MetaSerde<>;

const auto services =
    std::array{MgmtdBase::kServiceName, CoreBase::kServiceName, StorageBase::kServiceName, MetaBase::kServiceName};

auto getParser() {
  argparse::ArgumentParser parser("remote-call");
  parser.add_argument("-n", "--node-id").scan<'u', uint32_t>();
  parser.add_argument("-c", "--client-id");
  parser.add_argument("-a", "--addr");
  parser.add_argument("--primary").default_value(false).implicit_value(true);
  parser.add_argument("-s", "--service-name").help(fmt::format("choices: {}", fmt::join(services, " | ")));
  parser.add_argument("-m", "--method-name");
  parser.add_argument("-i", "--method-id").scan<'i', int32_t>();
  parser.add_argument("-j", "--input-json");
  parser.add_argument("-f", "--input-json-file");
  parser.add_argument("-o", "--output-file");
  parser.add_argument("--list-methods").default_value(false).implicit_value(true);

  return parser;
}

#define REMOTE_CALLER_ARGS serde::ClientContext &ctx, std::string_view input, bool prettyFormat

using RemoteCaller = std::function<CoTryTask<String>(REMOTE_CALLER_ARGS)>;

template <typename ServiceBase>
Result<Void> getRemoteCaller(RemoteCaller &caller, String &methodName, int32_t &methodId) {
  Result<Void> result = Void{};
  auto handler = [&](auto type) {
    using Type = decltype(type);
    if (Type::name == methodName || static_cast<int32_t>(Type::id) == methodId) {
      // complete method name and id
      methodName = Type::name;
      methodId = static_cast<int32_t>(Type::id);

      caller = [methodName](REMOTE_CALLER_ARGS) -> CoTryTask<String> {
        auto req = typename Type::ReqType{};
        auto fromRes = serde::fromJsonString(req, input);
        if (fromRes.hasError()) {
          co_return makeError(fromRes.error().code(),
                              fmt::format("Deserialize {} failed: {}", typeid(req).name(), fromRes.error().message()));
        }
        auto callRes = co_await ctx.template call<ServiceBase::kServiceNameWrapper,
                                                  Type::nameWrapper,
                                                  typename Type::ReqType,
                                                  typename Type::RspType,
                                                  ServiceBase::kServiceID,
                                                  Type::id>(req, nullptr, nullptr);
        if (callRes.hasError()) {
          co_return makeError(
              callRes.error().code(),
              fmt::format("Call {}::{} failed: {}", ServiceBase::kServiceName, Type::name, callRes.error().message()));
        }
        co_return serde::toJsonString(*callRes, /*sortKeys=*/false, prettyFormat);
      };
      return false;
    }
    return true;
  };

  auto matchNothing = refl::Helper::iterate<ServiceBase>(std::move(handler));
  if (matchNothing) {
    result = makeError(RPCCode::kInvalidMethodID,
                       fmt::format("Could not find method by name({})/id({})", methodName, methodId));
  }

  return result;
}

template <typename ServiceBase>
Result<Void> listMethods(Dispatcher::OutputTable &table) {
  table.push_back({"MethodId", "MethodName"});
  auto handler = [&](auto type) {
    using Type = decltype(type);
    table.push_back({std::to_string(Type::id), String(Type::name)});
    return true;
  };

  refl::Helper::iterate<ServiceBase>(std::move(handler));
  return Void{};
}

Result<String> loadInput(const String &inputJson, const String &inputFile) {
  if (!inputJson.empty()) return inputJson;
  return loadFile(inputFile);
}

CoTryTask<std::vector<net::Address>> getAddrs(Dispatcher::OutputTable &table,
                                              AdminEnv &env,
                                              const String &service,
                                              std::optional<uint32_t> nodeId,
                                              std::optional<String> clientId,
                                              std::optional<String> addr,
                                              bool primary) {
  std::vector<net::Address> addrs;
  if (nodeId) {
    table.push_back({"NodeId", std::to_string(*nodeId)});
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    auto node = routingInfo->getNode(flat::NodeId(*nodeId));
    if (!node) {
      co_return makeError(MgmtdCode::kNodeNotFound);
    }
    addrs = node->extractAddresses(service);
  } else if (clientId) {
    table.push_back({"ClientId", *clientId});
    auto clientSession = co_await env.mgmtdClientGetter()->getClientSession(*clientId);
    CO_RETURN_ON_ERROR(clientSession);

    if (clientSession->session) {
      addrs = flat::extractAddresses(clientSession->session->serviceGroups, service);
    } else {
      co_return makeError(MgmtdCode::kClientSessionNotFound,
                          fmt::format("bootstrapping: {}", clientSession->bootstrapping ? "Yes" : "No"));
    }
  } else if (addr) {
    table.push_back({"Address", *addr});
    auto addrRes = net::Address::from(*addr);
    CO_RETURN_ON_ERROR(addrRes);
    addrs.push_back(*addrRes);
  } else if (primary) {
    auto routingInfo = env.mgmtdClientGetter()->getRoutingInfo();
    auto nodes = routingInfo->getNodeBy(flat::selectNodeByStatus(flat::NodeStatus::PRIMARY_MGMTD));
    if (nodes.empty()) {
      co_return makeError(MgmtdClientCode::kPrimaryMgmtdNotFound);
    } else if (nodes.size() > 1) {
      co_return makeError(StatusCode::kUnknown, "Found multiple primary mgmtds");
    }
    table.push_back({"NodeId", std::to_string(nodes[0].app.nodeId)});
    addrs = nodes[0].extractAddresses(service);
  } else {
    XLOGF(FATAL, "Should not reach here");
  }
  co_return addrs;
}

CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                          const argparse::ArgumentParser &parser,
                                          const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());
  Dispatcher::OutputTable table;

  auto nodeId = parser.present<uint32_t>("-n");
  auto clientId = parser.present<String>("-c");
  auto addr = parser.present<String>("-a");
  auto primary = parser.get<bool>("--primary");
  auto service = parser.present<String>("-s").value_or("");
  auto methodName = parser.present<String>("-m").value_or("");
  auto methodId = parser.present<int32_t>("-i").value_or(-1);
  auto inputJson = parser.present<String>("-j").value_or("");
  auto inputFile = parser.present<String>("-f").value_or("");
  auto outputFile = parser.present<String>("-o").value_or("");
  auto isListMethods = parser.get<bool>("--list-methods");

  ENSURE_USAGE(nodeId.has_value() + clientId.has_value() + addr.has_value() + primary + isListMethods == 1,
               "must and can only specify one of -n, -c, -a, --primary, and --list-methods");

#define DISPATCH(service, function, ...)                                                       \
  if (service == MgmtdBase::kServiceName) {                                                    \
    CO_RETURN_ON_ERROR(function<MgmtdBase>(__VA_ARGS__));                                      \
  } else if (service == CoreBase::kServiceName) {                                              \
    CO_RETURN_ON_ERROR(function<CoreBase>(__VA_ARGS__));                                       \
  } else if (service == StorageBase::kServiceName) {                                           \
    CO_RETURN_ON_ERROR(function<StorageBase>(__VA_ARGS__));                                    \
  } else if (service == MetaBase::kServiceName) {                                              \
    CO_RETURN_ON_ERROR(function<MetaBase>(__VA_ARGS__));                                       \
  } else {                                                                                     \
    co_return makeError(StatusCode::kInvalidArg, fmt::format("Unknown service: {}", service)); \
  }

  if (isListMethods) {
    DISPATCH(service, listMethods, table);
  } else {
    auto addrsRes = co_await getAddrs(table, env, service, nodeId, clientId, addr, primary);
    CO_RETURN_ON_ERROR(addrsRes);
    const auto &addrs = *addrsRes;

    auto inputRes = loadInput(inputJson, inputFile);
    CO_RETURN_ON_ERROR(inputRes);

    RemoteCaller caller;
    DISPATCH(service, getRemoteCaller, caller, methodName, methodId);

    table.push_back({"Method", fmt::format("{}::{}({})", service, methodName, methodId)});

    for (auto addr : addrs) {
      auto ctx = env.clientGetter()->serdeCtx(addr);
      auto outputRes = co_await caller(ctx, *inputRes, true);

      if (outputRes.hasError()) {
        table.push_back({"Addr", addr.toString(), outputRes.error().describe()});
      } else {
        table.push_back({"Addr", addr.toString(), "OK"});
        if (!outputFile.empty()) {
          auto res = storeToFile(outputFile, *outputRes);
          CO_RETURN_ON_ERROR(res);
          table.push_back({"OutputFile", outputFile});
        } else {
          fmt::print("{}\n", *outputRes);
        }
        break;
      }
    }
  }

  co_return table;
}
}  // namespace
CoTryTask<void> registerRemoteCallHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handle);
}
}  // namespace hf3fs::client::cli
