#include "suicide.h"

#include <signal.h>
#include <unistd.h>

namespace hf3fs {
Result<Void> suicide(bool graceful) {
  if (::kill(getpid(), graceful ? SIGTERM : SIGKILL) != 0) {
    return MAKE_ERROR_F(StatusCode::kUnknown,
                        "send {} to self failed. errno={}. msg={}",
                        graceful ? "SIGTERM" : "SIGKILL",
                        errno,
                        strerror(errno));
  }
  return Void{};
}
}  // namespace hf3fs
