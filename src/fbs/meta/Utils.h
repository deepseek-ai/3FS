#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <folly/Conv.h>
#include <folly/logging/xlog.h>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "common/app/NodeId.h"
#include "common/monitor/Recorder.h"
#include "common/monitor/Sample.h"
#include "common/serde/Serde.h"
#include "common/utils/Duration.h"
#include "common/utils/MurmurHash3.h"
#include "common/utils/Reflection.h"
#include "common/utils/Result.h"
#include "common/utils/SerDeser.h"
#include "common/utils/Status.h"
#include "common/utils/StatusCode.h"
#include "common/utils/TypeTraits.h"
#include "common/utils/Uuid.h"
#include "fbs/core/user/User.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/RoutingInfo.h"
#include "fmt/core.h"

namespace hf3fs::meta {

struct ErrorHandling {
  static bool success(const auto &result) { return !result.hasError() || success(result.error()); }

  // The operation was performed successfully, or an expected error code was returned
  static bool success(const Status &status) {
    auto code = status.code();
    switch (StatusCode::typeOf(code)) {
      case StatusCodeType::Common:
        switch (code) {
          case StatusCode::kOK:
          case StatusCode::kInvalidArg:
          case StatusCode::kAuthenticationFail:
            return true;
          default:
            return false;
        }
      case StatusCodeType::Meta:
        switch (code) {
          case MetaCode::kNotFound:
          case MetaCode::kNotEmpty:
          case MetaCode::kNotDirectory:
          case MetaCode::kTooManySymlinks:
          case MetaCode::kIsDirectory:
          case MetaCode::kExists:
          case MetaCode::kNoPermission:
          case MetaCode::kNotFile:
          case MetaCode::kInvalidFileLayout:
          case MetaCode::kMoreChunksToRemove:
          case MetaCode::kNameTooLong:
            return true;
          default:
            return (code >= MetaCode::kExpected && code < MetaCode::kRetryable);
        }
      default:
        return false;
    }
  }

  static bool retryable(const Status &status) {
    auto code = status.code();
    switch (StatusCode::typeOf(code)) {
      case StatusCodeType::Common:
        return false;
      case StatusCodeType::Meta:
        switch (code) {
          case MetaCode::kNotFound:
          case MetaCode::kNotEmpty:
          case MetaCode::kNotDirectory:
          case MetaCode::kTooManySymlinks:
          case MetaCode::kIsDirectory:
          case MetaCode::kExists:
          case MetaCode::kNoPermission:
          case MetaCode::kInconsistent:
          case MetaCode::kNotFile:
          case MetaCode::kBadFileSystem:
          case MetaCode::kInvalidFileLayout:
          case MetaCode::kFileHasHole:
          case MetaCode::kNameTooLong:
          case MetaCode::kRequestCanceled:
          case MetaCode::kFoundBug:
            return false;
          case MetaCode::kOTruncFailed:
          case MetaCode::kMoreChunksToRemove:
          case MetaCode::kBusy:
          case MetaCode::kInodeIdAllocFailed:
            return true;
          default:
            return code < MetaCode::kNotRetryable;
        }
      case StatusCodeType::StorageClient:
        switch (code) {
          case StorageClientCode::kChunkNotFound:
          case StorageClientCode::kChecksumMismatch:
          case StorageClientCode::kReadOnlyServer:
          case StorageClientCode::kFoundBug:
            return false;
          default:
            return true;
        }
      default:
        return code != RPCCode::kInvalidMethodID;
    }
  }

  static bool serverError(const Status &status) {
    switch (StatusCode::typeOf(status.code())) {
      case StatusCodeType::Transaction:
        return status.code() == TransactionCode::kNetworkError;
      case StatusCodeType::RPC:
        return true;
      default:
        return false;
    }
  }

  static bool needPruneSession(const Status &error) {
    auto code = error.code();
    switch (StatusCode::typeOf(code)) {
      case StatusCodeType::Transaction:
        return code == TransactionCode::kMaybeCommitted;
      case StatusCodeType::Meta:
        return code == MetaCode::kOTruncFailed;
      case StatusCodeType::RPC:
        switch (code) {
          case RPCCode::kSendFailed:
          case RPCCode::kConnectFailed:
          case RPCCode::kIBInitFailed:
          case RPCCode::kInvalidMethodID:
            return false;
          default:
            return true;
        }
      default:
        return false;
    }
  }
};

class OperationRecorder {
 public:
  OperationRecorder(std::string_view prefix)
      : total_(fmt::format("{}_total", prefix)),
        failed_(fmt::format("{}_failed", prefix)),
        running_(fmt::format("{}_running", prefix), false),
        code_(fmt::format("{}_code", prefix)),
        latency_(fmt::format("{}_latency", prefix)),
        retry_(fmt::format("{}_retry", prefix)),
        idempotent_(fmt::format("{}_idempotent", prefix)),
        duplicate_(fmt::format("{}_duplicate", prefix)) {}

