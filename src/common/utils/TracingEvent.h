#pragma once

#include <cstdint>
#include <string_view>

namespace hf3fs::tracing {
inline constexpr uint64_t kEventPrefixShift = 32;
inline constexpr uint64_t kPairEventShift = kEventPrefixShift - 2;
inline constexpr uint64_t kEventValueMask = (static_cast<uint64_t>(1) << kPairEventShift) - 1;

inline constexpr uint64_t kCommonPrefix = 0;
inline constexpr uint64_t kRpcPrefix = 1;
inline constexpr uint64_t kMetaPrefix = 2;
inline constexpr uint64_t kFdbPrefix = 3;

inline constexpr uint64_t makeValue(uint64_t category, uint64_t value) {
  auto c = (category << kEventPrefixShift);
  auto v = (value & kEventValueMask);
  return c | v;
}

inline constexpr uint64_t makePairValue(uint64_t category, bool first, uint64_t value) {
  auto c = (category << kEventPrefixShift);
  auto p = ((first + 1ULL) << kPairEventShift);
  auto v = (value & kEventValueMask);
  return c | p | v;
}

inline constexpr uint64_t getBeginEvent(uint64_t event) { return event | (2ULL << kPairEventShift); }
inline constexpr uint64_t getEndEvent(uint64_t event) { return event | (1ULL << kPairEventShift); }

#define TRACING_EVENT_NAME(category, name) k##category##name
#define TRACING_BEGIN_EVENT_NAME(category, name) k##category##Begin##name
#define TRACING_END_EVENT_NAME(category, name) k##category##End##name
#define TRACING_EVENT_PREFIX(category) k##category##Prefix

#define EVENT(category, name, value) \
  inline constexpr uint64_t TRACING_EVENT_NAME(category, name) = makeValue(TRACING_EVENT_PREFIX(category), value);

#define PAIR_EVENT(category, name, value)                              \
  EVENT(category, name, value);                                        \
  inline constexpr uint64_t TRACING_BEGIN_EVENT_NAME(category, name) = \
      makePairValue(TRACING_EVENT_PREFIX(category), true, value);      \
  inline constexpr uint64_t TRACING_END_EVENT_NAME(category, name) =   \
      makePairValue(TRACING_EVENT_PREFIX(category), false, value);

#include "TracingEventDetails.h"

#undef EVENT
#undef PAIR_EVENT

std::string_view toString(uint64_t event);
}  // namespace hf3fs::tracing
