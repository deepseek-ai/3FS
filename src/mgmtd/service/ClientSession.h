#pragma once

#include "WithTimestamp.h"
#include "fbs/mgmtd/ClientSession.h"

namespace hf3fs::mgmtd {
class ClientSession : public WithTimestamp<flat::ClientSession> {
 public:
  using Base = WithTimestamp<flat::ClientSession>;
  using Base::Base;

  flat::ClientSessionVersion clientSessionVersion{0};
};
}  // namespace hf3fs::mgmtd
