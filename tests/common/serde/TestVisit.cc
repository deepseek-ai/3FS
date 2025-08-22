#include <folly/json.h>

#include "common/serde/Serde.h"
#include "common/serde/Visit.h"
#include "common/utils/Nameof.hpp"
#include "tests/GtestHelpers.h"

template <>
class hf3fs::serde::VisitOut<void> {
 public:
  auto tableBegin() { XLOGF(INFO, "start a table"); }
  void tableEnd() { XLOGF(INFO, "end a table"); }
  void arrayBegin() { XLOGF(INFO, "start an array"); }
  void arrayEnd() { XLOGF(INFO, "end an array"); }
  void variantBegin() { XLOGF(INFO, "start a variant"); }
  void variantEnd() { XLOGF(INFO, "end a variant"); }
  void key(std::string_view key) { XLOGF(INFO, "key {}", key); }
  void value(auto &&value) { XLOGF(INFO, "value {}", nameof::nameof_full_type<std::decay_t<decltype(value)>>()); }
};

namespace hf3fs::test {
namespace {

enum class Type {
  A,
  B,
  C,
};

struct Foo {
  SERDE_STRUCT_FIELD(val, double{});
  SERDE_STRUCT_FIELD(type, Type::A);
  SERDE_STRUCT_FIELD(num, (std::variant<int, double>{}));
  SERDE_STRUCT_FIELD(vec, std::vector<std::string>{});
};

TEST(TestVist, Normal) {
  serde::VisitOut<void> out;
  serde::visit(Foo{}, out);
}

}  // namespace
}  // namespace hf3fs::test
