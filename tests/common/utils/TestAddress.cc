#include "common/utils/Address.h"
#include "common/utils/ConfigBase.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::net::test {
namespace {

TEST(TestAddress, Normal) {
  constexpr std::string_view str = "IPoIB://192.168.1.1:8000";
  auto addr = Address::fromString(str);
  ASSERT_TRUE(addr.isTCP());
  ASSERT_EQ(addr.ipStr(), "192.168.1.1");
  ASSERT_EQ(addr.port, 8000);
  ASSERT_EQ(addr.type, Address::IPoIB);
  ASSERT_EQ(addr.str(), str);

  addr = Address::fromString("tcp://127.0.0.1:8888");
  ASSERT_TRUE(addr);
  ASSERT_EQ(addr.type, Address::TCP);
  ASSERT_EQ(addr.port, 8888);
}

static_assert(config::IsPrimitive<Address>, "address is not primitive");
auto kDefaultAddr = Address::from("tcp://192.168.1.1:8000").value();
class Config : public ConfigBase<Config> {
  CONFIG_ITEM(addr, Address{kDefaultAddr});
};

TEST(TestAddress, Config) {
  Config cfg;
  ASSERT_EQ(cfg.toString(), "addr = 'TCP://192.168.1.1:8000'");
}

}  // namespace
}  // namespace hf3fs::net::test
