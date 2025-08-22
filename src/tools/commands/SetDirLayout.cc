#include <folly/experimental/coro/BlockingWait.h>

#include "Commands.h"
#include "Layout.h"

DEFINE_string(dir_path, "", "Path of directory to set default layout");

namespace hf3fs::tools {
void setDirLayout(meta::client::MetaClient &metaClient, const flat::UserInfo &ui) {
  auto layout = layoutFromFlags();
  auto res = folly::coro::blockingWait(metaClient.setLayout(ui, meta::InodeId::root(), Path(FLAGS_dir_path), layout));
  XLOGF_IF(FATAL, !res, "failed to set layout of directory {} error {}", FLAGS_dir_path, res.error().describe());
}
}  // namespace hf3fs::tools
