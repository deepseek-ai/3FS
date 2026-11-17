#include <boost/intrusive/intrusive_fwd.hpp>
#include <cstdint>

#include "common/serde/TypeName.h"
#include "common/utils/Nameof.hpp"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

static_assert(serde::type_name_v<int> == "int");
static_assert(serde::type_name_v<float> == "float");
static_assert(serde::type_name_v<double> == "double");
static_assert(serde::type_name_v<std::string> == "basic_string");  // template arguments are ignored
static_assert(serde::type_name_v<std::vector<int>> == "vector");   // template arguments are ignored

struct Foo {};
static_assert(serde::type_name_v<Foo> == "Foo");

struct Bar {
  static constexpr std::string_view kTypeNameForSerde = "BAR";
};
static_assert(serde::type_name_v<Bar> == "BAR");

static_assert(serde::variant_type_names_v<std::variant<int, float>>[0] == "int");
static_assert(serde::variant_type_names_v<std::variant<int, float>>[1] == "float");
static_assert(serde::variant_type_names_v<std::variant<std::string, std::vector<int>>>[0] == "basic_string");
static_assert(serde::variant_type_names_v<std::variant<std::string, std::vector<int>>>[1] == "vector");
static_assert(serde::variant_type_names_v<std::variant<Foo, Bar>>[0] == "Foo");
static_assert(serde::variant_type_names_v<std::variant<Foo, Bar>>[1] == "BAR");

// Pin the serialized tag used by monitor::Sample's int64_t alternative across compilers.
static_assert(serde::type_name_v<int64_t> == "long");
static_assert(serde::variant_type_names_v<std::variant<int64_t, Bar>>[0] == "long");

TEST(TestTypeName, Int64VariantTag) {
  using Variant = std::variant<int64_t, Bar>;
  ASSERT_EQ(serde::variantTypeNameToIndex<Variant>("long"), 0);
  ASSERT_EQ(serde::variantTypeNameToIndex<Variant>("BAR"), 1);
  ASSERT_EQ(serde::variantTypeNameToIndex<Variant>("unknown"), 2);
}

}  // namespace
}  // namespace hf3fs::test
