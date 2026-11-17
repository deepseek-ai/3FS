#include <fmt/format.h>
#include <fstream>
#include <string>
#include <unistd.h>

#include "common/utils/Uuid.h"
#include "gtest/gtest.h"
#include "lib/common/Shm.h"

namespace hf3fs::lib {
namespace {

// Returns the VmFlags line of the mapping that starts at addr.
std::string vmFlags(const void *addr) {
  std::ifstream smaps("/proc/self/smaps");
  auto prefix = fmt::format("{:x}-", (uintptr_t)addr);
  std::string line;
  bool found = false;
  while (std::getline(smaps, line)) {
    if (!found) {
      found = line.rfind(prefix, 0) == 0;
    } else if (line.rfind("VmFlags:", 0) == 0) {
      return line;
    }
  }
  return {};
}

TEST(ShmBuf, AdvisesHugePagesOnlyWhenAsked) {
  if (access("/sys/kernel/mm/transparent_hugepage", F_OK) != 0) {
    GTEST_SKIP() << "kernel built without THP";
  }

  auto path = fmt::format("/hf3fs-test-shm-{}", Uuid::random().toHexString());
  size_t size = 4 << 20;
  ShmBuf owner(path, size, 0, -1, meta::Uid(getuid()), getpid(), getppid());
  ShmBuf peer(path, 0, size, 0, owner.id);

  EXPECT_EQ(vmFlags(owner.bufStart).find(" hg"), std::string::npos) << vmFlags(owner.bufStart);
  EXPECT_EQ(vmFlags(peer.bufStart).find(" hg"), std::string::npos) << vmFlags(peer.bufStart);

  peer.adviseHugePages();
  EXPECT_EQ(vmFlags(owner.bufStart).find(" hg"), std::string::npos) << vmFlags(owner.bufStart);
  EXPECT_NE(vmFlags(peer.bufStart).find(" hg"), std::string::npos) << vmFlags(peer.bufStart);
}

}  // namespace
}  // namespace hf3fs::lib
