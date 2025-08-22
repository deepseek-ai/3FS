#include "ServerLauncher.h"

namespace hf3fs::core {
Result<Void> ServerLauncherBase::parseFlags(int *argc, char ***argv) {
  static constexpr std::string_view appConfigPrefix = "--app_config.";
  static constexpr std::string_view launcherConfigPrefix = "--launcher_config.";
  RETURN_ON_ERROR(ApplicationBase::parseFlags(appConfigPrefix, argc, argv, appConfigFlags_));
  RETURN_ON_ERROR(ApplicationBase::parseFlags(launcherConfigPrefix, argc, argv, launcherConfigFlags_));
  return Void{};
}

}  // namespace hf3fs::core
