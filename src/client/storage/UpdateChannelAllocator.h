#pragma once

#include <boost/core/ignore_unused.hpp>
#include <mutex>
#include <stack>

#include "fbs/storage/Common.h"

namespace hf3fs::storage::client {

/*
  Allocate a channel id, which is used to de-duplicate updates at storage service,
  for example, write, truncate, remove etc.
*/
class UpdateChannelAllocator {
 public:
  static constexpr size_t kChannelIdNumBits = sizeof(ChannelId::UnderlyingType) * 8ULL;
  static constexpr size_t kMaxNumChannels = (1ULL << kChannelIdNumBits) - 1ULL;

 public:
  UpdateChannelAllocator(size_t numChannels);

  ~UpdateChannelAllocator();

  bool allocate(UpdateChannel &channel, uint32_t slots = 1);

  void release(UpdateChannel &channel);

 private:
  size_t numChannels_;
  std::mutex availableChannelMutex_;
  std::stack<ChannelId> availableChannelIds_;
  std::atomic_uint64_t nextSeqNum_ = 1;
};

}  // namespace hf3fs::storage::client
