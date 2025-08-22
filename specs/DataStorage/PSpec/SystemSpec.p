spec WriteComplete observes eWriteReq, eWriteResp {
  var completedWriteReqTags: map[tMessageTag, bool];
  var numPendingWriteReqs: int;

  fun OnWriteReq(writeReq: tWriteReq) {
    if (writeReq.fromClient && !(writeReq.tag in completedWriteReqTags)) {
      completedWriteReqTags += (writeReq.tag, false);
      numPendingWriteReqs = numPendingWriteReqs + 1;
      if (numPendingWriteReqs > 0) {
        goto PendingWrites;
      }
    }
  }

  fun OnWriteResp(writeResp: tWriteResp) {
    assert writeResp.tag in completedWriteReqTags;

    if (writeResp.status != ErrorCode_SUCCESS) {
      return;
    }

    if (!completedWriteReqTags[writeResp.tag]) {
      completedWriteReqTags[writeResp.tag] = true;
      numPendingWriteReqs = numPendingWriteReqs - 1;
      if (numPendingWriteReqs == 0) {
        goto NoPendingWrites;
      }
    }
  }

  start cold state NoPendingWrites {
    entry {
      print format("numPendingWriteReqs: {0}, completedWriteReqTags: {1}", numPendingWriteReqs, completedWriteReqTags);
      assert numPendingWriteReqs == 0, format("{0} pending writes not equal to zero", numPendingWriteReqs);
    }

    on eWriteReq do OnWriteReq;

    on eWriteResp do OnWriteResp;
  }

  hot state PendingWrites {
    entry {
      print format("numPendingWriteReqs: {0}, completedWriteReqTags: {1}", numPendingWriteReqs, completedWriteReqTags);
    }

    on eWriteReq do OnWriteReq;

    on eWriteResp do OnWriteResp;
  }
}

event eSystemConfig: (config: tSystemConfig);
event eStorageSystem: (system: tStorageSystem);

spec AllWriteItersProcessed observes eWriteWork, eWriteReq, eWriteResp, eSystemConfig, eStorageSystem {
  var config: tSystemConfig;
  var mgmtService: MgmtService;
  var storageServices: tStorageServiceMap;

  var seenWriteRequestTags: map[tNodeId, set[tMessageTag]];
  var seenWriteResponseTags: map[tNodeId, set[tMessageTag]];
  var seenWriteProcs: map[tMessageTag, map[tTargetId, map[tChunkVer, set[machine]]]];
  var clientDone: set[tNodeId];

  start state Init {
    on eSystemConfig goto SendingWriteReqs with (args: (config: tSystemConfig)) {
      config = args.config;
    }

    on eStorageSystem do (args: (system: tStorageSystem)) {
      mgmtService = args.system.mgmt;
      storageServices = args.system.storages;
    }
  }

  hot state SendingWriteReqs {
    entry {
      var tag: tMessageTag;

      foreach (tag in keys(seenWriteProcs)) {
        print format("write request tag: {0}, seenWriteProcs: {1}", tag, seenWriteProcs[tag]);
      }

      print format("seenWriteRequestTags: {0}", seenWriteRequestTags);
      print format("seenWriteResponseTags: {0}", seenWriteResponseTags);
    }

    on eWriteWork goto SendingWriteReqs with (writeWork: tWriteWork) {
      if (!(writeWork.tag in seenWriteProcs)) {
        seenWriteProcs += (writeWork.tag, default(map[tTargetId, map[tChunkVer, set[machine]]]));
      }

      if (!(writeWork.targetId in seenWriteProcs[writeWork.tag])) {
        seenWriteProcs[writeWork.tag] += (writeWork.targetId, default(map[tChunkVer, set[machine]]));
      }

      if (!(writeWork.updateVer in seenWriteProcs[writeWork.tag][writeWork.targetId])) {
        seenWriteProcs[writeWork.tag][writeWork.targetId] += (writeWork.updateVer, default(set[machine]));
      }

      seenWriteProcs[writeWork.tag][writeWork.targetId][writeWork.updateVer] += (writeWork.from);
    }

    on eStorageSystem do (args: (system: tStorageSystem)) {
      mgmtService = args.system.mgmt;
      storageServices = args.system.storages;
    }

    on eWriteReq do (writeReq: tWriteReq) {
      if (!(writeReq.tag.nodeId in seenWriteRequestTags))
        seenWriteRequestTags += (writeReq.tag.nodeId, default(set[tMessageTag]));
      seenWriteRequestTags[writeReq.tag.nodeId] += (writeReq.tag);
    }

    on eWriteResp do (writeResp: tWriteResp) {
      if (writeResp.status != ErrorCode_SUCCESS) {
        return;
      }

      assert writeResp.tag in seenWriteRequestTags[writeResp.tag.nodeId];

      if (!(writeResp.tag.nodeId in seenWriteResponseTags))
        seenWriteResponseTags += (writeResp.tag.nodeId, default(set[tMessageTag]));
      seenWriteResponseTags[writeResp.tag.nodeId] += (writeResp.tag);

      if (sizeof(seenWriteResponseTags[writeResp.tag.nodeId]) == config.numIters) {
        clientDone += (writeResp.tag.nodeId);
        if (sizeof(clientDone) == config.numClients)
          goto Done;
      }
    }
  }

  cold state Done {
    ignore eWriteWork, eWriteReq, eWriteResp;

    entry {
      print format("all iterations processed {0}", clientDone);
    }
  }
}

