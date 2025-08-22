#include "UpdateChannelAllocator.h"

#include <folly/experimental/coro/BlockingWait.h>

#include "common/monitor/Recorder.h"

namespace hf3fs::storage::client {

static monitor::CountRecorder num_update_channels_inuse{"storage_client.num_update_channels.inuse", false /*reset*/};
static monitor::CountRecorder num_update_channels_total{"storage_client.num_update_channels.total", false /*reset*/};

UpdateChannelAllocator::UpdateChannelAllocator(size_t numChannels)
    : numChannels_(std::min(kMaxNumChannels, numChannels)) {
  std::scoped_lock lock(availableChannelMutex_);
  for (auto id = (ChannelId)numChannels_; id > 0; id--) {
    availableChannelIds_.push(id);
    num_update_channels_total.addSample(1);
  }
}

UpdateChannelAllocator::~UpdateChannelAllocator() {
  std::scoped_lock lock(availableChannelMutex_);
  std::set<ChannelId> uniqueIds;

  while (!availableChannelIds_.empty()) {
    auto channelId = availableChannelIds_.top();
    availableChannelIds_.pop();
    XLOGF_IF(DFATAL, uniqueIds.count(channelId) > 0, "Found duplicate channel id {}", channelId);
    uniqueIds.insert(channelId);
  }

  XLOGF_IF(DFATAL,
           uniqueIds.size() != numChannels_,
           "Not all channels released during shutdown: #channelIds {} != numChannels_ {}",
           uniqueIds.size(),
           numChannels_);
}

bool UpdateChannelAllocator::allocate(UpdateChannel &channel, uint32_t slots) {
  if (channel.id != ChannelId{0}) {
    channel.seqnum = ChannelSeqNum{nextSeqNum_.fetch_add(slots)};
    XLOGF(DBG7, "Reallocated a channel {}#{}", channel, slots);
    return true;
  }

  {
    std::scoped_lock lock(availableChannelMutex_);

    if (availableChannelIds_.empty()) {
      XLOGF(WARN, "No available channel, numChannels: {}", numChannels_);
      return false;
    }

    channel.id = availableChannelIds_.top();
    availableChannelIds_.pop();
  }

  channel.seqnum = ChannelSeqNum{nextSeqNum_.fetch_add(slots)};
  XLOGF(DBG7, "Allocated a channel {}#{}", channel, slots);
  num_update_channels_inuse.addSample(1);

  return true;
}

void UpdateChannelAllocator::release(UpdateChannel &channel) {
  if (channel.id == ChannelId{0}) {
    XLOGF(DFATAL, "Failed to release a channel with invalid id: {}", channel);
    return;
  }

  {
    std::scoped_lock lock(availableChannelMutex_);
    availableChannelIds_.push(channel.id);
  }

  XLOGF(DBG7, "Released a channel {}", channel);
  num_update_channels_inuse.addSample(-1);
  channel = {ChannelId{0}, ChannelSeqNum{0}};
}

}  // namespace hf3fs::storage::client
