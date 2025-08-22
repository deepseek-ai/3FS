#include <boost/core/ignore_unused.hpp>
#include <boost/uuid/random_generator.hpp>
#include <boost/uuid/uuid.hpp>
#include <cstdlib>
#include <double-conversion/utils.h>
#include <fmt/core.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "common/utils/SerDeser.h"
#include "common/utils/Uuid.h"

namespace hf3fs::tests {
namespace {

std::pair<boost::uuids::uuid, boost::uuids::uuid> gen(auto &generator) {
  boost::ignore_unused(generator());
  int pipefd[2];
  EXPECT_NE(pipe(pipefd), -1);

  auto pid = fork();
  EXPECT_NE(pid, -1);
  auto uuid = generator();
  fmt::print("{} {}\n", pid == 0 ? "child " : "father", boost::lexical_cast<std::string>(uuid));
  if (pid == 0) {
    close(pipefd[0]);
    write(pipefd[1], uuid.data, sizeof(uuid));
    exit(0);
  }

  close(pipefd[1]);
  boost::uuids::uuid another;
  read(pipefd[0], another.data, sizeof(another));

  int status;
  EXPECT_EQ(wait(&status), pid);

  return std::make_pair(uuid, another);
}

TEST(Uuid, DISABLED_fork) {
  // disable this test by default because it hangs in gitlab CI.
  static thread_local boost::uuids::random_generator_mt19937 mt19937;
  static thread_local boost::uuids::random_generator random;

  fmt::print("random_generator_mt19937\n");
  for (size_t i = 0; i < 10; i++) {
    auto a = gen(mt19937);
    EXPECT_EQ(a.first, a.second);  // shouldn't use mt19937
  }
  fmt::print("random_generator\n");
  for (size_t i = 0; i < 10; i++) {
    auto b = gen(random);
    EXPECT_NE(b.first, b.second);
  }
  fmt::print("Uuid\n");
  for (size_t i = 0; i < 10; i++) {
    auto c = gen(Uuid::random);
    EXPECT_NE(c.first, c.second);
  }
}

TEST(Uuid, testZero) {
  uint8_t buffer[Uuid::static_size()] = {0};
  Uuid zero = Uuid::zero();
  Uuid def = Uuid{};

  ASSERT_EQ(memcmp(buffer, zero.data, Uuid::static_size()), 0);
  ASSERT_EQ(zero, def);

  Uuid zero1 = Uuid::zero();
  ASSERT_EQ(zero, zero1);
}

TEST(Uuid, testRandom) {
  Uuid a = Uuid::random();
  Uuid b = Uuid::random();

  ASSERT_NE(a, b);
}

TEST(Uuid, testToString) {
  Uuid a = Uuid::random();
  std::cout << a.toHexString() << std::endl;

  Uuid zero = Uuid::zero();
  std::cout << zero.toHexString() << std::endl;
  ASSERT_EQ(zero.toHexString(), "00000000-0000-0000-0000-000000000000");
}

TEST(Uuid, Compare) {
  for (size_t i = 0; i < 100; i++) {
    Uuid a = Uuid::random();
    Uuid b = Uuid::random();
    ASSERT_EQ((a < b), (b > a));
    ASSERT_NE((a < b), (b < a));
    ASSERT_TRUE(a > Uuid::zero());
    ASSERT_TRUE(b > Uuid::zero());
    ASSERT_TRUE(a < Uuid::max());
    ASSERT_TRUE(b < Uuid::max());

    auto aser = Serializer::serRawArgs(a);
    auto bser = Serializer::serRawArgs(b);
    ASSERT_EQ((a < b), (aser < bser));
    ASSERT_EQ((a > b), (aser > bser));
    ASSERT_TRUE(aser > Serializer::serRawArgs(Uuid::zero()));
    ASSERT_TRUE(bser > Serializer::serRawArgs(Uuid::zero()));
    ASSERT_TRUE(aser < Serializer::serRawArgs(Uuid::max()));
    ASSERT_TRUE(bser < Serializer::serRawArgs(Uuid::max()));
  }
}

TEST(Uuid, Gen) {
  for (size_t i = 0; i < 100; i++) {
    fmt::println("{}", Uuid::random());
  }
}

}  // namespace
}  // namespace hf3fs::tests