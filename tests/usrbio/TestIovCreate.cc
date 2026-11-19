#include <chrono>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <future>
#include <gtest/gtest.h>
#include <unistd.h>

#include "client/storage/StorageClientInMem.h"
#include "fuse/IoRing.h"
#include "fuse/IovTable.h"
#include "tests/FakeMgmtdClient.h"

namespace hf3fs::tests {
namespace {

using storage::client::IOBuffer;
using storage::client::StorageClientInMem;
using namespace std::chrono_literals;

class RegistrationClient : public StorageClientInMem {
 public:
  using StorageClientInMem::StorageClientInMem;

  Result<IOBuffer> registerIOBuffer(uint8_t *, size_t) override {
    const auto segment = ++calls;
    if (onRegister) {
      onRegister(segment);
    }
    if (segment == failAt) {
      return makeError(StorageClientCode::kMemoryError);
    }
    return IOBuffer{net::RDMABuf{}};
  }

  size_t calls = 0;
  size_t failAt = 0;
  std::function<void(size_t)> onRegister;
};

class IovCreateTest : public ::testing::Test {
 protected:
  static constexpr size_t blockSize = 4096;
  const Path mount{"/usrbio-test"};
  const meta::UserInfo user{meta::Uid{getuid()}, meta::Gid{getgid()}};
  FakeMgmtdClient mgmtd;
  storage::client::StorageClient::Config config;
  RegistrationClient client{ClientId::random(), config, mgmtd};
  folly::CPUThreadPoolExecutor executor{1};
  fuse::IovTable iovs;
  fuse::IoRingTable rings;

  void SetUp() override {
    iovs.init(mount, 1);
    rings.init(1);
  }

  Result<meta::Inode> add(const std::string &key, const lib::ShmBuf &shm) {
    return iovs.addIov(key.c_str(), Path("/dev/shm") / shm.path.relative_path(), getpid(), user, &executor, client,
                       rings, mount);
  }

  Path uniqueShmName() { return Path("/usrbio-test-" + Uuid::random().toHexString()); }
};

TEST_F(IovCreateTest, DoesNotPublishWhileRegistrationIsRunning) {
  lib::ShmBuf shm(uniqueShmName(), 2 * blockSize, blockSize, -1, user.uid, getpid(), getppid());
  const auto key = shm.id.toHexString() + ".b4096";
  std::promise<void> entered;
  std::promise<void> release;
  auto releaseFuture = release.get_future();
  client.onRegister = [&](size_t segment) {
    if (segment == 2) {
      entered.set_value();
      releaseFuture.wait();
    }
  };

  auto creating = std::async(std::launch::async, [&] { return add(key, shm); });
  // Release the worker even if an expectation fails.
  EXPECT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  EXPECT_EQ(creating.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(getStatusCode(iovs.lookupIov(key.c_str(), user)), MetaCode::kNotFound);
  EXPECT_FALSE(iovs.iovs->table[0].load());
  release.set_value();
  ASSERT_TRUE(creating.get());

  auto registered = iovs.rmIov(key.c_str(), user);
  ASSERT_TRUE(registered);
  EXPECT_NE((*registered)->memh(0), nullptr);
  EXPECT_NE((*registered)->memh(blockSize), nullptr);
}

TEST_F(IovCreateTest, RegistrationFailureReleasesSlotAndAllowsRetry) {
  lib::ShmBuf shm(uniqueShmName(), 3 * blockSize, blockSize, -1, user.uid, getpid(), getppid());
  const auto key = shm.id.toHexString() + ".b4096";
  client.failAt = 2;
  const auto failed = add(key, shm);
  EXPECT_EQ(getStatusCode(failed), StorageClientCode::kMemoryError);
  EXPECT_EQ(client.calls, 2);
  EXPECT_EQ(getStatusCode(iovs.lookupIov(key.c_str(), user)), MetaCode::kNotFound);
  EXPECT_TRUE(iovs.shmsById.empty());
  EXPECT_FALSE(iovs.iovs->table[0].load());

  client.failAt = 0;
  ASSERT_TRUE(add(key, shm));
  EXPECT_EQ(client.calls, 5);
  EXPECT_TRUE(iovs.rmIov(key.c_str(), user));
}

TEST_F(IovCreateTest, FullRingTableRollsBackIovEntry) {
  lib::ShmBuf shm(uniqueShmName(), fuse::IoRing::bytesRequired(8), 0, -1, user.uid, getpid(), getppid());
  const auto key = shm.id.toHexString() + ".r0.t0";
  const auto reserved = rings.ioRings->alloc();
  ASSERT_TRUE(reserved);
  EXPECT_EQ(getStatusCode(add(key, shm)), ClientAgentCode::kTooManyOpenFiles);
  EXPECT_EQ(getStatusCode(iovs.lookupIov(key.c_str(), user)), MetaCode::kNotFound);
  EXPECT_TRUE(iovs.shmsById.empty());
  EXPECT_FALSE(iovs.iovs->table[0].load());
  EXPECT_EQ(client.calls, 0);

  rings.ioRings->dealloc(*reserved);
  ASSERT_TRUE(add(key, shm));
  auto ring = iovs.rmIov(key.c_str(), user);
  ASSERT_TRUE(ring);
  EXPECT_EQ((*ring)->iorIndex, *reserved);
  rings.rmIoRing((*ring)->iorIndex);
}

}  // namespace
}  // namespace hf3fs::tests
