#include <array>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

#include "client/storage/StorageClient.h"
#include "client/storage/StorageClientInMem.h"
#include "common/net/ib/SetupIB.h"
#include "fuse/FuseClients.h"
#include "fuse/IoRing.h"
#include "fuse/IovTable.h"
#include "fuse/PioV.h"
#include "tests/FakeMgmtdClient.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::fuse {

class IovTableTestHelper {
 public:
  static Result<int> insert(IovTable &table, std::shared_ptr<IovEntry> entry) {
    auto iovd = table.entries_->alloc();
    if (!iovd) {
      return makeError(ClientAgentCode::kTooManyOpenFiles, "too many test iovs");
    }

    auto published = table.publishEntry(*iovd, std::move(entry));
    if (!published) {
      table.entries_->dealloc(*iovd);
      RETURN_ERROR(published);
    }
    return *iovd;
  }

  static bool removeExpected(IovTable &table, int iovd, const std::shared_ptr<IovEntry> &expected) {
    std::unique_lock lock(table.mutex_);
    return table.removeEntryLocked(iovd, expected);
  }
};

namespace {

meta::UserInfo user(uint32_t uid = 0, uint32_t gid = 0) { return meta::UserInfo{meta::Uid(uid), meta::Gid(gid)}; }

class TestShm {
 public:
  explicit TestShm(size_t size)
      : name_("/hf3fs-iov-table-" + Uuid::random().toHexString()),
        path_(Path("/dev/shm") / name_.substr(1)) {
    fd_ = shm_open(name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd_ >= 0 && ftruncate(fd_, size) == 0) {
      valid_ = true;
    }
  }

  ~TestShm() {
    if (fd_ >= 0) {
      close(fd_);
    }
    shm_unlink(name_.c_str());
  }

  bool valid() const { return valid_; }
  const Path &path() const { return path_; }

 private:
  std::string name_;
  Path path_;
  int fd_ = -1;
  bool valid_ = false;
};

class TestIovTableGdr : public ::testing::Test {
 protected:
  TestIovTableGdr()
      : mgmtd_(tests::FakeMgmtdClient::create()),
        storageClient_(ClientId::random(), storageConfig_, *mgmtd_) {}

  auto addIovForParser(IovTable &table, const char *key, const Path &target) {
    return table.addIov(key, target, 1234, user(), folly::Executor::KeepAlive<>{}, storageClient_);
  }

  void expectParserError(IovTable &table, const char *key, std::string_view message) {
    auto result = addIovForParser(table, key, Path("/dev/shm/unused"));
    ASSERT_TRUE(result.hasError());
    EXPECT_THAT(std::string(result.error().message()), testing::HasSubstr(std::string(message)));
  }

  storage::client::StorageClient::Config storageConfig_;
  std::shared_ptr<tests::FakeMgmtdClient> mgmtd_;
  storage::client::StorageClientInMem storageClient_;
};

class TestIovTableGdrWithIB : public TestIovTableGdr {
 public:
  static void SetUpTestSuite() { net::test::SetupIB::SetUpTestSuite(); }
};

}  // namespace

TEST_F(TestIovTableGdr, ParsesAndValidatesGdrV2Keys) {
  IovTable table;

  auto result = addIovForParser(table, "abcdef1234567890abcdef1234567890.gdr.d0", Path("gdr://invalid"));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(std::string(result.error().message()), testing::HasSubstr("failed to parse GDR target URI"));

  expectParserError(table, "", "invalid shm key");
  expectParserError(table, "abcdef1234567890abcdef1234567890..gdr.d0", "empty attr");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d", "invalid gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d-1", "invalid gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d1x", "invalid gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gpu.d0", "invalid gdr attr");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.unknown", "unknown attr");
  expectParserError(table, "abcdef1234567890abcdef1234567890.d0", "gpu device id set for non-gdr key");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr", "gdr key missing gpu device id");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.b4096", "gdr key does not support");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.r4", "gdr key does not support");
  expectParserError(table, "abcdef1234567890abcdef1234567890.gdr.d0.t10", "gdr key does not support");
  expectParserError(table, "abcdef1234567890abcdef1234567890.r4.phx", "invalid priority");
  expectParserError(table, "abcdef1234567890abcdef1234567890.b0", "invalid block size");
  expectParserError(table, "abcdef1234567890abcdef1234567890.r", "invalid io batch size");
}

