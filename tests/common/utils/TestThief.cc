#include <type_traits>

#include "common/utils/Thief.h"
#include "common/utils/TypeTraits.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::test {
namespace {

class A {
 public:
  auto &secret() { return secret_; }

 private:
  int secret_ = 0;
};

template <typename Tag, auto Value>
struct StoreValue : public std::type_identity<thief::steal<Tag, value_identity<Value>>> {};

struct Tag;
template struct StoreValue<Tag, &A::secret_>;

TEST(TestThief, AccessPrivateMember) {
  A a;
  a.secret() = 20;

  ASSERT_EQ(a.*thief::retrieve<Tag>::value, 20);
}

}  // namespace
}  // namespace hf3fs::test
