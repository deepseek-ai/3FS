#include "client/cli/admin/DumpSession.h"

#include <fmt/core.h>
#include <folly/Likely.h>
#include <string>

#include "AdminEnv.h"
#include "analytics/SerdeObjectWriter.h"
#include "client/cli/common/Dispatcher.h"
#include "client/cli/common/Utils.h"
#include "common/kv/ITransaction.h"
#include "common/kv/WithTransaction.h"
#include "common/utils/Result.h"
#include "fdb/FDBRetryStrategy.h"
#include "meta/components/SessionManager.h"
#include "meta/store/FileSession.h"

namespace hf3fs::client::cli {
namespace {
kv::FDBRetryStrategy getRetryStrategy() { return kv::FDBRetryStrategy({1_s, 10, false}); }

auto getParser() {
  argparse::ArgumentParser parser("dump-session");
  parser.add_argument("--output").help("output parquet filename");
  return parser;
}

CoTryTask<Dispatcher::OutputTable> handleDumpSession(IEnv &ienv,
                                                     const argparse::ArgumentParser &parser,
                                                     const Dispatcher::Args &args) {
  auto &env = dynamic_cast<AdminEnv &>(ienv);
  ENSURE_USAGE(args.empty());

  auto output = parser.present<std::string>("--output");
  ENSURE_USAGE(!output->empty(), "must specify output filename");
  ENSURE_USAGE(!boost::filesystem::exists(*output), "output filename already exists");

  auto openRes = analytics::SerdeObjectWriter<meta::server::FileSession>::open(*output);
  if (!openRes) {
    XLOGF(ERR, "Failed to open writer {}", *output);
    co_return makeError(StatusCode::kOSError, "failed to create writer");
  }
  auto &writer = *openRes;
  size_t cnt = 0;
  for (size_t shard = 0; shard < meta::server::FileSession::kShard; shard++) {
    std::optional<meta::server::FileSession> prev;
    while (true) {
      auto handler = [&](kv::IReadOnlyTransaction &txn) -> CoTryTask<std::vector<meta::server::FileSession>> {
        co_return co_await meta::server::FileSession::scan(txn, shard, prev);
      };
      auto sessions = co_await kv::WithTransaction(getRetryStrategy())
                          .run(env.kvEngineGetter()->createReadWriteTransaction(), std::move(handler));
      CO_RETURN_ON_ERROR(sessions);
      if (sessions->empty()) {
        break;
      }

      prev = sessions->back();
      for (auto &s : *sessions) {
        writer << s;
        cnt++;
        if (!writer) {
          co_return makeError(StatusCode::kUnknown, "serde writer error");
        }
      }
      if (cnt > 1 << 20) {
        writer.endRowGroup();
        if (!writer) {
          co_return makeError(StatusCode::kUnknown, "serde writer error");
        }
      }
    }
  }

  writer.endRowGroup();
  if (!writer) {
    co_return makeError(StatusCode::kUnknown, "serde writer error");
  }

  co_return Dispatcher::OutputTable{};
}

}  // namespace

CoTryTask<void> registerDumpSessionHandler(Dispatcher &dispatcher) {
  co_return co_await dispatcher.registerHandler(getParser, handleDumpSession);
}

}  // namespace hf3fs::client::cli
