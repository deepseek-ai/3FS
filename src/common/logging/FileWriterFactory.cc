#include "FileWriterFactory.h"

#include <folly/Conv.h>
#include <folly/File.h>

#include "AsyncFileWriter.h"
#include "ImmediateFileWriter.h"
#include "RotatingFile.h"
#include "SingleFile.h"

namespace hf3fs::logging {
namespace {
size_t getPositiveSize(folly::StringPiece v) {
  auto size = folly::to<size_t>(v);
  if (size == 0) {
    throw std::invalid_argument(folly::to<std::string>("must be a positive integer"));
  }
  return size;
}
}  // namespace

bool FileWriterFactory::processOption(folly::StringPiece name, folly::StringPiece value) {
  if (name == "path") {
    path_ = value.str();
    return true;
  }
  if (name == "async") {
    async_ = folly::to<bool>(value);
    return true;
  }
  if (name == "rotate") {
    rotate_ = folly::to<bool>(value);
    return true;
  }
  if (name == "max_buffer_size") {
    maxBufferSize_ = getPositiveSize(value);
    return true;
  }
  if (name == "max_file_size") {
    maxFileSize_ = getPositiveSize(value);
    return true;
  }
  if (name == "max_files") {
    maxFiles_ = getPositiveSize(value);
    return true;
  }
  if (name == "rotate_on_open") {
    rotateOnOpen_ = folly::to<bool>(value);
    return true;
  }
  return false;
}

std::shared_ptr<folly::LogWriter> FileWriterFactory::createWriter() {
  // Get the output file to use
  if (path_.empty()) {
    throw std::invalid_argument("no path specified for file handler");
  }
  if (!async_ && maxBufferSize_) {
    throw std::invalid_argument(
        folly::to<std::string>("the \"max_buffer_size\" option is only valid for async file "
                               "handlers"));
  }
  if (!rotate_) {
    if (maxFileSize_)
      throw std::invalid_argument(
          folly::to<std::string>("the \"max_file_size\" option is only valid for rotating file "
                                 "handlers"));
    if (maxFiles_)
      throw std::invalid_argument(
          folly::to<std::string>("the \"max_files\" option is only valid for rotating file "
                                 "handlers"));
    if (rotateOnOpen_)
      throw std::invalid_argument(
          folly::to<std::string>("the \"rotate_on_open\" option is only valid for rotating file "
                                 "handlers"));
  }

  auto file = [&]() -> std::shared_ptr<IWritableFile> {
    if (rotate_) {
      RotatingFile::Options options;
      options.maxFiles = maxFiles_;
      options.maxFileSize = maxFileSize_;
      options.rotateOnOpen = rotateOnOpen_.value_or(false);

      return std::make_shared<RotatingFile>(path_, options);
    } else {
      return std::make_shared<SingleFile>(folly::File{path_, O_WRONLY | O_APPEND | O_CREAT});
    }
  }();

  if (async_) {
    auto asyncWriter = std::make_shared<AsyncFileWriter>(std::move(file));
    if (maxBufferSize_ > 0) {
      asyncWriter->setMaxBufferSize(maxBufferSize_);
    }
    return asyncWriter;
  } else {
    return std::make_shared<ImmediateFileWriter>(std::move(file));
  }
}

}  // namespace hf3fs::logging
