#pragma once

#include "MgmtdClientFetcher.h"

namespace hf3fs::core::launcher {
struct ServerMgmtdClientFetcher : public MgmtdClientFetcher {
  using MgmtdClientFetcher::MgmtdClientFetcher;
  Result<Void> completeAppInfo(flat::AppInfo &appInfo) final;
};
}  // namespace hf3fs::core::launcher
