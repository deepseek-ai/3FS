#pragma once

#include "common/serde/CallContext.h"
#include "common/serde/Service.h"
#include "common/utils/ConfigBase.h"
#include "common/utils/Semaphore.h"

namespace hf3fs::net {

struct RDMATransmissionReq {
  SERDE_STRUCT_FIELD(uuid, size_t{});
};

struct RDMATransmissionRsp {
  SERDE_STRUCT_FIELD(dummy, Void{});
};

SERDE_SERVICE(RDMAControl, 10) { SERDE_SERVICE_METHOD(apply, 1, RDMATransmissionReq, RDMATransmissionRsp); };

class RDMATransmissionLimiter {
 public:
  RDMATransmissionLimiter(uint32_t maxConcurrentTransmission)
      : semaphore_(maxConcurrentTransmission) {}

  CoTask<void> co_wait();

  void signal(Duration latency);

  void updateMaxConcurrentTransmission(uint32_t value) { semaphore_.changeUsableTokens(value); }

 private:
  Semaphore semaphore_;
  std::atomic<uint32_t> current_{};
};
using RDMATransmissionLimiterPtr = std::shared_ptr<RDMATransmissionLimiter>;

class RDMAControlImpl : public serde::ServiceWrapper<RDMAControlImpl, RDMAControl> {
 public:
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(max_concurrent_transmission, 64u);
  };
  RDMAControlImpl(const Config &config)
      : config_(config),
        limiter_(std::make_shared<RDMATransmissionLimiter>(config_.max_concurrent_transmission())),
        guard_(config_.addCallbackGuard(
            [&] { limiter_->updateMaxConcurrentTransmission(config_.max_concurrent_transmission()); })) {}

  CoTryTask<RDMATransmissionRsp> apply(serde::CallContext &, const RDMATransmissionReq &req);

 private:
  const Config &config_;
  RDMATransmissionLimiterPtr limiter_;
  std::unique_ptr<ConfigCallbackGuard> guard_;
};

}  // namespace hf3fs::net
