#include <folly/Overload.h>
#include <folly/logging/xlog.h>
#include <map>

#include "common/utils/RandomUtils.h"
#include "common/utils/Transform.h"
#include "mgmtd/service/updateChain.h"
#include "tests/GtestHelpers.h"

namespace hf3fs::testing {
namespace {
class UpdateChainTest : public ::testing::Test {
 protected:
  void testStateMachine(int targetCount, bool injectFailure, bool allowCriticalShutdown);
};

using namespace ::hf3fs::mgmtd;
using flat::NodeId;
using flat::TargetId;
using PS = enum flat::PublicTargetState;
using LS = enum flat::LocalTargetState;
using Chain = std::vector<flat::TargetInfo>;

struct VersionedChain {
  int64_t ver = 0;
  Chain chain;
};

template <typename E>
requires(std::is_enum_v<E>) auto toString(E v) { return magic_enum::enum_name(v); }

String toString(PS ps, LS ls) { return fmt::format("{}-{}", toString(ps), toString(ls)); }

String toString(const flat::TargetInfo &ti) {
  return fmt::format("{}({})", ti.targetId, toString(ti.publicState, ti.localState));
}

String toString(const LocalTargetInfoWithNodeId &lti) {
  return fmt::format("{}({})", lti.targetId, toString(lti.localState));
}

String toString(const Chain &chain) {
  if (chain.empty()) return "[]";
  String s = fmt::format("[{}", toString(chain[0]));
  for (size_t i = 1; i < chain.size(); ++i) s += fmt::format(", {}", toString(chain[i]));
  return s + "]";
}

String toString(const VersionedChain &chain) { return fmt::format("{}(v = {})", toString(chain.chain), chain.ver); }

String toString(const std::vector<LocalTargetInfoWithNodeId> &changes) {
  String s = fmt::format("[{}", toString(changes[0]));
  for (size_t i = 1; i < changes.size(); ++i) s += fmt::format(", {}", toString(changes[i]));
  return s + "]";
}

std::vector<flat::TargetInfo> mergeAndGenerateNewChain(std::vector<flat::TargetInfo> oldTargets,
                                                       const std::vector<LocalTargetInfoWithNodeId> &changes,
                                                       bool onlyUpdateLocalState) {
  robin_hood::unordered_map<flat::TargetId, LocalTargetInfoWithNodeId> changesMap;
  for (const auto &lti : changes) {
    changesMap[lti.targetId] = lti;
  }
  for (auto &ti : oldTargets) {
    if (changesMap.contains(ti.targetId)) {
      const auto &lti = changesMap[ti.targetId];
      ti.localState = lti.localState;
      ti.nodeId = lti.nodeId;
    }
  }

  if (onlyUpdateLocalState) return oldTargets;

  auto oldCtis =
      transformTo<std::vector>(std::span{oldTargets.begin(), oldTargets.size()}, ChainTargetInfoEx::fromTargetInfo);

  auto newCtis = generateNewChain(oldCtis);

  std::map<flat::TargetId, flat::TargetInfo> tis;
  for (const auto &ti : oldTargets) {
    tis[ti.targetId] = ti;
  }

  return transformTo<std::vector>(std::span{newCtis.begin(), newCtis.size()}, [&](auto cti) {
    auto ti = tis[cti.targetId];
    ti.publicState = cti.publicState;
    ti.localState = cti.localState;
    return ti;
  });
}

String validatePs(const Chain &chain) {
  std::map<PS, int> cntByPs;
  std::map<PS, int> lastIdxByPs;
  for (size_t i = 0; i < chain.size(); ++i) {
    ++cntByPs[chain[i].publicState];
    lastIdxByPs[chain[i].publicState] = i;
  }
  lastIdxByPs.try_emplace(PS::OFFLINE, chain.size());
  lastIdxByPs.try_emplace(PS::WAITING, lastIdxByPs[PS::OFFLINE]);
  lastIdxByPs.try_emplace(PS::SYNCING, lastIdxByPs[PS::WAITING]);
  lastIdxByPs.try_emplace(PS::LASTSRV, lastIdxByPs[PS::SYNCING]);
  lastIdxByPs.try_emplace(PS::SERVING, lastIdxByPs[PS::LASTSRV]);

  XLOGF(DBG,
        "LastIdxByPs:\n* OFFLINE:{}\n* WAITING:{}\n* SYNCING:{}\n* LASTSRV:{}\n* SERVING:{}",
        lastIdxByPs[PS::OFFLINE],
        lastIdxByPs[PS::WAITING],
        lastIdxByPs[PS::SYNCING],
        lastIdxByPs[PS::LASTSRV],
        lastIdxByPs[PS::SERVING]);

  // rule: if any SERVING, no LASTSRV
  if (cntByPs[PS::SERVING] && cntByPs[PS::LASTSRV]) return "Both SERVING and LASTSRV";

  // rule: if any SYNCING, no LASTSRV
  if (cntByPs[PS::SYNCING] && cntByPs[PS::LASTSRV]) return "Both SYNCING and LASTSRV";

  // rule: SYNCING at most 1
  if (cntByPs[PS::SYNCING] > 1) return "SYNCING more than 1";

  // rule: LASTSRV at most 1
  if (cntByPs[PS::LASTSRV] > 1) return "LASTSRV more than 1";

  // rule: if no SERVING, no SYNCING
  if (!cntByPs[PS::SERVING] && cntByPs[PS::SYNCING]) return "SYNCING but no SERVING";

  // rule: orderd by SERVING, LASTSRV, SYNCING, WAITING, OFFLINE
  if (lastIdxByPs[PS::OFFLINE] < lastIdxByPs[PS::WAITING]) return "WAITING after OFFLINE";
  if (lastIdxByPs[PS::WAITING] < lastIdxByPs[PS::SYNCING]) return "SYNCING after WAITING";
  if (lastIdxByPs[PS::SYNCING] < lastIdxByPs[PS::LASTSRV]) return "LASTSRV after SYNCING";
  if (lastIdxByPs[PS::LASTSRV] < lastIdxByPs[PS::SERVING]) return "SERVING after LASTSRV";

  return "";
}

String validateLs(const Chain &chain) {
  // rule: online/offline should be consistent in ps and ls
  for (const auto &ti : chain) {
    bool lsOnline = ti.localState != LS::OFFLINE;
    bool psOnline = (ti.publicState != PS::LASTSRV && ti.publicState != PS::OFFLINE);
    if (lsOnline != psOnline)
      return fmt::format("{}: {} is {} but {} is {}",
                         ti.targetId,
                         toString(ti.publicState),
                         psOnline ? "online" : "offline",
                         toString(ti.localState),
                         lsOnline ? "online" : "offline");
  }

  return "";
}

String validatePsTransition(TargetId id, PS ops, LS nls, PS nps) {
  static std::map<PS, std::map<LS, std::set<PS>>> validTransitions = [] {
    std::map<PS, std::map<LS, std::set<PS>>> m;

    m[PS::OFFLINE][LS::OFFLINE].insert(PS::OFFLINE);
    m[PS::OFFLINE][LS::ONLINE].insert(PS::WAITING);
    m[PS::OFFLINE][LS::ONLINE].insert(PS::SYNCING);
    m[PS::OFFLINE][LS::UPTODATE].insert(PS::WAITING);

    m[PS::WAITING][LS::OFFLINE].insert(PS::OFFLINE);
    // when no serving or has syncing
    m[PS::WAITING][LS::ONLINE].insert(PS::WAITING);
    // when has serving and no syncing
    m[PS::WAITING][LS::ONLINE].insert(PS::SYNCING);
    // when no serving or has syncing
    // NOTE: WAITING + UPTODATE needn't be SYNCING since storage will
    //       shutdown or change self to ONLINE and reSYNCING next round
    m[PS::WAITING][LS::UPTODATE].insert(PS::WAITING);

    m[PS::SYNCING][LS::OFFLINE].insert(PS::OFFLINE);
    // when sync not finished
    m[PS::SYNCING][LS::ONLINE].insert(PS::SYNCING);
    // when no serving
    m[PS::SYNCING][LS::ONLINE].insert(PS::WAITING);
    // when sync finished
    m[PS::SYNCING][LS::UPTODATE].insert(PS::SERVING);

    m[PS::LASTSRV][LS::OFFLINE].insert(PS::OFFLINE);
    m[PS::LASTSRV][LS::OFFLINE].insert(PS::LASTSRV);
    m[PS::LASTSRV][LS::ONLINE].insert(PS::SERVING);
    m[PS::LASTSRV][LS::UPTODATE].insert(PS::SERVING);

    m[PS::SERVING][LS::OFFLINE].insert(PS::OFFLINE);
    m[PS::SERVING][LS::OFFLINE].insert(PS::LASTSRV);
    m[PS::SERVING][LS::ONLINE].insert(PS::SERVING);
    m[PS::SERVING][LS::UPTODATE].insert(PS::SERVING);

    return m;
  }();

  auto transition = fmt::format("{} + {} -> {}", toString(ops), toString(nls), toString(nps));
  XLOGF(DBG, "Validate transition {}", transition);
  if (!validTransitions[ops][nls].contains(nps)) return fmt::format("{}: invalid transition {}", id, transition);

  return "";
}

String validate(const Chain &before, const Chain &after) {
  if (auto r = validatePs(after); !r.empty()) return r;
  if (auto r = validateLs(after); !r.empty()) return r;

  struct State {
    TargetId id{0};
    size_t idx = 0;
    PS ps = PS::INVALID;
    LS ls = LS::INVALID;

    State() = default;
    State(size_t i, const flat::TargetInfo &ti)
        : id(ti.targetId),
          idx(i),
          ps(ti.publicState),
          ls(ti.localState) {}
  };
  std::map<TargetId, State> beforeById, afterById;
  std::multimap<PS, State> beforeByPs, afterByPs;
  for (size_t i = 0; i < before.size(); ++i) {
    const auto &ti = before[i];
    beforeById.try_emplace(ti.targetId, i, ti);
    beforeByPs.emplace(ti.publicState, State(i, ti));
  }
  for (size_t i = 0; i < after.size(); ++i) {
    const auto &ti = after[i];
    afterById.try_emplace(ti.targetId, i, ti);
    afterByPs.emplace(ti.publicState, State(i, ti));
  }

  // rule: size not changed
  if (beforeById.size() != before.size()) return "Before has duplicates";
  if (afterById.size() != after.size()) return "After has duplicates";

  // rule: single target transition valid
  for (const auto &ti : before) {
    const auto &ns = afterById[ti.targetId];
    if (auto r = validatePsTransition(ti.targetId, ti.publicState, ns.ls, ns.ps); !r.empty()) return r;
  }

  // rule: target list not changed
  for (const auto &[id, oti] : beforeById) {
    if (!afterById.contains(id)) return fmt::format("{} missed in after", id);
  }

  // rule: serving targets keep the order
  for (const auto &[id1, nti1] : afterById) {
    for (const auto &[id2, nti2] : afterById) {
      if (id1 >= id2) continue;
      if (nti1.ps == PS::SERVING && nti2.ps == PS::SERVING) {
        if ((nti1.idx < nti2.idx) != (beforeById[id1].idx < beforeById[id2].idx))
          return fmt::format("SERVING changed order: {} and {}", id1, id2);
      }
    }
  }

  // rule: servings have intersection if exist in both
  if (beforeByPs.count(PS::SERVING) && afterByPs.count(PS::SERVING)) {
    int cnt = 0;
    for (const auto &[id, oti] : beforeById) {
      if ((oti.ps == PS::SERVING || oti.ps == PS::SYNCING) && afterById[id].ps == PS::SERVING) ++cnt;
    }
    if (cnt == 0) return "SERVING has no intersection";
  }

  // rule: first before.SERVING == after.LASTSRV, other before.SERVING == after.OFFLINE
  if (!afterByPs.count(PS::SERVING)) {
    bool meetServing = false;
    for (const auto &ti : before) {
      if (ti.publicState == PS::SERVING) {
        if (!meetServing) {
          if (afterById[ti.targetId].ps != PS::LASTSRV) return "first SERVING should become LASTSRV";
          meetServing = true;
        } else {
          if (afterById[ti.targetId].ps != PS::OFFLINE) return "following SERVING should become OFFLINE";
        }
      }
    }
  }

  return "";
}

const std::vector<PS> &allPublicStates() {
  static std::vector<PS> states = [] {
    std::vector<PS> v;
    magic_enum::enum_for_each<PS>([&v](PS ps) {
      if (ps != PS::INVALID) v.push_back(ps);
    });
    return v;
  }();
  return states;
}

const std::vector<LS> &allLocalStates() {
  static std::vector<LS> states = [] {
    std::vector<LS> v;
    magic_enum::enum_for_each<LS>([&v](LS ls) {
      if (ls != LS::INVALID) v.push_back(ls);
    });
    return v;
  }();
  return states;
}

const std::vector<std::pair<PS, LS>> &allPosibleStates() {
  static std::vector<std::pair<PS, LS>> states = [] {
    std::vector<std::pair<PS, LS>> v;
    for (auto ps : allPublicStates()) {
      for (auto ls : allLocalStates()) {
        v.emplace_back(ps, ls);
        XLOGF(DBG, "PosibleState: {} + {}", toString(ps), toString(ls));
      }
    }
    return v;
  }();
  return states;
}

struct ChainBuilder {
  Chain chain;

