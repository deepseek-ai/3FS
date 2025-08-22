#include "AdminUserCtrl.h"

#include <fmt/chrono.h>
#include <string>

#include "AdminEnv.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/ArgParse.h"
#include "common/utils/OptionalUtils.h"
#include "common/utils/Result.h"
#include "common/utils/StringUtils.h"
#include "common/utils/Transform.h"
#include "common/utils/Uuid.h"
#include "core/user/UserStore.h"
#include "core/user/UserToken.h"
#include "fdb/FDBRetryStrategy.h"

namespace hf3fs::client::cli {
namespace {
using flat::Gid;
using flat::Uid;

kv::FDBRetryStrategy getRetryStrategy() { return kv::FDBRetryStrategy({1_s, 10, false}); }

CoTryTask<void> ensureAdmin([[maybe_unused]] kv::IReadOnlyTransaction &txn,
                            [[maybe_unused]] core::UserStore &store,
                            [[maybe_unused]] std::string_view token,
                            [[maybe_unused]] std::optional<flat::Uid> self = std::nullopt) {
  auto uid = core::decodeUidFromUserToken(token);
  if (UNLIKELY(uid.hasError())) {
    co_return makeError(StatusCode::kAuthenticationFail, "Token decode failed");
  }
  auto user = co_await store.getUser(txn, *uid);
  CO_RETURN_ON_ERROR(user);
  if (!user->has_value()) {
    co_return makeError(StatusCode::kAuthenticationFail, fmt::format("Uid {} not found", uid->toUnderType()));
  }
  if (auto res = user->value().validateToken(token, UtcClock::now()); res.hasError()) {
    XLOGF(ERR, "Token validate failed: {}, uid {}", res.error(), *uid);
    co_return makeError(StatusCode::kAuthenticationFail, "Token validate failed");
  }
  if (*uid != 0 && !user->value().admin && (!self || *self != *uid)) {
    co_return makeError(StatusCode::kAuthenticationFail, "Not Admin");
  }
  co_return Void{};
}

void printUserAttr(Dispatcher::OutputTable &table, flat::Uid uid, const flat::UserAttr &attr) {
  table.push_back({"Uid", std::to_string(uid)});
  table.push_back({"Name", attr.name});
  table.push_back({"Token", fmt::format("{}(Expired at N/A)", attr.token)});
  for (auto it = attr.tokens.rbegin(); it != attr.tokens.rend(); ++it) {
    const auto &sa = *it;
    table.push_back({"Token", fmt::format("{}(Expired at {})", sa.token, sa.endTime.YmdHMS())});
  }
  table.push_back({"IsRootUser", attr.root ? "true" : "false"});
  table.push_back({"IsAdmin", attr.admin ? "true" : "false"});
  table.push_back({"Gid", std::to_string(attr.gid)});
  auto groups = fmt::format("{}", fmt::join(attr.groups, ", "));
  table.push_back({"SupplementaryGids", groups});
}

struct UserAddHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-add");
    cmd.add_argument("uid").scan<'u', uint32_t>();
    cmd.add_argument("name");
    cmd.add_argument("--root").help("is root or not").default_value(false).implicit_value(true);
    cmd.add_argument("--admin").help("is admin or not").default_value(false).implicit_value(true);
    cmd.add_argument("--groups").help("group ids separated by comma (',')");
    cmd.add_argument("--token");
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    auto uid = Uid(parser.get<uint32_t>("uid"));
    auto name = parser.get<std::string>("name");
    auto isRootUser = parser.get<bool>("--root");
    auto isAdmin = parser.get<bool>("--admin");
    auto token = parser.present<String>("--token").value_or("");
    auto gids = splitAndTransform(parser.present<String>("--groups").value_or(""), boost::is_any_of(","), [](auto s) {
      return flat::Gid(folly::to<uint32_t>(s));
    });

    core::UserStore store;
    auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<flat::UserAttr> {
      auto userList = co_await store.listUsers(txn);
      CO_RETURN_ON_ERROR(userList);
      if (userList->empty()) {
        if (!isAdmin) {
          co_return makeError(StatusCode::kInvalidArg, "The first user must be admin");
        }
      } else {
        CO_RETURN_ON_ERROR(co_await ensureAdmin(txn, store, env.userInfo.token));
      }
      co_return co_await store.addUser(txn, uid, name, gids, isRootUser, isAdmin, token);
    };
    auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                         .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
    CO_RETURN_ON_ERROR(commitRes);
    printUserAttr(output, uid, *commitRes);
    co_return output;
  }
};

