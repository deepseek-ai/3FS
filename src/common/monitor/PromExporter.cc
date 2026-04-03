#include "common/monitor/PromExporter.h"
#include <prometheus/counter.h>
#include <prometheus/gauge.h>
#include <prometheus/exposer.h>
#include <prometheus/registry.h>
#include <prometheus/family.h>
#include <fmt/format.h>
namespace hf3fs::monitor {
PromExporter::PromExporter(const Config &config)
    : config_(config) {}
PromExporter::~PromExporter() = default;
Result<Void> PromExporter::init() {
    Result<Void> initResult = Void{};
    try {
        registry_ = std::make_shared<prometheus::Registry>();
        exposer_ = std::make_unique<prometheus::Exposer>(fmt::format("0.0.0.0:{}", config_.port()));
        exposer_->RegisterCollectable(registry_);
    } catch (const std::exception &e) {
        XLOGF(ERR, "Failed to initialize Prometheus exporter: {}", e.what());
        initResult = makeError(StatusCode::kMonitorInitFailed);
    }
    return initResult;
}
Result<Void> PromExporter::commit(const std::vector<Sample> &samples) {
    if (samples.empty()) {
        return Void{};
    }
    try {
        for (auto &sample : samples) {
            // Replace all '.' in metric names and metric label names with '_' to ensure Prometheus compatibility.
            std::string sample_name = sample.name;
            std::replace(sample_name.begin(), sample_name.end(), '.', '_');
            // Create labels from sample tags
            prometheus::Labels labels;
            for (const auto &[key, value] : sample.tags) {
                if (!key.empty()) {
                    auto key_copy = key;
                    std::replace(key_copy.begin(), key_copy.end(), '.', '_');
                    labels.emplace(key_copy, value);
                } else {
                    XLOGF(ERR, "Empty tag key in sample: {}", sample.name);
                }
            }
            // If the sample is a counter
            if (sample.isNumber()) {
                auto it = _3fs_counter_families_.find(sample_name);
                // If the family does not exist, create it
                if (it == _3fs_counter_families_.end()) {
                    auto& family = prometheus::BuildGauge()
                            .Name(sample_name)
                            .Help(fmt::format("3FS counter metric: {}", sample_name))
                            .Register(*registry_);
                    _3fs_counter_families_.insert(std::make_pair(sample_name, &family));
                }
                _3fs_counter_families_[sample_name]->Add(labels).Set(sample.number());
            } else if (sample.isDistribution()) {
                auto &dist = sample.dist();
                auto it = _3fs_dis_families_.find(sample_name);
                // If the distribution families do not exist, create it
                if (it == _3fs_dis_families_.end()) {
                    auto &cnt_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_cnt", sample_name))
                        .Help(fmt::format("3FS distribution count metric: {}", sample_name))
                        .Register(*registry_);
                    auto &mean_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_mean", sample_name))
                        .Help(fmt::format("3FS distribution mean metric: {}", sample_name))
                        .Register(*registry_);
                    auto &sum_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_sum", sample_name))
                        .Help(fmt::format("3FS distribution sum metric: {}", sample_name))
                        .Register(*registry_);
                    auto &min_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_min", sample_name))
                        .Help(fmt::format("3FS distribution min metric: {}", sample_name))
                        .Register(*registry_);
                    auto &max_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_max", sample_name))
                        .Help(fmt::format("3FS distribution max metric: {}", sample_name))
                        .Register(*registry_);
                    auto &p50_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_p50", sample_name))
                        .Help(fmt::format("3FS distribution p50 metric: {}", sample_name))
                        .Register(*registry_);
                    auto &p90_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_p90", sample_name))
                        .Help(fmt::format("3FS distribution p90 metric: {}", sample_name))
                        .Register(*registry_);
                    auto &p95_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_p95", sample_name))
                        .Help(fmt::format("3FS distribution p95 metric: {}", sample_name))
                        .Register(*registry_);
                    auto &p99_family = prometheus::BuildGauge()
                        .Name(fmt::format("{}_p99", sample_name))
                        .Help(fmt::format("3FS distribution p99 metric: {}", sample_name))
                        .Register(*registry_);
                    DistMap dist_family_map = {
                        {"cnt", &cnt_family},
                        {"mean", &mean_family},
                        {"sum", &sum_family},
                        {"min", &min_family},
                        {"max", &max_family},
                        {"p50", &p50_family},
                        {"p90", &p90_family},
                        {"p95", &p95_family},
                        {"p99", &p99_family}
                    };
                    _3fs_dis_families_.insert(std::make_pair(sample_name, dist_family_map));
                }
                auto &dist_family_map = _3fs_dis_families_[sample_name];
                dist_family_map["cnt"]->Add(labels).Set(dist.cnt);
                dist_family_map["mean"]->Add(labels).Set(dist.mean());
                dist_family_map["sum"]->Add(labels).Set(dist.sum);
                dist_family_map["min"]->Add(labels).Set(dist.min);
                dist_family_map["max"]->Add(labels).Set(dist.max);
                dist_family_map["p50"]->Add(labels).Set(dist.p50);
                dist_family_map["p90"]->Add(labels).Set(dist.p90);
                dist_family_map["p95"]->Add(labels).Set(dist.p95);
                dist_family_map["p99"]->Add(labels).Set(dist.p99);
            }
        }
    } catch (const std::exception &e) {
        XLOGF(ERR, "Failed to commit samples to Prometheus exporter: {}", e.what());
        errorHappened_ = true;
    }
    return Void{};
}
}