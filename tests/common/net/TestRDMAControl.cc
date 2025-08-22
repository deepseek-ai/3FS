#include "common/net/Client.h"
#include "common/net/RDMAControl.h"
#include "common/net/RequestOptions.h"
#include "common/net/Server.h"
#include "common/serde/ClientContext.h"
#include "tests/GtestHelpers.h"
#include "tests/common/net/ib/SetupIB.h"

namespace hf3fs::net::test {
namespace {

struct RDMAControlService : serde::ServiceWrapper<RDMAControlService, RDMAControl> {
  CoTryTask<RDMATransmissionRsp> apply(serde::CallContext &ctx, const RDMATransmissionReq &req) {
    auto &tr = ctx.transport();
    if (!tr->isRDMA()) {
      co_return makeError(StatusCode::kInvalidArg);
    }
    serde::ClientContext clientCtx(tr);
    auto result = co_await RDMAControl<>::apply(clientCtx, req);
    co_return result;
  }
};

TEST(TestRDMAControl, Normal) {
  SetupIB::SetUpTestSuite();

  net::Server::Config serverConfig;
  net::Server server_{serverConfig};
  ASSERT_OK(server_.setup());
  ASSERT_OK(server_.start());
  ASSERT_OK(server_.addSerdeService(std::make_unique<RDMAControlService>()));

  net::Client::Config clientConfig;
  net::Client client_{clientConfig};
  ASSERT_OK(client_.start());
  auto ctx = client_.serdeCtx(server_.groups().front()->addressList().front());

  folly::coro::blockingWait(folly::coro::co_invoke([&]() -> CoTask<void> {
    {
      RDMATransmissionReq req;
      auto result = co_await RDMAControl<>::apply(ctx, req);
      if (result.hasError()) {
        std::cout << result.error().describe() << std::endl;
      }
      CO_ASSERT_OK(result);
    }
  }));
}

}  // namespace
}  // namespace hf3fs::net::test
