#pragma once

#include "FuseConfig.h"
#include "common/utils/AtomicSharedPtrTable.h"
#include "fbs/meta/Common.h"
#include "fbs/meta/Schema.h"

namespace hf3fs::fuse {
class UserConfig {
 public:
  UserConfig() = default;
  void init(FuseConfig &config);
  Result<meta::Inode> setConfig(const char *key, const char *val, const meta::UserInfo &ui);
  Result<meta::Inode> lookupConfig(const char *key, const meta::UserInfo &ui);
  Result<meta::Inode> statConfig(meta::InodeId iid, const meta::UserInfo &ui);
  std::pair<std::shared_ptr<std::vector<meta::DirEntry>>, std::shared_ptr<std::vector<std::optional<meta::Inode>>>>
  listConfig(const meta::UserInfo &ui);

 public:
  const FuseConfig &getConfig(const meta::UserInfo &ui);

 public:
  const std::vector<std::string> systemKeys{"storage.net_client.rdma_control.max_concurrent_transmission",
                                            "periodic_sync.enable",
                                            "periodic_sync.interval",
                                            "periodic_sync.flush_write_buf",
                                            "io_worker_coros.hi",
                                            "io_worker_coros.lo",
                                            "max_jobs_per_ioring",
                                            "io_job_deq_timeout"};
  const std::vector<std::string> userKeys{"enable_read_cache",
                                          "readonly",
                                          "dryrun_bench_mode",
                                          "flush_on_stat",
                                          "sync_on_stat",
                                          "attr_timeout",
                                          "entry_timeout",
                                          "negative_timeout",
                                          "symlink_timeout"};

 private:
  Result<std::pair<bool, int>> parseKey(const char *key);
  meta::InodeId configIid(bool isGet, bool isSys, int kidx) {
    return meta::InodeId{(isGet ? meta::InodeId::getConf() : meta::InodeId::setConf()).u64() - 1 -
                         (isSys ? 0 : systemKeys.size()) - kidx};
  }

 private:
  FuseConfig *config_;
  struct LocalConfig {
    LocalConfig(const FuseConfig &globalConfig)
        : config(globalConfig) {}
    std::mutex mtx;
    FuseConfig config;
    std::vector<config::KeyValue> updatedItems;
  };
  std::unique_ptr<AtomicSharedPtrTable<LocalConfig>> configs_;
  std::mutex userMtx_;
  std::set<meta::Uid> users_;

 private:
  std::atomic<int> storageMaxConcXmit_;
};
}  // namespace hf3fs::fuse
