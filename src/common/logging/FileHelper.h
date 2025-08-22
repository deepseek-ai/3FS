#pragma once

#include <folly/File.h>
#include <tuple>

namespace hf3fs::logging {

class FileHelper {
 public:
  FileHelper() = default;

  FileHelper(const FileHelper &) = delete;
  FileHelper &operator=(const FileHelper &) = delete;
  ~FileHelper();

  void open(const std::string &fname, bool truncate = false);
  void reopen(bool truncate);
  void flush();
  void close();
  void writev(iovec *iov, int count);
  void write(const void *buf, size_t count);
  size_t size() const;
  const std::string &filename() const;

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
  static std::tuple<std::string, std::string> splitByExtension(const std::string &fname);

  operator bool() const { return file_ != nullptr; }

 private:
  int fd() const;

  const int open_tries_ = 5;
  const unsigned int open_interval_ = 10;
  std::unique_ptr<folly::File> file_;
  std::string filename_;
};
}  // namespace hf3fs::logging
