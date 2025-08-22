#pragma once

#include <fmt/core.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/hash/Hash.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/coro/ServerSocket.h>
#include <folly/io/coro/Transport.h>
#include <folly/logging/xlog.h>
#include <folly/net/NetworkSocket.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/utils/Address.h"
#include "common/utils/UtcTime.h"

namespace hf3fs::net {

using namespace std::chrono_literals;

using EventBase = folly::EventBase;

using IOBufQueue = folly::IOBufQueue;
using IOBuf = folly::IOBuf;
using IOBufPtr = std::unique_ptr<folly::IOBuf>;

using NamedThreadFactory = folly::NamedThreadFactory;
using CPUThreadPoolExecutor = folly::CPUThreadPoolExecutor;

template <size_t N>
struct MethodName {
  constexpr MethodName(const char (&str)[N]) { std::copy_n(str, N, string); }
  std::string str() const { return {string, N - 1}; }
  constexpr std::string_view string_view() const { return {string, N - 1}; }
  char string[N];
};

}  // namespace hf3fs::net