  ChainBuilder() = default;
  explicit ChainBuilder(Chain c)
      : chain(std::move(c)) {}

  ChainBuilder &add(TargetId id, PS ps, LS ls, NodeId nodeId) {
    flat::TargetInfo info;
    info.targetId = id;
    info.publicState = ps;
    info.localState = ls;
    info.nodeId = nodeId;
    info.chainId = flat::ChainId(0);  // placeholder
    chain.push_back(std::move(info));
    return *this;
  }

  auto build() { return std::move(chain); }
};

TEST_F(UpdateChainTest, testOneReplica_EmptyUpdates) {
  for (auto [ps, ls] : allPosibleStates()) {
    auto desc = toString(ps, ls);
    auto chain = ChainBuilder().add(TargetId(1), ps, ls, NodeId(1)).build();
    auto newChain = mergeAndGenerateNewChain(chain, {}, false);
    XLOGF(DBG, "[Debugging] test {} ->  {}", toString(chain), toString(newChain));
    EXPECT_EQ(chain.size(), newChain.size());
    EXPECT_EQ(validate(chain, newChain), "");
  }
}

TEST_F(UpdateChainTest, testOneReplica_RandomChanges) {
  for (int i = 0; i < 100; ++i) {
    auto chain = ChainBuilder().add(TargetId(1), PS::SERVING, LS::OFFLINE, NodeId(1)).build();
    for (int j = 0; j < 100; ++j) {
      std::vector<LocalTargetInfoWithNodeId> changes;
      changes.emplace_back(TargetId(1), NodeId(1), RandomUtils::randomSelect(allLocalStates()));
      auto newChain = mergeAndGenerateNewChain(chain, changes, false);
      XLOGF(DBG, "[Debugging] test {} + {} ->  {}", toString(chain), toString(changes), toString(newChain));
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "");
      chain = std::move(newChain);
    }
  }
}

TEST_F(UpdateChainTest, testOneReplica_Recover) {
  for (int i = 0; i < 100; ++i) {
    auto chain = ChainBuilder().add(TargetId(1), PS::SERVING, LS::OFFLINE, NodeId(1)).build();
    std::vector<LS> states;
    for (int j = 0; j < 100; ++j) {
      states.push_back(RandomUtils::randomSelect(allLocalStates()));
    }
    for (int j = 0; j < 5; ++j) {
      states.push_back(LS::UPTODATE);
    }
    for (auto ls : states) {
      std::vector<LocalTargetInfoWithNodeId> changes;
      changes.emplace_back(TargetId(1), NodeId(1), ls);
      auto newChain = mergeAndGenerateNewChain(chain, changes, false);
      XLOGF(DBG, "[Debugging] apply {} + {} ->  {}", toString(chain), toString(changes), toString(newChain));
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "");
      chain = std::move(newChain);
    }
    auto it = std::find_if(chain.begin(), chain.end(), [](const auto &ti) { return ti.publicState != PS::SERVING; });
    EXPECT_TRUE(it == chain.end());
  }
}

