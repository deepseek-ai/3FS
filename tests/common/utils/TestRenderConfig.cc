#include <cstdlib>

#include "common/utils/RenderConfig.h"
#include "common/utils/Toml.hpp"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {
String toPrettyToml(const String &s) {
  auto t = toml::parse(s);
  std::stringstream ss;
  ss << toml::toml_formatter(t, toml::toml_formatter::default_flags & ~toml::format_flags::indentation);
  return ss.str();
}

TEST(RenderConfig, testNormalV0) {
  flat::AppInfo app;
  app.nodeId = flat::NodeId(2);
  app.hostname = "hostname";
  app.pid = 100;
  app.tags = {flat::TagPair("k1", "v1"), flat::TagPair("k2"), flat::TagPair("k3", "v3")};
  app.releaseVersion = flat::ReleaseVersion::fromV0(0, 1, 2, 0, 0, 100);

  ::setenv("TEST_RENDER_CONFIG", "abc", 1);

  String rawConfig = R"(
  [server]
  {% if app.nodeId in [1, 3, 5] %}
  queue_size = 10
  {% elif hasTagValue(app, "k1", "v1") %}
  queue_size = 20
  {% else %}
  queue_size = 30
  {% endif %}

  {% if app.hostname is startsWith("host") %}
    zzz = 1
  {% else %}
    zzz = 0
  {% endif %}

  xxx = {{ "\"abc\"" if app is hasTag("k2") else "\"def\""}}

  port0 = {{ 10003 % 2 }}
  port1 = {{ 10007 % 2 }}

  version_less_than = {{ relaseVersionCompare(app, "<", "0.1.2") }}
  version_less_than_or_equal_to = {{ relaseVersionCompare(app, "<=", "0.1.2-100") }}
  version_equal_to = {{ relaseVersionCompare(app, "==", "0.1.2") }}
  version_greater_than = {{ releaseVersionCompare(app, ">", "0.1.2-100") }}
  version_greater_than_or_equal_to = {{ releaseVersionCompare(app, ">=", "0.1.2") }}
  version_not_equal_to = {{ releaseVersionCompare(app, "!=", "0.1.2-100") }}

  yyy = "{{ env.TEST_RENDER_CONFIG }}"

  log_level = '{{ env.HF3FS_LOG_LEVEL if env.HF3FS_LOG_LEVEL else "INFO" }}'

  # TODO: implement startswith
  {% if app.hostname == "hostname" %}
  enable_fast_mode = true
  {% endif %})";

  auto res = renderConfig(rawConfig, &app);
  ASSERT_OK(res);

  auto resConfig = toPrettyToml(res.value());

  auto expected = toPrettyToml(R"(
  [server]
  xxx = 'abc'
  yyy = 'abc'
  zzz = 1
  log_level = 'INFO'
  version_less_than = false
  version_less_than_or_equal_to = true
  version_equal_to = true
  version_greater_than_or_equal_to = true
  version_greater_than = false
  version_not_equal_to = false
  port0 = 1
  port1 = 1
  queue_size = 20 # hit second case
  enable_fast_mode = true)");

  ASSERT_EQ(resConfig, expected);
}

