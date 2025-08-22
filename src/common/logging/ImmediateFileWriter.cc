/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ImmediateFileWriter.h"

#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/logging/LoggerDB.h>
#include <folly/portability/Unistd.h>

namespace hf3fs::logging {

ImmediateFileWriter::ImmediateFileWriter(std::shared_ptr<IWritableFile> file)
    : file_{std::move(file)} {}

void ImmediateFileWriter::writeMessage(folly::StringPiece buffer, uint32_t /* flags */) {
  // Write the data.
  // We are doing direct file descriptor writes here, so there is no buffering
  // of log message data.  Each message is immediately written to the output.
  auto ret = file_->writeFull(buffer.data(), buffer.size());
  if (ret < 0) {
    int errnum = errno;
    folly::LoggerDB::internalWarning(__FILE__,
                                     __LINE__,
                                     "error writing to log file ",
                                     file_->desc(),
                                     ": ",
                                     folly::errnoStr(errnum));
  }
}

void ImmediateFileWriter::flush() {}
}  // namespace hf3fs::logging
