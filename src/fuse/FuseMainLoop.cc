#include "FuseMainLoop.h"

#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include "FuseOps.h"

namespace hf3fs::fuse {
int fuseMainLoop(const String &programName,
                 bool allowOther,
                 const String &mountpoint,
                 size_t maxbufsize,
                 const String &clusterId) {
  auto &d = getFuseClientsInstance();
  const auto &ops = getFuseOps();

  std::stack<std::function<void()>> onStopHooks;
  SCOPE_EXIT {
    while (!onStopHooks.empty()) {
      onStopHooks.top()();
      onStopHooks.pop();
    }
  };

  std::vector<std::string> fuseArgs;
  fuseArgs.push_back(programName);
  if (allowOther) {
    fuseArgs.push_back("-o");
    fuseArgs.push_back("allow_other");
    fuseArgs.push_back("-o");
    fuseArgs.push_back("default_permissions");
  }
  fuseArgs.push_back("-o");
  fuseArgs.push_back("auto_unmount");
  fuseArgs.push_back("-o");
  fuseArgs.push_back(fmt::format("max_read={}", maxbufsize));
  fuseArgs.push_back(mountpoint);
  fuseArgs.push_back("-o");
  fuseArgs.push_back("subtype=hf3fs");
  fuseArgs.push_back("-o");
  fuseArgs.push_back("fsname=hf3fs." + clusterId);
  std::vector<char *> fuseArgsPtr;
  for (auto &arg : fuseArgs) {
    fuseArgsPtr.push_back(const_cast<char *>(arg.c_str()));
  }

  struct fuse_args args = FUSE_ARGS_INIT((int)fuseArgsPtr.size(), fuseArgsPtr.data());
  // struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
  struct fuse_cmdline_opts opts;
  struct fuse_loop_config *config = fuse_loop_cfg_create();
  SCOPE_EXIT { fuse_loop_cfg_destroy(config); };

  if (fuse_parse_cmdline(&args, &opts) != 0) {
    return 1;
  }

  onStopHooks.push([&] {
    free(opts.mountpoint);
    fuse_opt_free_args(&args);
  });

  if (opts.show_help) {
    printf("This is hf3fs fuse!\n");
    fuse_cmdline_help();
    fuse_lowlevel_help();
    return 0;
  } else if (opts.show_version) {
    printf("What's my version?\n");
    fuse_lowlevel_version();
    return 0;
  }

  if (opts.mountpoint == nullptr) {
    printf("No mountpoint.\n");
    return 1;
  }

  d.se = fuse_session_new(&args, &ops, sizeof(ops), NULL);
  if (d.se == nullptr) {
    return 1;
  }
  onStopHooks.push([&] { fuse_session_destroy(d.se); });

  if (fuse_set_signal_handlers(d.se) != 0) {
    return 1;
  }
  onStopHooks.push([&] { fuse_remove_signal_handlers(d.se); });

  if (fuse_session_mount(d.se, opts.mountpoint) != 0) {
    return 1;
  }

  onStopHooks.push([&] { fuse_session_unmount(d.se); });

  int ret = -1;
  if (opts.singlethread) {
    ret = fuse_session_loop(d.se);
  } else {
    fuse_loop_cfg_set_clone_fd(config, opts.clone_fd);
    fuse_loop_cfg_set_idle_threads(config, d.maxIdleThreads);
    fuse_loop_cfg_set_max_threads(config, d.maxThreads);
    ret = fuse_session_loop_mt(d.se, config);
  }

  return ret ? 1 : 0;
}
}  // namespace hf3fs::fuse
