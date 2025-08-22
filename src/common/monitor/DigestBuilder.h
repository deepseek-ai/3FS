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

#pragma once

#include <folly/Memory.h>
#include <folly/SpinLock.h>
#include <folly/ThreadLocal.h>
#include <folly/concurrency/AtomicSharedPtr.h>
#include <folly/stats/TDigest.h>
#include <memory>

namespace hf3fs {

/*
 * Stat digests, such as TDigest, can be expensive to merge. It is faster to
 * buffer writes and merge them in larger chunks. DigestBuilder buffers writes
 * to improve performance.
 *
 * Values are stored in a cpu local buffer. Hot stats will merge the cpu local
 * buffer into a cpu-local digest when the buffer size is reached.
 *
 * All methods in this class are thread safe, but it probably doesn't make sense
 * for multiple threads to call build simultaneously. A typical usage is to
 * buffer writes for a period of time, and then have one thread call build to
 * merge the buffer into some other DigestT instance.
 */
class DigestBuilder {
 public:
  explicit DigestBuilder(size_t bufferSize, size_t digestSize);

  /*
   * Builds a TDigest from the buffer. All values used to build the TDigest are
   * removed from the buffer.
   */
  folly::TDigest build();

  /*
   * Adds a value to the buffer.
   */
  void append(double value);

 private:
  struct State {
    folly::SpinLock mutex;
    std::atomic<bool> collected{false};
    std::vector<double> buffer;
    std::unique_ptr<folly::TDigest> digest;
  };

  struct TlsBuffer {
    folly::atomic_shared_ptr<State> state{std::make_shared<State>()};

    TlsBuffer() noexcept = default;

    TlsBuffer(TlsBuffer &&other) = delete;
    TlsBuffer &operator=(TlsBuffer &&other) = delete;
  };

  struct TlsBufferTag {};

  folly::ThreadLocal<TlsBuffer, TlsBufferTag> tlsBuffers_;
  size_t bufferSize_;
  size_t digestSize_;
};

}  // namespace hf3fs
