#include "fbs/mgmtd/NodeInfo.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {
TEST(NodeSelector, testSelectByTrifficZone) {
  auto createNode = [](const String &zone) {
    flat::NodeInfo n;
    if (!zone.empty()) {
      n.tags = {flat::TagPair(flat::kTrafficZoneTagKey, zone)};
    }
    return n;
  };
  auto n0 = createNode("");
  auto n1 = createNode("zone1");
  auto n2 = createNode("zone2");

  {
    auto sel = flat::selectNodeByTrafficZone("");
    ASSERT_TRUE(sel(n0));
    ASSERT_TRUE(sel(n1));
    ASSERT_TRUE(sel(n2));
  }

  {
    auto sel = flat::selectNodeByTrafficZone("zone1");
    ASSERT_TRUE(!sel(n0));
    ASSERT_TRUE(sel(n1));
    ASSERT_TRUE(!sel(n2));
  }

  {
    auto sel = flat::selectNodeByTrafficZone(" zone1");
    ASSERT_TRUE(!sel(n0));
    ASSERT_TRUE(sel(n1));
    ASSERT_TRUE(!sel(n2));
  }

  {
    auto sel = flat::selectNodeByTrafficZone(" zone1 ");
    ASSERT_TRUE(!sel(n0));
    ASSERT_TRUE(sel(n1));
    ASSERT_TRUE(!sel(n2));
  }

  {
    auto sel = flat::selectNodeByTrafficZone(" zone1 ,");
    ASSERT_TRUE(!sel(n0));
    ASSERT_TRUE(sel(n1));
    ASSERT_TRUE(!sel(n2));
  }

  {
    auto sel = flat::selectNodeByTrafficZone(" zone1 , zone2");
    ASSERT_TRUE(!sel(n0));
    ASSERT_TRUE(sel(n1));
    ASSERT_TRUE(sel(n2));
  }
}
}  // namespace
}  // namespace hf3fs::test
