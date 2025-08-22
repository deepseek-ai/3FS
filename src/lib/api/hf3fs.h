#pragma once

#include <dirent.h>
#include <memory>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/vfs.h>

#include "hf3fs_expected.h"

#define HF3FS_SUPER_MAGIC 0x8f3f5fff  // hf3fs fff

namespace hf3fs::lib {

template <typename T>
using Result = nonstd::expected<T, std::pair<int, std::string>>;

struct Empty {};

using NoResult = Result<Empty>;

struct DIR;
struct dirent {
  unsigned char d_type;
  std::string d_name;
};

struct stat : public ::stat {
  std::optional<std::string> st_ltarg;  // symlink target
};

struct ioseg {
  int fd = -1;
  off_t off = -1;
};
struct iovec_handle_t;
struct iovec {
  // base/len can be different than returned by iovalloc, so long as the whole buffer is within the range
  void *iov_base;
  size_t iov_len;

  // returned by iovalloc
  std::shared_ptr<iovec_handle_t> iov_handle;

  void *user_data = nullptr;
};

class IClient {
 public:
  static Result<std::shared_ptr<IClient>> newClient(std::string_view mountName, std::string_view token);
  static Result<std::shared_ptr<IClient>> newSuperClient(std::string_view mountName, std::string_view token);
  IClient() = default;
  virtual ~IClient() = default;

 public:  // meta ops
  virtual NoResult statfs(std::string_view path, struct statfs *buf) = 0;
  virtual NoResult fstatfs(int fd, struct statfs *buf) = 0;

  virtual Result<std::shared_ptr<DIR>> opendir(std::string_view name, bool ignoreCache = false) = 0;
  virtual Result<std::shared_ptr<DIR>> opendirat(int dirfd, std::string_view name, bool ignoreCache = false) = 0;
  virtual Result<std::shared_ptr<DIR>> fdopendir(int fd) = 0;
  virtual NoResult rewinddir(const std::shared_ptr<DIR> &dirp) = 0;
  virtual Result<std::optional<dirent>> readdir(const std::shared_ptr<DIR> &dirp) = 0;
  virtual Result<int> dirfd(const std::shared_ptr<DIR> &dirp) = 0;

  virtual NoResult mkdir(std::string_view pathname, mode_t mode = 0777, bool recursive = false) = 0;
  virtual NoResult mkdirat(int dirfd, std::string_view pathname, mode_t mode = 0777, bool recursive = false) = 0;

  virtual NoResult rmdir(std::string_view pathname, bool recursive = false) = 0;
  virtual NoResult rmdirat(int dirfd, std::string_view pathname, bool recursive = false) = 0;
  virtual NoResult unlink(std::string_view pathname) = 0;
  virtual NoResult unlinkat(int dirfd, std::string_view pathname, int flags, bool recursive = false) = 0;
  virtual NoResult remove(std::string_view pathname,
                          std::optional<bool> isDir = std::nullopt,
                          bool recursive = false) = 0;
  virtual NoResult removeat(int dirfd,
                            std::string_view pathname,
                            std::optional<bool> isDir = std::nullopt,
                            bool recursive = false) = 0;

  virtual NoResult rename(std::string_view oldpath, std::string_view newpath) = 0;
  virtual NoResult renameat(int olddirfd, std::string_view oldpath, int newdirfd, std::string_view newpath) = 0;

  virtual NoResult stat(std::string_view pathname, struct stat *statbuf, bool ignoreCache = false) = 0;
  virtual NoResult fstat(int fd, struct stat *statbuf) = 0;
  virtual NoResult lstat(std::string_view pathname, struct stat *statbuf, bool ignoreCache = false) = 0;
  virtual NoResult fstatat(int dirfd, std::string_view pathname, struct stat *statbuf, int flags) = 0;

  virtual Result<std::string> readlink(std::string_view pathname, bool ignoreCache = false) = 0;
  virtual Result<std::string> readlinkat(int dirfd, std::string_view pathname, bool ignoreCache = false) = 0;
  virtual Result<std::string> realpath(std::string_view pathname, bool absolute = true) = 0;
  virtual Result<std::string> realpathat(int dirfd, std::string_view pathname, bool absolute = true) = 0;

  virtual Result<int> dup(int oldfd) = 0;
  virtual Result<int> dup2(int oldfd, int newfd) = 0;
  virtual Result<int> dup3(int oldfd, int newfd, int flags) = 0;

