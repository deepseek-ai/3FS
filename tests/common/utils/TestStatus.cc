#include <gtest/gtest.h>
#include <string>

#include "common/utils/Status.h"

namespace hf3fs::tests {
namespace {
TEST(Status, testCtor) {
  Status s1(1);
  ASSERT_EQ(s1.code(), 1);
  ASSERT_TRUE(s1.message().empty());
  ASSERT_TRUE(!s1.hasPayload());

  Status s2(1, "test");
  ASSERT_EQ(s2.code(), 1);
  ASSERT_EQ(s2.message(), "test");
  ASSERT_TRUE(!s2.hasPayload());
  ASSERT_EQ(fmt::format("{}", s2), "NotImplemented(1) test");
}

TEST(Status, testPayload) {
  Status s0(1);
  s0.setPayload<int>(5);
  ASSERT_TRUE(s0.hasPayload());
  ASSERT_EQ(*s0.payload<int>(), 5);

  s0.setPayload<std::string>("abc");
  ASSERT_TRUE(s0.hasPayload());
  ASSERT_EQ(*s0.payload<std::string>(), "abc");

  s0.resetPayload();
  ASSERT_TRUE(!s0.hasPayload());

  s0.emplacePayload<std::string>(3, 'a');
  ASSERT_EQ(*s0.payload<std::string>(), "aaa");
}

TEST(Status, testCopy) {
  Status s2(1, "test");
  s2.setPayload<std::string>("abc");
  ASSERT_EQ(s2.code(), 1);
  ASSERT_EQ(s2.message(), "test");
  ASSERT_TRUE(s2.hasPayload());
  ASSERT_EQ(*s2.payload<std::string>(), "abc");

  Status s3 = s2;
  ASSERT_EQ(s2.code(), 1);
  ASSERT_EQ(s2.message(), "test");
  ASSERT_TRUE(s2.hasPayload());
  ASSERT_EQ(*s2.payload<std::string>(), "abc");
  ASSERT_EQ(s3.code(), 1);
  ASSERT_EQ(s3.message(), "test");
  ASSERT_TRUE(s3.hasPayload());
  ASSERT_EQ(*s3.payload<std::string>(), "abc");

  auto *ps3 = &s3;
  s3 = *ps3;
  ASSERT_EQ(s3.code(), 1);
  ASSERT_EQ(s3.message(), "test");
  ASSERT_TRUE(s3.hasPayload());
  ASSERT_EQ(*s3.payload<std::string>(), "abc");

  Status s4(0);
  s4 = s3;
  ASSERT_EQ(s3.code(), 1);
  ASSERT_EQ(s3.message(), "test");
  ASSERT_TRUE(s3.hasPayload());
  ASSERT_EQ(*s3.payload<std::string>(), "abc");
  ASSERT_EQ(s4.code(), 1);
  ASSERT_EQ(s4.message(), "test");
  ASSERT_TRUE(s4.hasPayload());
  ASSERT_EQ(*s4.payload<std::string>(), "abc");
}

TEST(Status, testMove) {
  Status s2(1, "test");
  s2.setPayload<std::string>("abc");
  ASSERT_EQ(s2.code(), 1);
  ASSERT_EQ(s2.message(), "test");
  ASSERT_TRUE(s2.hasPayload());
  ASSERT_EQ(*s2.payload<std::string>(), "abc");

  Status s3 = std::move(s2);
  ASSERT_EQ(s2.code(), StatusCode::kOK);
  ASSERT_TRUE(s2.message().empty());
  ASSERT_TRUE(!s2.hasPayload());
  ASSERT_EQ(s3.code(), 1);
  ASSERT_EQ(s3.message(), "test");
  ASSERT_TRUE(s3.hasPayload());
  ASSERT_EQ(*s3.payload<std::string>(), "abc");

  auto *ps3 = &s3;
  s3 = std::move(*ps3);
  ASSERT_EQ(s3.code(), 1);
  ASSERT_EQ(s3.message(), "test");
  ASSERT_TRUE(s3.hasPayload());
  ASSERT_EQ(*s3.payload<std::string>(), "abc");

  Status s4(0);
  s4 = std::move(s3);
  ASSERT_EQ(s3.code(), StatusCode::kOK);
  ASSERT_TRUE(s3.message().empty());
  ASSERT_TRUE(!s3.hasPayload());
  ASSERT_EQ(s4.code(), 1);
  ASSERT_EQ(s4.message(), "test");
  ASSERT_TRUE(s4.hasPayload());
  ASSERT_EQ(*s4.payload<std::string>(), "abc");
}
}  // namespace
}  // namespace hf3fs::tests
