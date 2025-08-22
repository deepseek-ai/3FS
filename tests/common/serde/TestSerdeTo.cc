#include <folly/container/OrderedMap.h>

#include "common/net/ib/RDMABuf.h"
#include "common/serde/Serde.h"
#include "common/utils/Address.h"
#include "common/utils/Path.h"
#include "common/utils/Result.h"
#include "common/utils/Size.h"
#include "common/utils/StrongType.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

STRONG_TYPEDEF(uint32_t, ID);
enum class Type { A, B, C, D };

struct Demo {
  bool operator==(const Demo &) const = default;

  SERDE_STRUCT_FIELD(name, std::string{"Test"}, nullptr);
  SERDE_STRUCT_FIELD(size, 1_MB, nullptr);
  SERDE_STRUCT_FIELD(addr, net::Address{}, nullptr);
  SERDE_STRUCT_FIELD(id, ID{12138}, nullptr);
  SERDE_STRUCT_FIELD(type, Type::C, nullptr);
  SERDE_STRUCT_FIELD(values, (std::set<ID>{ID{1}, ID{2}}), nullptr);
  SERDE_STRUCT_FIELD(map, (std::map<std::string, Size>{{"a", 1_MB}, {"b", 2_MB}}), nullptr);
  SERDE_STRUCT_FIELD(option, std::optional<std::string>("OK"), nullptr);
  SERDE_STRUCT_FIELD(variant, (std::variant<int, std::string>("OK")), nullptr);
  SERDE_STRUCT_FIELD(path, Path{"/a/b/c"}, nullptr);
};

TEST(TestSerde, ToToml) {
  Demo demo;
  XLOGF(INFO, "Toml: {}", serde::toTomlString(demo));

  struct Demo2 {
    SERDE_STRUCT_FIELD(one, Demo{});
    SERDE_STRUCT_FIELD(two, Demo{});
  };
  XLOGF(INFO, "Toml: {}", serde::toTomlString(Demo2{}));
}

TEST(TestSerde, ToJson) {
  Demo demo;
  demo.option = std::nullopt;  // count - 1
  demo.variant = 100;
  demo.path = "/x/y/z";
  XLOGF(INFO, "Json: {}", serde::toJsonString(demo));

  auto out = serde::serialize(demo);
  Demo des;
  ASSERT_NE(des.path, demo.path);
  ASSERT_OK(serde::deserialize(des, out));
  ASSERT_EQ(des, demo);
  ASSERT_EQ(des.path, demo.path);
}

TEST(TestOrderedMap, Normal) {
  folly::OrderedMap<int, int> map;
  constexpr auto N = 1000;
  for (auto i = 0; i < N; ++i) {
    map[i] = i;
  }

  auto idx = 0;
  for (auto [k, v] : map) {
    ASSERT_EQ(k, idx);
    ASSERT_EQ(v, idx);
    ++idx;
  }
  ASSERT_EQ(idx, N);

  for (auto i = 0; i < N; i += 2) {
    map.erase(i);
  }

  idx = 1;
  for (auto [k, v] : map) {
    ASSERT_EQ(k, idx);
    ASSERT_EQ(v, idx);
    idx += 2;
  }
  ASSERT_EQ(idx, N + 1);

  for (auto i = 0; i < N; i += 2) {
    map[i] = i;
  }
  ASSERT_EQ(map.size(), N);

  idx = 1;
  for (auto [k, v] : map) {
    ASSERT_EQ(k, idx);
    ASSERT_EQ(v, idx);
    idx += 2;
    if (idx == N + 1) {
      idx = 0;
    }
  }
  ASSERT_EQ(idx, N);

  auto item = std::move(map);
  ASSERT_EQ(item.size(), N);
  ASSERT_EQ(map.size(), 0);
  item.clear();
  ASSERT_TRUE(item.empty());
}

TEST(TestSerde, RDMARemoteBuf) {
  net::RDMARemoteBuf buf;
  XLOGF(INFO, "buf is {}", serde::toJsonString(buf));

  auto rkeys = buf.rkeys();
  rkeys[0].devId = 1;
  rkeys[0].rkey = 100;
  rkeys[1].devId = 2;
  rkeys[1].rkey = 200;

  buf = net::RDMARemoteBuf(0x1000, 1024, rkeys);
  XLOGF(INFO, "buf is {}", serde::toJsonString(buf));

  auto out = serde::serialize(buf);
  ASSERT_EQ(out.size(), 8 + 8 + 1 + 8 * 2);  // addr + size + len + rkey * 2

  net::RDMARemoteBuf des;
  ASSERT_OK(serde::deserialize(des, out));
  ASSERT_EQ(des, buf);
}

TEST(TestSerde, DoubleFormat) {
  struct Foo {
    SERDE_STRUCT_FIELD(value, 0ul);
  };

  Foo foo;
  foo.value = ~0ul / 2;
  XLOGF(INFO, "foo is {}", foo);

  auto json = serde::toJsonString(foo);
  Foo bar;
  ASSERT_OK(serde::fromJsonString(bar, json));
  ASSERT_EQ(bar.value, foo.value);
}

}  // namespace
}  // namespace hf3fs::test
