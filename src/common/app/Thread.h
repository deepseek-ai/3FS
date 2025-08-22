#pragma once

namespace hf3fs {

class Thread {
 public:
  static bool blockInterruptSignals();
  static bool unblockInterruptSignals();
};

}  // namespace hf3fs
