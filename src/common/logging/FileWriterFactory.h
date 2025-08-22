#pragma once

#include <folly/logging/StandardLogHandlerFactory.h>
#include <optional>

namespace hf3fs::logging {
class FileWriterFactory : public folly::StandardLogHandlerFactory::WriterFactory {
 public:
  bool processOption(folly::StringPiece name, folly::StringPiece value) override;

  std::shared_ptr<folly::LogWriter> createWriter() override;

 private:
  std::string path_;
  bool async_{true};
  bool rotate_{false};
  size_t maxBufferSize_{0};
  size_t maxFileSize_{0};
  size_t maxFiles_{0};
  std::optional<bool> rotateOnOpen_;
};

}  // namespace hf3fs::logging
