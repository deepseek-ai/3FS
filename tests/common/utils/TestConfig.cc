#include <chrono>
#include <folly/experimental/TestUtil.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <type_traits>

#include "common/utils/ConfigBase.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

enum class Channel { Red, Green, Blue };

class Config : public ConfigBase<Config> {
  CONFIG_HOT_UPDATED_ITEM(string, "ing");
  CONFIG_HOT_UPDATED_ITEM(channel, Channel::Red);
  CONFIG_HOT_UPDATED_ITEM(optional, std::optional<std::string>{});
  CONFIG_SECT(sect, {
    CONFIG_HOT_UPDATED_ITEM(val, 100l, [](const int64_t &val) { return val < 200; });
    CONFIG_HOT_UPDATED_ITEM(foo, "foo");
    CONFIG_HOT_UPDATED_ITEM(score, 0.0);
    CONFIG_HOT_UPDATED_ITEM(ok, false);
    CONFIG_HOT_UPDATED_ITEM(succ, true);
    CONFIG_SECT(sub, {
      CONFIG_HOT_UPDATED_ITEM(val, 100l);
      CONFIG_HOT_UPDATED_ITEM(foo, "foo");
    });
  });
  CONFIG_SECT(vec, {
    CONFIG_HOT_UPDATED_ITEM(int_vec, std::vector<int64_t>({1, 2}), [](const std::vector<int64_t> &v) {
      return v.size() <= 2;
    });
    CONFIG_HOT_UPDATED_ITEM(str_vec, std::vector<std::string>({"foo", "foo"}));
    CONFIG_HOT_UPDATED_ITEM(double_vec, std::vector<double>({1.0, 2.0}));
    CONFIG_HOT_UPDATED_ITEM(bool_vec, std::vector<bool>({true, false}));
    CONFIG_HOT_UPDATED_ITEM(enum_vec, std::vector<Channel>({Channel::Red, Channel::Green}));
  });
};
static_assert(std::is_same_v<decltype(Config{}.sect().foo()), const std::string &>, "ok");
static_assert(std::is_same_v<decltype(Config{}.sect().val()), int64_t>, "ok");
static_assert(std::is_same_v<decltype(Config{}.vec().int_vec()), const std::vector<int64_t> &>, "ok");

class BigConfig : public ConfigBase<BigConfig> {
  CONFIG_OBJ(a, Config);
  CONFIG_OBJ(b, Config, [](Config &c) { c.set_string("replaced"); });
};

class ArrayConfig : public ConfigBase<ArrayConfig> {
  CONFIG_OBJ_ARRAY(cfgs, Config, 4);
} array;

TEST(TestConfig, Normal) {
  Config cfg;
  ASSERT_EQ(cfg.string(), "ing");
  ASSERT_FALSE(cfg.optional().has_value());
  ASSERT_EQ(cfg.sect().foo(), "foo");
  ASSERT_EQ(cfg.sect().val(), 100);
  ASSERT_EQ(cfg.sect().ok(), false);
  ASSERT_EQ(cfg.sect().succ(), true);
  ASSERT_EQ(cfg.sect().sub().val(), 100);

  toml::table result = toml::parse(R"(
    optional = "has value"

    [sect]
    foo = "bar"
    val = 123
    score = 1
    ok = true

    [sect.sub]
    val = 200
  )");
  ASSERT_EQ(cfg.sect().foo(), "foo");
  ASSERT_EQ(cfg.sect().val(), 100);
  ASSERT_EQ(cfg.sect().ok(), false);
  ASSERT_EQ(cfg.sect().sub().val(), 100);

  ASSERT_TRUE(cfg.update(result));
  ASSERT_TRUE(cfg.optional().has_value());
  ASSERT_EQ(cfg.optional().value(), "has value");
  ASSERT_EQ(cfg.sect().foo(), "bar");
  ASSERT_EQ(cfg.sect().val(), 123);
  ASSERT_EQ(cfg.sect().ok(), true);
  ASSERT_EQ(cfg.sect().sub().val(), 200);

  ASSERT_TRUE(cfg.sect().set_val(150));
  ASSERT_FALSE(cfg.sect().set_val(200));

  // support copy.
  auto other = cfg;
  ASSERT_EQ(other.sect().foo(), "bar");
  ASSERT_EQ(other.sect().val(), 150);

  ASSERT_TRUE(cfg.update(result));
  ASSERT_EQ(cfg.sect().val(), 123);
  ASSERT_EQ(other.sect().val(), 150);
}