spec MonotoneIncreasingVersionNumber observes eWriteOpFinishResult, eCommitOpResult {
  var chunkReplicaCommits: map[(tChunkId, tTargetId), map[tChunkVer, tCommitWork]];

  start state WaitForResponses {

    on eWriteOpFinishResult do (writeFinishRes: tWriteOpFinishResult) {
      var writeWork: tWriteWork;
      var chunkIdOnTarget: (tChunkId, tTargetId);

      if (writeFinishRes.status != ErrorCode_SUCCESS) {
        return;
      }

      writeWork = writeFinishRes.writeWork;
      chunkIdOnTarget = (writeWork.key.chunkId, writeWork.targetId);

      if (writeWork.fullChunkReplace) {
        chunkReplicaCommits -= (chunkIdOnTarget);
      }
    }

    on eCommitOpResult do (commitOpResult: tCommitOpResult) {
      var commitWork: tCommitWork;
      var commitMsg: tCommitMsg;
      var chunkVer: tChunkVer;
      var commitVer: tChunkVer;
      var chunkId: tChunkId;
      var chunkIdOnTarget: (tChunkId, tTargetId);

      if (commitOpResult.status != ErrorCode_SUCCESS) {
        return;
      }

      commitWork = commitOpResult.commitWork;
      commitMsg = commitWork.commitMsg;
      commitVer = commitOpResult.commitVer;
      chunkId = commitWork.key.chunkId;
      chunkIdOnTarget = (chunkId, commitWork.targetId);

      if (!(chunkIdOnTarget in chunkReplicaCommits)) {
        chunkReplicaCommits += (chunkIdOnTarget, default(map[tChunkVer, tCommitWork]));
      }

      if (commitOpResult.removeChunk) {
        print format("remove request {0} committed, clear chunkReplicaCommits[{1}]: {2}",
          commitMsg.tag, chunkIdOnTarget, chunkReplicaCommits[chunkIdOnTarget]);
        chunkReplicaCommits -= (chunkIdOnTarget);
        return;
      }

      if (!(commitVer in chunkReplicaCommits[chunkIdOnTarget])) {
        // all existing commits should have smaller version
        foreach (chunkVer in keys(chunkReplicaCommits[chunkIdOnTarget])) {
          assert commitVer > chunkVer,
            format ("current commit version {0} <= previous version {1} found in chunkReplicaCommits[chunkId:{2}]: {3}, commit result: {4}",
              commitVer, chunkVer, chunkIdOnTarget, chunkReplicaCommits[chunkIdOnTarget], commitOpResult);
        }

        chunkReplicaCommits[chunkIdOnTarget] += (commitVer, commitWork);
      }
    }
  }
}

// DONE: check chunk content after each update

