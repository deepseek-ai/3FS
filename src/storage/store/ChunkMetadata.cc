#include "ChunkMetadata.h"

#include "common/monitor/Recorder.h"

namespace hf3fs::storage {
namespace {

monitor::CountRecorder fatalEvent{"storage.fatal"};

}  // namespace

void reportFatalEvent() { fatalEvent.addSample(1); }

}  // namespace hf3fs::storage