TEST_F(UpdateChainTest, testTwoReplica_EmptyUpdates) {
  for (auto [ps1, ls1] : allPosibleStates()) {
    for (auto [ps2, ls2] : allPosibleStates()) {
      auto chain = ChainBuilder().add(TargetId(1), ps1, ls1, NodeId(1)).add(TargetId(2), ps2, ls2, NodeId(2)).build();
      if (auto r = validatePs(chain); !r.empty()) {
        XLOGF(DBG, "[Debugging] Skip {} due to {}", toString(chain), r);
        continue;
      }
      auto newChain = mergeAndGenerateNewChain(chain, {}, false);
      XLOGF(DBG, "[Debugging] test {} ->  {}", toString(chain), toString(newChain));
      // TODO: how to verify the result?
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "") << toString(chain) << "\n" << toString(newChain);
    }
  }
}

TEST_F(UpdateChainTest, testTwoReplica_RandomUpdates) {
  for (int i = 0; i < 100; ++i) {
    auto chain = ChainBuilder()
                     .add(TargetId(1), PS::SERVING, LS::OFFLINE, NodeId(1))
                     .add(TargetId(2), PS::SERVING, LS::OFFLINE, NodeId(2))
                     .build();
    for (int j = 0; j < 100; ++j) {
      std::vector<LocalTargetInfoWithNodeId> changes;
      for (const auto &ti : chain) {
        changes.emplace_back(ti.targetId, *ti.nodeId, RandomUtils::randomSelect(allLocalStates()));
      }
      auto newChain = mergeAndGenerateNewChain(chain, changes, false);
      XLOGF(DBG, "[Debugging] test {} + {} ->  {}", toString(chain), toString(changes), toString(newChain));
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "");
      chain = std::move(newChain);
    }
  }
}

TEST_F(UpdateChainTest, testTwoReplica_Recover) {
  for (int i = 0; i < 100; ++i) {
    auto chain = ChainBuilder()
                     .add(TargetId(1), PS::SERVING, LS::OFFLINE, NodeId(1))
                     .add(TargetId(2), PS::SERVING, LS::OFFLINE, NodeId(2))
                     .build();
    std::vector<std::tuple<LS, LS>> states;
    for (int j = 0; j < 100; ++j) {
      states.emplace_back(RandomUtils::randomSelect(allLocalStates()), RandomUtils::randomSelect(allLocalStates()));
    }
    for (int j = 0; j < 5; ++j) {
      states.emplace_back(LS::ONLINE, LS::ONLINE);
      states.emplace_back(LS::UPTODATE, LS::UPTODATE);
    }
    for (auto [ls1, ls2] : states) {
      std::vector<LocalTargetInfoWithNodeId> changes;
      changes.emplace_back(TargetId(1), NodeId(1), ls1);
      changes.emplace_back(TargetId(2), NodeId(2), ls2);
      auto newChain = mergeAndGenerateNewChain(chain, changes, false);
      XLOGF(DBG, "[Debugging] apply {} + {} ->  {}", toString(chain), toString(changes), toString(newChain));
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "");
      chain = std::move(newChain);
    }
    auto it = std::find_if(chain.begin(), chain.end(), [](const auto &ti) { return ti.publicState != PS::SERVING; });
    EXPECT_TRUE(it == chain.end());
  }
}