TEST_F(TestIovTableGdrWithIB, HostEntrySupportsMetadataLookupAndDeadPidRemoval) {
  constexpr pid_t kPid = 4242;
  constexpr int kIoRingIndex = 7;
  const auto size = IoRing::bytesRequired(4);
  TestShm testShm(size);
  if (!testShm.valid()) {
    GTEST_SKIP() << "POSIX shared memory is unavailable";
  }

  auto id = Uuid::random();
  auto key = id.toHexString() + ".r1";
  auto owner = user(17, 23);
  IovTable table;
  table.init(Path("/mnt/3fs"), 8);

  auto added = table.addIov(key.c_str(), testShm.path(), kPid, owner, folly::Executor::KeepAlive<>{}, storageClient_);
  ASSERT_OK(added);
  ASSERT_TRUE(added->second);
  added->second->iorIndex = kIoRingIndex;

  auto iovd = table.iovDesc(added->first.id);
  ASSERT_TRUE(iovd);
  auto entry = table.entryAt(*iovd);
  ASSERT_TRUE(entry);
  EXPECT_FALSE(entry->isGpu());
  EXPECT_EQ(entry->key, key);
  EXPECT_EQ(entry->id, id);
  EXPECT_EQ(entry->target, testShm.path());
  EXPECT_EQ(entry->user, owner.uid);
  EXPECT_EQ(entry->gid, owner.gid);
  EXPECT_EQ(entry->pid, kPid);

  auto stat = table.statIov(*iovd, owner);
  ASSERT_OK(stat);
  EXPECT_EQ(stat->asSymlink().target, testShm.path());
  EXPECT_EQ(stat->acl.uid, owner.uid);
  EXPECT_EQ(stat->acl.gid, owner.gid);
  EXPECT_EQ(stat->acl.perm, meta::Permission(0400));
  ASSERT_ERROR(table.statIov(*iovd, user(18, 23)), MetaCode::kNoPermission);

  auto [dirEntries, inodes] = table.listIovs(owner);
  ASSERT_EQ(dirEntries->size(), 4);
  ASSERT_EQ(inodes->size(), 4);
  EXPECT_EQ(dirEntries->back().name, key);
  ASSERT_TRUE(inodes->back());
  EXPECT_EQ(inodes->back()->asSymlink().target, testShm.path());

  std::vector<Result<IoBufForIO>> bufs;
  std::vector<IovLookupRequest> first{{id, 8, 16}, {id, 32, 8}};
  table.lookupBufs(bufs, first, owner.uid);
  ASSERT_EQ(bufs.size(), 2);
  ASSERT_OK(bufs[0]);
  ASSERT_OK(bufs[1]);
  auto *firstPtr = ioBufPtr(bufs[0].value());
  EXPECT_EQ(firstPtr, added->second->bufStart + 8);

  std::vector<Result<IoBufForIO>> denied;
  table.lookupBufs(denied, first, meta::Uid(18));
  ASSERT_EQ(denied.size(), first.size());
  ASSERT_ERROR(denied[0], MetaCode::kNoPermission);
  ASSERT_ERROR(denied[1], MetaCode::kNoPermission);
  ASSERT_ERROR(table.lookupBuf(IovLookupRequest{id, 0, 1}, meta::Uid(18)), MetaCode::kNoPermission);

  Uuid missing = Uuid::random();
  std::vector<IovLookupRequest> wrapped{{id, size - 4, 8}, {missing, 0, 1}};
  table.lookupBufs(bufs, wrapped, owner.uid);
  ASSERT_EQ(bufs.size(), 4);
  ASSERT_OK(bufs[0]);
  EXPECT_EQ(ioBufPtr(bufs[0].value()), firstPtr);
  EXPECT_TRUE(bufs[2].hasError());
  EXPECT_TRUE(bufs[3].hasError());

  auto ioRingIndexes = table.removeIovsByPid(kPid);
  ASSERT_EQ(ioRingIndexes, std::vector<int>({kIoRingIndex}));
  EXPECT_FALSE(table.entryAt(*iovd));
  EXPECT_TRUE(table.lookupIov(key.c_str(), owner).hasError());
  EXPECT_TRUE(table.lookupBuf(IovLookupRequest{id, 0, 1}, owner.uid).hasError());
}