spec AllReplicasOnChainUpdated observes eReadWorkDone, eWriteWorkDone, eCommitWorkDone, eNewRoutingInfo {
  var seenRoutingVers: set[tRoutingVer];
  var replicaChains: map[tVersionedChainId, tReplicaChain];
  var chunkVersionOnTarget: map[tChunkId, map[tTargetId, tChunkVer]];
  var chunkContentOnTarget: map[tChunkId, map[tChunkVer, map[tTargetId, tBytes]]];
  var updatesOfChunkReplica: map[tChunkId, map[tChunkVer, map[tTargetId, seq[tWriteWorkDone]]]];

  fun updateChunkVersionOnTarget(chunkId: tChunkId, targetId: tTargetId, updateVer: tChunkVer) {
    if (!(chunkId in chunkVersionOnTarget)) {
      chunkVersionOnTarget += (chunkId, default(map[tTargetId, tChunkVer]));
    }

    if (!(targetId in chunkVersionOnTarget[chunkId])) {
      chunkVersionOnTarget[chunkId] += (targetId, 0);
    }

    if (chunkVersionOnTarget[chunkId][targetId] < updateVer) {
      chunkVersionOnTarget[chunkId][targetId] = updateVer;
    }
  }

  fun updateChunkContentOnTarget(chunkId: tChunkId, targetId: tTargetId, updateVer: tChunkVer, chunkContent: tBytes) {
    if (!(chunkId in chunkContentOnTarget)) {
      chunkContentOnTarget += (chunkId, default(map[tChunkVer, map[tTargetId, tBytes]]));
    }

    if (!(updateVer in chunkContentOnTarget[chunkId])) {
      chunkContentOnTarget[chunkId] += (updateVer, default(map[tTargetId, tBytes]));
    }

    if (targetId in chunkContentOnTarget[chunkId][updateVer]) {
      if (chunkContentOnTarget[chunkId][updateVer][targetId] != chunkContent) {
        print format("find different chunk content {0} than chunkContentOnTarget[chunkId:{1}][updateVer:{2}] {3}",
            chunkContent, chunkId, updateVer, chunkContentOnTarget[chunkId][updateVer]);
      }
    }

    chunkContentOnTarget[chunkId][updateVer][targetId] = chunkContent;
  }

  fun addUpdateOfChunkReplica(chunkId: tChunkId, targetId: tTargetId, updateVer: tChunkVer, writeWorkDone: tWriteWorkDone) {
    if (!(chunkId in updatesOfChunkReplica)) {
      updatesOfChunkReplica += (chunkId, default(map[tChunkVer, map[tTargetId, seq[tWriteWorkDone]]]));
    }

    if (!(updateVer in updatesOfChunkReplica[chunkId])) {
      updatesOfChunkReplica[chunkId] += (updateVer, default(map[tTargetId, seq[tWriteWorkDone]]));
    }

    if (!(targetId in updatesOfChunkReplica[chunkId][updateVer])) {
      updatesOfChunkReplica[chunkId][updateVer] += (targetId, default(seq[tWriteWorkDone]));
    }

    updatesOfChunkReplica[chunkId][updateVer][targetId] += (sizeof(updatesOfChunkReplica[chunkId][updateVer][targetId]), writeWorkDone);
  }

  start state WaitForUpdates {
    on eReadWorkDone do (readWorkDone: tReadWorkDone) {
      var chunkId: tChunkId;
      var targetId: tTargetId;
      var updateVer: tChunkVer;

      chunkId = readWorkDone.chunkMetadata.chunkId;
      targetId = readWorkDone.targetId;
      updateVer = readWorkDone.chunkMetadata.updateVer;

      if (readWorkDone.status == ErrorCode_SUCCESS && sizeof(readWorkDone.dataBytes) == readWorkDone.chunkMetadata.chunkSize) {
        // updateChunkVersionOnTarget(chunkId, targetId, updateVer);
        // updateChunkContentOnTarget(chunkId, targetId, updateVer, readWorkDone.dataBytes);
      }
    }

    on eWriteWorkDone do (writeWorkDone: tWriteWorkDone) {
      var chunkId: tChunkId;
      var targetId: tTargetId;
      var updateVer: tChunkVer;

      if (writeWorkDone.status != ErrorCode_SUCCESS &&
          writeWorkDone.status != ErrorCode_CHUNK_COMMITTED_UPDATE &&
          writeWorkDone.status != ErrorCode_CHUNK_STALE_UPDATE)
        return;

      chunkId = writeWorkDone.key.chunkId;
      targetId = writeWorkDone.targetId;
      updateVer = writeWorkDone.updateVer;

      if (writeWorkDone.status == ErrorCode_SUCCESS) {
        updateChunkVersionOnTarget(chunkId, targetId, updateVer);
        updateChunkContentOnTarget(chunkId, targetId, updateVer, writeWorkDone.currentChunkContent);
        addUpdateOfChunkReplica(chunkId, targetId, updateVer, writeWorkDone);
      }
    }

    on eCommitWorkDone do (commitWorkDone: tCommitWorkDone) {
      var chunkId: tChunkId;
      var targetId: tTargetId;
      var targetIdx: tTargetId;
      var commitVer: tChunkVer;
      var updateVer: tChunkVer;
      var chunkVer: tChunkVer;
      var chunkContent: tBytes;
      var replicaChain: tReplicaChain;
      var commitMsg: tCommitMsg;
      var writeWorkDone: tWriteWorkDone;
      var writeWorkIdx: int;

      if (commitWorkDone.status != ErrorCode_SUCCESS &&
          commitWorkDone.status != ErrorCode_CHUNK_STALE_COMMIT)
        return;

      targetId = commitWorkDone.targetId;
      chunkId = commitWorkDone.key.chunkId;
      commitVer = commitWorkDone.commitVer;
      replicaChain = replicaChains[commitWorkDone.key.vChainId];
      commitMsg = commitWorkDone.commitMsg;

      // this is a special commit to remove an old chunk from a returning target
      if (commitWorkDone.removeChunk && commitWorkDone.commitVer == 0)
        return;

      if (chunkId in updatesOfChunkReplica) {
        // print all write works on the chunk
        foreach (chunkVer in keys(updatesOfChunkReplica[chunkId])) {
          foreach (targetIdx in keys(updatesOfChunkReplica[chunkId][chunkVer])) {
            writeWorkIdx = 0;
            while (writeWorkIdx < sizeof(updatesOfChunkReplica[chunkId][chunkVer][targetIdx])) {
              writeWorkDone = updatesOfChunkReplica[chunkId][chunkVer][targetIdx][writeWorkIdx];
              print format("updatesOfChunkReplica[chunkId:{0}][updateVer:{1}][targetIdx:{2}][#{3}][chainVer:{4}][remove:{5}][commit:{6}]: {7}",
                chunkId, chunkVer, targetIdx, writeWorkIdx, writeWorkDone.chainVer, writeWorkDone.currentChunkContent == default(tBytes), chunkVer <= commitVer, writeWorkDone);
              writeWorkIdx = writeWorkIdx + 1;
            }
          }
        }
      }

      if (chunkId in chunkVersionOnTarget) {
        // print all versions of the chunk
        print format("chunkVersionOnTarget[chunkId:{0}]: {1}", chunkId, chunkVersionOnTarget[chunkId]);
      }

      if (chunkId in chunkContentOnTarget) {
        // print all contents of the chunk
        print format("chunkContentOnTarget[chunkId:{0}]: {1}", chunkId, chunkContentOnTarget[chunkId]);
      }

      if (commitWorkDone.removeChunk) {
        if (chunkId in chunkVersionOnTarget && targetId in chunkVersionOnTarget[chunkId]) {
          print format("remove versions of chunk {0}: {1}", chunkId, chunkVersionOnTarget[chunkId]);
          chunkVersionOnTarget[chunkId] -= (targetId);
        }
        if (chunkId in chunkContentOnTarget) {
          print format("remove contents of chunk {0}: {1}", chunkId, chunkContentOnTarget[chunkId]);
          foreach (chunkVer in keys(chunkContentOnTarget[chunkId])) {
            chunkContentOnTarget[chunkId][chunkVer] -= (targetId);
          }
        }
        return;
      }

      // foreach (targetId in replicaChain.targets) {
      //   if (replicaChain.states[targetId] == PublicTargetState_SERVING) {
      //     assert chunkId in chunkVersionOnTarget && targetId in chunkVersionOnTarget[chunkId] && chunkVersionOnTarget[chunkId][targetId] >= commitVer,
      //       format("missing update, tag:{0}, chunkId:{1}, targetId:{2}, commitVer:{3}, chunkVersionOnTarget: {4}, replica chain: {5}",
      //         commitMsg.tag, chunkId, targetId, commitVer, chunkVersionOnTarget[chunkId], replicaChain);
      //   }
      // }

      // foreach (targetId in replicaChain.targets) {
      //   if (replicaChain.states[targetId] == PublicTargetState_SERVING) {
      //     if (sizeof(chunkContent) == 0) {
      //       assert chunkId in chunkContentOnTarget && commitVer in chunkContentOnTarget[chunkId] && targetId in chunkContentOnTarget[chunkId][commitVer],
      //         format("missing chunk content, chunkId:{0}, commitVer:{1}, targetId:{2}, chunkContentOnTarget: {3}",
      //           chunkId, commitVer, targetId, chunkContentOnTarget);
      //       chunkContent = chunkContentOnTarget[chunkId][commitVer][targetId];
      //     } else {
      //       assert chunkContentOnTarget[chunkId][commitVer][targetId] == chunkContent,
      //         format("inconsistent replica, chunkContentOnTarget[chunkId:{0}][commitVer:{1}] {2}",
      //           chunkId, commitVer, chunkContentOnTarget[chunkId][commitVer]);
      //     }
      //   }
      // }
    }

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      var replicaChain: tReplicaChain;

      if (routingInfo.routingVer in seenRoutingVers) {
        return;
      } else {
        seenRoutingVers += (routingInfo.routingVer);
      }

      foreach (replicaChain in values(routingInfo.replicaChains)) {
        replicaChains[replicaChain.vChainId] = replicaChain;
      }
    }
  }
}