struct UserRemoveHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-remove");
    cmd.add_argument("uid").scan<'u', uint32_t>();
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    auto uid = Uid(parser.get<uint32_t>("uid"));

    core::UserStore store;
    auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<void> {
      CO_RETURN_ON_ERROR(co_await ensureAdmin(txn, store, env.userInfo.token));
      co_return co_await store.removeUser(txn, uid);
    };
    auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                         .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
    CO_RETURN_ON_ERROR(commitRes);

    co_return output;
  }
};

struct UserSetHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-set");
    cmd.add_argument("uid").scan<'u', uint32_t>();
    cmd.add_argument("--name").help("name of the new user");
    cmd.add_argument("--root").scan<'i', int32_t>().help("1: set; 0: unset");
    cmd.add_argument("--admin").scan<'i', int32_t>().help("1: set; 0: unset");
    cmd.add_argument("--groups").help("group ids separated by comma (',')");
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    auto uid = Uid(parser.get<uint32_t>("uid"));
    auto name = parser.present("--name");
    auto isRootValue = parser.present<int32_t>("--root");
    auto isAdminValue = parser.present<int32_t>("--admin");
    auto groups = splitAndTransform(parser.present<String>("--groups").value_or(""), boost::is_any_of(","), [](auto s) {
      return flat::Gid(folly::to<uint32_t>(s));
    });

    if (isRootValue) {
      ENSURE_USAGE(*isRootValue == 0 || *isRootValue == 1, "-r should be 0 or 1");
    }
    if (isAdminValue) {
      ENSURE_USAGE(*isAdminValue == 0 || *isAdminValue == 1, "--admin should be 0 or 1");
    }

    auto toBool = [](auto v) -> bool { return v; };

    std::optional<bool> isRoot = optionalMap(isRootValue, toBool);
    std::optional<bool> isAdmin = optionalMap(isAdminValue, toBool);

    core::UserStore store;
    auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<meta::UserAttr> {
      CO_RETURN_ON_ERROR(co_await ensureAdmin(txn, store, env.userInfo.token));
      co_return co_await store.setUserAttr(txn,
                                           uid,
                                           name ? std::string_view(*name) : std::string_view{},
                                           groups.empty() ? nullptr : &groups,
                                           isRoot,
                                           isAdmin);
    };
    auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                         .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
    CO_RETURN_ON_ERROR(commitRes);
    printUserAttr(output, uid, *commitRes);

    co_return output;
  }
};

struct UserSetTokenHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-set-token");
    cmd.add_argument("uid").scan<'u', uint32_t>();
    cmd.add_argument("--new").help("generate new token").default_value(false).implicit_value(true);
    cmd.add_argument("--token");
    cmd.add_argument("--invalidate-existed-tokens").default_value(false).implicit_value(true);
    cmd.add_argument("--max-active-tokens").scan<'u', uint32_t>().default_value(5U);
    cmd.add_argument("--last-token-lifetime-days").scan<'u', uint32_t>().default_value(7U);
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    auto uid = Uid(parser.get<uint32_t>("uid"));
    auto token = parser.present<String>("--token").value_or("");
    auto generateNewToken = parser.get<bool>("--new");
    auto invalidateExistedToken = parser.get<bool>("--invalidate-existed-tokens");
    auto maxActiveTokens = parser.get<uint32_t>("--max-active-tokens");
    auto tokenLifetimeExtendLength = Duration(std::chrono::days(parser.get<uint32_t>("--last-token-lifetime-days")));

    ENSURE_USAGE(!token.empty() + generateNewToken == 1, "must set one of token and generate-token");

    core::UserStore store;
    auto handler = [&](kv::IReadWriteTransaction &txn) -> CoTryTask<meta::UserAttr> {
      CO_RETURN_ON_ERROR(co_await ensureAdmin(txn, store, env.userInfo.token));
      co_return co_await store
          .setUserToken(txn, uid, token, invalidateExistedToken, maxActiveTokens, tokenLifetimeExtendLength);
    };
    auto commitRes = co_await kv::WithTransaction(getRetryStrategy())
                         .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
    CO_RETURN_ON_ERROR(commitRes);
    printUserAttr(output, uid, *commitRes);

    co_return output;
  }
};

struct UserStatHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-stat");
    cmd.add_argument("uid").scan<'u', uint32_t>();
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    auto uid = Uid(parser.get<uint32_t>("uid"));

    core::UserStore store;
    auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<meta::UserAttr> {
      CO_RETURN_ON_ERROR(co_await ensureAdmin(txn, store, env.userInfo.token, uid));
      auto fetchResult = co_await store.getUser(txn, uid);
      CO_RETURN_ON_ERROR(fetchResult);
      if (!fetchResult->has_value()) {
        co_return makeError(MetaCode::kNotFound, fmt::format("Uid {} not found", uid.toUnderType()));
      }
      co_return **fetchResult;
    };
    auto res = co_await kv::WithTransaction(getRetryStrategy())
                   .run(env.kvEngineGetter()->createReadonlyTransaction(), std::move(handler));
    CO_RETURN_ON_ERROR(res);
    printUserAttr(output, uid, *res);
    co_return output;
  }
};

