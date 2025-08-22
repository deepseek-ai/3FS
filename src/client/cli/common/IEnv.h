#pragma once

namespace hf3fs::client::cli {
// hide implementations, user should dynamic cast IEnv to a known concrete Env class.
struct IEnv {
  virtual ~IEnv() = default;
};
}  // namespace hf3fs::client::cli
