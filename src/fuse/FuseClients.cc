#include "FuseClients.h"

#include <folly/Random.h>
#include <folly/ScopeGuard.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/experimental/coro/BlockingWait.h>
#include <folly/functional/Partial.h>
#include <folly/logging/xlog.h>
#include <fuse3/fuse_lowlevel.h>
#include <memory>
#include <thread>
#include <utility>

#include "common/app/ApplicationBase.h"
#include "common/monitor/Recorder.h"
#include "common/utils/BackgroundRunner.h"
#include "common/utils/Coroutine.h"
#include "common/utils/Duration.h"
#include "common/utils/FileUtils.h"
#include "common/utils/SysResource.h"
#include "fbs/meta/Common.h"
#include "fbs/mgmtd/Rpc.h"
#include "stubs/MetaService/MetaServiceStub.h"
#include "stubs/common/RealStubFactory.h"
#include "stubs/mgmtd/MgmtdServiceStub.h"

namespace hf3fs::fuse {
namespace {
monitor::ValueRecorder dirtyInodesCnt("fuse.dirty_inodes");

Result<Void> establishClientSession(client::IMgmtdClientForClient &mgmtdClient) {
  return folly::coro::blockingWait([&]() -> CoTryTask<void> {
    auto retryInterval = std::chrono::milliseconds(10);
    constexpr auto maxRetryInterval = std::chrono::milliseconds(1000);
    Result<Void> res = Void{};
    for (int i = 0; i < 40; ++i) {
      res = co_await mgmtdClient.extendClientSession();
      if (res) break;
      XLOGF(CRITICAL, "Try to establish client session failed: {}\nretryCount: {}", res.error(), i);
      co_await folly::coro::sleep(retryInterval);
      retryInterval = std::min(2 * retryInterval, maxRetryInterval);
    }
    co_return res;
  }());
}
}  // namespace

FuseClients::~FuseClients() { stop(); }

Result<Void> FuseClients::init(const flat::AppInfo &appInfo,
                               const String &mountPoint,
                               const String &tokenFile,
                               FuseConfig &fuseConfig) {
  config = &fuseConfig;

  fuseMount = appInfo.clusterId;
  XLOGF_IF(FATAL,
           fuseMount.size() >= 32,
           "FUSE only support mount name shorter than 32 characters, but {} got.",
           fuseMount);

  fuseMountpoint = Path(mountPoint).lexically_normal();

  if (fuseConfig.remount_prefix()) {
    fuseRemountPref = Path(*fuseConfig.remount_prefix()).lexically_normal();
  }

  if (const char *env_p = std::getenv("HF3FS_FUSE_TOKEN")) {
    XLOGF(INFO, "Use token from env var");
    fuseToken = std::string(env_p);
  } else {
    XLOGF(INFO, "Use token from config");
    auto tokenRes = loadFile(tokenFile);
    RETURN_ON_ERROR(tokenRes);
    fuseToken = folly::trimWhitespace(*tokenRes);
  }
  enableWritebackCache = fuseConfig.enable_writeback_cache();
  memsetBeforeRead = fuseConfig.memset_before_read();
  maxIdleThreads = fuseConfig.max_idle_threads();
  maxThreads = fuseConfig.max_threads();
  bufPool = net::RDMABufPool::create(fuseConfig.io_bufs().max_buf_size(), fuseConfig.rdma_buf_pool_size());

  iovs.init(fuseRemountPref.value_or(fuseMountpoint), fuseConfig.iov_limit());
  iors.init(fuseConfig.iov_limit());
  userConfig.init(fuseConfig);

  if (!client) {
    client = std::make_unique<net::Client>(fuseConfig.client());
    RETURN_ON_ERROR(client->start());
  }
  auto ctxCreator = [this](net::Address addr) { return client->serdeCtx(addr); };
  if (!mgmtdClient) {
    mgmtdClient = std::make_shared<client::MgmtdClientForClient>(
        appInfo.clusterId,
        std::make_unique<stubs::RealStubFactory<mgmtd::MgmtdServiceStub>>(ctxCreator),
        fuseConfig.mgmtd());
  }

  auto physicalHostnameRes = SysResource::hostname(/*physicalMachineName=*/true);
  RETURN_ON_ERROR(physicalHostnameRes);

  auto containerHostnameRes = SysResource::hostname(/*physicalMachineName=*/false);
  RETURN_ON_ERROR(containerHostnameRes);

  auto clientId = ClientId::random(*physicalHostnameRes);

  mgmtdClient->setClientSessionPayload({clientId.uuid.toHexString(),
                                        flat::NodeType::FUSE,
                                        flat::ClientSessionData::create(
                                            /*universalId=*/*physicalHostnameRes,
                                            /*description=*/fmt::format("fuse: {}", *containerHostnameRes),
                                            appInfo.serviceGroups,
                                            appInfo.releaseVersion),
                                        // TODO: use real user info
                                        flat::UserInfo{}});

  mgmtdClient->setConfigListener(ApplicationBase::updateConfig);

  folly::coro::blockingWait(mgmtdClient->start(&client->tpg().bgThreadPool().randomPick()));
  folly::coro::blockingWait(mgmtdClient->refreshRoutingInfo(/*force=*/false));
  RETURN_ON_ERROR(establishClientSession(*mgmtdClient));

  storageClient = storage::client::StorageClient::create(clientId, fuseConfig.storage(), *mgmtdClient);

  metaClient =
      std::make_shared<meta::client::MetaClient>(clientId,
                                                 fuseConfig.meta(),
                                                 std::make_unique<meta::client::MetaClient::StubFactory>(ctxCreator),
                                                 mgmtdClient,
                                                 storageClient,
                                                 true /* dynStripe */);
  metaClient->start(client->tpg().bgThreadPool());

  iojqs.reserve(3);
  iojqs.emplace_back(new BoundedQueue<IoRingJob>(fuseConfig.io_jobq_sizes().hi()));
  iojqs.emplace_back(new BoundedQueue<IoRingJob>(fuseConfig.io_jobq_size()));
  iojqs.emplace_back(new BoundedQueue<IoRingJob>(fuseConfig.io_jobq_sizes().lo()));

  jitter = fuseConfig.submit_wait_jitter();

  auto &tp = client->tpg().bgThreadPool();
  auto coros = fuseConfig.batch_io_coros();
  for (int i = 0; i < coros; ++i) {
    auto exec = &tp.get(i % tp.size());
    co_withCancellation(cancelIos.getToken(), ioRingWorker(i, coros)).scheduleOn(exec).start();
  }

  ioWatches.reserve(3);
  for (int i = 0; i < 3; ++i) {
    ioWatches.emplace_back(folly::partial(&FuseClients::watch, this, i));
  }

  periodicSyncWorker = std::make_unique<CoroutinesPool<InodeId>>(config->periodic_sync().worker());
  periodicSyncWorker->start(folly::partial(&FuseClients::periodicSync, this), tp);

  periodicSyncRunner = std::make_unique<BackgroundRunner>(&tp.pickNextFree());
  periodicSyncRunner->start("PeriodSync", folly::partial(&FuseClients::periodicSyncScan, this), [&]() {
    return config->periodic_sync().interval() * folly::Random::randDouble(0.7, 1.3);
  });

  onFuseConfigUpdated = fuseConfig.addCallbackGuard([&fuseConfig = fuseConfig, this] {
    memsetBeforeRead = fuseConfig.memset_before_read();
    jitter = std::chrono::duration_cast<std::chrono::nanoseconds>(fuseConfig.submit_wait_jitter());
  });

  notifyInvalExec =
      std::make_unique<folly::IOThreadPoolExecutor>(fuseConfig.notify_inval_threads(),
                                                    std::make_shared<folly::NamedThreadFactory>("NotifyInvalThread"));

  return Void{};
}

void FuseClients::stop() {
  if (notifyInvalExec) {
    notifyInvalExec->stop();
    notifyInvalExec.reset();
  }
  if (onFuseConfigUpdated) {
    onFuseConfigUpdated.reset();
  }

  cancelIos.requestCancellation();

  for (auto &t : ioWatches) {
    t.request_stop();
  }
  if (periodicSyncRunner) {
    folly::coro::blockingWait(periodicSyncRunner->stopAll());
    periodicSyncRunner.reset();
  }
  if (periodicSyncWorker) {
    periodicSyncWorker->stopAndJoin();
    periodicSyncWorker.reset();
  }
  if (metaClient) {
    metaClient->stop();
    metaClient.reset();
  }
  if (storageClient) {
    storageClient->stop();
    storageClient.reset();
  }
  if (mgmtdClient) {
    folly::coro::blockingWait(mgmtdClient->stop());
    mgmtdClient.reset();
  }
  if (client) {
    client->stopAndJoin();
    client.reset();
  }
}

CoTask<void> FuseClients::ioRingWorker(int i, int ths) {
  // a worker thread has its own priority, but it can also execute jobs from queues with a higher priority
  // checkHigher is used to make sure the job queue with the thread's own priority doesn't starve
  bool checkHigher = true;

  while (true) {
    auto res = co_await folly::coro::co_awaitTry([this, &checkHigher, i, ths]() -> CoTask<void> {
      IoRingJob job;
      auto hiThs = config->io_worker_coros().hi(), loThs = config->io_worker_coros().lo();
      auto prio = i < hiThs ? 0 : i < (ths - loThs) ? 1 : 2;
      if (!config->enable_priority()) {
        job = co_await iojqs[prio]->co_dequeue();
      } else {
        bool gotJob = false;

        // if checkHigher, dequeue from a higher job queue if it is full
        while (!gotJob) {
          if (checkHigher) {
            for (int nprio = 0; nprio < prio; ++nprio) {
              if (iojqs[nprio]->full()) {
                auto dres = iojqs[nprio]->try_dequeue();
                if (dres) {
                  // got a job from higher priority queue, next time pick a same priority job unless the queue is empty
                  checkHigher = false;
                  gotJob = true;
                  job = std::move(*dres);
                  break;
                }
              }
            }

            if (gotJob) {
              break;
            }
          }

          // if checkHigher, check from higher prio to lower; otherwise, reverse the checking direction
          for (int nprio = checkHigher ? 0 : prio; checkHigher ? nprio <= prio : nprio >= 0;
               nprio += checkHigher ? 1 : -1) {
            auto [sres, dres] =
                co_await folly::coro::collectAnyNoDiscard(folly::coro::sleep(config->io_job_deq_timeout()),
                                                          iojqs[nprio]->co_dequeue());
            if (dres.hasValue()) {
              // if the job is the thread's own priority, next time it can check from higher priority queues
              if (!checkHigher && nprio == prio) {
                checkHigher = true;
              }
              gotJob = true;
              job = std::move(*dres);
              break;
            } else if (sres.hasValue()) {
              continue;
            } else {
              dres.throwUnlessValue();
            }
          }
        }
      }

      while (true) {
        auto lookupFiles =
            [this](std::vector<std::shared_ptr<RcInode>> &ins, const IoArgs *args, const IoSqe *sqes, int sqec) {
              auto lastIid = 0ull;

              std::lock_guard lock(inodesMutex);
              for (int i = 0; i < sqec; ++i) {
                auto idn = args[sqes[i].index].fileIid;
                if (i && idn == lastIid) {
                  ins.emplace_back(ins.back());
                  continue;
                }

                lastIid = idn;
                auto iid = meta::InodeId(idn);
                auto it = inodes.find(iid);
                ins.push_back(it == inodes.end() ? (std::shared_ptr<RcInode>()) : it->second);
              }
            };
        auto lookupBufs =
            [this](std::vector<Result<lib::ShmBufForIO>> &bufs, const IoArgs *args, const IoSqe *sqe, int sqec) {
              auto lastId = Uuid::zero();
              std::shared_ptr<lib::ShmBuf> lastShm;

              std::lock_guard lock(iovs.shmLock);
              for (int i = 0; i < sqec; ++i) {
                auto &arg = args[sqe[i].index];
                Uuid id;
                memcpy(id.data, arg.bufId, sizeof(id.data));

                std::shared_ptr<lib::ShmBuf> shm;
                if (i && id == lastId) {
                  shm = lastShm;
                } else {
                  auto it = iovs.shmsById.find(id);
                  if (it == iovs.shmsById.end()) {
                    bufs.emplace_back(makeError(StatusCode::kInvalidArg, "buf id not found"));
                    continue;
                  }

                  auto iovd = it->second;
                  shm = iovs.iovs->table[iovd].load();
                  if (!shm) {
                    bufs.emplace_back(makeError(StatusCode::kInvalidArg, "buf id not found"));
                    continue;
                  } else if (shm->size < arg.bufOff + arg.ioLen) {
                    bufs.emplace_back(makeError(StatusCode::kInvalidArg, "invalid buf off and/or io len"));
                    continue;
                  }

                  lastId = id;
                  lastShm = shm;
                }

                bufs.emplace_back(lib::ShmBufForIO(std::move(shm), arg.bufOff));
              }
            };

        co_await job.ior->process(job.sqeProcTail,
                                  job.toProc,
                                  *storageClient,
                                  config->storage_io(),
                                  userConfig,
                                  std::move(lookupFiles),
                                  std::move(lookupBufs));

        if (iojqs[0]->full() || job.ior->priority != prio) {
          sem_post(iors.sems[job.ior->priority].get());  // wake the watchers
        } else {
          auto jobs = job.ior->jobsToProc(1);
          if (!jobs.empty()) {
            job = jobs.front();
            if (!iojqs[0]->try_enqueue(job)) {
              continue;
            }
          }
        }

        break;
      }
    }());
    if (UNLIKELY(res.hasException())) {
      XLOGF(INFO, "io worker #{} cancelled", i);
      if (res.hasException<OperationCancelled>()) {
        break;
      } else {
        XLOGF(FATAL, "got exception in io worker #{}", i);
      }
    }
  }
}

void FuseClients::watch(int prio, std::stop_token stop) {
  while (!stop.stop_requested()) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
      continue;
    }

