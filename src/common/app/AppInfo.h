#pragma once

#include <compare>
#include <scn/tuple_return.h>

#include "common/app/NodeId.h"
#include "common/serde/SerdeComparisons.h"
#include "common/serde/SerdeHelper.h"
#include "common/utils/Address.h"
#include "common/utils/UtcTime.h"
#include "common/utils/VersionInfo.h"

namespace hf3fs::flat {

struct ServiceGroupInfo : public serde::SerdeHelper<ServiceGroupInfo> {
  ServiceGroupInfo() = default;
  ServiceGroupInfo(std::set<String> ss, std::vector<net::Address> es);

  bool operator==(const ServiceGroupInfo &other) const;

  SERDE_STRUCT_FIELD(services, std::set<String>{});
  SERDE_STRUCT_FIELD(endpoints, std::vector<net::Address>{});
};

struct TagPair : public serde::SerdeHelper<TagPair> {
  TagPair() = default;
  explicit TagPair(String k);
  TagPair(String k, String v);

  bool operator==(const TagPair &other) const;

  SERDE_STRUCT_FIELD(key, String{});
  SERDE_STRUCT_FIELD(value, String{});
};

struct ReleaseVersion : public serde::SerdeHelper<ReleaseVersion> {
  ReleaseVersion() = default;

  ReleaseVersion(bool isReleaseVersion,
                 uint32_t tagDate,
                 uint16_t patchVersion,
                 uint32_t pipelineId,
                 uint32_t commitHashShort);

  static ReleaseVersion fromV0(uint8_t majorv,
                               uint8_t minorv,
                               uint8_t patchv,
                               uint32_t shortH,
                               uint64_t buildTimeInSec,
                               uint32_t buildPipelineId);

  static ReleaseVersion fromVersionInfoV0();
  static ReleaseVersion fromVersionInfo();

  std::strong_ordering operator<=>(const ReleaseVersion &other) const;
  bool operator==(const ReleaseVersion &other) const;

  uint32_t getTagDate() const { return HelperV1::getTagDate(*this); }

  uint32_t getPatchVersion() const { return HelperV1::getPatchVersion(*this); }

  bool getIsReleaseVersion() const { return HelperV1::getIsReleaseVersion(*this); }

  uint32_t getPipelineId() const { return HelperV1::getPipelineId(*this); }

  uint32_t getCommitHashShort() const { return HelperV1::getCommitHashShort(*this); }

  String toString() const;

 private:
  struct HelperV1 {
    static bool getIsReleaseVersion(const ReleaseVersion &rv);
    static void setIsReleaseVersion(ReleaseVersion &rv, bool isReleaseVersion);
    static uint32_t getTagDate(const ReleaseVersion &rv);
    static void setTagDate(ReleaseVersion &rv, uint32_t tagDate);
    static uint32_t getPatchVersion(const ReleaseVersion &rv);
    static void setPatchVersion(ReleaseVersion &rv, uint32_t patchVersion);
    static uint32_t getPipelineId(const ReleaseVersion &rv);
    static void setPipelineId(ReleaseVersion &rv, uint32_t pipelineId);
    static uint32_t getCommitHashShort(const ReleaseVersion &rv);
    static void setCommitHashShort(ReleaseVersion &rv, uint32_t commitHashShort);

   private:
    // just for demonstration
    // structVersion_ <=> rv.majorVersion
    // isReleaseVersion_ <=> rv.minorVersion
    // <tagDate_, patchVersion_> <=> rv.buildTimeInSeconds
    // pipelineId_ <=> rv.pipelineId
    // commitHashShort_ <=> rv.commitHashShort
    const uint8_t structVersion_ = 1;
    bool isReleaseVersion_ = false;
    uint32_t patchVersion_ = 0;
    uint32_t tagDate_ = 0;
    uint32_t pipelineId_ = 0;
    uint32_t commitHashShort_ = 0;
  };

  // NOTE: other fields are just for storage when majorVersion != 0
  SERDE_STRUCT_FIELD(majorVersion, uint8_t(1));
  SERDE_STRUCT_FIELD(minorVersion, uint8_t(0));
  SERDE_STRUCT_FIELD(patchVersion, uint8_t(0));
  SERDE_STRUCT_FIELD(commitHashShort, uint32_t(0));
  SERDE_STRUCT_FIELD(buildTimeInSeconds, uint64_t(0));
  SERDE_STRUCT_FIELD(buildPipelineId, uint32_t(0));
};

struct FbsAppInfo : public serde::SerdeHelper<FbsAppInfo> {
  bool operator==(const FbsAppInfo &other) const;

  SERDE_STRUCT_FIELD(nodeId, NodeId(0));
  SERDE_STRUCT_FIELD(hostname, String{});
  SERDE_STRUCT_FIELD(pid, uint32_t(0));
  SERDE_STRUCT_FIELD(serviceGroups, std::vector<ServiceGroupInfo>{});
  SERDE_STRUCT_FIELD(releaseVersion, ReleaseVersion{});
  SERDE_STRUCT_FIELD(podname, String{});
};

struct AppInfo : FbsAppInfo {
  bool operator==(const AppInfo &other) const;

  String clusterId;
  std::vector<TagPair> tags;
};

std::vector<net::Address> extractAddresses(const std::vector<ServiceGroupInfo> &serviceGroups,
                                           const String &serviceName,
                                           std::optional<net::Address::Type> addressType = std::nullopt);

inline constexpr auto kDisabledTagKey = "Disable";
inline constexpr auto kTrafficZoneTagKey = "TrafficZone";

int findTag(const std::vector<TagPair> &tags, std::string_view tag);
bool removeTag(std::vector<TagPair> &tags, std::string_view tag);
}  // namespace hf3fs::flat

FMT_BEGIN_NAMESPACE

template <>
struct formatter<hf3fs::flat::ReleaseVersion> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::flat::ReleaseVersion val, FormatContext &ctx) const -> decltype(ctx.out()) {
    return fmt::format_to(ctx.out(), "{}", val.toString());
  }
};

template <>
struct formatter<hf3fs::flat::TagPair> : formatter<std::string_view> {
  template <typename FormatContext>
  auto format(const hf3fs::flat::TagPair val, FormatContext &ctx) const -> decltype(ctx.out()) {
    if (val.value.empty()) {
      return fmt::format_to(ctx.out(), "{}", val.key);
    }
    return fmt::format_to(ctx.out(), "{}={}", val.key, val.value);
  }
};

FMT_END_NAMESPACE
