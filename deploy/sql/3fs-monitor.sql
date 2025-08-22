CREATE DATABASE IF NOT EXISTS 3fs;

CREATE TABLE IF NOT EXISTS 3fs.counters (
  `TIMESTAMP` DateTime CODEC(DoubleDelta),
  `metricName` LowCardinality(String) CODEC(ZSTD(1)),
  `host` LowCardinality(String) CODEC(ZSTD(1)),
  `tag` LowCardinality(String) CODEC(ZSTD(1)),
  `val` Int64 CODEC(ZSTD(1)),
  `mount_name` LowCardinality(String) CODEC(ZSTD(1)),
  `instance` String CODEC(ZSTD(1)),
  `io` LowCardinality(String) CODEC(ZSTD(1)),
  `uid` LowCardinality(String) CODEC(ZSTD(1)),
  `pod` String CODEC(ZSTD(1)),
  `thread` LowCardinality(String) CODEC(ZSTD(1)),
  `statusCode` LowCardinality(String) CODEC(ZSTD(1))
)
ENGINE = MergeTree
PRIMARY KEY (metricName, host, pod, instance, TIMESTAMP)
PARTITION BY toDate(TIMESTAMP)
ORDER BY (metricName, host, pod, instance, TIMESTAMP)
TTL TIMESTAMP + toIntervalMonth(1)
SETTINGS index_granularity = 8192;

CREATE TABLE IF NOT EXISTS 3fs.distributions (
  `TIMESTAMP` DateTime CODEC(DoubleDelta),
  `metricName` LowCardinality(String) CODEC(ZSTD(1)),
  `host` LowCardinality(String) CODEC(ZSTD(1)),
  `tag` LowCardinality(String) CODEC(ZSTD(1)),
  `count` Float64 CODEC(ZSTD(1)),
  `mean` Float64 CODEC(ZSTD(1)),
  `min` Float64 CODEC(ZSTD(1)),
  `max` Float64 CODEC(ZSTD(1)),
  `p50` Float64 CODEC(ZSTD(1)),
  `p90` Float64 CODEC(ZSTD(1)),
  `p95` Float64 CODEC(ZSTD(1)),
  `p99` Float64 CODEC(ZSTD(1)),
  `mount_name` LowCardinality(String) CODEC(ZSTD(1)),
  `instance` String CODEC(ZSTD(1)),
  `io` LowCardinality(String) CODEC(ZSTD(1)),
  `uid` LowCardinality(String) CODEC(ZSTD(1)),
  `method` LowCardinality(String) CODEC(ZSTD(1)),
  `pod` String CODEC(ZSTD(1)),
  `thread` LowCardinality(String) CODEC(ZSTD(1)),
  `statusCode` LowCardinality(String) CODEC(ZSTD(1))
)
ENGINE = MergeTree
PRIMARY KEY (metricName, host, pod, instance, TIMESTAMP)
PARTITION BY toDate(TIMESTAMP)
ORDER BY (metricName, host, pod, instance, TIMESTAMP)
TTL TIMESTAMP + toIntervalMonth(1)
SETTINGS index_granularity = 8192;
