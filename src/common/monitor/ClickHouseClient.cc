#include "common/monitor/ClickHouseClient.h"

#include <clickhouse/client.h>
#include <folly/sorted_vector_types.h>

namespace hf3fs::monitor {

ClickHouseClient::ClickHouseClient(const Config &config)
    : config_(config) {}

ClickHouseClient::~ClickHouseClient() = default;

Result<Void> ClickHouseClient::init() {
  Result<Void> initResult = Void{};
  try {
    client_ = std::make_unique<clickhouse::Client>(clickhouse::ClientOptions()
                                                       .SetHost(config_.host())
                                                       .SetPort(std::stoi(config_.port()))
                                                       .SetUser(config_.user())
                                                       .SetPassword(config_.passwd())
                                                       .SetDefaultDatabase(config_.db())
                                                       .SetPingBeforeQuery(false)
                                                       .SetCompressionMethod(clickhouse::CompressionMethod::LZ4));
  } catch (const std::exception &e) {
    XLOGF(ERR, "Failed to initialize ClickHouse: {}", e.what());
    initResult = makeError(StatusCode::kMonitorInitFailed);
  }

  return initResult;
}

static folly::sorted_vector_map<std::string, std::shared_ptr<clickhouse::ColumnString>> createTagColumns(
    const std::vector<Sample> &samples) {
  folly::sorted_vector_set<std::string> tag_key_set;
  for (auto &sample : samples) {
    for (auto &[key, value] : sample.tags) {
      if (key.empty()) {
        XLOGF(ERR, "Empty tag: {}", sample.name);
      } else {
        tag_key_set.insert(key);
      }
    }
  }

  folly::sorted_vector_map<std::string, std::shared_ptr<clickhouse::ColumnString>> tag_columns;
  for (auto &key : tag_key_set) {
    tag_columns.insert(std::make_pair(key, std::make_shared<clickhouse::ColumnString>()));
  }

  for (auto &sample : samples) {
    for (auto &key : tag_key_set) {
      auto it = sample.tags.find(key);
      if (it != sample.tags.end()) {
        tag_columns[key]->Append(it->second);
      } else {
        tag_columns[key]->Append("");
      }
    }
  }

  return tag_columns;
}

Result<Void> ClickHouseClient::commit(const std::vector<Sample> &samples) {
  std::vector<Sample> counters, distributions;
  for (auto &sample : samples) {
    if (sample.isNumber()) {
      counters.push_back(sample);
    } else {
      distributions.push_back(sample);
    }
  }

  if (!counters.empty()) {
    auto timestamp_col = std::make_shared<clickhouse::ColumnDateTime>();
    auto metric_name_col = std::make_shared<clickhouse::ColumnString>();
    auto val_col = std::make_shared<clickhouse::ColumnInt64>();
    for (auto &sample : counters) {
      timestamp_col->Append(hf3fs::UtcClock::to_time_t(sample.timestamp));
      metric_name_col->Append(sample.name);
      val_col->Append(sample.number());
    }

    clickhouse::Block block;
    block.AppendColumn("TIMESTAMP", timestamp_col);
    block.AppendColumn("metricName", metric_name_col);
    block.AppendColumn("val", val_col);

    auto tag_cols = createTagColumns(counters);
    for (auto &[name, col] : tag_cols) {
      block.AppendColumn(name, col);
    }

    try {
      if (UNLIKELY(errorHappened_)) {
        client_->ResetConnection();
        errorHappened_ = false;
      }
      client_->Insert("counters", block);
    } catch (const std::exception &e) {
      XLOGF(ERR, "ClickHouse insert counters failed: {}", e.what());
      errorHappened_ = true;
    }
  }

  if (!distributions.empty()) {
    auto timestamp_col = std::make_shared<clickhouse::ColumnDateTime>();
    auto metric_name_col = std::make_shared<clickhouse::ColumnString>();
    auto count_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto mean_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto min_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto max_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto p50_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto p90_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto p95_col = std::make_shared<clickhouse::ColumnFloat64>();
    auto p99_col = std::make_shared<clickhouse::ColumnFloat64>();
    for (auto &sample : distributions) {
      auto &dist = sample.dist();
      timestamp_col->Append(hf3fs::UtcClock::to_time_t(sample.timestamp));
      metric_name_col->Append(sample.name);
      count_col->Append(dist.cnt);
      mean_col->Append(dist.mean());
      min_col->Append(dist.min);
      max_col->Append(dist.max);
      p50_col->Append(dist.p50);
      p90_col->Append(dist.p90);
      p95_col->Append(dist.p95);
      p99_col->Append(dist.p99);
    }
    clickhouse::Block block;
    block.AppendColumn("TIMESTAMP", timestamp_col);
    block.AppendColumn("metricName", metric_name_col);
    block.AppendColumn("count", count_col);
    block.AppendColumn("mean", mean_col);
    block.AppendColumn("min", min_col);
    block.AppendColumn("max", max_col);
    block.AppendColumn("p50", p50_col);
    block.AppendColumn("p90", p90_col);
    block.AppendColumn("p95", p95_col);
    block.AppendColumn("p99", p99_col);

    auto tag_cols = createTagColumns(distributions);
    for (auto &[name, col] : tag_cols) {
      block.AppendColumn(name, col);
    }

    try {
      if (UNLIKELY(errorHappened_)) {
        client_->ResetConnection();
        errorHappened_ = false;
      }
      client_->Insert("distributions", block);
    } catch (const std::exception &e) {
      XLOGF(ERR, "ClickHouse insert distributions failed: {}", e.what());
      errorHappened_ = true;
    }
  }

  return Void{};
}

}  // namespace hf3fs::monitor
