#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <gtest/gtest.h>

#include "common/utils/Coroutine.h"
#include "common/utils/Tracing.h"
#include "common/utils/TracingEvent.h"

namespace hf3fs::tests {
namespace {
TEST(Tracing, testEventToString) {
  ASSERT_EQ(tracing::toString(tracing::kFdbBeginNewTransaction), "Fdb::BeginNewTransaction");
  ASSERT_EQ(tracing::toString(tracing::kFdbEndNewTransaction), "Fdb::EndNewTransaction");
  ASSERT_EQ(tracing::toString(tracing::kFdbBeginCommitTransaction), "Fdb::BeginCommitTransaction");
  ASSERT_EQ(tracing::toString(tracing::kFdbEndCommitTransaction), "Fdb::EndCommitTransaction");
  ASSERT_EQ(tracing::toString(tracing::kFdbBeginCancelTransaction), "Fdb::BeginCancelTransaction");
  ASSERT_EQ(tracing::toString(tracing::kFdbEndCancelTransaction), "Fdb::EndCancelTransaction");

  ASSERT_EQ(tracing::toString(tracing::kMetaLoadInode), "Meta::LoadInode");
  ASSERT_EQ(tracing::toString(tracing::kMetaBeginLoadInode), "Meta::BeginLoadInode");
  ASSERT_EQ(tracing::toString(tracing::kMetaEndLoadInode), "Meta::EndLoadInode");

  ASSERT_EQ(tracing::toString(tracing::kMetaSnapshotLoadInode), "Meta::SnapshotLoadInode");
  ASSERT_EQ(tracing::toString(tracing::kMetaBeginSnapshotLoadInode), "Meta::BeginSnapshotLoadInode");
  ASSERT_EQ(tracing::toString(tracing::kMetaEndSnapshotLoadInode), "Meta::EndSnapshotLoadInode");

  ASSERT_EQ(tracing::toString(tracing::kMetaLoadDirEntry), "Meta::LoadDirEntry");
  ASSERT_EQ(tracing::toString(tracing::kMetaBeginLoadDirEntry), "Meta::BeginLoadDirEntry");
  ASSERT_EQ(tracing::toString(tracing::kMetaEndLoadDirEntry), "Meta::EndLoadDirEntry");

  ASSERT_EQ(tracing::toString(tracing::kMetaSnapshotLoadDirEntry), "Meta::SnapshotLoadDirEntry");
  ASSERT_EQ(tracing::toString(tracing::kMetaBeginSnapshotLoadDirEntry), "Meta::BeginSnapshotLoadDirEntry");
  ASSERT_EQ(tracing::toString(tracing::kMetaEndSnapshotLoadDirEntry), "Meta::EndSnapshotLoadDirEntry");
}

TEST(Tracing, testScopeGuard) {
  tracing::Points points1, points2;
  {
    SCOPE_SET_TRACING_POINTS(points1);
    TRACING_ADD_SCOPE_EVENT(tracing::kFdbNewTransaction);

    {
      SCOPE_SET_TRACING_POINTS(points2);
      TRACING_ADD_SCOPE_EVENT(tracing::kFdbCommitTransaction);
    }

    TRACING_ADD_EVENT(tracing::kFdbBeginCancelTransaction);
  }
  TRACING_ADD_EVENT(tracing::kFdbEndCancelTransaction);

  auto v1 = points1.extractAll();
  ASSERT_EQ(v1.size(), 3);
  ASSERT_EQ(v1[0].event(), tracing::kFdbBeginNewTransaction);
  ASSERT_EQ(v1[1].event(), tracing::kFdbBeginCancelTransaction);
  ASSERT_EQ(v1[2].event(), tracing::kFdbEndNewTransaction);

  auto v2 = points2.extractAll();
  ASSERT_EQ(v2.size(), 2);
  ASSERT_EQ(v2[0].event(), tracing::kFdbBeginCommitTransaction);
  ASSERT_EQ(v2[1].event(), tracing::kFdbEndCommitTransaction);
}

TEST(Tracing, testPassThroughCoroutine) {
  tracing::Points points;
  folly::CPUThreadPoolExecutor executor(1);
  {
    SCOPE_SET_TRACING_POINTS(points);
    folly::coro::blockingWait([&]() -> CoTask<void> {
      TRACING_ADD_SCOPE_EVENT(tracing::kFdbNewTransaction);
      auto f = []() -> CoTask<void> {
        TRACING_ADD_SCOPE_EVENT(tracing::kFdbCommitTransaction);
        co_return;
      };
      co_await f().scheduleOn(folly::getKeepAliveToken(executor));
    }());
  }

  auto v = points.extractAll();
  ASSERT_EQ(v.size(), 4);
  ASSERT_EQ(v[0].event(), tracing::kFdbBeginNewTransaction);
  ASSERT_EQ(v[1].event(), tracing::kFdbBeginCommitTransaction);
  ASSERT_EQ(v[2].event(), tracing::kFdbEndCommitTransaction);
  ASSERT_EQ(v[3].event(), tracing::kFdbEndNewTransaction);
}

TEST(Tracing, testPointMessage) {
  auto t = UtcClock::now();
  tracing::Point p0(t, 0);
  ASSERT_EQ(p0.timestamp(), t);
  ASSERT_EQ(p0.event(), 0);
  ASSERT_EQ(p0.message(), "");

  std::string_view msg0 = "A very long message which contains more than 16 chars";
  p0.setMessage(msg0);
  ASSERT_EQ(p0.message(), msg0);
  ASSERT_NE(p0.message().data(), msg0.data());

  std::string_view msg1 = "A short message";
  p0.setMessage(msg1);
  ASSERT_EQ(p0.message(), msg1);
}

TEST(Tracing, testPointMove) {
  auto t0 = UtcClock::now();
  std::string_view msg0 = "A very long message which contains more than 16 chars";
  tracing::Point p0(t0, 0, msg0);
  ASSERT_EQ(p0.timestamp(), t0);
  ASSERT_EQ(p0.event(), 0);
  ASSERT_EQ(p0.message(), msg0);

  auto t1 = t0 + std::chrono::microseconds(1);
  std::string_view msg1 = "Yet another very long message but shorter than msg0";
  {
    tracing::Point p1(t1, 1, msg1);
    tracing::Point p2 = std::move(p1);
    ASSERT_EQ(p1.message(), "");

    ASSERT_EQ(p2.timestamp(), t1);
    ASSERT_EQ(p2.event(), 1);
    ASSERT_EQ(p2.message(), msg1);

    p0 = std::move(p2);
    ASSERT_EQ(p2.message(), "");
  }
  ASSERT_EQ(p0.timestamp(), t1);
  ASSERT_EQ(p0.event(), 1);
  ASSERT_EQ(p0.message(), msg1);
}
}  // namespace
}  // namespace hf3fs::tests