TEST_F(UpdateChainTest, testThreeReplica_EmptyUpdates) {
  for (auto [ps1, ls1] : allPosibleStates()) {
    for (auto [ps2, ls2] : allPosibleStates()) {
      for (auto [ps3, ls3] : allPosibleStates()) {
        auto chain = ChainBuilder()
                         .add(TargetId(1), ps1, ls1, NodeId(1))
                         .add(TargetId(2), ps2, ls2, NodeId(2))
                         .add(TargetId(3), ps3, ls3, NodeId(3))
                         .build();
        if (auto r = validatePs(chain); !r.empty()) {
          XLOGF(DBG, "[Debugging] Skip {} due to {}", toString(chain), r);
          continue;
        }
        auto newChain = mergeAndGenerateNewChain(chain, {}, false);
        XLOGF(DBG, "[Debugging] test {} ->  {}", toString(chain), toString(newChain));
        // TODO: how to verify the result?
        EXPECT_EQ(chain.size(), newChain.size());
        EXPECT_EQ(validate(chain, newChain), "");
      }
    }
  }
}

TEST_F(UpdateChainTest, testThreeReplica_RandomUpdates) {
  for (int i = 0; i < 100; ++i) {
    auto chain = ChainBuilder()
                     .add(TargetId(1), PS::SERVING, LS::OFFLINE, NodeId(1))
                     .add(TargetId(2), PS::SERVING, LS::OFFLINE, NodeId(2))
                     .add(TargetId(3), PS::SERVING, LS::OFFLINE, NodeId(3))
                     .build();
    for (int j = 0; j < 100; ++j) {
      std::vector<LocalTargetInfoWithNodeId> changes;
      for (const auto &ti : chain) {
        changes.emplace_back(ti.targetId, *ti.nodeId, RandomUtils::randomSelect(allLocalStates()));
      }
      auto newChain = mergeAndGenerateNewChain(chain, changes, false);
      XLOGF(DBG, "[Debugging] test {} + {} ->  {}", toString(chain), toString(changes), toString(newChain));
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "");
      chain = std::move(newChain);
    }
  }
}

TEST_F(UpdateChainTest, testThreeReplica_Recover) {
  for (int i = 0; i < 100; ++i) {
    auto chain = ChainBuilder()
                     .add(TargetId(1), PS::SERVING, LS::OFFLINE, NodeId(1))
                     .add(TargetId(2), PS::SERVING, LS::OFFLINE, NodeId(2))
                     .add(TargetId(3), PS::SERVING, LS::OFFLINE, NodeId(3))
                     .build();
    std::vector<std::tuple<LS, LS, LS>> states;
    for (int j = 0; j < 100; ++j) {
      states.emplace_back(RandomUtils::randomSelect(allLocalStates()),
                          RandomUtils::randomSelect(allLocalStates()),
                          RandomUtils::randomSelect(allLocalStates()));
    }
    for (int j = 0; j < 5; ++j) {
      states.emplace_back(LS::ONLINE, LS::ONLINE, LS::ONLINE);
      states.emplace_back(LS::UPTODATE, LS::UPTODATE, LS::UPTODATE);
    }
    for (auto [ls1, ls2, ls3] : states) {
      std::vector<LocalTargetInfoWithNodeId> changes;
      changes.emplace_back(TargetId(1), NodeId(1), ls1);
      changes.emplace_back(TargetId(2), NodeId(2), ls2);
      changes.emplace_back(TargetId(3), NodeId(3), ls3);
      auto newChain = mergeAndGenerateNewChain(chain, changes, false);
      XLOGF(DBG, "[Debugging] apply {} + {} ->  {}", toString(chain), toString(changes), toString(newChain));
      EXPECT_EQ(chain.size(), newChain.size());
      EXPECT_EQ(validate(chain, newChain), "");
      chain = std::move(newChain);
    }
    auto it = std::find_if(chain.begin(), chain.end(), [](const auto &ti) { return ti.publicState != PS::SERVING; });
    EXPECT_TRUE(it == chain.end());
  }
}

struct MachineBase;

enum class MessageType {
  DataReq,
  DataRsp,
  SyncDone,
  Chain,
  Heartbeat,
};

struct Message {
  int64_t arriveTime{0};
  MachineBase *from = nullptr;

  struct DataReqMsg {
    int64_t ver = 0;
    int64_t chainVer = 0;
    bool syncing = false;
  };

  struct DataRspMsg {
    int64_t ver = 0;
    bool syncing = false;
    bool succeeded = false;
  };

  struct SyncDoneMsg {
    int64_t ver = 0;
    int64_t chainVer = 0;
  };

  struct ChainMsg {
    VersionedChain chain;
  };

  struct HeartbeatMsg {
    LocalTargetInfoWithNodeId lti;
  };

  std::variant<DataReqMsg, DataRspMsg, SyncDoneMsg, ChainMsg, HeartbeatMsg> msg = DataReqMsg{};
};

enum class MachineState {
  SHUTDOWN,
  NORMAL,
  IN_CONN_BROKEN,
  OUT_CONN_BROKEN,
  HANG,
};

enum class Event {
  SHUTDOWN,
  CRITICAL_SHUTDOWN,
  IN_CONN_BROKEN,
  OUT_CONN_BROKEN,
  HANG,
};

#define MLOG(fmt, ...) XLOGF(DBG, "[T = {}] id:{} " fmt, now, id __VA_OPT__(, ) __VA_ARGS__)

#define MASSERT(condition, fmt, ...) \
  XLOGF_IF(FATAL, !(condition), "[T = {}] id:{} " fmt, now, id __VA_OPT__(, ) __VA_ARGS__)

String toString(const Message &msg);

struct MachineBase {
  void start(int64_t now) {
    state = MachineState::NORMAL;
    recoverTime.reset();
    mailbox.clear();
    startImpl(now);
  }