    auto nsec = ts.tv_nsec + jitter.load().count();
    ts.tv_nsec = nsec % 1000000000;
    ts.tv_sec += nsec / 1000000000;
    if (sem_timedwait(iors.sems[prio].get(), &ts) < 0 && errno == ETIMEDOUT) {
      continue;
    }

    auto gotJobs = false;
    do {
      gotJobs = false;

      auto n = iors.ioRings->slots.nextAvail.load();
      for (int i = 0; i < n; ++i) {
        auto ior = iors.ioRings->table[i].load();

        if (ior && ior->priority == prio) {
          auto jobs = ior->jobsToProc(config->max_jobs_per_ioring());
          for (auto &&job : jobs) {
            gotJobs = true;
            iojqs[prio]->enqueue(std::move(job));
          }
        }
      }
    } while (gotJobs);  // loop till we found no more jobs and then block in the next iter
  }
}

CoTask<void> FuseClients::periodicSyncScan() {
  if (!config->periodic_sync().enable() || config->readonly()) {
    co_return;
  }

  XLOGF(INFO, "periodicSyncScan run");
  std::set<InodeId> dirty;
  {
    auto guard = dirtyInodes.lock();
    auto limit = config->periodic_sync().limit();
    dirtyInodesCnt.set(guard->size());
    if (guard->size() <= limit) {
      dirty = std::exchange(*guard, {});
    } else {
      XLOGF(WARN, "dirty inodes {} > limit {}", guard->size(), limit);
      auto iter = guard->find(lastSynced);
      while (dirty.size() < limit) {
        if (iter == guard->end()) {
          iter = guard->begin();
          XLOGF_IF(FATAL, iter == guard->end(), "iter == guard->end() shouldn't happen");
        } else {
          auto inode = *iter;
          lastSynced = inode;
          iter = guard->erase(iter);
          dirty.insert(inode);
        }
      }
    }
  }

  for (auto inode : dirty) {
    co_await periodicSyncWorker->enqueue(inode);
  }

  co_return;
}

}  // namespace hf3fs::fuse
