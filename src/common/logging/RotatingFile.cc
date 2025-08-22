#include "RotatingFile.h"

#include <stdexcept>
#include <thread>

#include "common/utils/Path.h"

namespace hf3fs::logging {
namespace {
std::string genFilename(const std::string &base, size_t index) {
  if (index == 0) return base;

  auto [basename, ext] = FileHelper::splitByExtension(base);
  return fmt::format("{}.{}{}", basename, index, ext);
}
}  // namespace

RotatingFile::RotatingFile(std::string path, const Options &options)
    : path_(std::move(path)),
      options_(options) {
  if (!options_.maxFiles) throw std::invalid_argument("'maxFiles' cannot be zero");
  if (options_.maxFiles > 200000) throw std::invalid_argument("'maxFiles' cannot exceed 200000");
  if (!options_.maxFileSize) throw std::invalid_argument("'maxFileSize' cannot be zero");

  file_.open(genFilename(path_, 0));
  currentSize_ = file_.size();

  if (options_.rotateOnOpen && currentSize_ > 0) {
    rotate();
    currentSize_ = 0;
  }
}

void RotatingFile::rotate() {
  file_.close();

  for (auto i = options_.maxFiles; i > 0; --i) {
    auto src = Path(genFilename(path_, i - 1));
    if (!boost::filesystem::exists(src)) continue;
    auto target = Path(genFilename(path_, i));

    boost::system::error_code ec;
    boost::filesystem::rename(src, target, ec);

    if (ec) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      boost::filesystem::rename(src, target, ec);
      if (ec) {
        file_.reopen(/*truncate=*/true);
        currentSize_ = 0;
        throw std::runtime_error(
            fmt::format("failed renaming {} to {} error {}", src.native(), target.native(), ec.message()));
      }
    }
  }
  file_.reopen(/*truncate=*/true);
}

bool RotatingFile::ttyOutput() const { return false; }

std::string RotatingFile::desc() const {
  std::unique_lock lock(mu_);
  return fmt::format("Rotating({})", file_.filename());
}

ssize_t RotatingFile::writevFull(iovec *iov, int count) {
  size_t length = 0;
  for (int i = 0; i < count; ++i) length += iov[i].iov_len;

  std::unique_lock lock(mu_);
  auto newSize = checkRotate(length);
  file_.writev(iov, count);
  currentSize_ = newSize;
  return count;
}

ssize_t RotatingFile::writeFull(const void *buf, size_t count) {
  std::unique_lock lock(mu_);
  auto newSize = checkRotate(count);
  file_.write(buf, count);
  currentSize_ = newSize;
  return count;
}

size_t RotatingFile::checkRotate(size_t size) {
  if (!file_) {
    file_.reopen(/* truncate= */ false);
    currentSize_ = file_.size();
  }
  auto newSize = currentSize_ + size;
  if (newSize > options_.maxFileSize) {
    file_.flush();
    if (file_.size() > 0) {
      rotate();
      newSize = size;
    }
  }
  return newSize;
}

void RotatingFile::flush() {
  std::unique_lock lock(mu_);
  file_.flush();
}

}  // namespace hf3fs::logging
