#include "UserConfig.h"

#include "fbs/meta/Common.h"

namespace hf3fs::fuse {
void UserConfig::init(FuseConfig &config) {
  config_ = &config;
  configs_.reset(new AtomicSharedPtrTable<LocalConfig>(config.max_uid() + 1));

  storageMaxConcXmit_ = config.storage().net_client().rdma_control().max_concurrent_transmission();

  config.addCallbackGuard([&config = config, this] {
    storageMaxConcXmit_ = config.storage().net_client().rdma_control().max_concurrent_transmission();

    std::lock_guard lock(userMtx_);
    for (auto u : users_) {
      auto lconf = configs_->table[u.toUnderType()].load();
      if (!lconf) {
        continue;
      }

      FuseConfig conf2 = config;

      std::lock_guard lock2(lconf->mtx);
      conf2.atomicallyUpdate(lconf->updatedItems, true);
      lconf->config = std::move(conf2);
    }
  });
}

Result<std::pair<bool, int>> UserConfig::parseKey(const char *key) {
  if (!strncmp(key, "sys.", 4)) {
    auto it = std::find(systemKeys.begin(), systemKeys.end(), key + 4);
    if (it == systemKeys.end()) {
      return makeError(StatusCode::kInvalidArg, fmt::format("no such system key or key not customizable {}", key));
    } else {
      return std::make_pair(true, it - systemKeys.begin());
    }
  } else if (!strncmp(key, "usr.", 4)) {
    auto it = std::find(userKeys.begin(), userKeys.end(), key + 4);
    if (it == userKeys.end()) {
      return makeError(StatusCode::kInvalidArg, fmt::format("no such user key or key not customizable {}", key));
    } else {
      return std::make_pair(false, it - userKeys.begin());
    }
  } else {
    return makeError(StatusCode::kInvalidArg, fmt::format("key {} has to be prefixed with 'sys.' or 'usr.'", key));
  }
}

Result<meta::Inode> UserConfig::setConfig(const char *key, const char *val, const meta::UserInfo &ui) {
  auto kres = parseKey(key);
  RETURN_ON_ERROR(kres);
  key += 4;

  auto [isSys, kidx] = *kres;
  if (isSys) {
    if (!strcmp(key, "storage.net_client.rdma_control.max_concurrent_transmission")) {
      auto n = atoi(val);
      if (n <= 0 || n > 2 * storageMaxConcXmit_) {
        return makeError(
            StatusCode::kInvalidArg,
            fmt::format(
                "invalid value '{}' for key '{}', possible reason is it is larger than twice of system setting {}",
                val,
                key - 4,
                storageMaxConcXmit_.load()));
      }
    }

    RETURN_ON_ERROR(config_->atomicallyUpdate({std::make_pair(key, val)}, true));
    return meta::Inode{configIid(false, true, kidx),
                       {meta::Symlink{val}, meta::Acl{ui.uid, ui.gid, meta::Permission{0400}}}};
  } else {
    if (!strcmp(key, "readonly") && strcmp(val, "true") && config_->readonly()) {
      // if readonly is turned on cluster-wide, user cannot disable if locally
      return makeError(StatusCode::kInvalidArg, "cannot turn off readonly mode when it is turned on by the sys admin");
    }

    auto uid = ui.uid;

    std::lock_guard lock(userMtx_);
    auto uit = users_.find(uid);
    auto uidx = uid.toUnderType();

    if (uit == users_.end()) {
      if (uidx >= configs_->table.size()) {
        return makeError(MetaCode::kNoPermission, fmt::format("uid {} too large for user config", uid));
      }

      configs_->table[uidx].store(std::make_shared<LocalConfig>(*config_));
      users_.insert(uid);
    }

    auto lconf = configs_->table[uidx].load();

    auto kv = std::make_pair(key, val);
    auto res = lconf->config.atomicallyUpdate({kv}, true);
    RETURN_ON_ERROR(res);
    lconf->updatedItems.emplace_back(std::move(kv));

    return meta::Inode{configIid(false, false, kidx), {meta::Symlink{val}, {uid, ui.gid, meta::Permission{0400}}}};
  }

  return makeError(MetaCode::kNoPermission, fmt::format("key '{}' not found in config, or not allowed to be set", key));
}

Result<meta::Inode> UserConfig::lookupConfig(const char *key, const meta::UserInfo &ui) {
  auto kres = parseKey(key);
  RETURN_ON_ERROR(kres);
  key += 4;

  auto [isSys, kidx] = *kres;
  return statConfig(configIid(true, isSys, kidx), ui);
}

const FuseConfig &UserConfig::getConfig(const meta::UserInfo &ui) {
  auto uid = ui.uid;

  std::lock_guard lock(userMtx_);
  auto it = users_.find(uid);
  if (it == users_.end()) {
    return *config_;
  } else {
    auto lconf = configs_->table[uid.toUnderType()].load();
    return lconf->config;
  }
}

Result<meta::Inode> UserConfig::statConfig(meta::InodeId iid, const meta::UserInfo &ui) {
  auto kidx = (int64_t)(meta::InodeId::getConf().u64() - 1 - iid.u64());
  if (kidx < 0 || kidx >= (int64_t)(systemKeys.size() + userKeys.size())) {
    return makeError(MetaCode::kNotFound, "iid not a config entry");
  }
  auto isSys = kidx < (int64_t)systemKeys.size();
  if (!isSys) {
    kidx -= (int)systemKeys.size();
  }

  auto config = isSys ? *config_ : getConfig(ui);
  auto key = isSys ? systemKeys[kidx] : userKeys[kidx];
  return meta::Inode{iid,
                     {meta::Symlink{config.find(key).value()->toString()},
                      meta::Acl{ui.uid, ui.gid, meta::Permission{isSys ? 0444u : 0400u}}}};
}

std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
UserConfig::listConfig(const meta::UserInfo &ui) {
  meta::DirEntry de{meta::InodeId::getConf(), ""};

  std::vector<meta::DirEntry> des;
  std::vector<std::optional<meta::Inode>> ins;
  des.reserve(systemKeys.size() + userKeys.size());
  ins.reserve(systemKeys.size() + userKeys.size());

  for (const auto &k : systemKeys) {
    auto key = "sys." + k;
    de.name = key;
    des.emplace_back(de);
    auto inode = *lookupConfig(key.data(), ui);
    ins.emplace_back(std::move(inode));
  }
  for (const auto &k : userKeys) {
    auto key = "usr." + k;
    de.name = key;
    des.emplace_back(de);
    auto inode = *lookupConfig(key.data(), ui);
    ins.emplace_back(std::move(inode));
  }

  return std::make_pair(std::make_shared<std::vector<meta::DirEntry>>(std::move(des)),
                        std::make_shared<std::vector<std::optional<meta::Inode>>>(std::move(ins)));
}
}  // namespace hf3fs::fuse
