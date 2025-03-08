#include "FileUtils.h"

#include <fstream>

namespace hf3fs {
Result<std::string> loadFile(const Path &path) {
  try {
    std::ifstream file(path);
    if (!file) {
      return makeError(StatusCode::kIOError, fmt::format("Error opening file: {}", path));
    }
    std::string output((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return output;
  } catch (const std::exception &e) {
    return makeError(StatusCode::kIOError, fmt::format("Error when read {}: {}", path, e.what()));
  }
}

Result<Void> storeToFile(const Path &path, const std::string &content) {
  try {
    std::ofstream file(path);
    if (!file) {
      return makeError(StatusCode::kIOError, fmt::format("Error opening file for writing: {}", path));
    }

    file << content;
    if (!file) {
      return makeError(StatusCode::kIOError, fmt::format("Error writing to file: {}", path));
    }
    return Void{};
  } catch (const std::exception &e) {
    return makeError(StatusCode::kIOError, fmt::format("Error when write to {}: {}", path, e.what()));
  }
}

}  // namespace hf3fs
