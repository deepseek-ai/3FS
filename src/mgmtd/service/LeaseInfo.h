#pragma once

#include "fbs/mgmtd/MgmtdLeaseInfo.h"

namespace hf3fs::mgmtd {
struct LeaseInfo {
  std::optional<flat::MgmtdLeaseInfo> lease;
  bool bootstrapping = false;
};
}  // namespace hf3fs::mgmtd