  void insertMessage(Message msg) {
    mailbox.push_back(std::move(msg));
    for (int i = mailbox.size(); i > 1; --i) {
      if (mailbox[i - 1].arriveTime < mailbox[i - 2].arriveTime) {
        std::swap(mailbox[i - 1], mailbox[i - 2]);
      } else
        break;
    }
  }

  void receive(int64_t now, Message msg) {
    if (state != MachineState::SHUTDOWN && state != MachineState::IN_CONN_BROKEN) {
      MLOG("receive message: {}", toString(msg));
      insertMessage(std::move(msg));
    } else {
      MLOG("discard message: {}", toString(msg));
    }
  }

  void setState(Event e, int64_t now) {
    switch (e) {
      case Event::SHUTDOWN:
        state = MachineState::SHUTDOWN;
        shutdownImpl(false);
        break;
      case Event::CRITICAL_SHUTDOWN:
        state = MachineState::SHUTDOWN;
        shutdownImpl(true);
        break;
      case Event::IN_CONN_BROKEN:
        state = MachineState::IN_CONN_BROKEN;
        mailbox.clear();
        break;
      case Event::OUT_CONN_BROKEN:
        state = MachineState::OUT_CONN_BROKEN;
        break;
      case Event::HANG:
        state = MachineState::HANG;
        break;
      default:
        break;
    }
    recoverTime = now + folly::Random::rand32(maxRecoverTime) + 1;
    MLOG("setState e:{} recover:{}", magic_enum::enum_name(e), *recoverTime);
  }

  void checkRecover(int64_t now) {
    if (recoverTime && *recoverTime <= now) {
      ASSERT_NE(state, MachineState::NORMAL);
      if (state == MachineState::SHUTDOWN) start(now);
      state = MachineState::NORMAL;
      recoverTime.reset();
    }
  }

  void handleMessage(int64_t now) {
    while (!mailbox.empty() && mailbox.front().arriveTime <= now) {
      auto msg = mailbox.front();
      mailbox.pop_front();

      MLOG("handle message: {}", toString(msg));

      switch (msg.msg.index()) {
        case 0:
          handleDataReq(now, msg);
          break;
        case 1:
          handleDataRsp(now, msg);
          break;
        case 2:
          handleSyncDone(now, msg);
          break;
        case 3:
          handleChain(now, msg);
          break;
        case 4:
          handleHeartbeat(now, msg);
          break;
        default:
          // do nothing
          break;
      }
    }
  }

  void handle(int64_t now) {
    checkRecover(now);

    if (state != MachineState::SHUTDOWN && state != MachineState::HANG) {
      handleMessage(now);
      handleImpl(now);
    } else {
      MLOG("skip handle: {}", magic_enum::enum_name(state));
    }
  }

  Message makeReq(int64_t now, auto &&payload) {
    Message msg;
    msg.from = this;
    msg.arriveTime = now + folly::Random::rand32(maxNetworkTime) + 1;
    msg.msg = std::forward<decltype(payload)>(payload);
    return msg;
  }

  void send(int64_t now, MachineBase *m, Message msg) {
    if (folly::Random::rand32(100) < networkPacketLossPercent) {
      MLOG("send message lost to:{} msg:{}", m->id, toString(msg));
      return;
    }
    if (state != MachineState::OUT_CONN_BROKEN) {
      MLOG("send message to:{} msg:{}", m->id, toString(msg));
      m->receive(now, std::move(msg));
    } else {
      MLOG("send message failed to:{} state:{} msg:{}", m->id, magic_enum::enum_name(state), toString(msg));
    }
  }

  void logState(int64_t now) {
    MLOG("LogState state:{} recover:{} {}",
         magic_enum::enum_name(state),
         recoverTime ? std::to_string(*recoverTime) : "alive",
         logStateImpl());
  }

  virtual ~MachineBase() = default;
  virtual void startImpl(int64_t) {}
  virtual void shutdownImpl(bool) {}
  virtual void handleDataReq(int64_t now, const Message &) { MASSERT(false, "not implemented"); }
  virtual void handleDataRsp(int64_t now, const Message &) { MASSERT(false, "not implemented"); }
  virtual void handleSyncDone(int64_t now, const Message &) { MASSERT(false, "not implemented"); }
  virtual void handleChain(int64_t now, const Message &) { MASSERT(false, "not implemented"); }
  virtual void handleHeartbeat(int64_t now, const Message &) { MASSERT(false, "not implemented"); }
  virtual void handleImpl(int64_t) {}
  virtual String logStateImpl() = 0;

  static void restoreDefaultParameters() {
    maxRecoverTime = 50;
    maxNetworkTime = 8;
    networkPacketLossPercent = 10;
    heartbeatInterval = 20;
    writeFailPercent = 10;
    heartbeatTimeout = 60;
  }

  static void disableFail() {
    networkPacketLossPercent = 0;
    writeFailPercent = 0;
  }

  MachineState state{MachineState::SHUTDOWN};
  NodeId id{0};

  static int64_t maxRecoverTime;
  static int64_t maxNetworkTime;
  static int64_t networkPacketLossPercent;
  static int64_t heartbeatInterval;
  static int64_t writeFailPercent;
  static int64_t heartbeatTimeout;

  int64_t writeTimeout = 30;
  std::optional<int64_t> recoverTime;
  std::deque<Message> mailbox;

  MachineBase *mgmtd = nullptr;
  std::map<NodeId, MachineBase *> *machines = nullptr;
};

int64_t MachineBase::maxRecoverTime = 50;
int64_t MachineBase::maxNetworkTime = 8;
int64_t MachineBase::networkPacketLossPercent = 10;
int64_t MachineBase::heartbeatInterval = 20;
int64_t MachineBase::writeFailPercent = 10;
int64_t MachineBase::heartbeatTimeout = 60;

String toString(const Message &msg) {
  auto s = std::visit(
      folly::overload(
          [](const Message::DataReqMsg &msg) {
            return fmt::format("type:{} ver:{} chainVer:{} syncing:{}",
                               toString(MessageType::DataReq),
                               msg.ver,
                               msg.chainVer,
                               msg.syncing);
          },
          [](const Message::DataRspMsg &msg) {
            return fmt::format("type:{} ver:{} syncing:{} succeeded:{}",
                               toString(MessageType::DataRsp),
                               msg.ver,
                               msg.syncing,
                               msg.succeeded);
          },
          [](const Message::SyncDoneMsg &msg) {
            return fmt::format("type:{} ver:{} chainVer:{}", toString(MessageType::SyncDone), msg.ver, msg.chainVer);
          },
          [](const Message::ChainMsg &msg) {
            return fmt::format("type:{} chain:{}", toString(MessageType::Chain), toString(msg.chain));
          },
          [](const Message::HeartbeatMsg &msg) {
            return fmt::format("type:{} lti:{}", toString(MessageType::Heartbeat), toString(msg.lti));
          }),
      msg.msg);
  return fmt::format("from:{} arrival:{} content:{{{}}}", msg.from->id, msg.arriveTime, s);
}

