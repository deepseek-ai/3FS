#include <cstdlib>
#include <double-conversion/utils.h>
#include <fmt/compile.h>
#include <fmt/core.h>
#include <folly/Random.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/experimental/coro/GtestHelpers.h>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "common/kv/IKVEngine.h"
#include "common/kv/mem/MemKVEngine.h"
#include "common/utils/Coroutine.h"
#include "common/utils/UtcTime.h"
#include "fbs/mgmtd/ChainRef.h"
#include "fbs/mgmtd/MgmtdTypes.h"
#include "meta/store/Inode.h"
#include "meta/store/Utils.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

using flat::ChainId;
using flat::ChainRef;
using flat::ChainTableId;
using flat::ChainTableVersion;

TEST(TestInodeId, Special) {
  auto root = InodeId::root();
  auto gcRoot = InodeId::gcRoot();
  ASSERT_NE(root, gcRoot);
  ASSERT_EQ(root.u64(), 0);
  ASSERT_EQ(gcRoot.u64(), 1);
  ASSERT_EQ(root, InodeId::root());
  ASSERT_EQ(gcRoot, InodeId::gcRoot());

  ASSERT_EQ(fmt::format("{}", root), "0x0000000000000000");
  ASSERT_EQ(fmt::format("{}", gcRoot), "0x0000000000000001");
  ASSERT_EQ(root.toHexString(), "0x0000000000000000");
  ASSERT_EQ(gcRoot.toHexString(), "0x0000000000000001");
}

template <typename KV>
class TestInode : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestInode, KVTypes);

TYPED_TEST(TestInode, File) {
  Inode inode = Inode::newFile(MetaTestHelper::randomInodeId(),
                               Acl(Uid(0), Gid(0), Permission(0644)),
                               Layout(),
                               UtcClock::now().castGranularity(1_s));

  ASSERT_TRUE(inode.isFile());
  ASSERT_FALSE(inode.isDirectory());
  ASSERT_FALSE(inode.isSymlink());
  ASSERT_EQ(inode.getType(), InodeType::File);
  ASSERT_NO_THROW(inode.asFile());
}

TYPED_TEST(TestInode, FileLayout) {
  auto layout = Layout::newEmpty(ChainTableId(1), 1000, 4);
  ASSERT_TRUE(std::holds_alternative<Layout::Empty>(layout.chains));
  ASSERT_FALSE(layout.valid(true));
  layout = Layout::newEmpty(ChainTableId(1), 1024, 4);
  ASSERT_TRUE(layout.valid(true));
  ASSERT_FALSE(layout.valid(false));
  ASSERT_EQ(layout.getChainIndexList().size(), 0);

  layout.chains = Layout::ChainList{{1}};
  ASSERT_FALSE(layout.valid(false));

  layout.chains = Layout::ChainList{{1, 2, 3, 4}};
  ASSERT_FALSE(layout.valid(false)) << fmt::format("{}", layout);
  layout.tableVersion = ChainTableVersion(1);
  ASSERT_TRUE(layout.valid(false)) << fmt::format("{}", layout);
  ASSERT_TRUE(std::holds_alternative<Layout::ChainList>(layout.chains));
  ASSERT_EQ(layout.getChainIndexList().size(), layout.stripeSize);

  layout.chains = Layout::Empty();
  layout.chains = Layout::ChainRange(1, Layout::ChainRange::STD_SHUFFLE_MT19937, 0);
  ASSERT_TRUE(layout.valid(false));
  ASSERT_TRUE(std::holds_alternative<Layout::ChainRange>(layout.chains));
  ASSERT_EQ(layout.getChainIndexList().size(), layout.stripeSize);

  std::set<uint32_t> set;
  for (auto chain : layout.getChainIndexList()) {
    ASSERT_FALSE(set.contains(chain));
    set.emplace(chain);
  }
}

TYPED_TEST(TestInode, ChunkId) {
  Inode inode = Inode::newFile(MetaTestHelper::randomInodeId(),
                               Acl(Uid(0), Gid(0), Permission(0644)),
                               Layout(),
                               UtcClock::now().castGranularity(1_s));
  ASSERT_FALSE(inode.asFile().getChunkId(inode.id, 0));

  inode.asFile().layout = Layout::newEmpty(ChainTableId(1), 4096, 1);
  ASSERT_TRUE(inode.asFile().getChunkId(inode.id, 0));
  ASSERT_EQ(*inode.asFile().getChunkId(inode.id, 100), ChunkId(inode.id, 0, 0));
  ASSERT_EQ(*inode.asFile().getChunkId(inode.id, 9999), ChunkId(inode.id, 0, 2));
  ASSERT_EQ(inode.asFile().getChunkId(inode.id, 1ul << 63).error().code(), MetaCode::kFileTooLarge);

  for (int i = 0; i < 10; i++) {
    auto inode = meta::InodeId(folly::Random::rand64());
    auto chunk = folly::Random::rand32();
    ChunkId chunkId(inode, 0, chunk);
    ASSERT_EQ(chunkId.chunk(), chunk);
    ASSERT_EQ(chunkId.inode(), inode);
    auto packed = chunkId.pack();
    ASSERT_EQ(chunkId, ChunkId::unpack(packed));
  }
}

