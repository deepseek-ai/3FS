#include "LevelDBLogger.h"

#include <folly/logging/xlog.h>

namespace hf3fs::kv {

LevelDBLogger::~LevelDBLogger() = default;

void LevelDBLogger::Logv(const char *format, std::va_list ap) {
  static constexpr int fixedBufferSize = 1024;
  thread_local char fixedBuffer[fixedBufferSize];

  // ap can only be used once, we have to prepare a copy of ap for both paths
  std::va_list apCopy;
  va_copy(apCopy, ap);
  auto count = std::vsnprintf(fixedBuffer, fixedBufferSize, format, apCopy);
  va_end(apCopy);

  if (count < 0) {
    XLOGF(WARN, "[LevelDB] Print log from LevelDB failed. ret of vsnprintf is {}", count);
  } else if (count < fixedBufferSize) {
    // fit into fixedBuffer, note that the terminating '\0' will occupy one byte but not counted in the return value.
    if (count >= 1 && fixedBuffer[count - 1] == '\n') {
      --count;
    }
    XLOGF(INFO, "[LevelDB] {}", std::string_view{fixedBuffer, static_cast<size_t>(count)});
  } else {
    // not fit into fixedBuffer, allocate a dynamic buffer large enough for the content and the terminating '\0'
    auto dynamicBuffer = std::make_unique<char[]>(count + 1);
    std::vsnprintf(dynamicBuffer.get(), count + 1, format, ap);
    if (count >= 1 && dynamicBuffer.get()[count - 1] == '\n') {
      --count;
    }
    XLOGF(INFO, "[LevelDB] {}", std::string_view{dynamicBuffer.get(), static_cast<size_t>(count)});
  }
}
}  // namespace hf3fs::kv
