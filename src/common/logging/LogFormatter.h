#pragma once

#include <folly/logging/LogCategory.h>
#include <folly/logging/LogFormatter.h>
#include <folly/logging/LogMessage.h>
#include <folly/logging/StandardLogHandlerFactory.h>

namespace hf3fs::logging {
class LogFormatter : public folly::LogFormatter {
 public:
  std::string formatMessage(const folly::LogMessage &message, const folly::LogCategory *) override;
};

class LogFormatterFactory : public folly::StandardLogHandlerFactory::FormatterFactory {
 public:
  bool processOption(folly::StringPiece /* name */, folly::StringPiece /* value */) override { return false; }
  std::shared_ptr<folly::LogFormatter> createFormatter(
      const std::shared_ptr<folly::LogWriter> & /* logWriter */) override {
    return std::make_shared<LogFormatter>();
  }
};

class EventLogFormatter : public folly::LogFormatter {
 public:
  std::string formatMessage(const folly::LogMessage &message, const folly::LogCategory *) override;
};

class EventLogFormatterFactory : public folly::StandardLogHandlerFactory::FormatterFactory {
 public:
  bool processOption(folly::StringPiece /* name */, folly::StringPiece /* value */) override { return false; }
  std::shared_ptr<folly::LogFormatter> createFormatter(
      const std::shared_ptr<folly::LogWriter> & /* logWriter */) override {
    return std::make_shared<EventLogFormatter>();
  }
};

}  // namespace hf3fs::logging
