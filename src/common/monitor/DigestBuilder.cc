/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DigestBuilder.h"

#include <algorithm>
#include <folly/Random.h>
#include <folly/concurrency/CacheLocality.h>
#include <folly/lang/Bits.h>
#include <folly/logging/xlog.h>

namespace hf3fs {

DigestBuilder::DigestBuilder(size_t bufferSize, size_t digestSize)
    : bufferSize_(bufferSize),
      digestSize_(digestSize) {}

folly::TDigest DigestBuilder::build() {
  std::vector<std::vector<double>> valuesVec;
  std::vector<std::unique_ptr<folly::TDigest>> digestPtrs;

  for (auto &tls : tlsBuffers_.accessAllThreads()) {
    auto newState = std::make_shared<State>();
    newState->buffer.reserve(bufferSize_);
    auto oldState = tls.state.exchange(newState, std::memory_order_acq_rel);

    oldState->collected.store(true, std::memory_order_release);
    std::unique_lock<folly::SpinLock> g(oldState->mutex);
    valuesVec.push_back(std::move(oldState->buffer));
    if (oldState->digest) {
      digestPtrs.push_back(std::move(oldState->digest));
    }
  }

  std::vector<folly::TDigest> digests;
  digests.reserve(digestPtrs.size());
  for (auto &digestPtr : digestPtrs) {
    digests.push_back(std::move(*digestPtr));
  }

  size_t count = 0;
  for (const auto &vec : valuesVec) {
    count += vec.size();
  }
  if (count) {
    std::vector<double> values;
    values.reserve(count);
    for (const auto &vec : valuesVec) {
      values.insert(values.end(), vec.begin(), vec.end());
    }
    folly::TDigest digest(digestSize_);
    digests.push_back(digest.merge(values));
  }
  return folly::TDigest::merge(digests);
}

void DigestBuilder::append(double value) {
  for (;;) {
    auto state = tlsBuffers_->state.load(std::memory_order_acquire);
    if (UNLIKELY(state->collected.load(std::memory_order_acquire))) {
      continue;
    }

    std::unique_lock<folly::SpinLock> g(state->mutex, std::try_to_lock);
    if (UNLIKELY(!g.owns_lock())) {
      continue;
    }

    if (UNLIKELY(state->collected.load(std::memory_order_acquire))) {
      continue;
    }

    state->buffer.push_back(value);
    if (state->buffer.size() == bufferSize_) {
      if (!state->digest) {
        state->digest = std::make_unique<folly::TDigest>(digestSize_);
      }
      *state->digest = state->digest->merge(state->buffer);
      state->buffer.clear();
    }
  }
}

}  // namespace hf3fs
