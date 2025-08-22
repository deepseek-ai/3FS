#include "common/logging/LogInit.h"

#include <folly/logging/Init.h>
#include <folly/logging/LoggerDB.h>
#include <memory>

#include "common/logging/FileHandlerFactory.h"

namespace hf3fs::logging {
namespace {
bool tryInitLogHandlers() {
  static auto inited = [] {
    folly::LoggerDB::get().registerHandlerFactory(std::make_unique<FileHandlerFactory>());
    folly::LoggerDB::get().registerHandlerFactory(std::make_unique<EventLogHandlerFactory>());
    return true;
  }();
  return inited;
}
}  // namespace
void initLogHandlers() { tryInitLogHandlers(); }

bool init(const String &config) {
  tryInitLogHandlers();

  try {
    folly::initLogging(config);
  } catch (const std::exception &ex) {
    fprintf(stderr, "error parsing logging configuration: %s\n", ex.what());
    return false;
  }

  return true;
}

void initOrDie(const String &config) {
  tryInitLogHandlers();
  folly::initLoggingOrDie(config);
}
}  // namespace hf3fs::logging