TEST_F(TestIovTableGdrWithIB, HostRmIovReturnsIoRingBuffer) {
  TestShm testShm(IoRing::bytesRequired(2));
  if (!testShm.valid()) {
    GTEST_SKIP() << "POSIX shared memory is unavailable";
  }

  auto id = Uuid::random();
  auto key = id.toHexString() + ".r1";
  IovTable table;
  table.init(Path("/mnt/3fs"), 2);
  auto added = table.addIov(key.c_str(), testShm.path(), 123, user(), folly::Executor::KeepAlive<>{}, storageClient_);
  ASSERT_OK(added);

  auto removed = table.rmIov(key.c_str(), user());
  ASSERT_OK(removed);
  EXPECT_EQ(*removed, added->second);
}

TEST_F(TestIovTableGdrWithIB, HostAddRejectsDuplicateKeyAndUuidWithoutCorruptingReverseMaps) {
  TestShm testShm(IoRing::bytesRequired(2));
  if (!testShm.valid()) {
    GTEST_SKIP() << "POSIX shared memory is unavailable";
  }

  auto id = Uuid::random();
  auto readKey = id.toHexString() + ".r1";
  auto writeKey = id.toHexString() + ".w1";
  auto owner = user(31, 32);
  IovTable table;
  table.init(Path("/mnt/3fs"), 1);

  auto first =
      table.addIov(readKey.c_str(), testShm.path(), 1001, owner, folly::Executor::KeepAlive<>{}, storageClient_);
  ASSERT_OK(first);
  ASSERT_TRUE(first->second);
  ASSERT_ERROR(
      table.addIov(readKey.c_str(), testShm.path(), 1002, owner, folly::Executor::KeepAlive<>{}, storageClient_),
      MetaCode::kExists);
  ASSERT_ERROR(
      table.addIov(writeKey.c_str(), testShm.path(), 1003, owner, folly::Executor::KeepAlive<>{}, storageClient_),
      MetaCode::kExists);

  ASSERT_OK(table.lookupBuf(IovLookupRequest{id, 0, 1}, owner.uid));
  ASSERT_OK(table.rmIov(readKey.c_str(), owner));

  auto replacement =
      table.addIov(writeKey.c_str(), testShm.path(), 1004, owner, folly::Executor::KeepAlive<>{}, storageClient_);
  ASSERT_OK(replacement);
  ASSERT_ERROR(table.rmIov(writeKey.c_str(), owner, first->second), MetaCode::kNotFound);
  ASSERT_OK(table.lookupIov(writeKey.c_str(), owner));
  ASSERT_OK(table.lookupBuf(IovLookupRequest{id, 0, 1}, owner.uid));

  ASSERT_OK(table.rmIov(writeKey.c_str(), owner));
  EXPECT_TRUE(table.lookupBuf(IovLookupRequest{id, 0, 1}, owner.uid).hasError());
}

TEST_F(TestIovTableGdr, StalePidSnapshotCannotRemoveReusedDescriptor) {
  IovTable table;
  table.init(Path("/mnt/3fs"), 1);
  auto owner = user(41, 42);

  auto oldId = Uuid::random();
  auto oldEntry = std::make_shared<IovEntry>(
      IovEntry{oldId.toHexString(), oldId, Path("/old"), owner.uid, owner.gid, 2001, IovBuffer{}});
  auto oldIovd = IovTableTestHelper::insert(table, oldEntry);
  ASSERT_OK(oldIovd);
  ASSERT_OK(table.rmIov(oldEntry->key.c_str(), owner));

  auto newId = Uuid::random();
  auto newEntry = std::make_shared<IovEntry>(
      IovEntry{newId.toHexString(), newId, Path("/new"), owner.uid, owner.gid, 2002, IovBuffer{}});
  auto newIovd = IovTableTestHelper::insert(table, newEntry);
  ASSERT_OK(newIovd);
  ASSERT_EQ(*newIovd, *oldIovd);

  EXPECT_FALSE(IovTableTestHelper::removeExpected(table, *oldIovd, oldEntry));
  EXPECT_EQ(table.entryAt(*newIovd), newEntry);
  EXPECT_TRUE(table.removeIovsByPid(2001).empty());
  EXPECT_EQ(table.entryAt(*newIovd), newEntry);
}

