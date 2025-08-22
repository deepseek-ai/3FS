#include <folly/logging/xlog.h>
#include <memory>
#include <stack>
#include <vector>

#include "common/kv/ITransaction.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Result.h"
#include "meta/store/DirEntry.h"
#include "meta/store/MetaStore.h"
#include "meta/store/Operation.h"
#include "meta/store/Utils.h"

namespace hf3fs::meta::server {

static Path simplifyPath(const Path &path) {
  std::vector<Path> components;
  bool absolute = false;
  for (auto &name : path) {
    if (name == ".") {
      continue;
    } else if (name == "..") {
      if (!components.empty() && components.back() != "..") {
        components.pop_back();
      } else if (!absolute) {
        components.push_back(name);
      }
    } else if (name == "/") {
      absolute = true;
      components.clear();
    } else {
      components.push_back(name);
    }
  }

  Path p = absolute ? "/" : "";
  for (auto &name : components) {
    p = p / name;
  }

  XLOGF(DBG, "before {}, after {}", path, p);
  return p.empty() ? "." : p;
}

/** MetaStore::getRealPath */
class GetRealPathOp : public ReadOnlyOperation<GetRealPathRsp> {
 public:
  GetRealPathOp(MetaStore &meta, const GetRealPathReq &req)
      : ReadOnlyOperation<GetRealPathRsp>(meta),
        req_(req) {}

  OPERATION_TAGS(req_);

  CoTryTask<GetRealPathRsp> run(IReadOnlyTransaction &txn) override {
    XLOGF(DBG, "GetRealPathOp: {}", req_);

    CHECK_REQUEST(req_);

    DirEntry entry;
    if (req_.path.path.has_value()) {
      Path trace;
      auto result = co_await resolve(txn, req_.user, &trace).dirEntry(req_.path, AtFlags(AT_SYMLINK_FOLLOW));
      CO_RETURN_ON_ERROR(result);
      if (!req_.absolute) {
        co_return GetRealPathRsp(simplifyPath(trace));
      }
      entry = std::move(*result);
    } else {
      auto inode = (co_await Inode::snapshotLoad(txn, req_.path.parent)).then(checkMetaFound<Inode>);
      CO_RETURN_ON_ERROR(inode);
      if (!inode->isDirectory()) {
        co_return makeError(MetaCode::kNotDirectory, "Only support get absolute path of directory");
      } else if (UNLIKELY(inode->id.isTreeRoot())) {
        co_return GetRealPathRsp("/");
      }

      auto result = co_await inode->snapshotLoadDirEntry(txn);
      CO_RETURN_ON_ERROR(result);
      entry = std::move(*result);
    }

    Path path = entry.name;
    while (!entry.parent.isTreeRoot()) {
      auto parent = (co_await Inode::snapshotLoad(txn, entry.parent)).then(checkMetaFound<Inode>);
      CO_RETURN_ON_ERROR(parent);
      auto result = co_await parent->snapshotLoadDirEntry(txn);
      CO_RETURN_ON_ERROR(result);
      entry = std::move(*result);
      XLOGF(DBG, "get {}", entry.name);
      path = entry.name / path;
    }

    co_return simplifyPath("/" / path);
  }

 private:
  const GetRealPathReq &req_;
};

MetaStore::OpPtr<GetRealPathRsp> MetaStore::getRealPath(const GetRealPathReq &req) {
  return std::make_unique<GetRealPathOp>(*this, req);
}

}  // namespace hf3fs::meta::server