TEST(RenderConfig, testTemplateNotParsed) {
  flat::AppInfo app;
  app.nodeId = flat::NodeId(2);
  app.hostname = "hostname";
  app.pid = 100;
  app.tags = {flat::TagPair("k1", "v1"), flat::TagPair("k2"), flat::TagPair("k3", "v3")};
  app.releaseVersion = flat::ReleaseVersion::fromV0(0, 1, 2, 0, 0, 0);

  ::setenv("TEST_RENDER_CONFIG", "abc", 1);

  String rawConfig = R"(
  [server]
  {% if app.nodeId in (1, 3, 5) %}
  queue_size = 10
  {% elif app.hostname not in ('hostname') %}
  queue_size = 10
  {% elif hasTagValue(app, "k1", "v1") %}
  queue_size = 20
  {% else %}
  queue_size = 30
  {% endif %}

  xxx = {{ "\"abc\"" if app is hasTag("k2") else "\"def\""}}

  {% if app.releaseVersion.major == 0 and app.releaseVersion.minor == 1 and app.releaseVersion.patch == 0 %}
  yyy = 1
  {% endif %}

  version_less_than = {{ relaseVersionCompare(app, "<", "0.1.2") }}
  version_less_than_or_equal_to = {{ relaseVersionCompare(app, "<=", "0.1.2") }}
  version_equal_to = {{ relaseVersionCompare(app, "==", "0.1.2") }}
  version_greater_than = {{ relaseVersionCompare(app, ">", "0.1.2") }}
  version_greater_than_or_equal_to = {{ relaseVersionCompare(app, ">=", "0.1.2") }}
  version_not_equal_to = {{ relaseVersionCompare(app, "!=", "0.1.2") }}

  yyy = "{{ env.TEST_RENDER_CONFIG }}"

  log_level = '{{ env.HF3FS_LOG_LEVEL if env.HF3FS_LOG_LEVEL else "INFO" }}'

  # TODO: implement startswith
  {% if app.hostname == "hostname" %}
  enable_fast_mode = true
  {% endif %})";

  auto res = renderConfig(rawConfig, &app);
  ASSERT_ERROR(res, StatusCode::kInvalidConfig);
  ASSERT_TRUE(res.error().message().find("app.hostname not in") != std::string_view::npos);
}

#define ASSERT_REDNER_EQ(raw, app, expected)      \
  do {                                            \
    auto &&_raw = (raw);                          \
    auto &&_app = (app);                          \
    auto &&_expected = (expected);                \
    auto _res = renderConfig(_raw, _app);         \
    ASSERT_OK(_res);                              \
    auto _resConfig = toPrettyToml(_res.value()); \
    ASSERT_EQ(_resConfig, _expected);             \
  } while (0)

TEST(RenderConfig, testNormalV1) {
  String config0 = R"(
      x = {{ app.nodeId in [1, 2, 3, 4, 5] }}
  {% set port = app.nodeId % 2 %}
  {% if port == 0 %}
      target_paths = ["/storage/data1/hf3fs", "/storage/data2/hf3fs", "/storage/data3/hf3fs", "/storage/data4/hf3fs", "/storage/data5/hf3fs", "/storage/data6/hf3fs", "/storage/data7/hf3fs", "/storage/data8/hf3fs"]
  {% else %}
      target_paths = ["/storage/data9/hf3fs", "/storage/data10/hf3fs", "/storage/data11/hf3fs", "/storage/data12/hf3fs", "/storage/data13/hf3fs", "/storage/data14/hf3fs", "/storage/data15/hf3fs", "/storage/data16/hf3fs"]
  {% endif %}
  )";

  String config1 = R"(
      x = {{ app.nodeId in range(1, 6) }}
      target_paths = [ {% for i in range(8) %} "/storage/data{{i + 8 * (app.nodeId % 2) + 1}}/hf3fs", {% endfor %} ]
  )";

  flat::AppInfo app0;
  app0.nodeId = flat::NodeId(0);

  auto expected0 = toPrettyToml(R"(
      x = false
      target_paths = ["/storage/data1/hf3fs", "/storage/data2/hf3fs", "/storage/data3/hf3fs", "/storage/data4/hf3fs", "/storage/data5/hf3fs", "/storage/data6/hf3fs", "/storage/data7/hf3fs", "/storage/data8/hf3fs"]
  )");

  ASSERT_REDNER_EQ(config0, &app0, expected0);
  ASSERT_REDNER_EQ(config1, &app0, expected0);

  flat::AppInfo app1;
  app1.nodeId = flat::NodeId(1);

  auto expected1 = toPrettyToml(R"(
      x = true
      target_paths = ["/storage/data9/hf3fs", "/storage/data10/hf3fs", "/storage/data11/hf3fs", "/storage/data12/hf3fs", "/storage/data13/hf3fs", "/storage/data14/hf3fs", "/storage/data15/hf3fs", "/storage/data16/hf3fs"]
  )");

  ASSERT_REDNER_EQ(config0, &app1, expected1);
  ASSERT_REDNER_EQ(config1, &app1, expected1);
}
}  // namespace
}  // namespace hf3fs::test