TEST_F(TestIovTableGdr, ClosedReadHoleZerosOnlyMissingRanges) {
  std::array<uint8_t, 12> data{};
  std::vector<storage::client::ReadIO> reads;
  auto addRead = [&](uint32_t chunk, size_t dataOffset, Result<uint32_t> result) {
    auto chunkId = storage::ChunkId(meta::ChunkId(meta::InodeId{77}, 0, chunk).pack());
    auto read =
        storageClient_
            .createReadIO({}, chunkId, 0, 4, data.data() + dataOffset, nullptr, reinterpret_cast<void *>(size_t{0}));
    read.result.lengthInfo = std::move(result);
    reads.push_back(std::move(read));
  };
  addRead(0, 0, 2);
  addRead(1, 4, makeError(StorageClientCode::kChunkNotFound));
  addRead(2, 8, 4);

  std::vector<ssize_t> result{0};
  std::vector<std::pair<size_t, size_t>> zeroed;
  lib::agent::detail::finishReadResults(
      result,
      reads,
      true,
      [&](const storage::client::ReadIO &, size_t offset, size_t length) -> Result<Void> {
        zeroed.emplace_back(offset, length);
        return Void{};
      });

  EXPECT_EQ(result, std::vector<ssize_t>({12}));
  EXPECT_EQ(zeroed, (std::vector<std::pair<size_t, size_t>>{{2, 2}, {0, 4}}));
}

TEST_F(TestIovTableGdr, ReadHoleDoesNotLeakWhenNextIovStartsWithMissingChunk) {
  std::array<uint8_t, 4> firstBuffer;
  std::array<uint8_t, 8> secondBuffer;
  firstBuffer.fill(0xAA);
  secondBuffer.fill(0xBB);
  std::vector<storage::client::ReadIO> reads;
  auto addRead = [&](uint32_t chunk, size_t iovIdx, uint8_t *data, Result<uint32_t> result) {
    auto chunkId = storage::ChunkId(meta::ChunkId(meta::InodeId{80}, 0, chunk).pack());
    auto read = storageClient_.createReadIO({}, chunkId, 0, 4, data, nullptr, reinterpret_cast<void *>(iovIdx));
    read.result.lengthInfo = std::move(result);
    reads.push_back(std::move(read));
  };
  addRead(0, 0, firstBuffer.data(), 2);
  addRead(1, 1, secondBuffer.data(), makeError(StorageClientCode::kChunkNotFound));
  addRead(2, 1, secondBuffer.data() + 4, 4);

  std::vector<ssize_t> result{0, 0};
  lib::agent::detail::finishReadResults(
      result,
      reads,
      true,
      [](const storage::client::ReadIO &io, size_t offset, size_t length) -> Result<Void> {
        std::memset(io.data + offset, 0, length);
        return Void{};
      });

  EXPECT_EQ(result, (std::vector<ssize_t>{2, 8}));
  EXPECT_EQ(firstBuffer, (std::array<uint8_t, 4>{0xAA, 0xAA, 0xAA, 0xAA}));
  EXPECT_EQ(secondBuffer, (std::array<uint8_t, 8>{0, 0, 0, 0, 0xBB, 0xBB, 0xBB, 0xBB}));
}

TEST_F(TestIovTableGdr, ReadHoleDoesNotLeakWhenNextIovStartsWithShortRead) {
  std::array<uint8_t, 4> firstBuffer;
  std::array<uint8_t, 8> secondBuffer;
  firstBuffer.fill(0xAA);
  secondBuffer.fill(0xBB);
  std::vector<storage::client::ReadIO> reads;
  auto addRead = [&](uint32_t chunk, size_t iovIdx, uint8_t *data, uint32_t result) {
    auto chunkId = storage::ChunkId(meta::ChunkId(meta::InodeId{81}, 0, chunk).pack());
    auto read = storageClient_.createReadIO({}, chunkId, 0, 4, data, nullptr, reinterpret_cast<void *>(iovIdx));
    read.result.lengthInfo = result;
    reads.push_back(std::move(read));
  };
  addRead(0, 0, firstBuffer.data(), 2);
  addRead(1, 1, secondBuffer.data(), 2);
  addRead(2, 1, secondBuffer.data() + 4, 4);

  std::vector<ssize_t> result{0, 0};
  lib::agent::detail::finishReadResults(
      result,
      reads,
      true,
      [](const storage::client::ReadIO &io, size_t offset, size_t length) -> Result<Void> {
        std::memset(io.data + offset, 0, length);
        return Void{};
      });

  EXPECT_EQ(result, (std::vector<ssize_t>{2, 8}));
  EXPECT_EQ(firstBuffer, (std::array<uint8_t, 4>{0xAA, 0xAA, 0xAA, 0xAA}));
  EXPECT_EQ(secondBuffer, (std::array<uint8_t, 8>{0xBB, 0xBB, 0, 0, 0xBB, 0xBB, 0xBB, 0xBB}));
}

