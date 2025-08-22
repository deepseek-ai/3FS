#include <folly/experimental/TestUtil.h>
#include <gtest/gtest.h>

#include "kv/KVStore.h"

namespace hf3fs::kv::test {
namespace {

struct TestKVStore : public testing::TestWithParam<KVStore::Type> {};
TEST_P(TestKVStore, Normal) {
  folly::test::TemporaryDirectory tmpPath;

  KVStore::Config config;
  config.set_type(GetParam());
  KVStore::Options options;
  options.type = config.type();
  options.path = tmpPath.path();
  options.createIfMissing = true;
  auto store = KVStore::create(config, options);
  ASSERT_NE(store, nullptr);

  const std::string key = "hello";
  const std::string val = "world";
  ASSERT_FALSE(store->get(key));
  ASSERT_TRUE(store->put(key, val));

  auto result = store->get(key);
  ASSERT_TRUE(result);
  ASSERT_EQ(result.value(), val);

  const std::string key2 = "hello2";
  const std::string val2 = "world2";
  auto batchOp = store->createBatchOps();
  batchOp->put(key2, val2);
  batchOp->remove(key);
  ASSERT_TRUE(batchOp->commit());

  ASSERT_FALSE(store->get(key));
  auto result2 = store->get(key2);
  ASSERT_TRUE(result2);
  ASSERT_EQ(result2.value(), val2);

  auto iterator = store->createIterator();
  iterator->seekToFirst();
  ASSERT_TRUE(iterator->valid());
  ASSERT_EQ(iterator->key(), "hello2");
  ASSERT_EQ(iterator->value(), "world2");
  iterator->next();
  ASSERT_FALSE(iterator->valid());
}
INSTANTIATE_TEST_SUITE_P(Benchmark,
                         TestKVStore,
                         testing::Values(KVStore::Type::LevelDB, KVStore::Type::RocksDB, KVStore::Type::MemDB));

}  // namespace
}  // namespace hf3fs::kv::test