struct UserListHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-list");
    cmd.add_argument("--json").default_value(false).implicit_value(true);
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    core::UserStore store;
    auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<std::vector<flat::UserAttr>> {
      CO_RETURN_ON_ERROR(co_await ensureAdmin(txn, store, env.userInfo.token));
      co_return co_await store.listUsers(txn);
    };
    auto res = co_await kv::WithTransaction(getRetryStrategy())
                   .run(env.kvEngineGetter()->createReadonlyTransaction(), std::move(handler));
    CO_RETURN_ON_ERROR(res);
    if (parser.get<bool>("--json")) {
      struct UserAttrLite {
        SERDE_STRUCT_FIELD(name, std::string());
        SERDE_STRUCT_FIELD(gid, flat::Gid());
        SERDE_STRUCT_FIELD(groups, std::vector<flat::Gid>());
        SERDE_STRUCT_FIELD(uid, flat::Uid());
      };

      auto users = transformTo<std::vector>(std::span(res->begin(), res->end()), [](const flat::UserAttr &attr) {
        return UserAttrLite{
            .name = attr.name,
            .gid = attr.gid,
            .groups = attr.groups,
            .uid = attr.uid,
        };
      });
      output.push_back({serde::toJsonString(users, false, true)});
    } else {
      output.push_back({"Uid", "Name", "Token", "IsRoot", "IsAdmin", "Gids"});
      for (const auto &attr : *res) {
        std::vector<String> row;
        row.push_back(std::to_string(attr.uid));
        row.push_back(attr.name);
        row.push_back(attr.token);
        row.push_back(attr.root ? "true" : "false");
        row.push_back(attr.admin ? "true" : "false");
        row.push_back(fmt::format("{}", fmt::join(attr.groups, ", ")));
        output.push_back(std::move(row));
      }
    }
    co_return output;
  }
};

void printUserInfo(Dispatcher::OutputTable &output, const flat::UserInfo &userInfo) {
  output.push_back({"Uid", std::to_string(userInfo.uid)});
  output.push_back({"Gid", std::to_string(userInfo.gid)});

  auto groups = transformTo<std::vector>(std::span{userInfo.groups.begin(), userInfo.groups.size()},
                                         [](flat::Gid gid) { return gid.toUnderType(); });
  output.push_back({"Groups", fmt::format("[{}]", fmt::join(groups, ", "))});
  output.push_back({"Token", userInfo.token});
}

struct UserSwitchHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("user-switch");
    cmd.add_argument("-u", "--uid").help("-1 means geteuid").scan<'i', int64_t>();
    cmd.add_argument("-g", "--gid").help("-1 means getegid").scan<'i', int64_t>();
    cmd.add_argument("-t", "--token");
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &parser,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    auto u = parser.present<int64_t>("-u");
    auto g = parser.present<int64_t>("-g");
    auto t = parser.present<String>("-t");

    if (u) env.userInfo.uid = flat::Uid(*u == -1 ? geteuid() : *u);
    if (g) env.userInfo.gid = flat::Gid(*g == -1 ? getegid() : *g);
    if (t) env.userInfo.token = *t;

    printUserInfo(output, env.userInfo);
    co_return output;
  }
};

struct CurrentUserHandler {
  static auto getParser() {
    argparse::ArgumentParser cmd("current-user");
    return cmd;
  }

  static CoTryTask<Dispatcher::OutputTable> handle(IEnv &ienv,
                                                   const argparse::ArgumentParser &,
                                                   const Dispatcher::Args &args) {
    auto &env = dynamic_cast<AdminEnv &>(ienv);
    ENSURE_USAGE(args.empty());
    Dispatcher::OutputTable output;

    printUserInfo(output, env.userInfo);
    co_return output;
  }
};
}  // namespace

CoTryTask<void> registerAdminUserCtrlHandler(Dispatcher &dispatcher) {
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserAddHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserRemoveHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserSetHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserSetTokenHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserStatHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserListHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<UserSwitchHandler>());
  CO_RETURN_ON_ERROR(co_await dispatcher.registerHandler<CurrentUserHandler>());
  co_return Void{};
}
}  // namespace hf3fs::client::cli
