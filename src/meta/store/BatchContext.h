#pragma once

#include <cassert>
#include <folly/Likely.h>
#include <folly/Synchronized.h>
#include <folly/Utility.h>
#include <folly/experimental/coro/Baton.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/Request.h>
#include <folly/logging/xlog.h>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"
#include "meta/store/DirEntry.h"
#include "meta/store/Inode.h"
namespace hf3fs::meta::server {

class BatchContext : public folly::RequestData {
 public:
  template <typename T>
  struct SharedFuture : folly::NonCopyableNonMovable {
    Result<T> value = makeError(StatusCode::kUnknown);
    folly::coro::Baton baton;
  };

  template <typename T>
  struct LoadGuard {
    bool needLoad;
    std::shared_ptr<SharedFuture<T>> future;

    LoadGuard(bool needLoad, std::shared_ptr<SharedFuture<T>> future)
        : needLoad(needLoad),
          future(future) {}

    ~LoadGuard() {
      if (needLoad && !future->baton.ready()) {
        future->value = makeError(StatusCode::kUnknown, "load failed in BatchContext");
        future->baton.post();
      }
    }

    void set(const Result<T> &r) {
      assert(!future->baton.ready());
      future->value = r;
      future->baton.post();
    }

    CoTryTask<T> coAwait() {
      co_await future->baton;
      co_return future->value;
    }
  };

  static folly::ShallowCopyRequestContextScopeGuard create() {
    return folly::ShallowCopyRequestContextScopeGuard{token(), std::make_unique<BatchContext>()};
  }

  static inline BatchContext *get() {
    auto requestContext = folly::RequestContext::try_get();
    if (LIKELY(requestContext == nullptr)) {
      return nullptr;
    }
    return dynamic_cast<BatchContext *>(requestContext->getContextData(token()));
  }

  LoadGuard<std::optional<Inode>> loadInode(InodeId inodeId) {
    return loadImpl<InodeId, std::optional<Inode>>(inodes_, inodeId);
  }

  LoadGuard<std::optional<DirEntry>> loadDirEntry(InodeId parent, std::string name) {
    return loadImpl<std::pair<InodeId, std::string>, std::optional<DirEntry>>(entries_, {parent, std::move(name)});
  }

  bool hasCallback() override { return false; }

 private:
  static constexpr const char *kTokenName = "hf3fs::meta::server::BatchContext";

  static folly::RequestToken const &token() {
    static folly::RequestToken const token(kTokenName);
    return token;
  }

  template <typename K, typename T>
  using SynchronizedFutureMap = folly::Synchronized<std::map<K, std::shared_ptr<SharedFuture<T>>>, std::mutex>;

  template <typename K, typename T>
  LoadGuard<T> loadImpl(SynchronizedFutureMap<K, T> &map, K key) {
    auto guard = map.lock();
    auto iter = guard->find(key);
    if (iter != guard->end()) {
      return LoadGuard<T>(false, iter->second);
    }
    auto future = std::make_shared<SharedFuture<T>>();
    guard->emplace(std::move(key), future);
    return LoadGuard<T>(true, future);
  }

  SynchronizedFutureMap<InodeId, std::optional<Inode>> inodes_;
  SynchronizedFutureMap<std::pair<InodeId, std::string>, std::optional<DirEntry>> entries_;
};

}  // namespace hf3fs::meta::server