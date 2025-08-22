#include "RenderConfig.h"

#include "common/utils/StringUtils.h"
#include "common/utils/SysResource.h"

namespace hf3fs {

Result<flat::ReleaseVersion> parseReleaseVersion(const String &str, const flat::ReleaseVersion &actual) {
  {
    auto [r, tagDate] = scn::scan_tuple<uint32_t>(str, "{}");
    if (r && r.range().empty()) {
      return flat::ReleaseVersion(actual.getIsReleaseVersion(),
                                  tagDate,
                                  actual.getPatchVersion(),
                                  actual.getPipelineId(),
                                  actual.getCommitHashShort());
    }
  }
  {
    auto [r, tagDate, branch] = scn::scan_tuple<uint32_t, String>(str, "{}-{}");
    if (r && r.range().empty()) {
      return flat::ReleaseVersion(branch == "rel",
                                  tagDate,
                                  actual.getPatchVersion(),
                                  actual.getPipelineId(),
                                  actual.getCommitHashShort());
    }
  }
  {
    auto [r, tagDate, branch, patchVersion] = scn::scan_tuple<uint32_t, String, uint32_t>(str, "{}-{}-{}");
    if (r && r.range().empty()) {
      return flat::ReleaseVersion(branch == "rel",
                                  tagDate,
                                  patchVersion,
                                  actual.getPipelineId(),
                                  actual.getCommitHashShort());
    }
  }
  {
    auto [r, major, minor, patch, pipelineId] =
        scn::scan_tuple<uint8_t, uint8_t, uint8_t, uint32_t>(str, "{}.{}.{}-{}");
    if (r && r.range().empty()) {
      return flat::ReleaseVersion::fromV0(major,
                                          minor,
                                          patch,
                                          actual.commitHashShort,
                                          actual.buildTimeInSeconds,
                                          pipelineId);
    }
  }
  {
    auto [r, major, minor, patch] = scn::scan_tuple<uint8_t, uint8_t, uint8_t>(str, "{}.{}.{}");
    if (r && r.range().empty()) {
      return flat::ReleaseVersion::fromV0(major,
                                          minor,
                                          patch,
                                          actual.commitHashShort,
                                          actual.buildTimeInSeconds,
                                          actual.buildPipelineId);
    }
    if (!r) {
      return MAKE_ERROR_F(StatusCode::kInvalidArg,
                          "parse releaseVersion failed. SCN({}): {}",
                          toStringView(r.error().code()),
                          r.error().msg());
    }
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "parse releaseVersion failed: unknown format '{}'", str);
  }
}

Result<String> renderConfig(const String &configTemplate,
                            [[maybe_unused]] const flat::AppInfo *appInfo,
                            [[maybe_unused]] const std::map<String, String> *envs) {
  return configTemplate;
}
}  // namespace hf3fs
