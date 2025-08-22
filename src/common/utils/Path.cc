#include "Path.h"

#include "common/utils/RobinHood.h"

size_t std::hash<hf3fs::Path>::operator()(const hf3fs::Path &path) const {
  std::string_view sv = path.string();
  return robin_hood::hash_bytes(sv.data(), sv.size());
}
