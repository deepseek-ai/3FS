#pragma once

#include "client/cli/common/IEnv.h"
#include "client/core/CoreClient.h"
#include "client/meta/MetaClient.h"
#include "client/mgmtd/IMgmtdClientForAdmin.h"
#include "client/storage/StorageClient.h"
#include "common/kv/IKVEngine.h"
#include "fbs/core/user/User.h"

namespace hf3fs::client::cli {
struct AdminEnv : IEnv {
  flat::UserInfo userInfo{flat::Uid{0}, flat::Gid{0}, ""};
  meta::InodeId currentDirId = meta::InodeId::root();
  String currentDir = "/";

  std::function<std::shared_ptr<net::Client>()> clientGetter;
  std::function<std::shared_ptr<meta::client::MetaClient>()> metaClientGetter;
  std::function<std::shared_ptr<kv::IKVEngine>()> kvEngineGetter;
  std::function<std::shared_ptr<IMgmtdClientForAdmin>()> mgmtdClientGetter;
  std::function<std::shared_ptr<IMgmtdClientForAdmin>()> unsafeMgmtdClientGetter;
  std::function<std::shared_ptr<storage::client::StorageClient>()> storageClientGetter;
  std::function<std::shared_ptr<CoreClient>()> coreClientGetter;
};
}  // namespace hf3fs::client::cli
