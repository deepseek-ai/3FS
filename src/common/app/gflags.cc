#include <gflags/gflags.h>

DEFINE_bool(release_version, false, "Display release version");

DEFINE_string(app_cfg, "", "Path to .toml config file which contains specific config to this node");
DEFINE_bool(dump_default_app_cfg, false, "Dump default app config to stdout");

DEFINE_string(launcher_cfg, "", "Path to .toml config file which contains launcher config");
DEFINE_bool(dump_default_launcher_cfg, false, "Dump default launcher config to stdout");

DEFINE_bool(use_local_cfg, false, "Use local config instead of remote config for bootstrap");

DEFINE_string(cfg_persist_prefix,
              "",
              "If not empty, dump the full config to prefix_timestamp whenever config is updated");
