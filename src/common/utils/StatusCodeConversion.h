#pragma once

#include "common/utils/Result.h"
#include "common/utils/Status.h"
#include "common/utils/StatusCodeDetails.h"

namespace hf3fs {

class StatusCodeConversion {
 public:
  static Status convertToStorageClientCode(Status status);

  template <typename T>
  static Result<T> convertToStorageClientCode(const Result<T> &result) {
    if (UNLIKELY(result.hasError())) {
      return makeError(convertToStorageClientCode(result.error()));
    }

    return result;
  }
};

}  // namespace hf3fs