TYPED_TEST(TestInode, SerializedSize) {
  auto layout = Layout::newEmpty(ChainTableId(1), 4096, 1);
  layout.chains = Layout::ChainRange(5, Layout::ChainRange::NO_SHUFFLE, 0);
  auto file = Inode::newFile(MetaTestHelper::randomInodeId(),
                             Acl(Uid(0), Gid(0), Permission(0644)),
                             layout,
                             UtcClock::now().castGranularity(1_s));
  file.asFile().length = 100_MB;
  fmt::print("File with range layout, serialized {} bytes, data {} type {}, asFile {}.\n",
             serde::serialize(file).size(),
             serde::serialize(file.data()).size(),
             serde::serialize(file.data().type).size(),
             serde::serialize(file.data().asFile()).size());

  auto directory = Inode::newDirectory(MetaTestHelper::randomInodeId(),
                                       MetaTestHelper::randomInodeId(),
                                       "directory-name",
                                       Acl(Uid(0), Gid(0), Permission(0644)),
                                       layout,
                                       UtcClock::now().castGranularity(1_s));
  fmt::print("Directory with layout, serialized {} {} bytes.\n",
             serde::serialize(directory.data()).size(),
             serde::serialize(directory).size());
}

TYPED_TEST(TestInode, Directory) {
  Inode inode = Inode::newDirectory(MetaTestHelper::randomInodeId(),
                                    MetaTestHelper::randomInodeId(),
                                    "directory",
                                    Acl(Uid(0), Gid(0), Permission(0644)),
                                    Layout::newEmpty(ChainTableId(1), 512 << 10, 64),
                                    UtcClock::now().castGranularity(1_s));

  ASSERT_FALSE(inode.isFile());
  ASSERT_TRUE(inode.isDirectory());
  ASSERT_FALSE(inode.isSymlink());
  ASSERT_EQ(inode.getType(), InodeType::Directory);
  ASSERT_NO_THROW(inode.asDirectory());
}

TYPED_TEST(TestInode, Symlink) {
  Inode inode = Inode::newSymlink(MetaTestHelper::randomInodeId(),
                                  std::to_string(folly::Random::rand32()),
                                  Uid(0),
                                  Gid(0),
                                  UtcClock::now().castGranularity(1_s));

  ASSERT_FALSE(inode.isFile());
  ASSERT_FALSE(inode.isDirectory());
  ASSERT_TRUE(inode.isSymlink());
  ASSERT_EQ(inode.getType(), InodeType::Symlink);
  ASSERT_NO_THROW(inode.asSymlink());
}

TYPED_TEST(TestInode, LoadStore) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    static constexpr size_t kInodes = 1000;
    std::vector<Inode> inodes;
    for (size_t i = 0; i < kInodes; i++) {
      auto inode = MetaTestHelper::randomInode();
      READ_WRITE_TRANSACTION_OK({
        auto storeResult = co_await inode.store(*txn);
        CO_ASSERT_OK(storeResult);
      });

      inodes.push_back(inode);
    }

    for (auto &inode : inodes) {
      READ_ONLY_TRANSACTION({
        auto loadResult = (co_await Inode::snapshotLoad(*txn, inode.id)).then(checkMetaFound<Inode>);
        CO_ASSERT_OK(loadResult);
        auto other = std::move(loadResult.value());
        CO_ASSERT_EQ(inode, other) << inode.isFile();
        if (inode.isFile()) {
          auto cnts = inode.asFile().layout.getChainIndexList().size();
          CO_ASSERT_EQ(cnts, other.asFile().layout.getChainIndexList().size());
          for (size_t i = 0; i < cnts; i++) {
            CO_ASSERT_EQ(inode.asFile().layout.getChainIndexList()[i], other.asFile().layout.getChainIndexList()[i]);
          }
        }
      });
    }

    for (auto &inode : inodes) {
      READ_WRITE_TRANSACTION_OK({
        auto removeResult = co_await inode.remove(*txn);
        CO_ASSERT_OK(removeResult);
      });
    }

    for (auto &inode : inodes) {
      READ_ONLY_TRANSACTION({
        auto loadResult = (co_await Inode::snapshotLoad(*txn, inode.id)).then(checkMetaFound<Inode>);
        CO_ASSERT_ERROR(loadResult, MetaCode::kNotFound);
      });
    }
  }());
}

TYPED_TEST(TestInode, Conflict) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    for (int i = 0; i < 1000; i++) {
      // txn1 update inode, txn2 include inode into it's read conflict set (by load or addReadConflict) and set a random
      // key, txn1 commit first, txn2 should fail
      Inode inode = MetaTestHelper::randomInode();
      CO_ASSERT_CONFLICT([&](auto &txn) -> CoTask<void> { CO_ASSERT_OK(co_await inode.store(txn)); },
                         [&](auto &txn) -> CoTask<void> {
                           if (folly::Random::rand32() % 2) {
                             CO_ASSERT_OK(co_await Inode::load(txn, inode.id));
                           } else {
                             // use snapshotLoad to get a read version
                             CO_ASSERT_OK(co_await Inode::snapshotLoad(txn, inode.id));
                             CO_ASSERT_OK(co_await Inode(inode.id).addIntoReadConflict(txn));
                           }
                           auto set = co_await txn.set(std::to_string(folly::Random::rand32()),
                                                       std::to_string(folly::Random::rand32()));
                           CO_ASSERT_OK(set);
                         });
    }
  }());
}

}  // namespace hf3fs::meta::server
