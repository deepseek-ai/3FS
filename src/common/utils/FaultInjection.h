#pragma once

#include <atomic>
#include <cassert>
#include <folly/Likely.h>
#include <folly/Preprocessor.h>
#include <folly/Random.h>
#include <folly/io/async/Request.h>
#include <memory>
#include <optional>

// Set fault injection parameter in current code scope and automatically restore after leaving the scope.
// Probability is expressed as a percentage between 0 and 100.
// Times determines the maximum number of faults that can be injected (-1 means unlimited).
// eg: `FAULT_INJECTION_SET(1, 5)` will inject faults with a probability of 1% up to five times.
#define FAULT_INJECTION_SET(prob, times) hf3fs::FaultInjection::ScopeGuard FB_ANONYMOUS_VARIABLE(guard)(prob, times)

// Fault injection code may be wrapped in loops, which may cause same fault injected many times. In this case, the
// effect of the loop can be reduced by setting a probability factor. The probability factor only works in the current
// scope and is automatically restored after leaving the scope.
#define FAULT_INJECTION_SET_FACTOR(factor) hf3fs::FaultInjectionFactor::ScopeGuard FB_ANONYMOUS_VARIABLE(guard)(factor)

// Use probability factor of current scope.
#define FAULT_INJECTION()                               \
  (UNLIKELY(hf3fs::FaultInjection::get() != nullptr) && \
   hf3fs::FaultInjection::get()->injectWithFactor(hf3fs::FaultInjectionFactor::get()))
// Specify a probability factor.
#define FAULT_INJECTION_WITH_FACTOR(factor) \
  (UNLIKELY(hf3fs::FaultInjection::get() != nullptr) && hf3fs::FaultInjection::get()->injectWithFactor(factor))

namespace hf3fs {

class FaultInjection : public folly::RequestData {
 public:
  class ScopeGuard {
   public:
    ScopeGuard(double prob, int times)
        : guard_(std::nullopt) {
      if (LIKELY(prob == 0)) {
        if (LIKELY(FaultInjection::get() == nullptr)) {
          // fast path, no need to record anything.
          return;
        }

        guard_.emplace(token(), nullptr);
        return;
      }
      guard_.emplace(token(),
                     std::unique_ptr<FaultInjection>(new FaultInjection(prob / 100 * kProbabilityBase, times)));
    }

   private:
    std::optional<folly::ShallowCopyRequestContextScopeGuard> guard_;
  };

  static ScopeGuard clone() {
    auto fi = FaultInjection::get();
    return fi ? ScopeGuard((double)fi->probability_ / kProbabilityBase * 100, fi->times_) : ScopeGuard(0, 0);
  }

  static inline FaultInjection *get() {
    auto requestContext = folly::RequestContext::try_get();
    if (LIKELY(requestContext == nullptr)) {
      return nullptr;
    }
    return dynamic_cast<FaultInjection *>(requestContext->getContextData(token()));
  }

  bool hasCallback() override { return false; }

  bool injectWithFactor(uint32_t factor) {
    bool inject = folly::Random::rand32(kProbabilityBase) < probability_ && folly::Random::oneIn(factor);
    if (LIKELY(!inject)) {
      return false;
    }
    if (times_ == -1) {
      return true;
    }
    if (injected_ >= times_) return false;
    return injected_.fetch_add(1) < times_;
  }

  /** Get fault injection percentage probability. */
  double getProbability() const { return (double)probability_ * 100.0 / kProbabilityBase; }

 private:
  static constexpr const char *kTokenName = "hf3fs::FaultInjection";
  static constexpr uint32_t kProbabilityBase = 1000000;

  static folly::RequestToken const &token() {
    static folly::RequestToken const token(kTokenName);
    return token;
  }

  explicit FaultInjection(uint32_t prob, int times)
      : probability_(prob),
        times_(times),
        injected_(0) {
    probability_ = std::min(probability_, (uint32_t)kProbabilityBase);
  }

  uint32_t probability_;          // probability to trigger fault injection, between 0 and kProbabilityBase
  int32_t times_;                 // max fault injection times, -1 means unlimited
  std::atomic_int32_t injected_;  // injected times, use std::atomic for thread safety.
};

// This is read only, so it's thread safe as required by folly::RequestData.
class FaultInjectionFactor : public folly::RequestData {
 public:
  class ScopeGuard {
   public:
    explicit ScopeGuard(uint32_t factor)
        : guard_(std::nullopt) {
      if (LIKELY(FaultInjection::get() == nullptr)) {
        // fast path, fault injection not enabled.
        return;
      }
      guard_.emplace(token(), std::unique_ptr<FaultInjectionFactor>(new FaultInjectionFactor(factor)));
    }

   private:
    std::optional<folly::ShallowCopyRequestContextScopeGuard> guard_;
  };

  static inline uint32_t get() {
    auto requestContext = folly::RequestContext::try_get();
    if (LIKELY(requestContext == nullptr)) {
      return 1;
    }
    auto ptr = dynamic_cast<FaultInjectionFactor *>(requestContext->getContextData(token()));
    return ptr ? ptr->factor_ : 1;
  }

  bool hasCallback() override { return false; }

  uint32_t getFactor() const { return factor_; }

 private:
  static constexpr const char *kTokenName = "hf3fs::FaultInjectionFactor";
  static folly::RequestToken const &token() {
    static folly::RequestToken const token(kTokenName);
    return token;
  }

  FaultInjectionFactor(uint32_t factor)
      : factor_(factor) {}
  uint32_t factor_;
};
}  // namespace hf3fs
