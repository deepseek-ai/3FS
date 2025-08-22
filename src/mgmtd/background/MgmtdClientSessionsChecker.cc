#include "MgmtdClientSessionsChecker.h"

#include "core/utils/ServiceOperation.h"
#include "core/utils/runOp.h"
#include "mgmtd/service/MgmtdState.h"
#include "mgmtd/service/helpers.h"

namespace hf3fs::mgmtd {
namespace {
struct Op : core::ServiceOperationWithMetric<"MgmtdService", "CheckClientSessions", "bg"> {
  String toStringImpl() const final { return "CheckClientSessions"; }
  auto handle(MgmtdState &state) -> CoTryTask<void> {
    auto timeout = state.config_.client_session_timeout().asUs();
    auto now = SteadyClock::now();

    std::vector<String> timeoutedClients;
    {
      auto clientSessionMap = co_await state.clientSessionMap_.coSharedLock();
      for (const auto &[clientId, sessionData] : *clientSessionMap) {
        if (sessionData.ts() + timeout < now) timeoutedClients.push_back(clientId);
      }
    }
    if (!timeoutedClients.empty()) {
      LOG_OP_INFO(*this, "remove timeouted client sessions [{}]", fmt::join(timeoutedClients, ","));

      auto clientSessionMap = co_await state.clientSessionMap_.coLock();
      for (const auto &clientId : timeoutedClients) {
        auto it = clientSessionMap->find(clientId);
        if (it->second.ts() + timeout < now) {
          clientSessionMap->erase(it);
        }
      }
    }
    co_return Void{};
  }
};
}  // namespace

MgmtdClientSessionsChecker::MgmtdClientSessionsChecker(MgmtdState &state)
    : state_(state) {}

CoTask<void> MgmtdClientSessionsChecker::check() {
  Op op;
  auto handler = [&]() -> CoTryTask<void> { CO_INVOKE_OP_INFO(op, "background", state_); };

  auto res = co_await doAsPrimary(state_, std::move(handler));
  if (res.hasError()) {
    if (res.error().code() == MgmtdCode::kNotPrimary)
      LOG_OP_INFO(op, "self is not primary, skip");
    else
      LOG_OP_ERR(op, "failed: {}", res.error());
  }
}

}  // namespace hf3fs::mgmtd
