#include <cstdint>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <type_traits>
#include <typeinfo>

#include "common/serde/Serde.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/Uuid.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "fbs/meta/Service.h"
#include "fbs/meta/Utils.h"
#include "meta/store/DirEntry.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "tests/GtestHelpers.h"
#include "tests/meta/MetaTestBase.h"

DEFINE_string(path, "/a", "path to print");

namespace hf3fs::meta::server {

using hf3fs::meta::InodeId;

static void print(const Path &path) {
  fmt::print("path {}\n", path);
  fmt::print("abs {}, relative {}\n", path.is_absolute(), path.is_relative());
  fmt::print("has root {}, {}\n", path.has_root_path(), path.root_path());
  fmt::print("has relative {}, {}\n", path.has_relative_path(), path.relative_path());
  fmt::print("has branch {}, {}\n", path.has_parent_path(), path.parent_path());
  fmt::print("has stem {}, {}\n", path.has_stem(), path.stem());
  fmt::print("has filename {}, {}, is dot {}, is dot dot {}\n",
             path.has_filename(),
             path.filename(),
             path.filename_is_dot(),
             path.filename_is_dot_dot());
  fmt::print("has leaf {}, {}", !path.empty(), path.filename());
  fmt::print("size {}, components {}, first {}, last {}\n",
             path.size(),
             std::distance(path.begin(), path.end()),
             *path.begin(),
             *path.rbegin());
}

TEST(TestMetaCommon, Json) {
  fmt::print("{}\n", StatReq(flat::UserInfo(), Path("random-path"), AtFlags(AT_SYMLINK_FOLLOW)));
  fmt::print("{}\n", StatReq(flat::UserInfo(), Path("path"), AtFlags(AT_SYMLINK_FOLLOW)));
  fmt::print("{}\n", OpenReq(flat::UserInfo(), Path("path"), std::nullopt, 0));
  fmt::print("{}\n", OpenReq(flat::UserInfo(), Path("path"), SessionInfo(ClientId::random(), Uuid::random()), 0));
  fmt::print("{}\n", DirEntry::newSymlink(InodeId::root(), "symlink", InodeId(folly::Random::rand64())));
}

TEST(TestMetaCommon, PathAppend) {}

TEST(TestMetaCommon, PrintPath) {
  Path path(FLAGS_path);
  print(path);
}

TEST(TestMetaCommon, Path) {
  ASSERT_OK(PathAt("a").validForCreate());
  ASSERT_OK(PathAt("a/b").validForCreate());
  ASSERT_OK(PathAt("a/./b").validForCreate());
  ASSERT_OK(PathAt("a/b/../c").validForCreate());

  ASSERT_ERROR(PathAt("/").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt("").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt(".").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt("..").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt("a/").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt("a/.").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt("a/..").validForCreate(), StatusCode::kInvalidArg);
  ASSERT_ERROR(PathAt("a/b/../.").validForCreate(), StatusCode::kInvalidArg);

  for (auto [path, parent, ppath, fname] :
       std::vector<std::tuple<Path, bool, Path, std::string>>{{"/a", true, "/", "a"},
                                                              {"/", false, "", "/"},
                                                              {"/a/", true, "/a", "."},
                                                              {"/a/..", true, "/a", ".."},
                                                              {"/a/../", true, "/a/..", "."},
                                                              {"a/.././b/c", true, "a/.././b", "c"}}) {
    ASSERT_EQ(path.has_parent_path(), parent);
    ASSERT_EQ(path.parent_path(), ppath);
    ASSERT_EQ(path.filename(), fname);
  }
}

TEST(TestMetaCommon, ChunkSize) {
  auto chunk = Layout::ChunkSize(1 << 20);
  auto length = 1 * chunk;
  static_assert(std::is_same_v<uint64_t, decltype(length)>);
  auto gb = Layout::ChunkSize(1 << 30);
  auto tb = (1 << 10) * gb;
  auto pb = (1 << 20) * gb;
  auto eb = (1 << 30) * gb;
  ASSERT_EQ(gb, 1ull << 30);
  ASSERT_EQ(tb, 1ull << 40);
  ASSERT_EQ(pb, 1ull << 50);
  ASSERT_EQ(eb, 1ull << 60);

  for (size_t i = 0; i < 10; i++) {
    auto val = folly::Random::rand32();
    auto chunk = Layout::ChunkSize(val);
    auto ser1 = serde::serialize(val);
    auto ser2 = serde::serialize(chunk);
    ASSERT_EQ(ser1, ser2);
    auto des1 = Layout::ChunkSize();
    serde::deserialize(des1, ser1);
    ASSERT_EQ(des1, chunk);
    auto des2 = Layout::ChunkSize();
    serde::deserialize(des2, ser2);
    ASSERT_EQ(des2, chunk);
  }
}

TEST(TestMetaCommon, Compare) {
  EXPECT_LT('\xff', '0');
  EXPECT_GT(std::string("\xff"), std::string("0"));
}

meta::VersionedLength mergeAll(const std::vector<VersionedLength> &vec) {
  auto hint = meta::VersionedLength{0, 0};
  for (auto l : vec) {
    hint = *VersionedLength::mergeHint(hint, l);
  }
  return hint;
}

TEST(TestMetaCommon, VersionedLength) {
  for (size_t i = 0; i < 100; i++) {
    auto l = VersionedLength{folly::Random::rand32(), folly::Random::rand32()};
    ASSERT_EQ(VersionedLength::mergeHint(l, VersionedLength{0, 0}), l);
    ASSERT_EQ(VersionedLength::mergeHint(l, std::nullopt), std::nullopt);
    ASSERT_EQ(VersionedLength::mergeHint(VersionedLength{0, 0}, l), l);
    ASSERT_EQ(VersionedLength::mergeHint(std::nullopt, l), std::nullopt);
  }

  for (size_t i = 0; i < 100; i++) {
    auto l1 = VersionedLength{folly::Random::rand32(), folly::Random::rand32()};
    auto l2 = VersionedLength{folly::Random::rand32(), folly::Random::rand32()};
    ASSERT_EQ(VersionedLength::mergeHint(l1, l2), VersionedLength::mergeHint(l2, l1));
  }

  auto l1 = VersionedLength{34, 0};
  auto l2 = VersionedLength{100, 0};
  auto l3 = VersionedLength{60, 1};
  auto l4 = VersionedLength{200, 3};
  auto l5 = VersionedLength{32, 4};
  ASSERT_EQ(VersionedLength::mergeHint(l1, l2), l2);
  ASSERT_EQ(VersionedLength::mergeHint(l1, l3), l3);
  ASSERT_EQ(VersionedLength::mergeHint(l1, l5), l1);
  ASSERT_EQ(mergeAll({l1, l2, l3}), l2);
  ASSERT_EQ(mergeAll({l2, l3}), l2);
  ASSERT_EQ(mergeAll({l1, l2, l3, l4}), l4);
  ASSERT_EQ(mergeAll({l2, l3, l4}), l4);
  ASSERT_EQ(mergeAll({l1, l2, l3, l4, l5}), l4);
}

TEST(TestMetaCommon, RetryThrottled) { ASSERT_TRUE(ErrorHandling::retryable(Status(TransactionCode::kThrottled))); }

}  // namespace hf3fs::meta::server