TEST_F(TestIovTableGdr, ReadHolePropagatesZeroFailureAndHonorsForbidMode) {
  std::array<uint8_t, 8> data{};
  std::vector<storage::client::ReadIO> reads;
  for (uint32_t chunk = 0; chunk < 2; ++chunk) {
    auto chunkId = storage::ChunkId(meta::ChunkId(meta::InodeId{78}, 0, chunk).pack());
    auto read =
        storageClient_
            .createReadIO({}, chunkId, 0, 4, data.data() + chunk * 4, nullptr, reinterpret_cast<void *>(size_t{0}));
    read.result.lengthInfo = chunk == 0 ? 2 : 4;
    reads.push_back(std::move(read));
  }

  std::vector<ssize_t> result{0};
  size_t zeroCalls = 0;
  lib::agent::detail::finishReadResults(result,
                                        reads,
                                        true,
                                        [&](const storage::client::ReadIO &, size_t, size_t) -> Result<Void> {
                                          ++zeroCalls;
                                          return makeError(StorageClientCode::kRemoteIOError,
                                                           "injected GPU memset failure");
                                        });
  EXPECT_EQ(zeroCalls, 1u);
  EXPECT_EQ(result[0], -static_cast<ssize_t>(StorageClientCode::kRemoteIOError));

  result = {0};
  zeroCalls = 0;
  lib::agent::detail::finishReadResults(result,
                                        reads,
                                        false,
                                        [&](const storage::client::ReadIO &, size_t, size_t) -> Result<Void> {
                                          ++zeroCalls;
                                          return Void{};
                                        });
  EXPECT_EQ(zeroCalls, 0u);
  EXPECT_EQ(result[0], -static_cast<ssize_t>(ClientAgentCode::kHoleInIoOutcome));
}

TEST_F(TestIovTableGdr, FullReadHoleRemainsEofAndIsNotZeroFilled) {
  std::array<uint8_t, 8> data{};
  std::vector<storage::client::ReadIO> reads;
  for (uint32_t chunk = 0; chunk < 2; ++chunk) {
    auto chunkId = storage::ChunkId(meta::ChunkId(meta::InodeId{79}, 0, chunk).pack());
    auto read =
        storageClient_
            .createReadIO({}, chunkId, 0, 4, data.data() + chunk * 4, nullptr, reinterpret_cast<void *>(size_t{0}));
    if (chunk == 0) {
      read.result.lengthInfo = 0;
    } else {
      read.result.lengthInfo = makeError(StorageClientCode::kChunkNotFound);
    }
    reads.push_back(std::move(read));
  }

  std::vector<ssize_t> result{0};
  size_t zeroCalls = 0;
  lib::agent::detail::finishReadResults(result,
                                        reads,
                                        true,
                                        [&](const storage::client::ReadIO &, size_t, size_t) -> Result<Void> {
                                          ++zeroCalls;
                                          return Void{};
                                        });

  EXPECT_EQ(result[0], 0);
  EXPECT_EQ(zeroCalls, 0u);
}

#ifdef HF3FS_ENABLE_GDR

