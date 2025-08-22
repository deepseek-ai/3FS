#include <folly/experimental/coro/BlockingWait.h>

#include "Commands.h"
#include "Layout.h"

DEFINE_string(path, "", "Path of file/directory to set layout");
DEFINE_bool(create_dir, false, "True to create a directory, false to create a file");
DEFINE_bool(recursive, false, "If to recursively create intermediate directories when creating a directory");

namespace hf3fs::tools {
void createWithLayout(meta::client::MetaClient &metaClient, const flat::UserInfo &ui) {
  auto layout = layoutFromFlags();
  if (FLAGS_create_dir) {
    auto res = folly::coro::blockingWait(
        metaClient
            .mkdirs(ui, meta::InodeId::root(), Path(FLAGS_path), meta::Permission(0755), FLAGS_recursive, layout));
    XLOGF_IF(FATAL, !res, "failed to create directory {} with layout error {}", FLAGS_path, res.error().describe());
  } else {
    auto res = folly::coro::blockingWait(metaClient.create(ui,
                                                           meta::InodeId::root(),
                                                           Path(FLAGS_path),
                                                           std::nullopt,
                                                           meta::Permission(0644),
                                                           O_CREAT | O_EXCL | O_RDONLY,
                                                           layout));
    XLOGF_IF(FATAL, !res, "failed to create file {} with layout error {}", FLAGS_path, res.error().describe());
  }
}
}  // namespace hf3fs::tools
