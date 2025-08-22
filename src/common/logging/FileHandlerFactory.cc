#include "common/logging/FileHandlerFactory.h"

#include <folly/logging/FileWriterFactory.h>
#include <folly/logging/LogHandler.h>
#include <folly/logging/StandardLogHandler.h>
#include <folly/logging/StandardLogHandlerFactory.h>
#include <memory>

#include "common/logging/FileWriterFactory.h"
#include "common/logging/LogFormatter.h"

namespace hf3fs::logging {
std::shared_ptr<folly::LogHandler> FileHandlerFactory::createHandler(const Options &options) {
  FileWriterFactory writerFactory;
  LogFormatterFactory formatterFactory;
  return folly::StandardLogHandlerFactory::createHandler(getType(), &writerFactory, &formatterFactory, options);
}

std::shared_ptr<folly::LogHandler> EventLogHandlerFactory::createHandler(const Options &options) {
  FileWriterFactory writerFactory;
  EventLogFormatterFactory formatterFactory;
  return folly::StandardLogHandlerFactory::createHandler(getType(), &writerFactory, &formatterFactory, options);
}

}  // namespace hf3fs::logging