  static OperationRecorder &server() {
    static OperationRecorder recorder("meta_server.op");
    return recorder;
  }

  static OperationRecorder &client() {
    static OperationRecorder recorder("meta_client.op");
    return recorder;
  }

  class Guard {
   public:
    Guard(OperationRecorder &recorder, std::string_view op, flat::Uid user)
        : recorder_(recorder),
          op_("instance", std::string(op)),
          user_("uid", folly::to<std::string>((int32_t)user.toUnderType())),
          begin_(RelativeTime::now()) {
      begin_ = RelativeTime::now();
      recorder_.running_.addSample(1, {{op_}});
    }

    ~Guard() {
      auto op = monitor::TagSet{op_};
      auto opUser = monitor::TagSet{{op_, user_}};
      auto latency = RelativeTime::now() - begin_;
      recorder_.total_.addSample(1, opUser);
      if (!succ_) {
        recorder_.failed_.addSample(1, opUser);
        recorder_.code_.addSample(1, {{"tag", folly::to<std::string>(code_.value_or(0))}, user_});
      } else {
        if (code_) {
          recorder_.code_.addSample(1, {{"tag", folly::to<std::string>(*code_)}, user_});
        }
      }
      recorder_.running_.addSample(-1, op);
      recorder_.latency_.addSample(latency, op);
      recorder_.retry_.addSample(retry_, op);
      if (duplicate_) {
        recorder_.duplicate_.addSample(1, op);
      }
    }

    void finish(const auto &result, bool duplicate = false) {
      duplicate_ = duplicate;
      succ_ = ErrorHandling::success(result);
      code_ = result.hasError() ? result.error().code() : 0;
    }

    void finish(const Status &status, bool duplicate = false) {
      duplicate_ = duplicate;
      succ_ = ErrorHandling::success(status) || status.code() == MetaCode::kRequestCanceled;
      code_ = status.code();
    }

    int &retry() { return retry_; }

   private:
    using Tag = std::pair<std::string, std::string>;
    OperationRecorder &recorder_;
    Tag op_;
    Tag user_;
    RelativeTime begin_;
    std::optional<status_code_t> code_;
    bool succ_ = false;
    int retry_ = 1;
    bool duplicate_ = false;
  };

  void addIdempotentCount() { idempotent_.addSample(1); }

 private:
  monitor::CountRecorder total_;
  monitor::CountRecorder failed_;
  monitor::CountRecorder running_;
  monitor::CountRecorder code_;
  monitor::LatencyRecorder latency_;
  monitor::DistributionRecorder retry_;
  monitor::CountRecorder idempotent_;
  monitor::CountRecorder duplicate_;
};

struct Weight : std::array<uint8_t, 16> {
  static Weight calculate(flat::NodeId node, InodeId inodeId) {
    // Note: don't change this
    auto key = Serializer::serRawArgs((uint64_t)node.toUnderType(), inodeId.u64());
    return hash(key.data(), key.size());
  }

  static flat::NodeId select(const std::vector<flat::NodeId> &nodes, InodeId inodeId) {
    return selectImpl(nodes, inodeId);
  }

  static Weight calculate(flat::NodeId node, Uuid clientId) {
    // todo: maybe should change to client host name?
    auto key = Serializer::serRawArgs((uint64_t)node.toUnderType(), clientId.data);
    return hash(key.data(), key.size());
  }

  static flat::NodeId select(const std::vector<flat::NodeId> &nodes, Uuid clientId) {
    return selectImpl(nodes, clientId);
  }

 private:
  static Weight hash(void *key, size_t len) {
    // NOTE: don't change this
    Weight w;
    MurmurHash3_x64_128(key, len, 0, &w);
    return w;
  }

  static flat::NodeId selectImpl(const std::vector<flat::NodeId> &nodes, auto &key) {
    if (nodes.empty()) {
      return flat::NodeId(0);  // flat::NodeId(0) is invalid
    }

    auto node = nodes.at(0);
    auto weight = calculate(node, key);
    for (size_t i = 1; i < nodes.size(); i++) {
      auto n = nodes.at(i);
      auto w = calculate(n, key);
      if (w > weight) {
        node = n;
        weight = w;
      }
    }
    return node;
  }
};
static_assert(sizeof(Weight) == 16);

struct RoutingInfoChecker {
  template <typename T>
  static constexpr bool hasInode() {
    return false;
  }

  template <typename T>
  requires serde::SerdeType<T>
  static constexpr bool hasInode() {
    if constexpr (std::is_same_v<Inode, T>) {
      return true;
    }
    bool inode = false;
    refl::Helper::iterate<T>([&](auto info) {
      using Item = std::remove_reference_t<decltype(std::declval<T>().*info.getter)>;
      inode |= hasInode<Item>();
    });
    return inode;
  }