bool writeable(const flat::TargetInfo &ti, bool syncing = false) {
  return ti.localState != LS::OFFLINE &&
         ((!syncing && ti.publicState == PS::SERVING) || (syncing && ti.publicState == PS::SYNCING));
}

struct StorageMachine : MachineBase {
  LS ls{LS::INVALID};
  int64_t ver{0};

  std::optional<int64_t> ongoingWriteDeadline;
  std::optional<int64_t> ongoingSyncDoneDeadline;
  MachineBase *writeFrom = nullptr;
  int64_t nextHeartbeatTime = 0;
  bool bootstrapping = false;

  VersionedChain localChain;
  bool chainSyncing = false;
  size_t selfPos = 0;

  String logStateImpl() override {
    return fmt::format("ls:{} ver:{} ongoing:{} nextHeartbeat:{} localChain:{} bootstrapping:{}",
                       toString(ls),
                       ver,
                       ongoingWriteDeadline ? std::to_string(*ongoingWriteDeadline) : "empty",
                       nextHeartbeatTime,
                       toString(localChain),
                       bootstrapping);
  }

  void startImpl(int64_t now) override {
    ls = LS::ONLINE;
    ongoingWriteDeadline.reset();
    ongoingSyncDoneDeadline.reset();
    writeFrom = nullptr;
    nextHeartbeatTime = now;
    bootstrapping = true;
    localChain = {0, {}};
    chainSyncing = false;
    selfPos = 0;
  }

  MachineBase *prev() {
    if (localChain.ver && selfPos != 0) return machines->at(*localChain.chain[selfPos - 1].nodeId);
    return nullptr;
  }

  MachineBase *next() {
    if (localChain.ver && selfPos + 1 != localChain.chain.size()) {
      const auto &ti = localChain.chain[selfPos + 1];
      if (ti.publicState == PS::SYNCING || ti.publicState == PS::SERVING) return machines->at(*ti.nodeId);
    }
    return nullptr;
  }

  const flat::TargetInfo *selfTi() const {
    if (localChain.ver) return &localChain.chain[selfPos];
    return nullptr;
  }

  const flat::TargetInfo *nextTi() const {
    if (localChain.ver && selfPos + 1 != localChain.chain.size()) return &localChain.chain[selfPos + 1];
    return nullptr;
  }

  void shutdownImpl(bool critical) override {
    ls = LS::OFFLINE;
    if (critical) ver = 0;
  }

  void handleImpl(int64_t now) override {
    if (ongoingWriteDeadline) {
      if (!next()) {
        if (writeFrom && selfTi()->publicState == PS::SERVING) {
          MLOG("send back to {} since self is tail", writeFrom->id);
          // self is tail
          send(now, writeFrom, makeReq(now, Message::DataRspMsg{ver, false, true}));
          writeFrom = nullptr;
        } else {
          MLOG("stop retry send no next writeFrom?:{} ps:{}", writeFrom != nullptr, toString(selfTi()->publicState));
        }
        ongoingWriteDeadline.reset();
      } else if (*ongoingWriteDeadline <= now) {
        MLOG("retry send DataReq: timeout");
        sendData(now);
      }
    }
    if (ongoingSyncDoneDeadline && *ongoingSyncDoneDeadline <= now) {
      MLOG("retry send syncDone");
      auto req = makeReq(now, Message::SyncDoneMsg{ver, localChain.ver});
      ongoingSyncDoneDeadline = now + 2 * heartbeatInterval;
      send(now, next(), std::move(req));
    }
    if (nextHeartbeatTime <= now) {
      sendHeartbeat(now);
    }
  }

  void handleDataReq(int64_t now, const Message &msg) override {
    auto &payload = std::get<Message::DataReqMsg>(msg.msg);
    auto rsp = Message::DataRspMsg{payload.ver, payload.syncing};

    if (bootstrapping) {
      MLOG("write failed: bootstrapping");
    } else if (ongoingWriteDeadline) {
      MLOG("write failed: ongoing");
    } else if (!localChain.ver) {
      MLOG("write failed: no chain");
    } else if (!writeable(*selfTi(), payload.syncing)) {
      MLOG("write failed: not writable. self:{} syncing:{}", toString(*selfTi()), payload.syncing);
    } else if (!payload.syncing && chainSyncing) {
      MLOG("write failed: cannot do normal write on a syncing chain");
    } else if (localChain.ver != payload.chainVer) {
      MLOG("write failed: chain version mismatch chainVer:{} payload.chainVer:{}", localChain.ver, payload.chainVer);
    } else if (!payload.syncing && ver != payload.ver && ver + 1 != payload.ver) {
      MLOG("write failed: unexpected ver for normal write ver:{} payload.ver:{}", ver, payload.ver);
    } else if (auto r = folly::Random::rand32(100); r < writeFailPercent) {
      MLOG("write failed: random value:{} percent:{}", r, writeFailPercent);
    } else {
      ver = payload.ver;
      rsp.succeeded = true;
      MLOG("write succeed ver:{} syncing:{}", ver, payload.syncing);
      sendData(now);
      if (ongoingWriteDeadline) {
        writeFrom = msg.from;
      }
    }
    if (!rsp.succeeded || !ongoingWriteDeadline) send(now, msg.from, makeReq(now, std::move(rsp)));
  }

