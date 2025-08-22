#include "AppInfo.h"

namespace hf3fs::flat {
ServiceGroupInfo::ServiceGroupInfo(std::set<String> ss, std::vector<net::Address> es)
    : services(std::move(ss)),
      endpoints(std::move(es)) {}

bool ServiceGroupInfo::operator==(const ServiceGroupInfo &other) const { return serde::equals(*this, other); }

TagPair::TagPair(String k)
    : key(std::move(k)) {}

TagPair::TagPair(String k, String v)
    : key(std::move(k)),
      value(std::move(v)) {}

bool TagPair::operator==(const TagPair &other) const { return serde::equals(*this, other); }

ReleaseVersion::ReleaseVersion(bool isReleaseVersion,
                               uint32_t tagDate,
                               uint16_t patchVersion,
                               uint32_t pipelineId,
                               uint32_t commitHashShort) {
  HelperV1::setIsReleaseVersion(*this, isReleaseVersion);
  HelperV1::setTagDate(*this, tagDate);
  HelperV1::setPatchVersion(*this, patchVersion);
  HelperV1::setPipelineId(*this, pipelineId);
  HelperV1::setCommitHashShort(*this, commitHashShort);
}

ReleaseVersion ReleaseVersion::fromV0(uint8_t majorv,
                                      uint8_t minorv,
                                      uint8_t patchv,
                                      uint32_t shortH,
                                      uint64_t buildTimeInSec,
                                      uint32_t buildPipelineId) {
  ReleaseVersion rv;
  rv.majorVersion = majorv;
  rv.minorVersion = minorv;
  rv.patchVersion = patchv;
  rv.commitHashShort = shortH;
  rv.buildTimeInSeconds = buildTimeInSec;
  rv.buildPipelineId = buildPipelineId;
  return rv;
}

ReleaseVersion ReleaseVersion::fromVersionInfoV0() {
  ReleaseVersion rv;
  rv.majorVersion = VersionInfo::versionMajor();
  rv.minorVersion = VersionInfo::versionMinor();
  rv.patchVersion = VersionInfo::versionPatch();
  rv.commitHashShort = VersionInfo::commitHashShort();
  rv.buildTimeInSeconds = VersionInfo::buildTimeInSeconds();
  rv.buildPipelineId = VersionInfo::buildPipelineId();
  return rv;
}

ReleaseVersion ReleaseVersion::fromVersionInfo() {
  return ReleaseVersion(VersionInfo::isReleaseBranch(),
                        VersionInfo::gitTag(),
                        VersionInfo::gitTagSeqNum(),
                        VersionInfo::buildPipelineId(),
                        VersionInfo::commitHashShort());
}

std::strong_ordering ReleaseVersion::operator<=>(const ReleaseVersion &other) const {
  if (majorVersion != other.majorVersion) {
    return majorVersion <=> other.majorVersion;
  }
  switch (majorVersion) {
    case 0: {
#define CMP(method)                                               \
  do {                                                            \
    if (auto cmp = method <=> other.method; cmp != 0) return cmp; \
  } while (false)

      CMP(majorVersion);
      CMP(minorVersion);
      CMP(patchVersion);
      CMP(buildPipelineId);
      CMP(buildTimeInSeconds);
      CMP(commitHashShort);
      return std::strong_ordering::equal;
#undef CMP
    }
    case 1: {
#define CMP(method)                                                           \
  do {                                                                        \
    if (auto cmp = (method(*this)) <=> (method(other)); cmp != 0) return cmp; \
  } while (false)

      CMP(HelperV1::getTagDate);
      CMP(!HelperV1::getIsReleaseVersion);
      CMP(HelperV1::getPatchVersion);
      return std::strong_ordering::equal;
#undef CMP
    }
  }
  XLOGF(FATAL, "Shouldn't reach here: {}", majorVersion);
  return std::strong_ordering::equal;
}

bool ReleaseVersion::operator==(const ReleaseVersion &other) const {
  if (majorVersion != other.majorVersion) return false;
  switch (majorVersion) {
    case 0: {
#define EQ(method) method == other.method
      return EQ(commitHashShort) && EQ(majorVersion) && EQ(minorVersion) && EQ(patchVersion) &&
             EQ(buildTimeInSeconds) && EQ(buildPipelineId);
#undef EQ
    }
    case 1: {
#define EQ(method) (method(*this)) == (method(other))
      return EQ(HelperV1::getTagDate) && EQ(HelperV1::getIsReleaseVersion) && EQ(HelperV1::getPatchVersion);
#undef EQ
    }
  }
  XLOGF(FATAL, "Shouldn't reach here: {}", majorVersion);
  return true;
}

String ReleaseVersion::toString() const {
  switch (majorVersion) {
    case 0:
      return fmt::format("{}.{}.{}-{}-{:%Y%m%d}-{:08x}",
                         majorVersion,
                         minorVersion,
                         patchVersion,
                         buildPipelineId,
                         hf3fs::UtcTime::from(std::chrono::seconds(buildTimeInSeconds)),
                         commitHashShort);
    case 1:
      return fmt::format("{}-{}-{}-{}-{:08x}",
                         getTagDate(),
                         getIsReleaseVersion() ? "rel" : "dev",
                         getPatchVersion(),
                         getPipelineId(),
                         getCommitHashShort());
  }
  XLOGF(FATAL, "Shouldn't reach here: {}", majorVersion);
  return "UNKNOWN";
}

bool ReleaseVersion::HelperV1::getIsReleaseVersion(const ReleaseVersion &rv) {
  switch (rv.majorVersion) {
    case 0:
      return true;
    case 1:
      return rv.minorVersion;
  }
  XLOGF(FATAL, "Shouldn't reach here: {}", rv.majorVersion);
  return true;
}

void ReleaseVersion::HelperV1::setIsReleaseVersion(ReleaseVersion &rv, bool isReleaseVersion) {
  rv.minorVersion = static_cast<uint8_t>(isReleaseVersion);
}

uint32_t ReleaseVersion::HelperV1::getTagDate(const ReleaseVersion &rv) {
  switch (rv.majorVersion) {
    case 0:
      return 230626;
    case 1:
      return static_cast<uint32_t>(rv.buildTimeInSeconds >> 32);
  }
  XLOGF(FATAL, "Shouldn't reach here: {}", rv.majorVersion);
  return 0;
}

void ReleaseVersion::HelperV1::setTagDate(ReleaseVersion &rv, uint32_t tagDate) {
  auto pv = getPatchVersion(rv);
  rv.buildTimeInSeconds = (static_cast<uint64_t>(tagDate) << 32) + pv;
}

uint32_t ReleaseVersion::HelperV1::getPatchVersion(const ReleaseVersion &rv) {
  switch (rv.majorVersion) {
    case 0:
      return rv.patchVersion;
    case 1:
      return static_cast<uint32_t>(rv.buildTimeInSeconds & 0xFFFFFFFFu);
  }
  XLOGF(FATAL, "Shouldn't reach here: {}", rv.majorVersion);
  return 0;
}

void ReleaseVersion::HelperV1::setPatchVersion(ReleaseVersion &rv, uint32_t patchVersion) {
  auto td = getTagDate(rv);
  rv.buildTimeInSeconds = (static_cast<uint64_t>(td) << 32) + patchVersion;
}

uint32_t ReleaseVersion::HelperV1::getPipelineId(const ReleaseVersion &rv) { return rv.buildPipelineId; }

void ReleaseVersion::HelperV1::setPipelineId(ReleaseVersion &rv, uint32_t pipelineId) {
  rv.buildPipelineId = pipelineId;
}

uint32_t ReleaseVersion::HelperV1::getCommitHashShort(const ReleaseVersion &rv) { return rv.commitHashShort; }

void ReleaseVersion::HelperV1::setCommitHashShort(ReleaseVersion &rv, uint32_t commitHashShort) {
  rv.commitHashShort = commitHashShort;
}

bool FbsAppInfo::operator==(const FbsAppInfo &other) const { return serde::equals(*this, other); }

bool AppInfo::operator==(const AppInfo &other) const {
  return FbsAppInfo::operator==(other) && clusterId == other.clusterId && tags == other.tags;
}

std::vector<net::Address> extractAddresses(const std::vector<ServiceGroupInfo> &serviceGroups,
                                           const String &serviceName,
                                           std::optional<net::Address::Type> addressType) {
  std::vector<net::Address> addresses;
  for (const auto &group : serviceGroups) {
    if (group.services.contains(serviceName)) {
      std::copy_if(group.endpoints.begin(),
                   group.endpoints.end(),
                   std::back_inserter(addresses),
                   [&](const auto &addr) { return !addressType.has_value() || addr.type == *addressType; });
    }
  }
  return addresses;
}

int findTag(const std::vector<TagPair> &tags, std::string_view tag) {
  for (int i = 0; i < static_cast<int>(tags.size()); ++i) {
    if (tags[i].key == tag) return i;
  }
  return -1;
}

bool removeTag(std::vector<TagPair> &tags, std::string_view tag) {
  for (auto it = tags.begin(); it != tags.end(); ++it) {
    if (it->key == tag) {
      tags.erase(it);
      return true;
    }
  }
  return false;
}
}  // namespace hf3fs::flat