TEST_F(TestIovTableGdr, GpuPublicationRejectsDuplicateKeyAndUuidAndSupportsReverseRemoval) {
  IovTable table;
  table.init(Path("/mnt/3fs"), 3);

  std::shared_ptr<lib::GpuShmBuf> noHardwareBuffer;
  auto owner = user(51, 52);
  auto id = Uuid::random();
  auto key = id.toHexString() + ".gdr.d0";
  Path target("gdr://v2/device/0/allocation/64/offset/0/size/64/ipc/" + std::string(128, '0'));
  auto first =
      std::make_shared<IovEntry>(IovEntry{key, id, target, owner.uid, owner.gid, 3001, IovBuffer{noHardwareBuffer}});
  ASSERT_OK(IovTableTestHelper::insert(table, first));

  auto differentId = Uuid::random();
  auto duplicateKey = std::make_shared<IovEntry>(
      IovEntry{key, differentId, target, owner.uid, owner.gid, 3002, IovBuffer{noHardwareBuffer}});
  ASSERT_ERROR(IovTableTestHelper::insert(table, duplicateKey), MetaCode::kExists);

  auto replacementKey = id.toHexString() + ".gdr.d1";
  auto duplicateId = std::make_shared<IovEntry>(
      IovEntry{replacementKey, id, target, owner.uid, owner.gid, 3003, IovBuffer{noHardwareBuffer}});
  ASSERT_ERROR(IovTableTestHelper::insert(table, duplicateId), MetaCode::kExists);
  ASSERT_OK(table.lookupIov(key.c_str(), owner));

  ASSERT_OK(table.rmIov(key.c_str(), owner));
  ASSERT_OK(IovTableTestHelper::insert(table, duplicateId));
  ASSERT_OK(table.lookupIov(replacementKey.c_str(), owner));
  ASSERT_OK(table.rmIov(replacementKey.c_str(), owner));
  EXPECT_TRUE(table.lookupIov(replacementKey.c_str(), owner).hasError());
}

TEST_F(TestIovTableGdr, TaggedGpuEntryUsesNonNullSlotAndUnifiedMetadataPaths) {
  IovTable table;
  table.init(Path("/mnt/3fs"), 4);

  auto id = Uuid::random();
  auto key = id.toHexString() + ".gdr.d0";
  Path target("gdr://v2/device/0/allocation/4096/offset/0/size/4096/ipc/" + std::string(128, '0'));
  std::shared_ptr<lib::GpuShmBuf> noHardwareBuffer;
  auto entry = std::make_shared<IovEntry>(
      IovEntry{key, id, target, meta::Uid(7), meta::Gid(9), 5150, IovBuffer{noHardwareBuffer}});

  auto iovd = IovTableTestHelper::insert(table, entry);
  ASSERT_OK(iovd);
  auto stored = table.entryAt(*iovd);
  ASSERT_TRUE(stored);
  EXPECT_TRUE(stored->isGpu());

  auto stat = table.statIov(*iovd, user(7, 9));
  ASSERT_OK(stat);
  EXPECT_EQ(stat->asSymlink().target, target);
  EXPECT_EQ(stat->acl.uid, meta::Uid(7));
  EXPECT_EQ(stat->acl.gid, meta::Gid(9));

  auto [dirEntries, inodes] = table.listIovs(user(7, 9));
  ASSERT_EQ(dirEntries->size(), 4);
  EXPECT_EQ(dirEntries->back().name, key);
  ASSERT_TRUE(inodes->back());
  EXPECT_EQ(inodes->back()->asSymlink().target, target);
  ASSERT_ERROR(table.lookupBuf(IovLookupRequest{id, 0, 1}, meta::Uid(8)), MetaCode::kNoPermission);

  auto removed = table.rmIov(key.c_str(), user(7, 9));
  ASSERT_OK(removed);
  EXPECT_FALSE(*removed);
  EXPECT_FALSE(table.entryAt(*iovd));

  auto deadId = Uuid::random();
  auto deadKey = deadId.toHexString() + ".gdr.d0";
  auto deadEntry = std::make_shared<IovEntry>(
      IovEntry{deadKey, deadId, target, meta::Uid(7), meta::Gid(9), 5150, IovBuffer{noHardwareBuffer}});
  auto deadIovd = IovTableTestHelper::insert(table, deadEntry);
  ASSERT_OK(deadIovd);
  EXPECT_TRUE(table.removeIovsByPid(5150).empty());
  EXPECT_FALSE(table.entryAt(*deadIovd));

  auto clearId = Uuid::random();
  auto clearKey = clearId.toHexString() + ".gdr.d0";
  auto clearEntry = std::make_shared<IovEntry>(
      IovEntry{clearKey, clearId, target, meta::Uid(7), meta::Gid(9), 6160, IovBuffer{noHardwareBuffer}});
  auto clearIovd = IovTableTestHelper::insert(table, clearEntry);
  ASSERT_OK(clearIovd);
  table.clearGpuIovs();
  EXPECT_FALSE(table.entryAt(*clearIovd));
}

#endif

}  // namespace hf3fs::fuse