  void handleDataRsp(int64_t now, const Message &msg) override {
    auto &payload = std::get<Message::DataRspMsg>(msg.msg);

    if (bootstrapping) {
      MLOG("discard DataRsp: bootstrapping");
    } else if (!ongoingWriteDeadline) {
      MLOG("discard DataRsp: no ongoing");
    } else if (payload.ver != ver) {
      MLOG("discard DataRsp: ver:{} != payload.ver:{}", ver, payload.ver);
    } else if (next() != msg.from) {
      MLOG("discard DataRsp: next:{} != msg:{}", next() ? std::to_string(next()->id) : "null", toString(msg));
    } else if (!payload.succeeded) {
      MLOG("retry send DataReq: failed");
      sendData(now);
    } else {
      if (writeFrom) {
        auto rsp = Message::DataRspMsg{payload.ver, false, true};
        MLOG("DataRsp send rsp back to {}", writeFrom->id);
        send(now, writeFrom, makeReq(now, std::move(rsp)));
        writeFrom = nullptr;
      } else {
        MLOG("receive succeeded DataRsp without writeFrom");
      }
      ongoingWriteDeadline.reset();

      if (nextTi()->publicState == PS::SYNCING) {
        MLOG("DataRsp downstream is syncing, send syncDone to {}", next()->id);
        auto req = makeReq(now, Message::SyncDoneMsg{payload.ver, localChain.ver});
        ongoingSyncDoneDeadline = now + 2 * heartbeatInterval;
        send(now, next(), std::move(req));
      }
    }
  }

  void handleSyncDone(int64_t now, const Message &msg) override {
    auto &payload = std::get<Message::SyncDoneMsg>(msg.msg);

    if (payload.ver <= ver && payload.chainVer == localChain.ver) {
      ls = LS::UPTODATE;
      MLOG("receive syncDone, change to UPTODATE");
    } else {
      MLOG("receive mismatch syncDone ver:{} chainVer:{}", ver, localChain.ver);
    }
  }

  void handleChain(int64_t now, const Message &msg) override {
    auto &payload = std::get<Message::ChainMsg>(msg.msg);
    if (payload.chain.ver <= localChain.ver) {
      MLOG("discard Chain: stale");
      return;
    }
    MLOG("receive new chain:{}", toString(payload.chain));
    bool newChainSyncing = false;
    size_t pos = 0;
    for (size_t i = 0; i < payload.chain.chain.size(); ++i) {
      const auto &ti = payload.chain.chain[i];
      if (ti.publicState == PS::SYNCING) {
        newChainSyncing = true;
        break;
      }
    }
    for (size_t i = 0; i < payload.chain.chain.size(); ++i) {
      const auto &ti = payload.chain.chain[i];
      if (ti.targetId == id) {
        pos = i;
        if (ti.publicState != PS::SERVING && bootstrapping) {
          bootstrapping = false;
        } else if (ls == LS::UPTODATE && (ti.publicState == PS::OFFLINE || ti.publicState == PS::WAITING)) {
          MLOG("self status:{} ls:{} shutdown", toString(ti.publicState), toString(ls));
          setState(Event::SHUTDOWN, now);
          return;
        }
        if (ti.publicState == PS::SERVING) ls = LS::ONLINE;
        break;
      }
    }
    auto *oldNext = next();
    localChain = payload.chain;
    chainSyncing = newChainSyncing;
    selfPos = pos;
    ongoingSyncDoneDeadline.reset();
    auto *newNext = next();
    if (newNext && newNext != oldNext) {
      MLOG("find new downstream {}", toString(*nextTi()));
    }
    if (!ongoingWriteDeadline && nextTi() && nextTi()->publicState == PS::SYNCING) {
      sendData(now);
    }
  }

  void sendHeartbeat(int64_t now) {
    LocalTargetInfoWithNodeId lti(TargetId(id), id, bootstrapping ? LS::OFFLINE : ls);
    auto msg = makeReq(now, Message::HeartbeatMsg{lti});
    send(now, mgmtd, std::move(msg));
    nextHeartbeatTime = now + heartbeatInterval;
  }

  void sendData(int64_t now) {
    if (next()) {
      auto req = makeReq(now, Message::DataReqMsg{ver, localChain.ver, nextTi()->publicState == PS::SYNCING});
      send(now, next(), std::move(req));
      ongoingWriteDeadline = now + writeTimeout;
    }
  }
};

struct MgmtdMachine : MachineBase {
  std::map<MachineBase *, int64_t> lastHeartbeat;
  VersionedChain localChain;
  int64_t committedVer = 0;
  int64_t startTime = 0;
  bool stopWrite = false;
  std::optional<int64_t> ongoingWriteDeadline;

  String logStateImpl() override {
    return fmt::format("localChain:{} ver:{} ongoing:{}",
                       toString(localChain),
                       committedVer,
                       ongoingWriteDeadline ? std::to_string(*ongoingWriteDeadline) : "empty");
  }

  void startImpl(int64_t now) override {
    MASSERT(localChain.ver != 0, "{}", toString(localChain));
    startTime = now;
    for (auto [id, m] : *machines) {
      if (m != this) lastHeartbeat[m] = now;
    }
    ongoingWriteDeadline.reset();
    for (auto &ti : localChain.chain) {
      ti.localState = LS::OFFLINE;
    }
    stopWrite = false;
  }

  void shutdownImpl(bool) override {}

  void handleDataRsp(int64_t now, const Message &msg) override {
    auto &payload = std::get<Message::DataRspMsg>(msg.msg);

    bool resetOngoing = true;
    if (!ongoingWriteDeadline) {
      MLOG("discard DataRsp: no ongoing");
    } else if (payload.ver != committedVer + 1) {
      MASSERT(payload.ver <= committedVer,
              "shouldn't see so large version committed:{} payload.ver:{}",
              committedVer,
              payload.ver);
      MLOG("discard DataRsp: committed:{} + 1 != payload.ver:{}", committedVer, payload.ver);
    } else if (currentHead() != msg.from) {
      MLOG("discard DataRsp: head:{} != msg:{}",
           currentHead() ? std::to_string(currentHead()->id) : "null",
           toString(msg));
    } else if (!payload.succeeded) {
      MLOG("retry send DataReq: failed");
      sendData(now);
      resetOngoing = false;
    } else {
      ++committedVer;
      MLOG("confirm commit ver:{}", committedVer);
    }
    if (resetOngoing) ongoingWriteDeadline.reset();
  }

  void handleHeartbeat(int64_t now, const Message &msg) override {
    lastHeartbeat[msg.from] = now;
    auto &payload = std::get<Message::HeartbeatMsg>(msg.msg);
    applyUpdate(now, payload.lti, "heartbeat");
    auto req = makeReq(now, Message::ChainMsg{localChain});
    send(now, msg.from, req);
  }

  void handleImpl(int64_t now) override {
    if (ongoingWriteDeadline && *ongoingWriteDeadline <= now) {
      ongoingWriteDeadline.reset();
    }

    if (!ongoingWriteDeadline) {
      sendData(now);
    }

    checkHeartbeatTimeout(now);
  }