  template <typename T>
  requires is_variant_v<T>
  static constexpr bool hasInode() { return variantHasInode<T>(); }

  template <typename T, size_t I = 0>
  static constexpr bool variantHasInode() {
    if constexpr (I < std::variant_size_v<T>) {
      if constexpr (hasInode<std::variant_alternative<I, T>>()) {
        return true;
      }
      return variantHasInode<T, I + 1>();
    }
    return false;
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  static constexpr bool hasInode() { return hasInode<typename T::value_type>(); }

  template <typename T>
  requires is_map_v<T>
  static constexpr bool hasInode() { return hasInode<typename T::key_type>() || hasInode<typename T::mapped_type>(); }

  template <typename T>
  requires is_optional_v<T>
  static constexpr bool hasInode() { return hasInode<typename T::value_type>(); }

  static bool checkRoutingInfo(const Inode &inode, const flat::RoutingInfo &routing) {
    if (inode.isFile()) {
      auto table = inode.asFile().layout.tableId;
      auto tableVer = inode.asFile().layout.tableVersion;
      switch (inode.asFile().layout.type()) {
        case Layout::Type::ChainRange:
          XLOGF_IF(DFATAL, (!table || !tableVer), "File {}, invalid layout", inode);
          if (!routing.getChainTable(table, tableVer)) {
            XLOGF(WARN, "File {}, chain table {} version {}, not found in RoutingInfo", inode.id, table, tableVer);
            return false;
          }
          break;
        case Layout::Type::ChainList:
          if (table && tableVer && !routing.getChainTable(table, tableVer)) {
            XLOGF(WARN, "File {}, chain table {} version {}, not found in RoutingInfo", inode.id, table, tableVer);
            return false;
          }
          break;
        case Layout::Type::Empty:
          break;
      }
    }
    return true;
  }

  template <typename T>
  static bool checkRoutingInfo(const T &, const flat::RoutingInfo &) {
    return true;
  }

  template <typename T>
  requires serde::SerdeType<T>
  static bool checkRoutingInfo(const T &t, const flat::RoutingInfo &routing) {
    if constexpr (!hasInode<T>()) {
      return true;
    }

    bool succ = true;
    refl::Helper::iterate<T>([&](auto info) { succ &= checkRoutingInfo(t.*info.getter, routing); });
    return succ;
  }

  template <typename T>
  requires is_variant_v<T>
  static bool checkRoutingInfo(const T &t, const flat::RoutingInfo &routing) {
    if constexpr (!hasInode<T>()) {
      return true;
    }
    return std::visit([&](const auto &v) { return checkRoutingInfo(v, routing); }, t);
  }

  template <typename T>
  requires is_vector_v<T> || is_set_v<T>
  static bool checkRoutingInfo(const T &t, const flat::RoutingInfo &routing) {
    if constexpr (!hasInode<T>()) {
      return true;
    }
    for (auto &i : t) {
      if (!checkRoutingInfo(i, routing)) {
        return false;
      }
    }
    return true;
  }

  template <typename T>
  requires is_map_v<T>
  static bool checkRoutingInfo(const T &t, const flat::RoutingInfo &routing) {
    if constexpr (!hasInode<T>()) {
      return true;
    }
    for (auto &[k, v] : t) {
      if (!checkRoutingInfo(k, routing) || !checkRoutingInfo(v, routing)) {
        return false;
      }
    }
    return true;
  }

  template <typename T>
  requires is_optional_v<T>
  static bool checkRoutingInfo(const T &t, const flat::RoutingInfo &routing) {
    if constexpr (!hasInode<T>()) {
      return true;
    }
    return t ? checkRoutingInfo(*t, routing) : true;
  }
};

static_assert(RoutingInfoChecker::hasInode<Inode>());
static_assert(RoutingInfoChecker::hasInode<std::vector<Inode>>());
static_assert(RoutingInfoChecker::hasInode<std::set<Inode>>());
static_assert(RoutingInfoChecker::hasInode<std::optional<Inode>>());
static_assert(RoutingInfoChecker::hasInode<std::map<Inode, int>>());
static_assert(RoutingInfoChecker::hasInode<std::map<int, Inode>>());
static_assert(RoutingInfoChecker::hasInode<OpenRsp>());
static_assert(RoutingInfoChecker::hasInode<StatRsp>());
static_assert(RoutingInfoChecker::hasInode<SyncRsp>());
static_assert(RoutingInfoChecker::hasInode<CloseRsp>());
static_assert(RoutingInfoChecker::hasInode<BatchStatRsp>());
}  // namespace hf3fs::meta
