#include "common/serde/MessagePacket.h"
#include "common/serde/Serde.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::serde::test {
namespace {

struct Demo {
  bool operator==(const Demo &) const = default;

  SERDE_STRUCT_FIELD(foo, int{}, nullptr);
  SERDE_STRUCT_FIELD(bar, std::string{}, nullptr);
};

TEST(TestMessagePacket, Normal) {
  // send.
  Demo req;
  req.foo = 100;
  req.bar = "Good";
  MessagePacket send(req);
  auto bytes = serde::serialize(send);
  XLOGF(INFO, "send json: {}, binary: {:02X}", send, fmt::join(bytes, "-"));

  // recv.
  MessagePacket recv;
  ASSERT_OK(serde::deserialize(recv, bytes));
  Demo rsp;
  ASSERT_OK(serde::deserialize(rsp, recv.payload));
  ASSERT_EQ(req, rsp);
  XLOGF(INFO, "rsp json: {}", rsp);
}

}  // namespace
}  // namespace hf3fs::serde::test