  MachineBase *currentHead() {
    const auto &ti = headTi();
    if (ti.publicState == PS::SERVING) return machines->at(*ti.nodeId);
    return nullptr;
  }

  const flat::TargetInfo &headTi() { return localChain.chain.front(); }

  void sendData(int64_t now) {
    auto *head = currentHead();
    if (!stopWrite && head && writeable(headTi(), false)) {
      auto req = makeReq(now, Message::DataReqMsg{committedVer + 1, localChain.ver});
      send(now, head, std::move(req));
      ongoingWriteDeadline = now + writeTimeout;
    }
  }

  void checkHeartbeatTimeout(int64_t now) {
    for (auto [m, lastHbTime] : lastHeartbeat) {
      if (m != this && lastHbTime + heartbeatTimeout < now) {
        LocalTargetInfoWithNodeId lti(TargetId(m->id), m->id, LS::OFFLINE);
        applyUpdate(now, lti, "timeout");
      }
    }
  }

  void applyUpdate(int64_t now, LocalTargetInfoWithNodeId lti, String reason) {
    bool bootstrapping = now < startTime + heartbeatTimeout;
    auto newChain = mergeAndGenerateNewChain(localChain.chain, {lti}, bootstrapping);
    auto validateRes = bootstrapping ? "" : validate(localChain.chain, newChain);
    MASSERT(validateRes.empty(),
            "Mgmtd chain validation failed: {} {} + {} -> {}",
            validateRes,
            toString(localChain.chain),
            toString(lti),
            toString(newChain));
    if (localChain.chain != newChain) {
      MLOG("update chain {} {}: {} + {} -> {}",
           bootstrapping ? "bootstrapping" : "",
           reason,
           toString(localChain),
           toString(lti),
           toString(VersionedChain{localChain.ver + 1, newChain}));
      localChain.chain = newChain;
      ++localChain.ver;
    }
  }
};

TEST_F(UpdateChainTest, testOneMachine) { testStateMachine(1, true, false); }

TEST_F(UpdateChainTest, testTwoMachines) { testStateMachine(2, true, true); }

TEST_F(UpdateChainTest, testThreeMachines) { testStateMachine(3, true, true); }

void UpdateChainTest::testStateMachine(int targetCount, bool injectFailure, bool allowCritical) {
  ChainBuilder builder;
  MgmtdMachine mgmtd;
  std::vector<StorageMachine> storages(targetCount);
  std::map<NodeId, MachineBase *> machines = {{NodeId{100}, &mgmtd}};
  std::vector<MachineBase *> machinesVec = {&mgmtd};
  for (int i = 0; i < targetCount; ++i) {
    builder.add(TargetId(i + 1), PS::SERVING, LS::OFFLINE, NodeId(i + 1));
    storages[i].id = NodeId(i + 1);
    storages[i].mgmtd = &mgmtd;
    storages[i].machines = &machines;
    storages[i].recoverTime = 40 * (i + 1);
    storages[i].writeTimeout = MachineBase::maxNetworkTime * 2;
    machines.try_emplace(NodeId(i + 1), &storages[i]);
    machinesVec.push_back(&storages[i]);
  }

  mgmtd.id = NodeId{100};
  mgmtd.machines = &machines;
  mgmtd.mgmtd = &mgmtd;
  mgmtd.writeTimeout = MachineBase::maxNetworkTime * 6;
  mgmtd.recoverTime = 1;
  mgmtd.localChain = {1, builder.build()};

  std::vector events = {
      Event::SHUTDOWN,
      Event::IN_CONN_BROKEN,
      Event::OUT_CONN_BROKEN,
      Event::HANG,
  };

  MachineBase::disableFail();
  constexpr auto halfPeriod = 50000;
  constexpr auto safePeriod = 1000;
  constexpr auto eventCycle = 200;
  int t = 0;
  auto roundedRun = [&] {
    for (auto [id, m] : machines) m->handle(t);
    for (auto [id, m] : machines) m->logState(t);
  };
  auto triggerNormalEvent = [&] {
    if (injectFailure && t % eventCycle == 0) {
      auto m = RandomUtils::randomSelect(machinesVec);
      auto e = RandomUtils::randomSelect(events);
      if (m->state == MachineState::NORMAL) m->setState(e, t);
    }
  };
  auto triggerCriticalEvent = [&] {
    if (injectFailure && allowCritical) {
      auto m = RandomUtils::randomSelect(machinesVec);
      auto e = Event::CRITICAL_SHUTDOWN;
      if (m->state == MachineState::NORMAL) m->setState(e, t);
    }
  };

  if (injectFailure) MachineBase::restoreDefaultParameters();
  // 0 - 49000: run with potential failures
  for (; t < halfPeriod - safePeriod; ++t) {
    roundedRun();
    triggerNormalEvent();
  }
  // 49000 - 50000: disable all failures, keep writing
  MachineBase::disableFail();
  for (; t < halfPeriod; ++t) {
    roundedRun();
  }
  // 50000: expected the critical shutdown is triigered on an all-normal cluster
  triggerCriticalEvent();
  // 50000 - 51000: disable all failures, wait for cluster fully recover
  for (; t < halfPeriod + safePeriod; ++t) {
    roundedRun();
  }
  // 51000 - 99000: run with potential failures
  if (injectFailure) MachineBase::restoreDefaultParameters();
  for (; t < 2 * halfPeriod - safePeriod; ++t) {
    roundedRun();
    triggerNormalEvent();
  }
  // 99000 - 100000: disable all failures and stop writing
  MachineBase::disableFail();
  mgmtd.stopWrite = true;
  for (; t < 2 * halfPeriod; ++t) {
    roundedRun();
  }

  for (auto [id, m] : machines) {
    ASSERT_EQ(m->state, MachineState::NORMAL);
  }
  for (const auto &ti : mgmtd.localChain.chain) {
    ASSERT_EQ(ti.publicState, PS::SERVING);
  }
  ASSERT_TRUE(mgmtd.committedVer == storages[0].ver || mgmtd.committedVer + 1 == storages[0].ver)
      << mgmtd.committedVer << " " << storages[0].ver;
  for (int i = 0; i < targetCount; ++i) {
    ASSERT_EQ(storages[0].ver, storages[i].ver);
    ASSERT_EQ(mgmtd.localChain.ver, storages[i].localChain.ver);
  }
}
}  // namespace
}  // namespace hf3fs::testing
