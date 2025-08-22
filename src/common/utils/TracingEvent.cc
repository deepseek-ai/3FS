#include "TracingEvent.h"

namespace hf3fs::tracing {
std::string_view toString(uint64_t event) {
  switch (event) {
#define EVENT(category, name, value)                     \
  case makeValue(TRACING_EVENT_PREFIX(category), value): \
    return #category "::" #name;

#define PAIR_EVENT(category, name, value)                           \
  case makeValue(TRACING_EVENT_PREFIX(category), value):            \
    return #category "::" #name;                                    \
  case makePairValue(TRACING_EVENT_PREFIX(category), true, value):  \
    return #category "::Begin" #name;                               \
  case makePairValue(TRACING_EVENT_PREFIX(category), false, value): \
    return #category "::End" #name;

#include "TracingEventDetails.h"

#undef EVENT
#undef PAIR_EVENT
  }
  return "UnknownEvent";
}
}  // namespace hf3fs::tracing
