#include <vector>

#include "common/utils/RobinHood.h"
#include "common/utils/TypeTraits.h"

namespace hf3fs {
namespace {

static_assert(is_vector_v<std::vector<int>>, "is vector");
static_assert(!is_vector_v<std::string>, "not vector");

static_assert(is_set_v<std::set<int>>, "is set");
static_assert(!is_set_v<std::string>, "not set");

static_assert(is_map_v<std::map<int, int>>, "is map");
static_assert(!is_map_v<std::vector<int>>, "not map");

static_assert(GenericPair<std::pair<int, int>>, "is generic pair");
static_assert(!GenericPair<std::map<int, int>>, "not generic pair");

static_assert(GenericPair<robin_hood::pair<int, int>>, "is generic pair");
static_assert(Container<robin_hood::unordered_map<int, int>>, "ok");

struct Base {
  int x;
};
struct Inherit : Base {
  int y;
};
static_assert(std::is_same_v<member_pointer_to_class_t<&Inherit::x>, Base>);
static_assert(std::is_same_v<member_pointer_to_class_t<&Inherit::y>, Inherit>);

}  // namespace
}  // namespace hf3fs
