#include <algorithm>
#include <fmt/core.h>
#include <folly/Random.h>
#include <gtest/gtest.h>
#include <vector>

#include "common/utils/FaultInjection.h"
#include "common/utils/StatusCode.h"
#include "meta/store/MetaStore.h"
#include "tests/meta/MetaTestBase.h"

namespace hf3fs::meta::server {

template <typename KV>
class TestList : public MetaTestBase<KV> {};

using KVTypes = ::testing::Types<mem::MemKV, fdb::DB>;
TYPED_TEST_SUITE(TestList, KVTypes);

TYPED_TEST(TestList, FaultInjection) {
  folly::coro::blockingWait([&]() -> CoTask<void> {
    auto cluster = this->createMockCluster();
    auto &meta = cluster.meta().getOperator();

    std::vector<std::string> paths;
    size_t numFiles = folly::Random::rand32(0, 4096);
    for (size_t i = 0; i < numFiles; i++) {
      std::string path = std::to_string(i);
      auto result = co_await meta.create({SUPER_USER, path, {}, O_RDONLY, p644});
      CO_ASSERT_OK(result);
      paths.push_back(path);
    }
    std::sort(paths.begin(), paths.end());

    for (size_t i = 0; i < 10; i++) {
      FAULT_INJECTION_SET(10, 3);  // 10%, 3 faults
      std::vector<DirEntry> entries;
      bool hasMore = true;
      const bool needStatus = folly::Random::oneIn(2);
      String prev;

      // do load
      while (hasMore) {
        auto limit = folly::Random::rand32(0, 512);
        auto req = i % 2 == 0 ? ListReq(SUPER_USER, Path("/"), prev, (int)limit, needStatus)
                              : ListReq(SUPER_USER, InodeId::root(), prev, (int)limit, needStatus);
        auto list = co_await meta.list(req);
        CO_ASSERT_OK(list);
        if (needStatus) {
          CO_ASSERT_TRUE(list->inodes.size() == list->entries.size());
        }
        for (size_t i = 0; i < list->entries.size(); i++) {
          if (needStatus) {
            auto entry = list->entries.at(i);
            auto inode = list->inodes.at(i);
            entries.push_back(entry);
            CO_ASSERT_EQ(entry.id, inode.id);
          } else {
            entries.push_back(list->entries.at(i));
          }
        }

        hasMore = list->more;
        if (list->entries.size() == 0) {
          CO_ASSERT_FALSE(hasMore);
        }
        if (hasMore) {
          prev = list->entries.at(list->entries.size() - 1).name;
        }
      }

      // check
      CO_ASSERT_EQ(entries.size(), numFiles);
      for (size_t i = 0; i < numFiles; i++) {
        auto entry = entries[i];
        CO_ASSERT_EQ(entry.name, paths[i]);
      }
    }
  }());
}
}  // namespace hf3fs::meta::server