event eStopMonitorTargetStates;

spec AllReplicasInServingState observes eNewRoutingInfo, eSyncStartReq, eSyncDoneResp, eStopMonitorTargetStates {
  var knownReplicaChains: tReplicaChainMap;
  var unavailableTargets: map[tTargetId, tPublicTargetState];
  var syncWorkers: map[tTargetId, set[machine]];

  fun checkForUnavailableTargets(routingInfo: tRoutingInfo) {
    var targetId: tTargetId;
    var replicaChain: tReplicaChain;

    unavailableTargets = default(map[tTargetId, tPublicTargetState]);

    foreach (replicaChain in values(routingInfo.replicaChains)) {
      if (!(replicaChain.vChainId.chainId in knownReplicaChains) ||
          replicaChain.vChainId.chainVer > knownReplicaChains[replicaChain.vChainId.chainId].vChainId.chainVer)
      {
        foreach (targetId in replicaChain.targets) {
          if (replicaChain.states[targetId] != PublicTargetState_SERVING &&
              replicaChain.states[targetId] != PublicTargetState_LASTSRV)
          {
            unavailableTargets[targetId] = replicaChain.states[targetId];
          }
        }

        knownReplicaChains[replicaChain.vChainId.chainId] = replicaChain;
        print format("added a new chain: {0}, unavailableTargets: {1}", replicaChain, unavailableTargets);
      }
    }

    if (sizeof(unavailableTargets) > 0) {
      goto SomeTargetsUnavailable;
    } else {
      goto AllTargetsAvailable;
    }
  }

  fun onSyncDone(syncDoneResp: tSyncDoneResp) {
    // assert syncDoneResp.targetId in unavailableTargets,
    //   format("sync target {0} not found in unavailableTargets: {1}", syncDoneResp, unavailableTargets);
  }

  fun onSyncStart(syncStartReq: tSyncStartReq) {
    if (!(syncStartReq.targetId in syncWorkers)) {
      syncWorkers += (syncStartReq.targetId, default(set[machine]));
    }

    syncWorkers[syncStartReq.targetId] += (syncStartReq.from);
  }

  start cold state AllTargetsAvailable {
    on eNewRoutingInfo do checkForUnavailableTargets;

    on eSyncDoneResp do onSyncDone;

    on eSyncStartReq do onSyncStart;

    on eStopMonitorTargetStates goto Done;
  }

  hot state SomeTargetsUnavailable {
    entry {
      var replicaChain: tReplicaChain;

      print format("unavailable targets: {0}, sync workers: {1}", unavailableTargets, syncWorkers);

      // foreach (replicaChain in values(knownReplicaChains)) {
      //   print format("known replica chain: {0}", replicaChain);
      // }
    }

    on eNewRoutingInfo do checkForUnavailableTargets;

    on eSyncDoneResp do onSyncDone;

    on eSyncStartReq do onSyncStart;

    on eStopMonitorTargetStates goto Done;
  }

  cold state Done {
    ignore eNewRoutingInfo, eSyncStartReq, eSyncDoneResp;
  }
}