TEST(TestConfig, Vector) {
  Config cfg;
  ASSERT_TRUE(cfg.vec().enum_vec() == std::vector({Channel::Red, Channel::Green}));

  toml::table result = toml::parse(R"(
    [vec]
    int_vec = [1, 2]
    str_vec = ["foo", "bar"]
    enum_vec = ["Red", "Blue"]
  )");
  ASSERT_TRUE(cfg.update(result));
  std::vector<int64_t> expected_int_vec{1, 2};
  ASSERT_TRUE(cfg.vec().int_vec() == expected_int_vec);
  std::vector<std::string> expected_str_vec{"foo", "bar"};
  ASSERT_TRUE(cfg.vec().str_vec() == expected_str_vec);
  std::vector expected_enum_vec{Channel::Red, Channel::Blue};
  ASSERT_TRUE(cfg.vec().enum_vec() == expected_enum_vec);

  result = toml::parse(R"(
    [vec]
    int_vec = [1, 2, 3]
  )");
  ASSERT_FALSE(cfg.update(result));

  result = toml::parse(R"(
    [vec]
    enum_vec = ["Yellow"]
  )");
  ASSERT_FALSE(cfg.update(result));
}

TEST(TestConfig, ParseValue) {
  Config cfg;

  // 1. empty config.
  ASSERT_TRUE(cfg.update(toml::parse("")));

  // 2. redundant entries.
  ASSERT_FALSE(cfg.update(toml::parse(R"(
    something = "else"
  )")));
  ASSERT_FALSE(cfg.update(toml::parse(R"(
    [sect]
    something = "else"
  )")));
  ASSERT_FALSE(cfg.update(toml::parse(R"(
    [some]
    thing = "else"
  )")));

  // 3. invalid values.
  ASSERT_FALSE(cfg.update(toml::parse(R"(
    [sect]
    val = 200
  )")));

  // 4. invalid enum.
  ASSERT_FALSE(cfg.update(toml::parse(R"(
    channel = "Yellow"
  )")));
}

TEST(TestConfig, Nested) {
  BigConfig cfg;
  ASSERT_EQ(cfg.a().channel(), Channel::Red);
  ASSERT_EQ(cfg.a().string(), "ing");
  ASSERT_EQ(cfg.b().string(), "replaced");

  cfg.b().set_channel(Channel::Green);
  ASSERT_EQ(cfg.a().channel(), Channel::Red);
  ASSERT_EQ(cfg.b().channel(), Channel::Green);

  BigConfig other;
  ASSERT_TRUE(other.update(toml::parse(cfg.toString())));
  ASSERT_EQ(other.a().channel(), Channel::Red);
  ASSERT_EQ(other.b().channel(), Channel::Green);
}

TEST(TestConfig, ToToml) {
  Config cfg;
  cfg.set_string("set string with \"quotes\"");
  cfg.sect().set_ok(false);
  cfg.vec().set_str_vec({"1", "2", "3"});
  cfg.set_channel(Channel::Blue);

  XLOGF(INFO, "toString: {}", cfg.toString());

  toml::table result = toml::parse(cfg.toString());
  Config other;
  ASSERT_TRUE(other.update(result));
  ASSERT_EQ(cfg.string(), other.string());
  ASSERT_EQ(cfg.sect().ok(), other.sect().ok());
  ASSERT_EQ(cfg.vec().str_vec(), other.vec().str_vec());
  ASSERT_EQ(cfg.toString(), other.toString());
  ASSERT_EQ(cfg.channel(), other.channel());
}

// TEST(TestConfig, Init) {
//   char *argvArr[] = {
//       const_cast<char *>("./test"),
//       const_cast<char *>("--config.channel=Blue"),
//       const_cast<char *>("--config.string"),
//       const_cast<char *>("a long string"),
//       const_cast<char *>("--config.sect.val"),
//       const_cast<char *>("123"),
//   };
//   int argc = ARRAY_SIZE(argvArr);

//   Config cfg;
//   auto *argv = argvArr;  // decay char *[] to char **
//   ASSERT_TRUE(cfg.init(&argc, &argv, false));

//   ASSERT_EQ(cfg.channel(), Channel::Blue);
//   ASSERT_EQ(cfg.string(), "a long string");
//   ASSERT_EQ(cfg.sect().val(), 123);
// }

TEST(TestConfig, ArrayOfTable) {
  ASSERT_EQ(array.cfgs_length(), 1u);
  ASSERT_EQ(array.cfgs(0).channel(), Channel::Red);

  toml::table table = toml::parse(R"(
    [[cfgs]]
    string = "A"
    channel = "Blue"

    [[cfgs]]
    string = "B"
    channel = "Red"

    [cfgs.sect]
    val = 121
  )");

  ASSERT_TRUE(array.update(table));
  ASSERT_EQ(array.cfgs_length(), 2u);
  ASSERT_EQ(array.cfgs(0).string(), "A");
  ASSERT_EQ(array.cfgs(0).channel(), Channel::Blue);
  ASSERT_EQ(array.cfgs(0).sect().val(), 100);

  ASSERT_EQ(array.cfgs(1).string(), "B");
  ASSERT_EQ(array.cfgs(1).channel(), Channel::Red);
  ASSERT_EQ(array.cfgs(1).sect().val(), 121);

  XLOGF(INFO, "toString: {}", array.toString());
}

TEST(TestConfig, CheckIsParsedFromString) {
  Config cfg;

  ASSERT_TRUE(cfg.find("string"));
  ASSERT_TRUE(cfg.find("string").value()->isParsedFromString());

  ASSERT_TRUE(cfg.find("channel"));
  ASSERT_TRUE(cfg.find("channel").value()->isParsedFromString());

  ASSERT_TRUE(cfg.find("optional"));
  ASSERT_TRUE(cfg.find("optional").value()->isParsedFromString());

  ASSERT_FALSE(cfg.find("sect"));
  ASSERT_FALSE(cfg.find("not_found"));

  ASSERT_TRUE(cfg.find("sect.foo"));
  ASSERT_TRUE(cfg.find("sect.foo").value()->isParsedFromString());

  ASSERT_TRUE(cfg.find("sect.ok"));
  ASSERT_FALSE(cfg.find("sect.ok").value()->isParsedFromString());
}

TEST(TestConfig, HotUpdated) {
  Config cfg;
  auto now = std::chrono::steady_clock::now;
  cfg.set_string(cfg.sect().foo());

  std::jthread read([&] {
    auto start = now();
    while (now() - start <= std::chrono::milliseconds(100)) {
      auto clone = cfg.clone();
      ASSERT_EQ(clone.string(), clone.sect().foo());

      clone.copy(cfg);
      ASSERT_EQ(clone.string(), clone.sect().foo());
    }
  });

  std::jthread update([&] {
    auto start = now();
    while (now() - start <= std::chrono::milliseconds(100)) {
      ASSERT_TRUE(cfg.update(toml::parse(fmt::format(R"(
        string = "{0}"
        [sect]
        foo = "{0}")",
                                                     now().time_since_epoch().count()))));
    }
  });
}

TEST(TestConfig, InlineTable) {
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(map, (std::map<std::string, int>{}));
    CONFIG_HOT_UPDATED_ITEM(string_to_string, (std::map<std::string, std::string>{}));
  } cfg;
  ASSERT_TRUE(cfg.map().empty());
  ASSERT_TRUE(cfg.string_to_string().empty());

  toml::table table = toml::parse(R"(
    map = { one = 1, two = 2 }

    [string_to_string]
    hello = 'world'
    language = 'C++'
  )");
  ASSERT_TRUE(cfg.update(table));
  ASSERT_EQ(cfg.map().at("one"), 1);
  ASSERT_EQ(cfg.map().at("two"), 2);
  ASSERT_EQ(cfg.string_to_string().at("hello"), "world");
  ASSERT_EQ(cfg.string_to_string().at("language"), "C++");

  XLOGF(INFO, "toString: {}", cfg.toString());
}

TEST(TestConfig, Set) {
  class Config : public ConfigBase<Config> {
    CONFIG_HOT_UPDATED_ITEM(set, std::set<std::string>{});
  } cfg;
  ASSERT_TRUE(cfg.set().empty());

  toml::table table = toml::parse(R"(
    set = ["one", "two"]
  )");
  ASSERT_TRUE(cfg.update(table));
  ASSERT_EQ(cfg.set().count("one"), 1);
  ASSERT_EQ(cfg.set().count("two"), 1);
  ASSERT_EQ(cfg.set().count("three"), 0);

  XLOGF(INFO, "toString: {}", cfg.toString());
  ASSERT_EQ(cfg.toString(), "set = [ 'one', 'two' ]");
}

TEST(TestConfig, AtomicallyUpdate) {
  std::string_view str = R"(
    [sect]
    val = 123
  )";

  Config cfg;
  ASSERT_TRUE(cfg.atomicallyUpdate(str));
  ASSERT_EQ(cfg.sect().val(), 123);
  cfg.sect().set_val(100);

  folly::test::TemporaryFile file;
  ::write(file.fd(), str.data(), str.length());
  ASSERT_TRUE(cfg.atomicallyUpdate(file.path()));
  ASSERT_EQ(cfg.sect().val(), 123);
}

TEST(TestConfig, VariantType) {
  class Config : public ConfigBase<Config> {
    CONFIG_SECT(type1, {
      CONFIG_HOT_UPDATED_ITEM(a, 0);
      CONFIG_HOT_UPDATED_ITEM(b, 0);
      CONFIG_HOT_UPDATED_ITEM(c, 0);
    });
    CONFIG_SECT(type2, {
      CONFIG_HOT_UPDATED_ITEM(x, 0);
      CONFIG_HOT_UPDATED_ITEM(y, 0);
      CONFIG_HOT_UPDATED_ITEM(z, 0);
    });
    CONFIG_VARIANT_TYPE("type1");
  };

  Config config;
  XLOGF(INFO, "{}", config.toString());
  ASSERT_OK(config.validate());

  ASSERT_TRUE(config.set_type("type2"));
  XLOGF(INFO, "{}", config.toString());
  ASSERT_OK(config.validate());

  ASSERT_FALSE(config.set_type("type3"));
}

TEST(TestConfig, UpdateCallback) {
  Config config;

  auto cnt = 0;
  auto guard = config.addCallbackGuard([&] { ++cnt; });

  ASSERT_EQ(cnt, 0);
  ASSERT_OK(config.update(toml::parse(R"( string = "else" )")));
  ASSERT_EQ(cnt, 1);

  guard->dismiss();
  ASSERT_OK(config.update(toml::parse(R"( string = "else" )")));
  ASSERT_EQ(cnt, 1);

  guard = config.addCallbackGuard();
  guard->setCallback([&] { cnt += 7; });
  ASSERT_OK(config.update(toml::parse(R"( string = "else" )")));
  ASSERT_EQ(cnt, 8);
}

TEST(TestConfig, HotUpdatedMacro) {
  class Config : public ConfigBase<Config> {
    CONFIG_SECT(cold, {
      CONFIG_ITEM(a, 0);
      CONFIG_ITEM(b, (std::vector<int>{1, 2, 3}));
      CONFIG_ITEM(c, (std::map<std::string, int>{{"one", 1}, {"two", 2}}));
    });
    CONFIG_SECT(hot, {
      CONFIG_HOT_UPDATED_ITEM(a, 0);
      CONFIG_HOT_UPDATED_ITEM(b, (std::vector<int>{1, 2, 3}));
      CONFIG_HOT_UPDATED_ITEM(c, (std::map<std::string, int>{{"one", 1}, {"two", 2}}));
    });
  };

  Config config;

  // hot update but keep value, succ.
  ASSERT_OK(config.update(toml::parse(R"(
    [cold]
    a = 0
    b = [1, 2, 3]
    c = { one = 1, two = 2 }
  )")));

  // hot update and change value, fail.
  ASSERT_FALSE(config.update(toml::parse(R"(
    [cold]
    a = 1
  )")));
  ASSERT_FALSE(config.update(toml::parse(R"(
    [cold]
    b = [1, 2]
  )")));
  ASSERT_FALSE(config.update(toml::parse(R"(
    [cold]
    c = { one = 1 }
  )")));

  // not a hot update. succ.
  ASSERT_OK(config.update(toml::parse(R"(
    [cold]
    a = 1
  )"),
                          false));
  ASSERT_OK(config.update(toml::parse(R"(
    [cold]
    b = [1, 2]
  )"),
                          false));
  ASSERT_OK(config.update(toml::parse(R"(
    [cold]
    c = { one = 1 }
  )"),
                          false));

  ASSERT_EQ(config.cold().a(), 1);
  ASSERT_EQ(config.cold().b(), (std::vector<int>{1, 2}));
  ASSERT_EQ(config.cold().c(), (std::map<std::string, int>{{"one", 1}}));

  // support hot updated. succ.
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    a = 0
    b = [1, 2, 3]
    c = { one = 1, two = 2 }
  )")));
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    a = 1
  )")));
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    b = [1, 2]
  )")));
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    c = { one = 1 }
  )")));
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    a = 1
  )"),
                          false));
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    b = [1, 2]
  )"),
                          false));
  ASSERT_OK(config.update(toml::parse(R"(
    [hot]
    c = { one = 1 }
  )"),
                          false));
}

TEST(TestConfig, DiffWith) {
  auto stringfy = [](const config::IConfig::ItemDiff &diff) {
    return fmt::format("{}: {} -> {}", diff.key, diff.left, diff.right);
  };

  config::IConfig::ItemDiff diffs[3];
  Config c0;
  Config c1;
  ASSERT_EQ(c0.diffWith(c1, std::span(diffs)), 0);

  c1.set_optional("nonnull");
  c1.sect().set_foo("abc");
  c1.sect().sub().set_val(101);
  auto diffCnt = c0.diffWith(c1, std::span(diffs));
  ASSERT_EQ(diffCnt, 3);
  ASSERT_EQ(stringfy(diffs[0]), "optional: nullopt -> 'nonnull'");
  ASSERT_EQ(stringfy(diffs[1]), "sect.foo: 'foo' -> 'abc'");
  ASSERT_EQ(stringfy(diffs[2]), "sect.sub.val: 100 -> 101");

  c1.sect().sub().set_foo("");
  ASSERT_EQ(c0.diffWith(c1, std::span(diffs)), 3);
}

}  // namespace
}  // namespace hf3fs::test
