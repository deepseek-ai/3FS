/*
#include "common/monitor/TaosClient.h"

#include <chrono>
#include <folly/logging/xlog.h>
#include <mutex>
#include <taos.h>
#include <taoserror.h>

#include "common/utils/Size.h"

namespace hf3fs::monitor {

static std::once_flag initOnce;

Result<Void> TaosClient::init() {
  Result<Void> initResult = Void{};
  std::call_once(initOnce, [&] {
    int ret = taos_options(TSDB_OPTION_CONFIGDIR, config_.cfg_dir().c_str());
    if (ret != 0) {
      XLOGF(ERR, "failed to change taos config dir");
      initResult = makeError(StatusCode::kMonitorInitFailed);
    }

    auto jsonConfig = fmt::format(R"({{ "logDir": "{}", "rpcForceTcp": "1" }})", config_.log_dir());
    auto result = taos_set_config(jsonConfig.c_str());
    if (result.retCode != 0) {
      XLOGF(ERR, "failed to set taos config, reason: {}", result.retMsg);
      initResult = makeError(StatusCode::kMonitorInitFailed);
    }
  });
  RETURN_ON_ERROR(initResult);

  taos_options(TSDB_OPTION_TIMEZONE, "GMT-8");
  taos_ = taos_connect(config_.host().c_str(), config_.user().c_str(), config_.passwd().c_str(), "", 0);
  if (taos_ == nullptr) {
    XLOGF(ERR, "failed to connect to db, reason: {}", taos_errstr(taos_));
    return makeError(StatusCode::kMonitorInitFailed);
  }

  std::string serverInfo = taos_get_server_info(taos_);
  std::string clientInfo = taos_get_client_info();
  if (serverInfo != clientInfo) {
    XLOGF(ERR, "failed to connect to db, version mismatch: {} != {}", serverInfo, clientInfo);
    return makeError(StatusCode::kMonitorInitFailed);
  }

  return query(fmt::format("use {};", config_.db()));
}

void TaosClient::stop() {
  if (taos_ != nullptr) {
    taos_close(std::exchange(taos_, nullptr));
  }
}

void TaosClient::cleanUp() { taos_cleanup(); }

Result<Void> TaosClient::query(const std::string &sql) {
  TAOS_RES *result = taos_query(taos_, sql.c_str());
  if (taos_errno(result) != TSDB_CODE_SUCCESS) {
    XLOGF(ERR, "failed to run SQL {}, error: {}", sql, taos_errstr(result));
    taos_free_result(result);
    return makeError(StatusCode::kMonitorQueryFailed);
  }
  taos_free_result(result);
  return Void{};
}

Result<Void> TaosClient::commit(const std::vector<Sample> &samples) {
  constexpr uint32_t kMaxWriteSize = 256_KB;

  std::string buffer;
  std::vector<uintptr_t> offsets;
  offsets.reserve(samples.size());

  for (auto &sample : samples) {
    std::string tag_str;
    sample.tags.traverseTags([&tag_str](auto &key, auto &val) { tag_str += "," + key + "=" + val; });

    offsets.push_back(buffer.size());
    if (sample.isNumber()) {
      fmt::format_to(std::back_inserter(buffer),
                     "{}{} val={}u {}",
                     sample.name,
                     tag_str,
                     sample.number(),
                     sample.timestamp.toMicroseconds());
    } else if (sample.isDistribution()) {
      auto &dist = sample.dist();
      fmt::format_to(std::back_inserter(buffer),
                     "{}{} count={},mean={},min={},max={},p50={},p90={},p95={},p99={} {}",
                     sample.name,
                     tag_str,
                     dist.cnt,
                     dist.mean(),
                     dist.min,
                     dist.max,
                     dist.p50,
                     dist.p90,
                     dist.p95,
                     dist.p99,
                     sample.timestamp.toMicroseconds());
    } else {
      XLOGF(ERR, "invalid sample type to TDengine");
      return makeError(StatusCode::kMonitorWriteFailed);
    }
    buffer.push_back('\0');

    if (buffer.size() >= kMaxWriteSize || &sample == &samples.back()) {
      RETURN_ON_ERROR(insert(buffer, offsets));
      buffer.clear();
      offsets.clear();
    }
  }
  return Void{};
}

Result<Void> TaosClient::insert(std::string &buffer, std::vector<uintptr_t> &offsets) {
  for (auto &offset : offsets) {
    offset += reinterpret_cast<uintptr_t>(buffer.data());
  }
  auto result = taos_schemaless_insert(taos_,
                                       reinterpret_cast<char **>(offsets.data()),
                                       offsets.size(),
                                       TSDB_SML_LINE_PROTOCOL,
                                       TSDB_SML_TIMESTAMP_MICRO_SECONDS);
  if (taos_errno(result) != TSDB_CODE_SUCCESS) {
    XLOGF(ERR, "failed to write to TDengine, error: {}", taos_errstr(result));
    taos_free_result(result);
    return makeError(StatusCode::kMonitorWriteFailed);
  }
  taos_free_result(result);
  return Void{};
}

}  // namespace hf3fs::monitor
*/
