#include "common/logging/LogFormatter.h"

#include <fmt/chrono.h>
#include <fmt/core.h>
#include <folly/logging/LogMessage.h>
#include <folly/system/ThreadId.h>
#include <folly/system/ThreadName.h>

namespace hf3fs::logging {
namespace {
std::string_view getLevelName(folly::LogLevel level) {
  if (level < folly::LogLevel::INFO) {
    return "DEBUG";
  } else if (level < folly::LogLevel::WARN) {
    return "INFO";
  } else if (level < folly::LogLevel::ERR) {
    return "WARNING";
  } else if (level < folly::LogLevel::CRITICAL) {
    return "ERROR";
  } else if (level < folly::LogLevel::DFATAL) {
    return "CRITICAL";
  }
  return "FATAL";
}
}  // namespace

std::string LogFormatter::formatMessage(const folly::LogMessage &message,
                                        const folly::LogCategory * /*handlerCategory*/) {
  // this implementation (except the thread_local optimization) takes from folly::GlogStyleFormatter.
  thread_local auto cachedSeconds =
      std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::time_point().time_since_epoch());
  thread_local std::string cachedDateTimeStr;
  thread_local uint64_t cachedThreadId = 0;
  thread_local std::string cachedThreadIdStr;
  thread_local folly::Optional<std::string> currentThreadName;
  thread_local auto currentOsThreadId = folly::getOSThreadID();

  auto localTime = fmt::localtime(std::chrono::system_clock::to_time_t(message.getTimestamp()));

  // NOTE: assume nobody will change the time zone in runtime
  static const auto timeZoneStr = fmt::format("{:%Ez}", localTime);

  // Get the local time info
  auto timeSinceEpoch = message.getTimestamp().time_since_epoch();
  auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(timeSinceEpoch);
  auto nsecs = std::chrono::duration_cast<std::chrono::nanoseconds>(timeSinceEpoch) - epochSeconds;

  if (cachedSeconds != epochSeconds) {
    cachedSeconds = epochSeconds;
    cachedDateTimeStr = fmt::format("{:%FT%H:%M:%S}", localTime);
  }

  std::string_view threadName;
  {
    if (!currentThreadName) {
      currentThreadName = folly::getCurrentThreadName();
    }

    auto tid = message.getThreadID();
    if (tid == currentOsThreadId && currentThreadName) {
      threadName = *currentThreadName;
    }

    if (tid != cachedThreadId) {
      cachedThreadId = tid;
      cachedThreadIdStr = fmt::format("{:5d}", tid);
    }
  }

  auto basename = message.getFileBaseName();
  auto header = fmt::format("[{}.{:09d}{} {}:{} {}:{} {}] ",
                            cachedDateTimeStr,
                            nsecs.count(),
                            timeZoneStr,
                            threadName,
                            cachedThreadIdStr,
                            basename,
                            message.getLineNumber(),
                            getLevelName(message.getLevel()));

  // Format the data into a buffer.
  std::string buffer;
  folly::StringPiece msgData{message.getMessage()};
  if (message.containsNewlines()) {
    // If there are multiple lines in the log message, add a header
    // before each one.

    buffer.reserve(((header.size() + 1) * message.getNumNewlines()) + msgData.size());

    size_t idx = 0;
    while (true) {
      auto end = msgData.find('\n', idx);
      if (end == folly::StringPiece::npos) {
        end = msgData.size();
      }

      buffer.append(header);
      auto line = msgData.subpiece(idx, end - idx);
      buffer.append(line.data(), line.size());
      buffer.push_back('\n');

      if (end == msgData.size()) {
        break;
      }
      idx = end + 1;
    }
  } else {
    buffer.reserve(header.size() + msgData.size());
    buffer.append(header);
    buffer.append(msgData.data(), msgData.size());
    buffer.push_back('\n');
  }

  return buffer;
}

std::string EventLogFormatter::formatMessage(const folly::LogMessage &message, const folly::LogCategory *) {
  return fmt::format("{}\n", message.getMessage());
}

}  // namespace hf3fs::logging
