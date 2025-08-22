#include "common/utils/Uuid.h"

#include <boost/uuid/random_generator.hpp>

#include "common/utils/RobinHood.h"

size_t std::hash<hf3fs::Uuid>::operator()(const hf3fs::Uuid &uuid) const {
  return robin_hood::hash_bytes(uuid.data, uuid.static_size());
}

namespace hf3fs {
thread_local boost::uuids::random_generator Uuid::uuidGenerator;
}