  // NOTE: we don't use umask to modify mode when open()/creat() new files
  virtual Result<int> creat(std::string_view pathname,
                            mode_t mode = 0666,
                            bool excl = false,
                            bool ignoreCache = false) = 0;
  virtual Result<int> creatat(int dirfd,
                              std::string_view pathname,
                              mode_t mode = 0666,
                              bool excl = false,
                              bool ignoreCache = false) = 0;
  virtual NoResult symlink(std::string_view target, std::string_view linkpath) = 0;
  virtual NoResult symlinkat(std::string_view target, int newdirfd, std::string_view linkpath) = 0;

  virtual NoResult link(std::string_view oldpath, std::string_view newpath) = 0;
  virtual NoResult linkat(int olddirfd,
                          std::string_view oldpath,
                          int newdirfd,
                          std::string_view newpath,
                          int flags) = 0;

  // we use O_NONBLOCK flag to indicate we want to ignore the inode cache
  // if you want to read file immediately after operating on it, use this flag
  virtual Result<int> open(std::string_view pathname, int flags, mode_t mode = 0666) = 0;
  virtual Result<int> openat(int dirfd, std::string_view pathname, int flags, mode_t mode = 0666) = 0;
  virtual NoResult close(int fd) = 0;

  virtual NoResult utimes(std::string_view filename, const struct timeval times[2]) = 0;
  virtual NoResult futimens(int fd, const struct timespec times[2]) = 0;
  virtual NoResult utimensat(int dirfd, std::string_view pathname, const struct timespec times[2], int flags) = 0;

  virtual NoResult chmod(std::string_view pathname, mode_t mode) = 0;
  // virtual NoResult fchmod(int fd, mode_t mode) = 0;
  virtual NoResult fchmodat(int dirfd, std::string_view pathname, mode_t mode, int flags) = 0;

  virtual NoResult chown(std::string_view pathname, uid_t owner, gid_t group) = 0;
  // virtual NoResult fchown(int fd, uid_t owner, gid_t group) = 0;
  virtual NoResult lchown(std::string_view pathname, uid_t owner, gid_t group) = 0;
  virtual NoResult fchownat(int dirfd, std::string_view pathname, uid_t owner, gid_t group, int flags) = 0;

  virtual NoResult chdir(std::string_view path, bool ignoreCache = false) = 0;
  virtual NoResult fchdir(int fd) = 0;
  virtual Result<std::string> getcwd() = 0;

 public:  // io ops
  virtual Result<struct iovec> iovalloc(size_t bytes, int numa = -1, bool global = false, size_t blockSize = 0) = 0;
  virtual NoResult iovfree(const std::shared_ptr<iovec_handle_t> &iovh) = 0;

  // has to use iovec returned by iovalloc, zero copy
  virtual NoResult preadv(int iovcnt, const struct iovec *iov, const struct ioseg *segv, ssize_t *resv) = 0;
  virtual NoResult pwritev(int iovcnt, const struct iovec *iov, const struct ioseg *segv, ssize_t *resv) = 0;

  // can use buffer allocated by caller, not zero copy, offset is maintained by client
  virtual Result<size_t> read(int fd, void *buf, size_t count, size_t readahead = 0) = 0;
  virtual Result<size_t> write(int fd, const void *buf, size_t count, bool flush = false) = 0;

  // may not be very accurate if seek from end,
  // since other clients may be writing and moving the eof when we're seeking
  virtual Result<off_t> lseek(int fd, off_t offset, int whence, size_t readahead = 0) = 0;

  virtual NoResult fdatasync(int fd) = 0;
  virtual NoResult fsync(int fd) = 0;
  virtual NoResult ftruncate(int fd, off_t length) = 0;

 public:  // advanced ops
  // caller can distribute the serialized sharedFileHandles to clients on other machines
  // and call openWithFileHandles() there to get fds without server communication
  // only fds opened for read/path/dirfd can be used this way, the root dir cannot though
  // used to reduce calls to the meta server, thus improve perf
  // how to distribute the bytes is not in consideration of the hf3fs client
  virtual Result<std::vector<uint8_t>> sharedFileHandles(const std::vector<int> &fds) = 0;
  // can return fewer than fhs, if too many files are opened
  virtual Result<std::vector<int>> openWithFileHandles(const std::vector<uint8_t> &fhs) = 0;
  virtual Result<std::string> sharedIovecHandle(const std::shared_ptr<iovec_handle_t> &iovh) = 0;
  virtual Result<struct iovec> openIovecHandle(const std::string &iovh, bool acrossAgent = false) = 0;
};
}  // namespace hf3fs::lib
