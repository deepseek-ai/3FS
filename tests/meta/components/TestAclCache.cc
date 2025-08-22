#include <atomic>
#include <folly/Random.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <gtest/gtest.h>
#include <map>
#include <thread>
#include <vector>

#include "common/utils/Coroutine.h"
#include "common/utils/UtcTime.h"
#include "fbs/meta/Schema.h"
#include "meta/components/AclCache.h"

namespace hf3fs::meta::server {

TEST(TestAclCache, basic) {
  std::map<InodeId, Acl> map;
  AclCache cache(1 << 20);
  for (size_t i = 0; i < (64 << 10); i++) {
    InodeId inode{folly::Random::rand64()};
    Acl acl{flat::Uid(folly::Random::rand32()),
            flat::Gid(folly::Random::rand32()),
            meta::Permission(folly::Random::rand32())};
    map[inode] = acl;
    cache.set(inode, acl);
  }

  for (auto [inode, acl] : map) {
    ASSERT_EQ(cache.get(inode, 0_s), std::nullopt);
    ASSERT_EQ(cache.get(inode, 1_h), acl);
    cache.invalid(inode);
    ASSERT_EQ(cache.get(inode, 1_h), std::nullopt);
    Acl newacl{flat::Uid(folly::Random::rand32()),
               flat::Gid(folly::Random::rand32()),
               meta::Permission(folly::Random::rand32())};
    cache.set(inode, newacl);
    ASSERT_EQ(cache.get(inode, 1_h), newacl);
  }
}

TEST(TestAclCache, benchmark) {
  std::vector<InodeId> inodes;
  AclCache cache(4 << 20);
  for (size_t i = 0; i < (1 << 20); i++) {
    InodeId inode{folly::Random::rand64(1 << 20)};
    inodes.emplace_back(inode);
    cache.set(inode, Acl());
  }

  std::atomic<size_t> total;
  std::vector<std::jthread> threads;
  for (size_t i = 0; i < 8; i++) {
    threads.emplace_back([&]() {
      size_t ops = 0;
      auto now = SteadyClock::now();
      while (SteadyClock::now() - now < 1_s) {
        auto inode = inodes.at(folly::Random::rand64(inodes.size()));
        if (folly::Random::oneIn(4)) {
          // set
          cache.set(inode, Acl());
        } else if (folly::Random::oneIn(4)) {
          cache.invalid(inode);
        } else {
          // get
          cache.get(inode, 1_h);
        }
        ops += 1;
      }
      total += ops;
    });
  }

  for (auto &th : threads) {
    th.join();
  }

  fmt::print("total {} ops in 1s\n", total.load());
}

}  // namespace hf3fs::meta::server
