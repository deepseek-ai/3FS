#include "FileHelper.h"

#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/logging/LoggerDB.h>

namespace hf3fs::logging {

FileHelper::~FileHelper() { close(); }

void FileHelper::open(const std::string &fname, bool truncate) {
  close();
  filename_ = fname;

  if (truncate) {
    // Truncate by opening-and-closing a tmp file in "wb" mode, always
    // opening the actual log-we-write-to in "ab" mode, since that
    // interacts more politely with eternal processes that might
    // rotate/truncate the file underneath us.
    auto f = folly::File(fname.c_str(), O_WRONLY | O_CREAT);
  }

  file_ = std::make_unique<folly::File>(filename_, O_WRONLY | O_APPEND | O_CREAT);
}

void FileHelper::reopen(bool truncate) {
  assert(!filename_.empty());
  this->open(filename_, truncate);
}

void FileHelper::flush() {}

void FileHelper::close() { file_.reset(); }

void FileHelper::writev(iovec *iov, int count) {
  auto ret = folly::writevFull(fd(), iov, count);
  if (ret < 0) {
    int errnum = errno;
    folly::LoggerDB::internalWarning(__FILE__,
                                     __LINE__,
                                     "error writing to log file ",
                                     filename_,
                                     ": ",
                                     folly::errnoStr(errnum));
  }
}

void FileHelper::write(const void *buf, size_t count) {
  auto ret = folly::writeFull(fd(), buf, count);
  if (ret < 0) {
    int errnum = errno;
    folly::LoggerDB::internalWarning(__FILE__,
                                     __LINE__,
                                     "error writing to log file ",
                                     filename_,
                                     ": ",
                                     folly::errnoStr(errnum));
  }
}

size_t FileHelper::size() const {
  struct stat buf;
  if (fstat(fd(), &buf) == -1) {
    int errnum = errno;
    auto msg = fmt::format("error stat log file {}: {}", filename_, folly::errnoStr(errnum));
    throw std::runtime_error(msg);
  }
  return buf.st_size;
}

const std::string &FileHelper::filename() const { return filename_; }

//
// return file path and its extension:
//
// "mylog.txt" => ("mylog", ".txt")
// "mylog" => ("mylog", "")
// "mylog." => ("mylog.", "")
// "/dir1/dir2/mylog.txt" => ("/dir1/dir2/mylog", ".txt")
//
// the starting dot in filenames is ignored (hidden files):
//
// ".mylog" => (".mylog". "")
// "my_folder/.mylog" => ("my_folder/.mylog", "")
// "my_folder/.mylog.txt" => ("my_folder/.mylog", ".txt")
std::tuple<std::string, std::string> FileHelper::splitByExtension(const std::string &fname) {
  auto ext_index = fname.rfind('.');

  // no valid extension found - return whole path and empty string as
  // extension
  if (ext_index == std::string::npos || ext_index == 0 || ext_index == fname.size() - 1) {
    return std::make_tuple(fname, std::string());
  }

  // treat cases like "/etc/rc.d/somelogfile or "/abc/.hiddenfile"
  auto folder_index = fname.find_last_of('/');
  if (folder_index != std::string::npos && folder_index >= ext_index - 1) {
    return std::make_tuple(fname, std::string());
  }

  // finally - return a valid base and extension tuple
  return std::make_tuple(fname.substr(0, ext_index), fname.substr(ext_index));
}

int FileHelper::fd() const {
  if (!file_) {
    throw std::runtime_error("log file is nullptr");
  }
  return file_->fd();
}

}  // namespace hf3fs::logging
