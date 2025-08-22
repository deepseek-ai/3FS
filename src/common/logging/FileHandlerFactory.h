#pragma once

#include <folly/Range.h>
#include <folly/logging/LogHandler.h>
#include <folly/logging/LogHandlerFactory.h>
#include <memory>

namespace hf3fs::logging {
class FileHandlerFactory : public folly::LogHandlerFactory {
 public:
  folly::StringPiece getType() const override { return "file"; }

  std::shared_ptr<folly::LogHandler> createHandler(const Options &options) override;
};

class EventLogHandlerFactory : public folly::LogHandlerFactory {
 public:
  folly::StringPiece getType() const override { return "event"; }

  std::shared_ptr<folly::LogHandler> createHandler(const Options &options) override;
};

}  // namespace hf3fs::logging
