/* User Defined Types */

enum tErrorCode {
  ErrorCode_SUCCESS,
  ErrorCode_ERROR,
  ErrorCode_CHAIN_VERION_MISMATCH,
  ErrorCode_TARGET_OFFLINE,
  ErrorCode_TARGET_INACTIVE,
  ErrorCode_TARGET_ALREADY_UPTODATE,
  ErrorCode_CHUNK_NOT_FOUND,
  ErrorCode_CHUNK_NOT_CLEAN,
  ErrorCode_CHUNK_STALE_COMMIT,
  ErrorCode_CHUNK_COMMITTED_UPDATE,
  ErrorCode_CHUNK_STALE_UPDATE,
  ErrorCode_CHUNK_MISSING_UPDATE,
  ErrorCode_CHUNK_NOT_COMMIT,
  ErrorCode_CHUNK_IO_ERROR,
  ErrorCode_CHUNK_BUSY,
  ErrorCode_DUPLICATE_REQUEST,
  ErrorCode_TIMEOUT
}

type tBytes      = seq[int];
type tChainId    = int;
type tChainVer   = int;
type tChunkId    = int;
type tChunkVer   = int;
type tTargetId   = int;
type tNodeId     = int;
type tRequestId  = int;
type tVersionedChainId = (chainId: tChainId, chainVer: tChainVer);
type tReplicaChain     = (vChainId: tVersionedChainId, targets: seq[tTargetId], states: map[tTargetId, tPublicTargetState], nodes: map[tTargetId, tNodeId], services: map[tTargetId, StorageService]);
type tGlobalKey        = (vChainId: tVersionedChainId, chunkId: tChunkId);

type tMessageTag  = (nodeId: tNodeId, requestId: tRequestId);
type tWriteReq    = (from: machine, retries: int, tag: tMessageTag, key: tGlobalKey,
                     updateVer: tChunkVer, commitChainVer: tChainVer, fullChunkReplace: bool, removeChunk: bool, fromClient: bool,
                     offset: int, length: int, dataBytes: tBytes);
type tReadReq     = (from: machine, retries: int, tag: tMessageTag, key: tGlobalKey, offset: int, length: int);

type tGenericResp = (tag: tMessageTag, status: tErrorCode);
type tWriteResp   = (tag: tMessageTag, key: tGlobalKey, status: tErrorCode, commitVer: tChunkVer);
type tReadResp    = (tag: tMessageTag, key: tGlobalKey, status: tErrorCode, chunkMetadata: tChunkMetadata, dataBytes: tBytes);

type tUpdateMsg  = (from: machine, tag: tMessageTag, writeReq: tWriteReq);
type tCommitMsg  = (retries: int, tag: tMessageTag, key: tGlobalKey, status: tErrorCode, commitVer: tChunkVer, commitChainVer: tChainVer, currentChunkContent: tBytes);

type tSyncChunkReq   = (from: machine, tag: tMessageTag, writeReq: tWriteReq);
type tSyncChunkResp  = (from: machine, key: tGlobalKey, status: tErrorCode, commitVer: tChunkVer);

type tChunkSnapshot  = (metadata: tChunkMetadata, content: tBytes);
type tTargetSyncInfo = (targetId: tTargetId, targetState: tLocalTargetState, lastServeChainVer: tChainVer,
                        chunkStoreSnapshot: map[tChunkId, tChunkSnapshot]);
type tSyncStartReq   = (from: machine, vChainId: tVersionedChainId, targetId: tTargetId);
type tSyncStartResp  = (status: tErrorCode, syncInfo: tTargetSyncInfo);
type tSyncDoneReq    = (from: machine, vChainId: tVersionedChainId, targetId: tTargetId);
type tSyncDoneResp   = (status: tErrorCode, vChainId: tVersionedChainId, targetId: tTargetId);

fun ReplicaChainToString(replicaChain: tReplicaChain): string {
  return format("chain {0}: states: {1}, services: {2}",
    replicaChain.vChainId,
    PublicTargetStatesToString(replicaChain.states),
    replicaChain.services);
}

/* Event Types */

event eWriteReq  : tWriteReq;
event eReadReq   : tReadReq;
event eWriteResp : tWriteResp;
event eReadResp  : tReadResp;
event eUpdateMsg : tUpdateMsg;
event eCommitMsg : tCommitMsg;

// event eSyncChunkMsg  : tSyncChunkMsg;
event eSyncStartReq  : tSyncStartReq;
event eSyncStartResp : tSyncStartResp;
event eSyncDoneReq   : tSyncDoneReq;
event eSyncDoneResp  : tSyncDoneResp;

/* Storage Service */

// DONE: group in-memory states by replication chains
// DONE: rewrite the storage service/target code in sequential style (create a machine for each write request/work)
// DONE: simplify interaction between StorageTarget and ChunkReplica (move begin/write/finish ops into replica)
// TODO: create update context and queue for write/update/commit requests (including service, chain, target state etc. for debug)

type tLocalReplicaChainInfo = (status: tErrorCode, vChainId: tVersionedChainId,
                               isHeadTarget: bool, isTailTarget: bool,
                               successorNodeId: tNodeId, predecessorNodeId: tNodeId,
                               localTargetId: tTargetId, publicTargetState: tPublicTargetState,
                               successorTargetId: tTargetId, successorTargetState: tPublicTargetState);
type tMessageTagUpdateVer = (tMessageTag, tChunkVer);

type tWriteContext = (request: tWriteReq, process: WriteProcess, status: tErrorCode);
// event eWriteReqBlocked: tWriteContext;
// event eWriteReqUpdated: tWriteContext;
// event eWriteReqFailed: tWriteContext;
event eWriteReqCompleted: tWriteContext;
event eProcssStopped: machine;

fun ReadLocalChunk(from: machine, storageTarget: StorageTarget,
  chunkId: tChunkId, offset: int, length: int, readUncommitted: bool): tReadWorkDone
{
  var readWork: tReadWork;
  var readWorkDone: tReadWorkDone;

  readWork = (from = from, chunkId = chunkId, offset = offset, length = length, readUncommitted = readUncommitted);

  send storageTarget, eReadWork, readWork;

  receive {
    case eReadWorkDone: (workDone: tReadWorkDone) {
      readWorkDone = workDone;
    }
  }

  return readWorkDone;
}

machine ReadProcess
{
  var readReq: tReadReq;
  var chainInfo: tLocalReplicaChainInfo;
  var replicaChain: tReplicaChain;
  var localTarget: StorageTarget;
  var localService: StorageService;

  start state Init {
    entry (args: (
        readReq: tReadReq,
        chainInfo: tLocalReplicaChainInfo,
        replicaChain: tReplicaChain,
        localTarget: StorageTarget,
        localService: StorageService))
    {
      readReq = args.readReq;
      chainInfo = args.chainInfo;
      replicaChain = args.replicaChain;
      localTarget = args.localTarget;
      localService = args.localService;
      goto Running;
    }
  }

  state Running {
    entry {
      var readWorkDone: tReadWorkDone;

      print format("{0}: start read process: {1}", this, readReq);
      print format("{0}: replication chain: {1}", this, ReplicaChainToString(replicaChain));
      print format("{0}: local chain info: {1}", this, chainInfo);
      print format("{0}: local target: {1}, local service: {2}", this, localTarget, localService);

      if (chainInfo.status != ErrorCode_SUCCESS || chainInfo.publicTargetState != PublicTargetState_SERVING || readReq.key.vChainId != chainInfo.vChainId) {
        send readReq.from, eReadResp, (tag = readReq.tag, key = readReq.key, status = ErrorCode_CHAIN_VERION_MISMATCH,
          chunkMetadata = default(tChunkMetadata), dataBytes = default(tBytes));
        return;
      }

      readWorkDone = ReadLocalChunk(this, localTarget, readReq.key.chunkId, readReq.offset, readReq.length, false /*readUncommitted*/);

      send readReq.from, eReadResp,
        (tag = readReq.tag, key = readReq.key, status = readWorkDone.status,
          chunkMetadata = readWorkDone.chunkMetadata, dataBytes = readWorkDone.dataBytes);

      raise halt;
    }
  }
}

fun updateGlobalKey(key: tGlobalKey, chainVer: tChainVer): tGlobalKey {
  key.vChainId.chainVer = Max(key.vChainId.chainVer, chainVer);
  return key;
}

type tRetryWriteReq   = (chainInfo: tLocalReplicaChainInfo, replicaChain: tReplicaChain, localTarget: StorageTarget, localService: StorageService, successorService: StorageService);
event eRetryWriteReq  : tRetryWriteReq;

type  tGetLocalChainInfoResult = (chainInfo: tLocalReplicaChainInfo, replicaChain: tReplicaChain, successorService: StorageService);
event eGetLocalChainInfo       : (from: machine, chainId: tChainId);
event eGetLocalChainInfoResult : tGetLocalChainInfoResult;

fun GetLocalChainInfo(caller: machine, storageService: StorageService, chainId: tChainId): tGetLocalChainInfoResult {
  var chainInfoResult: tGetLocalChainInfoResult;

  send storageService, eGetLocalChainInfo, (from = caller, chainId = chainId);
  receive {
    case eGetLocalChainInfoResult: (result: tGetLocalChainInfoResult) { chainInfoResult = result; }
  }

  return chainInfoResult;
}

machine WriteProcess
{
  var writeReq: tWriteReq;
  var chainInfo: tLocalReplicaChainInfo;
  var replicaChain: tReplicaChain;
  var localTarget: StorageTarget;
  var localService: StorageService;
  var successorService: StorageService;
  var fromSyncWorker: bool;
  var timer: Timer;
  var forwardRetries: int;
  var currentChunkContent: tBytes; // this serves as checksum

  fun readFullChunk(chunkId: tChunkId): tReadWorkDone {
    return ReadLocalChunk(this, localTarget, chunkId, 0 /*offset*/, 1024*1024*1024 /*max chunk size*/, true /*readUncommitted*/);
  }

  fun postWriteCommitted(commitWorkDone: tCommitWorkDone) {
    var commitMsg: tCommitMsg;
    commitMsg = commitWorkDone.commitMsg;

    if (commitWorkDone.status == ErrorCode_SUCCESS ||
        commitWorkDone.status == ErrorCode_CHUNK_STALE_COMMIT ||
        (commitWorkDone.status == ErrorCode_CHUNK_NOT_FOUND && commitWorkDone.removeChunk)) {
      assert writeReq.removeChunk == commitWorkDone.removeChunk,
        format("{0}: chunk removed in commit but not in write request, commitWorkDone: {1}, writeReq: {2}", this, commitWorkDone, writeReq);
      if (chainInfo.isHeadTarget && writeReq.fromClient) {
        // send write response to client
        send writeReq.from, eWriteResp, (tag = commitMsg.tag, key = commitMsg.key,
          status = ErrorCode_SUCCESS, commitVer = commitWorkDone.commitVer);
      } else {
        // send commit message to predecessor
        commitMsg.retries = writeReq.retries;
        send writeReq.from, eCommitMsg, commitMsg;
      }
    } else {
      assert false, format("failed commit work: {0}", commitWorkDone);
    }

    // save the actual used commit chain version
    writeReq.commitChainVer = commitMsg.commitChainVer;
    send localService, eWriteReqCompleted, (request = writeReq, process = this, status = commitWorkDone.status);
  }

  fun getChainVer(): tChainVer {
    if (writeReq.commitChainVer != 0) {
      assert writeReq.fullChunkReplace == true;
      return writeReq.commitChainVer;
    } else {
      return chainInfo.vChainId.chainVer;
    }
  }

  start state Init {
    entry (args: (
        writeReq: tWriteReq,
        chainInfo: tLocalReplicaChainInfo,
        replicaChain: tReplicaChain,
        localTarget: StorageTarget,
        localService: StorageService,
        successorService: StorageService))
    {
      writeReq = args.writeReq;
      chainInfo = args.chainInfo;
      replicaChain = args.replicaChain;
      localTarget = args.localTarget;
      localService = args.localService;
      successorService = args.successorService;
      timer = new Timer(this);

      print format("{0}: start write process: {1}", this, writeReq);
      print format("{0}: remove chunk {1}? {2}, dummy write? {3}", this, writeReq.key.chunkId, writeReq.removeChunk, writeReq.length == 0);
      print format("{0}: replication chain: {1}", this, ReplicaChainToString(replicaChain));
      print format("{0}: local chain info: {1}", this, chainInfo);
      print format("{0}: local target: {1}, local service: {2}", this, localTarget, localService);

      if (writeReq.removeChunk) {
        goto UpdateLocalChunk;
      } else if (writeReq.length > 0) {
        goto UpdateLocalChunk;
      } else if (writeReq.length == 0) { // dummy write triggered by sync worker
        fromSyncWorker = true;
        goto ReadLocalFullChunk;
      }
    }
  }

  state UpdateLocalChunk {
    defer eShutDown, eRetryWriteReq;

    entry {
      var writeWork: tWriteWork;
      var writeWorkDone: tWriteWorkDone;
      var commitMsg: tCommitMsg;
      var commitWorkDone: tCommitWorkDone;

      writeWork = (
        from = this, tag = writeReq.tag,
        key = writeReq.key, targetId = chainInfo.localTargetId,
        updateVer = writeReq.updateVer, chainVer = getChainVer(),
        fullChunkReplace = writeReq.fullChunkReplace, removeChunk = writeReq.removeChunk,
        offset = writeReq.offset, length = writeReq.length, dataBytes = writeReq.dataBytes);

      send localTarget, eWriteWork, writeWork;

      receive {
        case eWriteWorkDone: (workDone: tWriteWorkDone) {
          writeWorkDone = workDone;
        }
      }

      if (writeWorkDone.status == ErrorCode_SUCCESS) {
        currentChunkContent = writeWorkDone.currentChunkContent;
        if (writeReq.updateVer == 0) {
          writeReq.updateVer = writeWorkDone.updateVer;
        } else {
          assert writeReq.updateVer == writeWorkDone.updateVer,
            format("{0}: writeReq.updateVer {1} != writeWorkDone.updateVer {2}, writeWorkDone: {3}",
              this, writeReq.updateVer, writeWorkDone.updateVer, writeWorkDone);
        }
      } else if (writeWorkDone.status == ErrorCode_CHUNK_MISSING_UPDATE) {
        // since one write request is running over the entire replication chain,
        // there should not exist any out-of-order update request along the chain
        assert false, format("{0}: write request blocked: {1}", this, writeWorkDone);
        return;
      } else if (writeWorkDone.status == ErrorCode_CHUNK_COMMITTED_UPDATE) {
        currentChunkContent = writeWorkDone.currentChunkContent;
        print format("{0}: the update already committed: {1}", this, writeWorkDone);
        commitMsg = (
          retries = writeReq.retries,
          tag = writeReq.tag,
          key = writeReq.key,
          status = ErrorCode_CHUNK_STALE_COMMIT,
          commitVer = writeReq.updateVer,
          commitChainVer = writeWorkDone.chainVer,
          currentChunkContent = currentChunkContent);
        commitWorkDone = (caller = default(machine), commitMsg = commitMsg,
          key = commitMsg.key, targetId = chainInfo.localTargetId, force = false, status = ErrorCode_SUCCESS,
          updateVer = commitMsg.commitVer, commitVer = commitMsg.commitVer, chainVer = commitMsg.commitChainVer, removeChunk = writeReq.removeChunk);
        postWriteCommitted(commitWorkDone);
        raise halt;
      } else if (writeWorkDone.status == ErrorCode_CHUNK_STALE_UPDATE) {
        currentChunkContent = writeWorkDone.currentChunkContent;
        print format("{0}: find a stale update: {1}", this, writeReq);
      } else {
        assert false, format("{0}: failed to write to local chunk #{1}: {2}", this, writeReq.key.chunkId, writeWorkDone);
        // send localService, eWriteReqCompleted, (request = writeReq, process = this, status = writeWorkDone.status);
        return;
      }

      if (chainInfo.successorTargetState == PublicTargetState_SYNCING) {
        goto ReadLocalFullChunk;
      } else {
        goto ForwardWriteRequest;
      }
    }
  }

  state ReadLocalFullChunk {
    entry {
      var readWorkDone: tReadWorkDone;

      if (writeReq.fullChunkReplace) {
        goto ForwardWriteRequest;
        return;
      }

      readWorkDone = readFullChunk(writeReq.key.chunkId);

      if (readWorkDone.status == ErrorCode_CHUNK_NOT_FOUND) {
        writeReq.removeChunk = true;
      } else if (readWorkDone.status == ErrorCode_SUCCESS) {
        assert writeReq.updateVer == 0 || readWorkDone.chunkMetadata.updateVer == writeReq.updateVer,
          format("{0}: chunk update version changed, readWorkDone: {1}, writeReq: {2}", this, readWorkDone, writeReq);
        writeReq.updateVer = readWorkDone.chunkMetadata.updateVer;
        if (fromSyncWorker) {
          print format("{0}: set commit chain version to: {1}, writeReq: {2}", this, readWorkDone.chunkMetadata.chainVer, writeReq);
          writeReq.commitChainVer = readWorkDone.chunkMetadata.chainVer;
        }
      } else {
        assert false, format("{0}: failed to read chunk: {1}", this, readWorkDone);
      }

      writeReq.fullChunkReplace = true;
      writeReq.offset = 0;
      writeReq.length = sizeof(readWorkDone.dataBytes);
      writeReq.dataBytes = readWorkDone.dataBytes;
      currentChunkContent = readWorkDone.dataBytes;

      goto ForwardWriteRequest;
    }
  }

  state ForwardWriteRequest {
    entry {
      var commitMsg: tCommitMsg;
      var forwardWrite: tWriteReq;
      var chainInfoResult: tGetLocalChainInfoResult;

      chainInfoResult = GetLocalChainInfo(this, localService, chainInfo.vChainId.chainId);

      if (chainInfoResult.chainInfo.status == ErrorCode_SUCCESS ||
          chainInfoResult.chainInfo.status == ErrorCode_TARGET_INACTIVE) {
        chainInfo = chainInfoResult.chainInfo;
        replicaChain = chainInfoResult.replicaChain;
        successorService = chainInfoResult.successorService;

        assert chainInfo.vChainId.chainVer >= writeReq.key.vChainId.chainVer;
        if (chainInfo.successorTargetState == PublicTargetState_SYNCING && !writeReq.fullChunkReplace) {
          goto ReadLocalFullChunk;
          return;
        } else if (writeReq.key.vChainId.chainVer == chainInfo.vChainId.chainVer && forwardRetries > 1) {
          print format("{0}: chain version is the same, skip retrying #{1}, chain info: {2}", this, forwardRetries, chainInfo.vChainId);
          StartTimer(timer);
          return;
        } else {
          writeReq.key.vChainId.chainVer = chainInfo.vChainId.chainVer;
        }
      } else {
        print format("{0}: cannot get local chain info, error: {1}, stopping", this, chainInfoResult.chainInfo.status);
        raise halt;
      }

      if (chainInfo.isTailTarget) {
        print format("{0}: chain {1}: commit write request: {2}", this, writeReq.key.vChainId, writeReq);
        commitMsg = (
          retries = writeReq.retries,
          tag = writeReq.tag,
          key = writeReq.key,
          status = ErrorCode_SUCCESS,
          commitVer = writeReq.updateVer,
          commitChainVer = getChainVer(),
          currentChunkContent = currentChunkContent);
        goto CommitLocalChunk, commitMsg;
      } else {
        forwardRetries = forwardRetries + 1;
        forwardWrite = writeReq;
        forwardWrite.from = this;
        forwardWrite.fromClient = false;
        forwardWrite.retries = forwardRetries;
        print format("{0}: chain {1}: forward write request #{2} to {3}: {4}", this, writeReq.key.vChainId, forwardRetries, successorService, forwardWrite);
        send successorService, eWriteReq, forwardWrite;
        StartTimer(timer);
      }
    }

    on eCommitMsg do (commitMsg: tCommitMsg) {
      if (commitMsg.status == ErrorCode_SUCCESS || commitMsg.status == ErrorCode_CHUNK_STALE_COMMIT) {
        if (commitMsg.retries != forwardRetries) {
          print format("{0}: retries not equal to {1}, commitMsg: {2}", this, forwardRetries, commitMsg);
          goto ForwardWriteRequest;
          return;
        } else if (commitMsg.commitChainVer <= chainInfo.vChainId.chainVer) {
          assert currentChunkContent == commitMsg.currentChunkContent, format("local chunk content {0} not equal to the successor's: {1}", currentChunkContent, commitMsg);
          goto CommitLocalChunk, commitMsg;
          return;
        } else {
          print format("{0}: reject commit message with chain version {1} > {2}",
            this, commitMsg.key.vChainId, chainInfo.vChainId);
          goto ForwardWriteRequest;
          return;
        }
      } else if (commitMsg.status == ErrorCode_CHAIN_VERION_MISMATCH) {
        print format("{0}: chain version mismatch: {1}", this, commitMsg);
        writeReq.key.vChainId.chainVer = 0;
        goto ForwardWriteRequest;
        return;
      } else {
        assert false, format("{0}: failed commit message: {1}", this, commitMsg);
      }

      send localService, eWriteReqCompleted, (request = writeReq, process = this, status = commitMsg.status);
    }

    on eTimeOut goto ForwardWriteRequest;

    on eShutDown do (from: machine) {
      // send from, eProcssStopped, this;
      raise halt;
    }
  }

  state CommitLocalChunk {
    ignore eRetryWriteReq, eCommitMsg, eTimeOut;

    entry (commitMsg: tCommitMsg) {
      send localTarget, eCommitWork, (from = this, commitMsg = commitMsg, key = commitMsg.key,
        targetId = chainInfo.localTargetId, commitVer = commitMsg.commitVer, commitChainVer = commitMsg.commitChainVer, force = false);
      CancelTimer(timer);
    }

    on eCommitWorkDone do (commitWorkDone: tCommitWorkDone) {
      postWriteCommitted(commitWorkDone);
      raise halt;
    }

    on eShutDown do (from: machine) {
      // send from, eProcssStopped, this;
      raise halt;
    }
  }
}

machine StorageService
{
  var nodeId: tNodeId;
  var nextRequestId: tRequestId;
  var timer: Timer;
  var mgmtClient: MgmtClient;
  // internal state machines
  var localTargets: tLocalTargetMap;
  var syncWorkers: map[tVersionedChainId, SyncWorker];
  // configuration info from mgmt service
  var routingVer: tRoutingVer;
  var replicaChains: tReplicaChainMap; // current serving replication chains
  var storageServices: tStorageServiceMap; // this works like a connection pool, which is just for P code
  // calculated from localTargets and replicaChains
  var predecessorNodeIds: map[tChainId, tNodeId];
  var successorNodeIds: map[tChainId, tNodeId];
  var successorTargetIds: map[tChainId, tTargetId];
  var successorTargetStates: map[tChainId, tPublicTargetState];
  var localTargetIds: map[tChainId, tTargetId];
  var publicTargetStates: map[tTargetId, tPublicTargetState];
  var localTargetStates: map[tTargetId, tLocalTargetState];
  var uncommittedWriteRetryCountdown: int;
  // read/write processes
  var runningReadProcs: set[ReadProcess];
  var runningWriteProcs: map[tChunkId, map[tMessageTag, WriteProcess]];
  // in-memory state
  var runningWriteReqs: map[tChunkId, map[tMessageTag, tWriteReq]];
  var blockedWriteReqs: map[tChunkId, map[tMessageTag, tWriteReq]]; // pending writes queued on a busy chunk
  var committedWriteReqs: map[tMessageTag, (commitVer: tChunkVer, commitChainVer: tChainVer)];
  var restarting: bool;

  fun newMessageTag(): tMessageTag {
    nextRequestId = nextRequestId + 1;
    return (nodeId = nodeId, requestId = nextRequestId);
  }

  fun lookupLocalReplicaChain(chainId: tChainId): tLocalReplicaChainInfo {
    var chainInfo: tLocalReplicaChainInfo;
    var replicaChain: tReplicaChain;
    var targetId: tTargetId;

    if (!(chainId in replicaChains)) {
      print format("chain id {0} does not exist", chainId);
      chainInfo.status = ErrorCode_CHAIN_VERION_MISMATCH;
      return chainInfo;
    }

    replicaChain = replicaChains[chainId];
    chainInfo.vChainId = replicaChain.vChainId;
    chainInfo.localTargetId = localTargetIds[chainId];
    chainInfo.publicTargetState = publicTargetStates[localTargetIds[chainId]];

    if (chainId in predecessorNodeIds) {
      chainInfo.predecessorNodeId = predecessorNodeIds[chainId];
    } else {
      chainInfo.isHeadTarget = true;
    }

    if (chainId in successorNodeIds) {
      chainInfo.successorNodeId = successorNodeIds[chainId];
      chainInfo.successorTargetId = successorTargetIds[chainId];
      chainInfo.successorTargetState = successorTargetStates[chainId];
    } else {
      chainInfo.isTailTarget = true;
    }

    if (!(chainId in localTargetIds &&
          localTargetIds[chainId] in publicTargetStates &&
          IsActiveTargetState(publicTargetStates[localTargetIds[chainId]])))
    {
      print format("{0}: cannot find an active local target of chain {1}, targets: {2}, localTargetIds: {3}, publicTargetStates: {4}",
        this, replicaChain.vChainId, replicaChain.targets, localTargetIds, PublicTargetStatesToString(publicTargetStates));
      chainInfo.status = ErrorCode_TARGET_INACTIVE;
      return chainInfo;
    }

    chainInfo.status = ErrorCode_SUCCESS;
    return chainInfo;
  }

  fun createWriteProcess(writeReq: tWriteReq): tErrorCode {
    var chainInfo: tLocalReplicaChainInfo;
    var successorService: StorageService;

    if (!(writeReq.key.chunkId in runningWriteProcs)) {
      runningWriteProcs += (writeReq.key.chunkId, default(map[tMessageTag, WriteProcess]));
    }

    if (!(writeReq.key.chunkId in runningWriteReqs)) {
      runningWriteReqs += (writeReq.key.chunkId, default(map[tMessageTag, tWriteReq]));
    }

    // if (writeReq.tag in runningWriteReqs[writeReq.key.chunkId]) {
    //   print format("{0}: duplicate running write request, retries:{1}, tag:{2}, key:{3}, runningWriteProcs: {4}, runningWriteReqs: {5}",
    //     this, writeReq.retries, writeReq.tag, writeReq.key, runningWriteProcs[writeReq.key.chunkId], runningWriteReqs[writeReq.key.chunkId]);
    //   return ErrorCode_DUPLICATE_REQUEST;
    // }

    if (!(writeReq.key.chunkId in blockedWriteReqs)) {
      blockedWriteReqs += (writeReq.key.chunkId, default(map[tMessageTag, tWriteReq]));
    }

    // if (writeReq.tag in blockedWriteReqs[writeReq.key.chunkId]) {
    //   print format("{0}: duplicate blocked write request, retries:{1}, tag:{2}, key:{3}, blockedWriteReqs: {4}, runningWriteProcs: {5}, runningWriteReqs: {6}",
    //     this, writeReq.retries, writeReq.tag, writeReq.key, keys(blockedWriteReqs[writeReq.key.chunkId]), runningWriteProcs[writeReq.key.chunkId], runningWriteReqs[writeReq.key.chunkId]);
    //   return ErrorCode_DUPLICATE_REQUEST;
    // }

    if (sizeof(runningWriteProcs[writeReq.key.chunkId]) > 0) {
      print format("{0}: add blocked write request: <tag:{1}, key:{2}, updateVer:{3}>, runningWriteProcs: {4}, runningWriteReqs: {5}, blockedWriteReqs: {6}",
        this, writeReq.tag, writeReq.key, writeReq.updateVer, runningWriteProcs[writeReq.key.chunkId],
        keys(runningWriteReqs[writeReq.key.chunkId]), keys(blockedWriteReqs[writeReq.key.chunkId]));
      blockedWriteReqs[writeReq.key.chunkId][writeReq.tag] = writeReq;
      return ErrorCode_CHUNK_BUSY;
    }

    chainInfo = lookupLocalReplicaChain(writeReq.key.vChainId.chainId);

    if (chainInfo.status != ErrorCode_SUCCESS || writeReq.key.vChainId != chainInfo.vChainId) {
      print format("{0}: chain version mismatch, vChainId: {1}, writeReq: {2}", this, chainInfo.vChainId, writeReq);
      if (writeReq.fromClient) {
        send writeReq.from, eWriteResp, (tag = writeReq.tag, key = writeReq.key, status = ErrorCode_CHAIN_VERION_MISMATCH, commitVer = 0);
      } else {
        send writeReq.from, eCommitMsg, (retries = writeReq.retries, tag = writeReq.tag, key = writeReq.key, status = ErrorCode_CHAIN_VERION_MISMATCH, commitVer = 0, commitChainVer = 0, currentChunkContent = default(tBytes));
      }
      return ErrorCode_CHAIN_VERION_MISMATCH;
    }

    if (chainInfo.successorNodeId in storageServices) {
      successorService = storageServices[chainInfo.successorNodeId];
    }

    // if (writeReq.tag in committedWriteReqs) {
    //   print format("{0}: duplicate committed write request, retries:{1}, tag:{2}, key:{3}, commitVer: {4}",
    //     this, writeReq.retries, writeReq.tag, writeReq.key, committedWriteReqs[writeReq.tag]);
    //   if (writeReq.fromClient) {
    //     send writeReq.from, eWriteResp, (tag = writeReq.tag, key = writeReq.key, status = ErrorCode_SUCCESS,
    //       commitVer = committedWriteReqs[writeReq.tag].commitVer);
    //   } else {
    //     send writeReq.from, eCommitMsg, (tag = writeReq.tag, key = writeReq.key, status = ErrorCode_SUCCESS,
    //       commitVer = committedWriteReqs[writeReq.tag].commitVer, commitChainVer = committedWriteReqs[writeReq.tag].commitChainVer, currentChunkContent = default(tBytes));
    //   }
    //   return ErrorCode_DUPLICATE_REQUEST;
    // }

    runningWriteReqs[writeReq.key.chunkId][writeReq.tag] = writeReq;
    runningWriteProcs[writeReq.key.chunkId][writeReq.tag] = new WriteProcess((
      writeReq = writeReq,
      chainInfo = chainInfo,
      replicaChain = replicaChains[chainInfo.vChainId.chainId],
      localTarget = localTargets[chainInfo.localTargetId],
      localService = this,
      successorService = successorService));
    return ErrorCode_SUCCESS;
  }

  fun hasRunningWriteReqsWithChainIdNotEqualTo(vChainId: tVersionedChainId): bool {
    var chunkId: tChunkId;
    var writeReq: tWriteReq;
    var successorService: StorageService;

    foreach (chunkId in keys(runningWriteReqs)) {
      foreach (writeReq in values(runningWriteReqs[chunkId])) {
        if (writeReq.key.vChainId.chainId == vChainId.chainId && writeReq.key.vChainId.chainVer != vChainId.chainVer) {
          print format("{0}: found running write request {1} with chain id {2} not equal to {3}: {4}",
            this, runningWriteProcs[chunkId], writeReq.key.vChainId, vChainId, writeReq);
          return true;
        }
      }
    }

    return false;
  }

  fun startSyncWorker(chainId: tChainId) {
    var vChainId: tVersionedChainId;
    var chainInfo: tLocalReplicaChainInfo;

    chainInfo = lookupLocalReplicaChain(chainId);

    if (chainInfo.status != ErrorCode_SUCCESS) {
      return;
    }

    foreach (vChainId in keys(syncWorkers)) {
      if (vChainId.chainId == chainInfo.vChainId.chainId && vChainId.chainVer != chainInfo.vChainId.chainVer) {
        print format("shutdown old sync worker {0}: chain {1} local target {2}",
          syncWorkers[vChainId], vChainId, chainInfo.localTargetId);
        send syncWorkers[vChainId], halt;
        syncWorkers -= (vChainId);
      }
    }

    if (chainInfo.successorTargetState == PublicTargetState_SYNCING) {
      if (!(chainInfo.vChainId in syncWorkers)) {
        syncWorkers[chainInfo.vChainId] = new SyncWorker((
          nodeId = chainInfo.vChainId.chainId * 10000 + chainInfo.vChainId.chainVer,
          vChainId = chainInfo.vChainId,
          localTarget = localTargets[chainInfo.localTargetId],
          localService = this,
          successorTargetId = chainInfo.successorTargetId,
          successorService = storageServices[chainInfo.successorNodeId]));
        print format("create new sync worker {0}: chain {1} local target {2}, successor target {3}",
          syncWorkers[chainInfo.vChainId], chainInfo.vChainId, chainInfo.localTargetId, chainInfo.successorTargetId);
      }
    }
  }

  fun startBlockedWriteReqs(chunkId: tChunkId, updateVer: tChunkVer) {
    var blockedReq: tWriteReq;

    if (!(chunkId in blockedWriteReqs)) {
      print format("{0}: no blocked write request on chunk {1}", this, chunkId);
      return;
    }

    foreach (blockedReq in values(blockedWriteReqs[chunkId])) {
      print format("{0}: blocked write request: {1}", this, blockedReq);
    }

    foreach (blockedReq in values(blockedWriteReqs[chunkId])) {
      blockedWriteReqs[chunkId] -= (blockedReq.tag);
      if (createWriteProcess(blockedReq) == ErrorCode_SUCCESS) {
        print format("{0}: restart blocked write request: {1}", this, blockedReq);
        return;
      }
    }
  }

  fun retryRunningWriteReqs(chainId: tChainId) {
    var chunkId: tChunkId;
    var writeReq: tWriteReq;
    var chainInfo: tLocalReplicaChainInfo;
    var currentWriteReqs: map[tChunkId, map[tMessageTag, tWriteReq]];
    var retryWrite: tRetryWriteReq;
    var successorService: StorageService;

    chainInfo = lookupLocalReplicaChain(chainId);

    if (chainInfo.status != ErrorCode_SUCCESS) {
      print format("{0}: look up chain error {1}, skip retrying running write requests", this, chainInfo.status);
      return;
    }

    if (chainInfo.successorNodeId in storageServices) {
      successorService = storageServices[chainInfo.successorNodeId];
    }

    print format("{0}: retrying running writes on chain {1}", this, ReplicaChainToString(replicaChains[chainInfo.vChainId.chainId]));

    foreach (chunkId in keys(runningWriteReqs)) {
      foreach (writeReq in values(runningWriteReqs[chunkId])) {
        if (writeReq.key.vChainId.chainId == chainId) {
          print format("{0}: retrying running write, process: {1}, request: {2}",
            this, runningWriteProcs[chunkId][writeReq.tag], runningWriteReqs[chunkId][writeReq.tag]);
          assert writeReq.tag in runningWriteProcs[chunkId],
            format("{0}: write request {1} not in runningWriteProcs[chunkId] {2}", this, writeReq, runningWriteProcs[chunkId]);

          retryWrite = (
            chainInfo = chainInfo,
            replicaChain = replicaChains[chainInfo.vChainId.chainId],
            localTarget = localTargets[chainInfo.localTargetId],
            localService = this,
            successorService = successorService);
          send runningWriteProcs[chunkId][writeReq.tag], eRetryWriteReq, retryWrite;
        }
      }
    }
  }

  fun retryAllRunningWriteReqs() {
    var chainId: tChainId;

    print format("{0}: retry all running write requests: {1}", this, runningWriteReqs);

    foreach (chainId in keys(replicaChains)) {
      retryRunningWriteReqs(chainId);
    }
  }

  fun commitChunksOnLastServingTarget(routingInfo: tRoutingInfo) {
    var chainId: tChainId;
    var vChainId: tVersionedChainId;
    var chunkId: tChunkId;
    var targetId: tTargetId;
    var commitVer: tChunkVer;
    var targetSyncInfo: tTargetSyncInfo;
    var chunkMetadata: tChunkMetadata;

    foreach (chainId in keys(routingInfo.replicaChains)) {
      vChainId = routingInfo.replicaChains[chainId].vChainId;

      foreach (targetId in routingInfo.replicaChains[chainId].targets) {
        if (targetId in localTargets && routingInfo.replicaChains[chainId].states[targetId] == PublicTargetState_LASTSRV) {
          print format("{0}: commit chunks on target {1} of chain {2}", this, targetId, vChainId);
          targetSyncInfo = GetTargetSyncInfo(this, localTargets[targetId]);

          foreach (chunkId in keys(targetSyncInfo.chunkStoreSnapshot)) {
            chunkMetadata = targetSyncInfo.chunkStoreSnapshot[chunkId].metadata;

            if (chunkMetadata.chunkState == ChunkState_COMMIT) {
              assert chunkMetadata.updateVer == chunkMetadata.commitVer, format("{0}: invalid committed chunk state: {1}", this, chunkMetadata);
              continue;
            } else {
              assert chunkMetadata.updateVer != chunkMetadata.commitVer, format("{0}: invalid uncommitted chunk state: {1}", this, chunkMetadata);
            }

            if (chunkMetadata.chunkState == ChunkState_DIRTY) {
              print format("{0}: [warning] force commit dirty chunk {1} on last-serving target {2} of {3}: {4}", this, chunkId, targetId, vChainId, chunkMetadata);
            } else {
              print format("{0}: [info] commit chunk {1} on last-serving target {2} of {3}: {4}", this, chunkId, targetId, vChainId, chunkMetadata);
            }

            send localTargets[targetId], eCommitWork, (from = this, commitMsg = default(tCommitMsg),
              key = (vChainId = vChainId, chunkId = chunkId),
              targetId = targetId, commitVer = chunkMetadata.updateVer, commitChainVer = vChainId.chainVer, force = true);
            receive {
              case eShutDown: (from: machine) {
                print format("{0}: node {1} is going to shutdown during restart", this, nodeId);
                goto Offline;
              }
              case eCommitWorkDone: (workDone: tCommitWorkDone) {
                print format("{0}: chunk committed: {1}", this, workDone);
              }
            }
          }
        }
      }
    }
  }

  fun getNodeIdOrDefaultValue(chainId: tChainId, nodeIds: map[tChainId, tNodeId], defaultVal: tNodeId): tNodeId {
    if (chainId in nodeIds)
      return nodeIds[chainId];
    return defaultVal;
  }

  fun processRoutingInfo(routingInfo: tRoutingInfo) {
    var newRoutingVer: tRoutingVer;
    var newReplicaChains: tReplicaChainMap;
    var newSuccessorNodeIds: map[tChainId, tNodeId];
    var replicaChain: tReplicaChain;
    var localTargetId: tTargetId;
    var localState: tLocalTargetState;
    var publicState: tPublicTargetState;
    var successorTargetId: tTargetId;
    var predecessorTargetId: tTargetId;
    var vChainId: tVersionedChainId;
    var chainId: tChainId;
    var nodeId: tNodeId;
    var newSuccessorNodeId: tNodeId;
    var oldSuccessorNodeId: tNodeId;
    var targetIdx: int;

    newRoutingVer = routingInfo.routingVer;
    newReplicaChains = routingInfo.replicaChains;

    if (routingVer > newRoutingVer) {
      print format("{0}: error: routingVer {1} > newRoutingVer {2}", this, routingVer, newRoutingVer);
      return;
    } else if (routingVer == newRoutingVer) {
      print format("{0}: ignore: routingVer {1} == newRoutingVer {2}", this, routingVer, newRoutingVer);
      return;
    }

    print format("{0}: updating replica chains from version {1} to {2}", this, routingVer, newRoutingVer);
    routingVer = newRoutingVer;
    storageServices = routingInfo.storageServices;

    foreach (chainId in keys(replicaChains)) {
      if (!(chainId in newReplicaChains)) {
        replicaChains         -= (chainId);
        predecessorNodeIds    -= (chainId);
        successorNodeIds      -= (chainId);
        successorTargetIds    -= (chainId);
        successorTargetStates -= (chainId);
        if (chainId in localTargetIds) {
          publicTargetStates    -= (localTargetIds[chainId]);
          localTargetIds        -= (chainId);
        }
      }
    }

    foreach (chainId in keys(newReplicaChains)) {
      if (chainId in replicaChains) {
        assert replicaChains[chainId].vChainId.chainVer <= newReplicaChains[chainId].vChainId.chainVer,
          format("error: local replica chain version {0} > {1}",
            replicaChains[chainId].vChainId.chainVer, newReplicaChains[chainId].vChainId.chainVer);

        if (newReplicaChains[chainId].vChainId == replicaChains[chainId].vChainId) {
          print format("{0}: chain version not changed, skipping {1}", this, replicaChains[chainId].vChainId);
          continue;
        }
      }

      replicaChain = newReplicaChains[chainId];
      replicaChains[chainId] = replicaChain;

      print format("{0}: new replica chain: {1}", this, ReplicaChainToString(replicaChain));

      targetIdx = 0;
      while (targetIdx < sizeof(replicaChain.targets)) {
        localTargetId = replicaChain.targets[targetIdx];

        if (localTargetId in localTargets)
        {
          localState = localTargetStates[localTargetId];
          publicState = replicaChain.states[localTargetId];
          localTargetIds[chainId] = localTargetId;
          publicTargetStates[localTargetId] = publicState;

          print format("{0}: local target #{1} {2} with local state {3} & public state {4} on chain {5}",
            this, targetIdx, localTargetId, LocalTargetStateToString(localState), PublicTargetStateToString(publicState to int), replicaChain.vChainId);

          if (IsActiveTargetState(publicState)) {
            if (targetIdx > 0) {
              predecessorTargetId = replicaChain.targets[targetIdx - 1];
              nodeId = replicaChain.nodes[predecessorTargetId];
              print format("chain {0} predecessor target #{1} {2} node {3}", chainId, targetIdx - 1, predecessorTargetId, nodeId);
              predecessorNodeIds[chainId] = nodeId;
            } else {
              predecessorNodeIds -= (chainId);
            }

            newSuccessorNodeId = -1;
            oldSuccessorNodeId = getNodeIdOrDefaultValue(chainId, successorNodeIds, -1);

            if (targetIdx + 1 < sizeof(replicaChain.targets) &&
                IsActiveTargetState(replicaChain.states[replicaChain.targets[targetIdx + 1]]))
            {
              successorTargetId = replicaChain.targets[targetIdx + 1];
              nodeId = replicaChain.nodes[successorTargetId];
              print format("chain {0} successor target #{1} {2} node {3}", chainId, targetIdx + 1, successorTargetId, nodeId);
              successorNodeIds[chainId] = nodeId;
              successorTargetIds[chainId] = successorTargetId;
              successorTargetStates[chainId] = replicaChain.states[successorTargetId];
              newSuccessorNodeId = nodeId;
            } else {
              successorNodeIds -= (chainId);
              successorTargetIds -= (chainId);
              successorTargetStates -= (chainId);
            }

            if (newSuccessorNodeId != oldSuccessorNodeId) {
              print format("{0}: chain {1} target #{2} {3} 's successor node is changed from {4} to {5}, public state: {6}, sizeof(runningWriteReqs): {7}",
                this, replicaChain.vChainId, targetIdx, localTargetId, oldSuccessorNodeId, newSuccessorNodeId, PublicTargetStateToString(publicState to int), sizeof(runningWriteReqs));

              // retryRunningWriteReqs(chainId);
            }
          }
        }

        targetIdx = targetIdx + 1;
      }
    }
  }

  fun updateLocalTargetStates() {
    var targetId: tTargetId;
    var localState: tLocalTargetState;
    // var targetStates: tLocalTargetStateMap;
    var publicState: tPublicTargetState;

    foreach (targetId in keys(localTargets)) {
      localState = localTargetStates[targetId];
      // targetState = GetTargetState(this, localTargets[targetId]);
      // targetStates += (targetId, targetState);

      if (targetId in publicTargetStates) {
        publicState = publicTargetStates[targetId];
      }

      if ((localState == LocalTargetState_ONLINE && !restarting &&
           (publicState == PublicTargetState_OFFLINE ||
            publicState == PublicTargetState_LASTSRV)) ||
          (localState == LocalTargetState_UPTODATE &&
           (publicState == PublicTargetState_OFFLINE ||
            publicState == PublicTargetState_LASTSRV ||
            publicState == PublicTargetState_WAITING)))
      {
        print format("{0}: move to offline state (shutdown), local target: {1}, local state: {2}, public state: {3}",
          this, targetId, LocalTargetStateToString(localState), PublicTargetStateToString(publicState to int));
        goto Offline;
        return;
        // targetStates = default(tLocalTargetStateMap);
        // return default(tLocalTargetStateMap);
      }
      else if (localState == LocalTargetState_ONLINE && publicState == PublicTargetState_SERVING)
      {
        print format("{0}: move to up-to-date state, local target: {1}, local state: {2}, public state: {3}",
          this, targetId, LocalTargetStateToString(localState), PublicTargetStateToString(publicState to int));
        // SetTargetState(this, localTargets[targetId], LocalTargetState_UPTODATE);
        localTargetStates[targetId] = LocalTargetState_UPTODATE;
      }
    }

    // return localTargetStates;
  }

  fun reportTargetStates(heartbeatConns: tHeartbeatConns) {
    var targetStates: tLocalTargetStateMap;
    var updateTargetStateMsg: tUpdateTargetStateMsg;

    updateLocalTargetStates();

    updateTargetStateMsg = (
      from = heartbeatConns.mgmtClient,
      tag = newMessageTag(),
      routingVer = routingVer,
      nodeId = nodeId,
      targetStates = localTargetStates,
      localTargets = localTargets,
      storageService = this);

    send heartbeatConns.mgmtService, eUpdateTargetStateMsg, updateTargetStateMsg;
  }

  start state Init {
    defer eReadReq, eWriteReq, eUpdateMsg, eCommitMsg, eShutDown, eRestart;

    entry (args: (
          nodeId: tNodeId,
          localTargets: tLocalTargetMap,
          mgmtService: MgmtService)) {
      var targetId: tTargetId;

      nodeId = args.nodeId;
      localTargets = args.localTargets;

      foreach (targetId in keys(localTargets)) {
        localTargetStates += (targetId, LocalTargetState_UPTODATE);
      }

      timer = new Timer(this);
      mgmtClient = new MgmtClient((nodeId = nodeId, clientHost = this, mgmtService = args.mgmtService, sendHeartbeats = true));
    }

    on eSendHeartbeat do reportTargetStates;

    on eNewRoutingInfo goto WaitForRequests with processRoutingInfo;
  }

  state WaitForRequests {
    ignore eRestart;

    entry {
      restarting = false;
    }

    // entry {
    //   StartTimer(timer);
    // }

    // on eTimeOut do {
    //   var chainId: tChainId;

    //   foreach (chainId in keys(replicaChains)) {
    //     startSyncWorker(chainId);
    //   }

    //   StartTimer(timer);
    // }

    on eSendHeartbeat do reportTargetStates;

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      var chainId: tChainId;

      processRoutingInfo(routingInfo);

      updateLocalTargetStates();

      foreach (chainId in keys(replicaChains)) {
        startSyncWorker(chainId);
      }
    }

    on eShutDown goto Offline with (from: machine) {
      print format("{0}: node {1} is going to shutdown", this, nodeId);
    }

    on eSyncStartReq do (syncStartReq: tSyncStartReq) {
      var chainInfo: tLocalReplicaChainInfo;
      var syncInfo: tTargetSyncInfo;
      var targetState: tLocalTargetState;

      chainInfo = lookupLocalReplicaChain(syncStartReq.vChainId.chainId);

      if (chainInfo.status != ErrorCode_SUCCESS || chainInfo.vChainId != syncStartReq.vChainId) {
        send syncStartReq.from, eSyncStartResp, (status = ErrorCode_CHAIN_VERION_MISMATCH, syncInfo = default(tTargetSyncInfo));
        return;
      }

      // targetState = GetTargetState(this, localTargets[syncStartReq.targetId]);

      // if (localTargetStates[syncStartReq.targetId] == LocalTargetState_UPTODATE) {
      //   send syncStartReq.from, eSyncStartResp, (status = ErrorCode_TARGET_ALREADY_UPTODATE, syncInfo = default(tTargetSyncInfo));
      //   return;
      // }

      syncInfo = GetTargetSyncInfo(this, localTargets[syncStartReq.targetId]);
      send syncStartReq.from, eSyncStartResp, (status = ErrorCode_SUCCESS, syncInfo = syncInfo);
    }

    on eSyncDoneReq do (syncDoneReq: tSyncDoneReq) {
      var chainInfo: tLocalReplicaChainInfo;

      chainInfo = lookupLocalReplicaChain(syncDoneReq.vChainId.chainId);

      if (chainInfo.status != ErrorCode_SUCCESS || chainInfo.vChainId != syncDoneReq.vChainId) {
        send syncDoneReq.from, eSyncDoneResp, (status = ErrorCode_CHAIN_VERION_MISMATCH, vChainId = syncDoneReq.vChainId, targetId = syncDoneReq.targetId);
        return;
      }

      print format("{0}: done syncing: {1}", this, syncDoneReq);

      localTargetStates[chainInfo.localTargetId] = LocalTargetState_UPTODATE;
      // SetTargetState(this, localTargets[chainInfo.localTargetId], LocalTargetState_UPTODATE);
      send syncDoneReq.from, eSyncDoneResp, (status = ErrorCode_SUCCESS, vChainId = syncDoneReq.vChainId, targetId = syncDoneReq.targetId);
    }

    on eGetLocalChainInfo do (args: (from: machine, chainId: tChainId)) {
      var chainInfo: tLocalReplicaChainInfo;
      var replicaChain: tReplicaChain;
      var successorService: StorageService;

      chainInfo = lookupLocalReplicaChain(args.chainId);

      if (chainInfo.vChainId.chainId in replicaChains) {
        replicaChain = replicaChains[chainInfo.vChainId.chainId];
      }

      if (chainInfo.successorNodeId in storageServices) {
        successorService = storageServices[chainInfo.successorNodeId];
      }

      send args.from, eGetLocalChainInfoResult, (chainInfo = chainInfo, replicaChain = replicaChain, successorService = successorService);
    }

    on eReadReq do (readReq: tReadReq) {
      var chainId: tChainId;
      var targetId: tTargetId;
      var chainInfo: tLocalReplicaChainInfo;

      chainInfo = lookupLocalReplicaChain(readReq.key.vChainId.chainId);

      if (chainInfo.status != ErrorCode_SUCCESS) {
        send readReq.from, eReadResp, (tag = readReq.tag, key = readReq.key, status = ErrorCode_CHAIN_VERION_MISMATCH,
          chunkMetadata = default(tChunkMetadata), dataBytes = default(tBytes));
        return;
      }

      runningReadProcs += (new ReadProcess((
        readReq = readReq,
        chainInfo = chainInfo,
        replicaChain = replicaChains[chainInfo.vChainId.chainId],
        localTarget = localTargets[chainInfo.localTargetId],
        localService = this)));
    }

    on eWriteReq do createWriteProcess;

    // on eUpdateMsg do (updateMsg: tUpdateMsg) {
    //   var writeReq: tWriteReq;
    //   var chainInfo: tLocalReplicaChainInfo;

    //   writeReq = updateMsg.writeReq;
    //   writeReq.predecessor = updateMsg.from;

    //   chainInfo = lookupLocalReplicaChain(writeReq.key.vChainId.chainId);

    //   if (chainInfo.status != ErrorCode_SUCCESS || writeReq.key.vChainId != chainInfo.vChainId) {
    //     print format("{0}: reject update request: {1}, chainInfo: {2}", this, updateMsg, chainInfo);
    //     return;
    //   }

    //   createWriteProcess(writeReq);
    // }

    on eWriteReqCompleted do (context: tWriteContext) {
      var writeReq: tWriteReq;
      var writeProcess: WriteProcess;
      var status : tErrorCode;

      writeReq = context.request;
      writeProcess = context.process;
      status = context.status;

      if (!(writeReq.key.chunkId in runningWriteProcs) ||
          !(writeReq.tag in runningWriteProcs[writeReq.key.chunkId]) ||
          writeProcess != runningWriteProcs[writeReq.key.chunkId][writeReq.tag])
      {
        print format("{0}: write process {1} has been removed from the running set",
          this, writeProcess);
        return;
      }

      if (status == ErrorCode_SUCCESS) {
        if (writeReq.tag in committedWriteReqs)
          print format("{0}: found write request in committedWriteReqs[tag:{1}] {2}", this, writeReq.tag, committedWriteReqs[writeReq.tag]);
        else
          committedWriteReqs += (writeReq.tag, (commitVer = writeReq.updateVer, commitChainVer = writeReq.commitChainVer));
      }

      print format("{0}: write completed, process: {1}, request: {2}", this, writeProcess, writeReq);

      runningWriteProcs[writeReq.key.chunkId] -= (writeReq.tag);
      runningWriteReqs[writeReq.key.chunkId] -= (writeReq.tag);

      startBlockedWriteReqs(writeReq.key.chunkId, writeReq.updateVer);
    }
  }

  state Offline {
    defer eSyncStartReq, eSyncDoneReq, eNewRoutingInfo;
    ignore eTimeOut, eSendHeartbeat, eShutDown,
      eReadReq, eWriteReq, eUpdateMsg,
      eWriteReqCompleted, eGetLocalChainInfo,
      eReadWorkDone, eWriteWorkDone, eCommitWorkDone;

    entry {
      var chunkId: tChunkId;
      var targetId: tTargetId;
      var syncWorker: SyncWorker;
      var readProcess: ReadProcess;
      var writeProcess: WriteProcess;

      restarting = false;
      CancelTimer(timer);

      foreach (readProcess in runningReadProcs) {
        send readProcess, halt;
      }

      foreach (chunkId in keys(runningWriteProcs)) {
        foreach (writeProcess in values(runningWriteProcs[chunkId])) {
          send writeProcess, halt;
        }
      }

      foreach (syncWorker in values(syncWorkers)) {
        send syncWorker, halt;
      }

      foreach (targetId in keys(localTargets)) {
        send localTargets[targetId], eShutDown, this;
      }

      send mgmtClient, eShutDown, this;

      syncWorkers       = default(map[tVersionedChainId, SyncWorker]);
      runningReadProcs  = default(set[ReadProcess]);
      runningWriteProcs = default(map[tChunkId, map[tMessageTag, WriteProcess]]);

      runningWriteReqs  = default(map[tChunkId, map[tMessageTag, tWriteReq]]);
      blockedWriteReqs  = default(map[tChunkId, map[tMessageTag, tWriteReq]]);
      committedWriteReqs = default(map[tMessageTag, (commitVer: tChunkVer, commitChainVer: tChainVer)]);

      routingVer      = default(tRoutingVer);
      replicaChains   = default(tReplicaChainMap);
      storageServices = default(tStorageServiceMap);

      predecessorNodeIds    = default(map[tChainId, tNodeId]);
      successorNodeIds      = default(map[tChainId, tNodeId]);
      successorTargetIds    = default(map[tChainId, tTargetId]);
      successorTargetStates = default(map[tChainId, tPublicTargetState]);
      localTargetIds        = default(map[tChainId, tTargetId]);
      publicTargetStates    = default(map[tTargetId, tPublicTargetState]);

      foreach (targetId in keys(localTargets)) {
        localTargetStates[targetId] = LocalTargetState_OFFLINE;
      }

      print format("{0} #{1} is offline", this, nodeId);
    }

    on eRestart goto WaitOfflineState with (from: machine) {
      var targetId: tTargetId;

      foreach (targetId in keys(localTargets)) {
        send localTargets[targetId], eRestart, this;
        receive {
          case eStartUp: (target: machine) {
            assert target == localTargets[targetId];
          }
        }
      }

      send mgmtClient, eRestart, this;
      // send from, eStarted, routingVer;

      print format("{0} #{1} is restarted by {2}", this, nodeId, from);
    }
  }

  state WaitOfflineState {
    defer eSyncStartReq, eSyncDoneReq;
    ignore eTimeOut, eSendHeartbeat, eRestart,
      eReadReq, eWriteReq, eUpdateMsg,
      eWriteReqCompleted, eGetLocalChainInfo,
      eReadWorkDone, eWriteWorkDone;

    on eShutDown goto Offline with (from: machine) {
      print format("{0}: node {1} is going to shutdown during restart", this, nodeId);
    }

    on eCommitWorkDone do (workDone: tCommitWorkDone) {
      print format("{0}: chunk committed: {1}", this, workDone);
    }

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      if (nodeId in routingInfo.offlineServices) {
        processRoutingInfo(routingInfo);
        commitChunksOnLastServingTarget(routingInfo);
        goto WaitOnlineState;
      } else {
        print format("{0}: current node {1} not offline in routing info: {2}", this, nodeId, routingInfo);
        goto WaitOfflineState;
      }
    }
  }

  state WaitOnlineState {
    defer eSyncStartReq, eSyncDoneReq,
      eReadReq, eWriteReq, eUpdateMsg;
    ignore eTimeOut, eRestart,
      eWriteReqCompleted, eGetLocalChainInfo,
      eReadWorkDone, eWriteWorkDone, eCommitWorkDone;

    entry {
      var targetId: tTargetId;
      foreach (targetId in keys(localTargets)) {
        localTargetStates[targetId] = LocalTargetState_ONLINE;
      }
      restarting = true;
    }

    on eShutDown goto Offline with (from: machine) {
      print format("{0}: node {1} is going to shutdown during restart", this, nodeId);
    }

    on eSendHeartbeat do reportTargetStates;

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      processRoutingInfo(routingInfo);
      if (nodeId in routingInfo.offlineServices) {
        print format("{0}: current node {1} still offline in routing info: {2}", this, nodeId, routingInfo);
        goto WaitOnlineState;
      } else {
        goto WaitForRequests;
      }
    }
  }
}

machine SyncWorker {
  var nodeId: tNodeId;
  var vChainId: tVersionedChainId;
  var localTarget: StorageTarget;
  var localService: StorageService;
  var successorTargetId: tTargetId;
  var successorService: StorageService;
  var localSyncInfo: tTargetSyncInfo;
  var remoteSyncInfo: tTargetSyncInfo;
  var nextRequestId: tRequestId;
  var timer: Timer;
  var syncInfoDelay: int;  // simulate a temporal delay between remote and local sync infos

  fun newMessageTag(): tMessageTag {
    nextRequestId = nextRequestId + 1;
    return (nodeId = nodeId, requestId = nextRequestId);
  }

  fun syncChunkData(chunkId: tChunkId): tErrorCode {
    var dummyWrite: tWriteReq;
    var status: tErrorCode;

    // create a dummy write to trigger a sync update for the chunk
    dummyWrite = (from = this,
      retries = 0,
      tag = newMessageTag(),
      key = (vChainId = vChainId, chunkId = chunkId),
      updateVer = 0,
      commitChainVer = 0,
      fullChunkReplace = false, /* a full chunk replace could be triggered in the write process */
      removeChunk = false,
      fromClient = false,
      offset = 0,
      length = 0,
      dataBytes = default(tBytes));
    send localService, eWriteReq, dummyWrite;

    receive {
      case eCommitMsg: (resp: tCommitMsg) {
        if (resp.status != ErrorCode_SUCCESS) {
          print format("error in sync chunk resp: {0}", resp);
        }

        assert resp.retries == 0, format("{0}: retries not equal to zero, commitMsg: {1}", this, resp);
        status = resp.status;
      }
    }

    return status;
  }

  start state Init {
    entry (args: (nodeId: tNodeId, vChainId: tVersionedChainId,
      localTarget: StorageTarget, localService: StorageService,
      successorTargetId: tTargetId, successorService: StorageService))
    {
      nodeId = args.nodeId;
      vChainId = args.vChainId;
      localTarget = args.localTarget;
      localService = args.localService;
      successorTargetId = args.successorTargetId;
      successorService = args.successorService;
      timer = new Timer(this);
      syncInfoDelay = 7;
      goto GetRemoteSyncInfo;
    }
  }

  state GetRemoteSyncInfo {
    entry {
      if (syncInfoDelay == 0) {
        send successorService, eSyncStartReq, (from = this, vChainId = vChainId, targetId = successorTargetId);
        receive {
          case eSyncStartResp: (syncStartResp: tSyncStartResp) {
            if (syncStartResp.status == ErrorCode_SUCCESS) {
              print format("{0}: start sync, chain {1}, localTarget {2}, localService {3}, successorService {4}",
                this, vChainId, localTarget, localService, successorService);
              remoteSyncInfo = syncStartResp.syncInfo;
              CancelTimer(timer);
              syncInfoDelay = 7; // restart count down
              goto GetLocalSyncInfo;
            } else {
              print format("{0}: failed to start sync, chain {1}, localTarget {2}, localService {3}, successorService {4}",
                this, vChainId, localTarget, localService, successorService);
            }
          }
        }
      } else {
        syncInfoDelay = syncInfoDelay - 1;
      }

      StartTimer(timer);
    }

    on eTimeOut goto GetRemoteSyncInfo;

    // on eSyncStartResp do (syncStartResp: tSyncStartResp) {
    //   if (syncStartResp.status != ErrorCode_SUCCESS) {
    //     print format("chain {0}: cannot start sync {1}", vChainId, syncStartResp);
    //   } else {
    //     remoteSyncInfo = syncStartResp.syncInfo;
    //     CancelTimer(timer);
    //     syncInfoDelay = 7; // restart count down
    //     goto GetLocalSyncInfo;
    //   }
    // }
  }

  state GetLocalSyncInfo {
    ignore eSyncStartResp;

    entry {
      if (syncInfoDelay == 0) {
        localSyncInfo = GetTargetSyncInfo(this, localTarget);
        goto SyncChunks;
      } else {
        syncInfoDelay = syncInfoDelay - 1;
        StartTimer(timer);
      }
    }

    on eTimeOut goto GetLocalSyncInfo;
  }

  state SyncChunks {
    ignore eTimeOut;

    entry {
      var chunkId: tChunkId;
      var status: tErrorCode;
      var localChunk: tChunkSnapshot;
      var remoteChunk: tChunkSnapshot;
      var localMetadata: tChunkMetadata;
      var remoteMetadata: tChunkMetadata;
      var chunksNeedSync: set[tChunkId];

      foreach (chunkId in keys(remoteSyncInfo.chunkStoreSnapshot)) {
        remoteChunk = remoteSyncInfo.chunkStoreSnapshot[chunkId];
        remoteMetadata = remoteChunk.metadata;

        if (!(chunkId in keys(localSyncInfo.chunkStoreSnapshot))) {
          print format("{0}: try to remove chunk {1} on peer target {2}", this, remoteMetadata, successorTargetId);
          chunksNeedSync += (chunkId);
          continue;
        }

        localChunk = localSyncInfo.chunkStoreSnapshot[chunkId];
        localMetadata = localChunk.metadata;

        if (localMetadata.chainVer > remoteMetadata.chainVer) {
          print format("{0}: sync local chunk {1} to peer target {2}, remote chunk: {3}",
            this, localMetadata, successorTargetId, remoteMetadata);
          chunksNeedSync += (chunkId);
        } else if (remoteMetadata.updateVer != remoteMetadata.commitVer) {
          assert remoteMetadata.chunkState != ChunkState_COMMIT, format("unexpected chunk state: {0}", remoteMetadata);
          print format("{0}: sync local chunk {1} to peer target {2}, uncommitted remote chunk: {3}",
            this, localMetadata, successorTargetId, remoteMetadata);
          chunksNeedSync += (chunkId);
        } else if (localMetadata.chainVer < remoteMetadata.chainVer) {
          if (localMetadata.chunkState == ChunkState_COMMIT)
            assert false, format("chainVer of localMetadata {0} less than chainVer of remoteMetadata {1}", localMetadata, remoteMetadata);
          else
            print format("{0}: local chunk being updated on stale chain version: {1}, localMetadata: {2}, remoteMetadata: {3}", this, vChainId, localMetadata, remoteMetadata);
        } else if (localMetadata.updateVer != remoteMetadata.commitVer) { // localMetadata.chainVer == remoteMetadata.chainVer
          if (localMetadata.chainVer != vChainId.chainVer && localMetadata.chunkState == ChunkState_COMMIT)
            assert false, format("updateVer of localMetadata {0} not equal to commitVer of remoteMetadata {1}", localMetadata, remoteMetadata);
          else
            print format("{0}: local chunk being updated on chain version: {1}, localMetadata: {2}, remoteMetadata: {3}", this, vChainId, localMetadata, remoteMetadata);
        } else if (localChunk.content != remoteChunk.content) {
          if (localMetadata.chainVer != vChainId.chainVer || remoteMetadata.chainVer != vChainId.chainVer)
            assert false, format("local chunk content not equal to remote content, localChunk: {0}, remoteChunk: {1}", localChunk, remoteChunk);
          else
            print format("{0}: local chunk being removed and updated on chain version: {1}, localChunk: {2}, remoteChunk: {3}", this, vChainId, localChunk, remoteChunk);
        } else {
          print format("skip syncing chunk {0}, localMetadata: {1}, remoteMetadata: {2}", chunkId, localMetadata, remoteMetadata);
        }
      }

      foreach (chunkId in keys(localSyncInfo.chunkStoreSnapshot)) {
        if (!(chunkId in remoteSyncInfo.chunkStoreSnapshot)) {
          localMetadata = localSyncInfo.chunkStoreSnapshot[chunkId].metadata;
          print format("{0}: sync local chunk {1} to peer target {2}, remote chunk: not found",
            this, localMetadata, successorTargetId);
          chunksNeedSync += (chunkId);
        }
      }

      foreach (chunkId in chunksNeedSync) {
        status = syncChunkData(chunkId);
        if (status != ErrorCode_SUCCESS) {
          print format("{0}: stop syncing, error: {1}", this, status);
          raise halt;
        }
      }

      goto SyncDone;
    }
  }

  state SyncDone {
    ignore eSyncStartResp;

    entry {
      print format("{0} sync loop stopped: chain {1} local target {2} successor {3}", this, vChainId, localTarget, successorService);
      send successorService, eSyncDoneReq, (from = this, vChainId = vChainId, targetId = successorTargetId);
      receive {
        case eSyncDoneResp: (syncDoneResp: tSyncDoneResp) {
          if (syncDoneResp.status == ErrorCode_SUCCESS) {
            print format("{0}: sync done: {1}", this, syncDoneResp);
          } else {
            print format("{0}: sync failed: {1}", this, syncDoneResp);
          }
          raise halt;
        }
      }
    }

    // on eTimeOut goto SyncDone;

    // on eSyncDoneResp do (syncDoneResp: tSyncDoneResp) {
    //   if (syncDoneResp.status == ErrorCode_SUCCESS) {
    //     print format("{0}: sync done: {1}", this, syncDoneResp);
    //   } else {
    //     print format("{0}: sync failed: {1}", this, syncDoneResp);
    //   }

    //   CancelTimer(timer);
    //   raise halt;
    // }
  }
}

// public interfaces of storage engine

enum tLocalTargetState {
  LocalTargetState_INVALID  = 0,   // invalid state
  LocalTargetState_UPTODATE = 1,  // ready to serve client requests
  LocalTargetState_ONLINE   = 2,  // waiting or syncing
  LocalTargetState_OFFLINE  = 4   // crashed or stopped
}

fun LocalTargetStateToString(x: tLocalTargetState): string {
  if (x == LocalTargetState_INVALID) return "INVALID";
  if (x == LocalTargetState_UPTODATE) return "UPTODATE";
  if (x == LocalTargetState_ONLINE) return "ONLINE";
  if (x == LocalTargetState_OFFLINE) return "OFFLINE";
  return format("UNKNOWN {0}", x);
}

fun LocalTargetStatesToString(targetStates: map[tTargetId, tLocalTargetState]): string {
  var targetId: tTargetId;
  var str: string;

  foreach (targetId in keys(targetStates)) {
    if (str != "") str = str + ", ";
    str = str + format("<{0}->{1}>", targetId, LocalTargetStateToString(targetStates[targetId]));
  }

  return str;
}

type tLocalTargetStateMap = map[tTargetId, tLocalTargetState];

type  tReadWork = (from: machine, chunkId: tChunkId, offset: int, length: int, readUncommitted: bool);
event eReadWork : tReadWork;

type  tReadWorkDone = (status: tErrorCode, targetId: tTargetId, chunkMetadata: tChunkMetadata, dataBytes: tBytes);
event eReadWorkDone :  tReadWorkDone;

type  tWriteWork = (from: machine, tag: tMessageTag, key: tGlobalKey, targetId: tTargetId, updateVer: tChunkVer, chainVer: tChainVer,
                    fullChunkReplace: bool, removeChunk: bool,
                    offset: int, length: int, dataBytes: tBytes);
event eWriteWork : tWriteWork;

type  tWriteWorkDone = (caller: machine, tag: tMessageTag, key: tGlobalKey, targetId: tTargetId,
                        status: tErrorCode, updateVer: tChunkVer, commitVer: tChunkVer, chainVer: tChainVer, currentChunkContent: tBytes);
event eWriteWorkDone : tWriteWorkDone;

type  tCommitWork = (from: machine, commitMsg: tCommitMsg, key: tGlobalKey, targetId: tTargetId, commitVer: tChunkVer, commitChainVer: tChainVer, force: bool);
event eCommitWork : tCommitWork;

type  tCommitWorkDone = (caller: machine, commitMsg: tCommitMsg, key: tGlobalKey, targetId: tTargetId, force: bool,
                         status: tErrorCode, updateVer: tChunkVer, commitVer: tChunkVer, chainVer: tChainVer, removeChunk: bool);
event eCommitWorkDone : tCommitWorkDone;

event eGetTargetState        : machine;
event eGetTargetStateResult  : tLocalTargetState;
type  tSetTargetState        = (from: machine, targetState: tLocalTargetState);
event eSetTargetState        : tSetTargetState;
event eSetTargetStateResult  : tLocalTargetState;

event eGetTargetSyncInfo        : machine;
event eGetTargetSyncInfoResult  : tTargetSyncInfo;

machine StorageTarget {
  var targetId: tTargetId;
  var chunkSize: int;
  var lastServeChainVer: tChainVer;
  var targetState: tLocalTargetState;
  var chunkStore: map[tChunkId, ChunkReplica];
  var localService: machine;

  fun getOrCreateChunkReplica(chunkId: tChunkId, createIfNotExists: bool): ChunkReplica {
    var chunkReplica: ChunkReplica;

    if (!(chunkId in chunkStore)) {
      if (!createIfNotExists) {
        return default(ChunkReplica);
      }

      chunkStore += (chunkId,
        new ChunkReplica((chunkId = chunkId, chunkSize = chunkSize)));
      print format("{0}: created a new chunk {1}: {2}", this, chunkId, chunkStore[chunkId]);
    }

    chunkReplica = chunkStore[chunkId];
    return chunkReplica;
  }

  fun removeAndHaltChunkReplica(chunkId: tChunkId) {
    var chunkReplica: ChunkReplica;

    chunkReplica = getOrCreateChunkReplica(chunkId, false);
    print format("{0}: remove chunk #{1} {2}", this, chunkId, chunkReplica);
    if (chunkReplica != default(ChunkReplica)) {
      chunkStore -= (chunkId);
      send chunkReplica, halt;
    }
  }

  fun processWriteFinishResult(writeFinishRes: tWriteOpFinishResult) {
    var writeWork: tWriteWork;
    var i: int;

    writeWork = writeFinishRes.writeWork;

    if (writeFinishRes.status != ErrorCode_SUCCESS) {
      send writeWork.from, eWriteWorkDone, (
        caller = writeWork.from, tag = writeWork.tag, key = writeWork.key, targetId = writeWork.targetId, status = writeFinishRes.status,
        updateVer = writeFinishRes.updateVer, commitVer = writeFinishRes.commitVer, chainVer = writeFinishRes.chainVer, currentChunkContent = writeFinishRes.currentChunkContent);
      return;
    }

    send writeWork.from, eWriteWorkDone, (
      caller = writeWork.from, tag = writeWork.tag, key = writeWork.key, targetId = writeWork.targetId, status = ErrorCode_SUCCESS,
      updateVer = writeFinishRes.updateVer, commitVer = writeFinishRes.commitVer, chainVer = writeFinishRes.chainVer, currentChunkContent = writeFinishRes.currentChunkContent);
  }

  fun processCommitOpResult(commitOpResult: tCommitOpResult) {
    var commitWork: tCommitWork;
    var chunkReplica: ChunkReplica;

    commitWork = commitOpResult.commitWork;

    if (commitOpResult.removeChunk &&
        (commitOpResult.status == ErrorCode_SUCCESS ||
          commitOpResult.status == ErrorCode_CHUNK_STALE_COMMIT))
    {
      removeAndHaltChunkReplica(commitWork.key.chunkId);
    }

    send commitWork.from, eCommitWorkDone, (caller = commitWork.from, commitMsg = commitWork.commitMsg,
      key = commitWork.key, targetId = commitWork.targetId, force = commitWork.force, status = commitOpResult.status,
      updateVer = commitOpResult.updateVer, commitVer = commitOpResult.commitVer, chainVer = commitOpResult.chainVer, removeChunk = commitOpResult.removeChunk);
  }

  start state Init {
    entry (args: (targetId: tTargetId, chunkSize: int)) {
      targetId = args.targetId;
      chunkSize = args.chunkSize;
      targetState = LocalTargetState_UPTODATE;
      goto WaitForWorks;
    }
  }

  state StartUp {
    entry {
      targetState = LocalTargetState_ONLINE;

      if (localService != default(machine))
        send localService, eStartUp, this;

      goto WaitForWorks;
    }
  }

  state WaitForWorks {
    on eShutDown goto Offline with (from: machine) {
      print format("{0} with target id {1} is going to offline", this, targetId);
    }

    on eGetTargetState do (from: machine) {
      send from, eGetTargetStateResult, targetState;
    }

    on eSetTargetState do (setTargetState: tSetTargetState) {
      targetState = setTargetState.targetState;
      send setTargetState.from, eSetTargetStateResult, targetState;
    }

    on eGetTargetSyncInfo do (from: machine) {
      var chunkId: tChunkId;
      var getChunkSnapshotRes: tGetChunkSnapshotResult;
      var chunkSnapshot: tChunkSnapshot;
      var chunkMetadata: tChunkMetadata;
      var chunkStoreSnapshot: map[tChunkId, tChunkSnapshot];

      foreach (chunkId in keys(chunkStore)) {
        send chunkStore[chunkId], eGetChunkSnapshot, (from = this, chunkId = chunkId);

        receive {
          case eGetChunkSnapshotResult: (result: tGetChunkSnapshotResult) {
            getChunkSnapshotRes = result;
          }
        }

        if (getChunkSnapshotRes.status == ErrorCode_SUCCESS) {
          chunkSnapshot = getChunkSnapshotRes.chunkSnapshot;
          chunkMetadata = chunkSnapshot.metadata;
          chunkStoreSnapshot += (chunkId, chunkSnapshot);

          if (chunkMetadata.updateVer != chunkMetadata.commitVer) {
            print format("found an uncommitted chunk {0}, metadata: {1}", chunkId, chunkMetadata);
            // if a normal write updates and commits a chunk during syncing, its chain version could be greater than the lastServeChainVer
            // so the following assertion is not applicable
            // assert lastServeChainVer >= chunkMetadata.chainVer, format("lastServeChainVer {0} < uncommitted chunk chainVer {1}", lastServeChainVer, chunkMetadata.chainVer);
          }
        }
      }

      send from, eGetTargetSyncInfoResult, (targetId = targetId, targetState = targetState, lastServeChainVer = lastServeChainVer,
        chunkStoreSnapshot = chunkStoreSnapshot);
    }

    on eReadWork do (readWork: tReadWork) {
      var chunkReplica: ChunkReplica;

      chunkReplica = getOrCreateChunkReplica(readWork.chunkId, false);

      if (chunkReplica == default(ChunkReplica)) {
        send readWork.from, eReadWorkDone, (status = ErrorCode_CHUNK_NOT_FOUND, targetId = targetId,
          chunkMetadata = default(tChunkMetadata), dataBytes = default(tBytes));
        return;
      }

      send chunkReplica, eReadOp, (from = this, readWork = readWork,
        chunkId = readWork.chunkId, offset = readWork.offset, length = readWork.length);
    }

    on eReadOpResult do (readOpRes: tReadOpResult) {
      send readOpRes.readWork.from, eReadWorkDone, (status = readOpRes.status, targetId = targetId,
      chunkMetadata = readOpRes.chunkMetadata, dataBytes = readOpRes.dataBytes);
    }

    on eWriteWork do (writeWork: tWriteWork) {
      var chunkReplica: ChunkReplica;

      if (targetState == LocalTargetState_UPTODATE) {
        if (lastServeChainVer < writeWork.key.vChainId.chainVer)
          lastServeChainVer = writeWork.key.vChainId.chainVer;
      }

      if (writeWork.removeChunk) {
        chunkReplica = getOrCreateChunkReplica(writeWork.key.chunkId, false);
        if (chunkReplica == default(ChunkReplica)) {
          send writeWork.from, eWriteWorkDone, (
            caller = writeWork.from, tag = writeWork.tag, key = writeWork.key, targetId = writeWork.targetId, status = ErrorCode_SUCCESS,
            updateVer = writeWork.updateVer, commitVer = 0, chainVer = writeWork.key.vChainId.chainVer, currentChunkContent = default(tBytes));
          return;
        }
      }

      chunkReplica = getOrCreateChunkReplica(writeWork.key.chunkId, true);
      assert chunkReplica != default(ChunkReplica);

      send chunkReplica, eWriteOpBegin, (from = this, writeWork = writeWork, tag = writeWork.tag, key = writeWork.key,
        updateVer = writeWork.updateVer, chainVer = writeWork.chainVer,
        fullChunkReplace = writeWork.fullChunkReplace, removeChunk = writeWork.removeChunk);
    }

    on eCommitWork do (commitWork: tCommitWork) {
      var chunkReplica: ChunkReplica;

      chunkReplica = getOrCreateChunkReplica(commitWork.key.chunkId, false);

      if (chunkReplica == default(ChunkReplica)) {
        send commitWork.from, eCommitWorkDone, (caller = commitWork.from, commitMsg = commitWork.commitMsg,
          key = commitWork.key, targetId = commitWork.targetId, force = commitWork.force, status = ErrorCode_CHUNK_NOT_FOUND,
          updateVer = commitWork.commitVer, commitVer = commitWork.commitVer, chainVer = commitWork.key.vChainId.chainVer, removeChunk = true);
        return;
      }

      send chunkReplica, eCommitOp, (from = this, commitWork = commitWork, key = commitWork.key, commitVer = commitWork.commitVer, commitChainVer = commitWork.commitChainVer, force = commitWork.force);
    }

    on eWriteOpFinishResult do processWriteFinishResult;

    on eCommitOpResult do processCommitOpResult;
  }

  state Offline {
    ignore eShutDown, eReadOpResult, eSetTargetState, eGetTargetSyncInfo;

    entry {
      var chunkId: tChunkId;

      print format("{0} with target id {1} is offline", this, targetId);
      targetState = LocalTargetState_OFFLINE;

      foreach (chunkId in keys(chunkStore)) {
        send chunkStore[chunkId], eShutDown, this;
      }
    }

    on eWriteOpFinishResult do processWriteFinishResult;

    on eCommitOpResult do processCommitOpResult;

    on eGetTargetState do (from: machine) {
      send from, eGetTargetStateResult, LocalTargetState_OFFLINE;
    }

    on eReadWork do (readWork: tReadWork) {
      send readWork.from, eReadWorkDone, (status = ErrorCode_TARGET_OFFLINE, targetId = targetId,
        chunkMetadata = default(tChunkMetadata), dataBytes = default(tBytes));
    }

    on eWriteWork do (writeWork: tWriteWork) {
      send writeWork.from, eWriteWorkDone, (
        caller = writeWork.from, tag = writeWork.tag, key = writeWork.key, targetId = writeWork.targetId,
        status = ErrorCode_TARGET_OFFLINE, updateVer = 0, commitVer = 0, chainVer = 0, currentChunkContent = default(tBytes));
    }

    on eCommitWork do (commitWork: tCommitWork) {
      send commitWork.from, eCommitWorkDone, (caller = commitWork.from, commitMsg = commitWork.commitMsg,
        key = commitWork.key, targetId = commitWork.targetId, force = commitWork.force,
        status = ErrorCode_TARGET_OFFLINE, updateVer = 0, commitVer = 0, chainVer = 0, removeChunk = false);
    }

    on eRestart goto StartUp with (from: machine) {
      var chunkId: tChunkId;
      var chunkReplica: ChunkReplica;

      foreach (chunkId in keys(chunkStore)) {
        chunkReplica = chunkStore[chunkId];
        send chunkReplica, eRestart, this;
      }

      localService = from;
      print format("{0} #{1} is restarted by {2}", this, targetId, from);
    }
  }
}

fun SetTargetState(caller: machine, storageTarget: StorageTarget, targetState: tLocalTargetState)
  : tLocalTargetState
{
  send storageTarget, eSetTargetState, (from = caller, targetState = targetState);
  receive {
    case eSetTargetStateResult: (result: tLocalTargetState) { targetState = result; }
  }

  return targetState;
}

fun GetTargetState(caller: machine, storageTarget: StorageTarget)
  : tLocalTargetState
{
  var targetState: tLocalTargetState;

  send storageTarget, eGetTargetState, caller;
  receive {
    case eGetTargetStateResult: (result: tLocalTargetState) { targetState = result; }
  }

  return targetState;
}

fun GetTargetSyncInfo(caller: machine, storageTarget: StorageTarget): tTargetSyncInfo {
  var syncInfo: tTargetSyncInfo;

  send storageTarget, eGetTargetSyncInfo, caller;
  receive {
    case eGetTargetSyncInfoResult: (result: tTargetSyncInfo) { syncInfo = result; }
  }

  return syncInfo;
}

type  tReadOp       = (from: machine, readWork: tReadWork, chunkId: tChunkId, offset: int, length: int);
type  tReadOpResult = (readWork: tReadWork, status: tErrorCode, chunkMetadata: tChunkMetadata, dataBytes: tBytes);
event eReadOp       : tReadOp;
event eReadOpResult : tReadOpResult;

type  tGetChunkSnapshot       = (from: machine, chunkId: tChunkId);
type  tGetChunkSnapshotResult = (status: tErrorCode, chunkSnapshot: tChunkSnapshot);
event eGetChunkSnapshot       : tGetChunkSnapshot;
event eGetChunkSnapshotResult : tGetChunkSnapshotResult;

type  tWriteOp            = (from: machine, writeWork: tWriteWork, key: tGlobalKey, offset: int, length: int, dataBytes: tBytes, fullChunkReplace: bool, removeChunk: bool);
type  tWriteOpBegin       = (from: machine, writeWork: tWriteWork, tag: tMessageTag, key: tGlobalKey, updateVer: tChunkVer, chainVer: tChainVer, fullChunkReplace: bool, removeChunk: bool);
event eWriteOpBegin       : tWriteOpBegin;

type  tWriteOpFinish       = (from: machine, writeWork: tWriteWork, key: tGlobalKey);
type  tWriteOpFinishResult = (writeWork: tWriteWork, status: tErrorCode, updateVer: tChunkVer, commitVer: tChunkVer, chainVer: tChainVer, currentChunkContent: tBytes);
event eWriteOpFinishResult : tWriteOpFinishResult;

type  tCommitOp       = (from: machine, commitWork: tCommitWork, key: tGlobalKey, commitVer: tChunkVer, commitChainVer: tChainVer, force: bool);
type  tCommitOpResult = (commitWork: tCommitWork, status: tErrorCode, updateVer: tChunkVer, commitVer: tChunkVer, chainVer: tChainVer, removeChunk: bool);
event eCommitOp       : tCommitOp;
event eCommitOpResult : tCommitOpResult;

enum tChunkState {
  ChunkState_DIRTY,
  ChunkState_CLEAN,
  ChunkState_COMMIT
}

type tChunkDebugInfo   = (lastWriteReqTag: tMessageTag);
type tChunkMetadata    = (chunkId: tChunkId, chunkSize: int,
                          chainVer: tChainVer, // the chain version on which the chunk is committed
                          commitVer: tChunkVer,
                          updateVer: tChunkVer,
                          chunkState: tChunkState,
                          debugInfo: tChunkDebugInfo);

fun CreateChunkMetadata(chunkId: tChunkId, chunkSize: int) : tChunkMetadata {
  var metadata: tChunkMetadata;
  metadata = (chunkId = chunkId, chunkSize = chunkSize,
              chainVer = 0,
              commitVer = 0,
              updateVer = 0,
              chunkState = ChunkState_CLEAN,
              debugInfo = default(tChunkDebugInfo));
  return metadata;
}

machine ChunkReplica {
  // on-disk chunk state
  var content: tBytes;  // it's a removed but not committed chunk if content equals to default(tBytes)
  var metadata: tChunkMetadata;

  fun processCommitOp(commitOp: tCommitOp) {
    var commitRes: tErrorCode;
    var removeChunk: bool;

    assert commitOp.force || metadata.updateVer >= commitOp.commitVer,
      format("metadata: {0}, commitOp: {1}", metadata, commitOp);
    print format("{0}: commit chunk: metadata {1}, content {2}", this, metadata, content);

    if (commitOp.force) {
      assert metadata.commitVer <= commitOp.commitVer && commitOp.commitVer <= metadata.updateVer;
      metadata.chunkState = ChunkState_CLEAN;
      metadata.commitVer = commitOp.commitVer;
      commitRes = ErrorCode_SUCCESS;
    } else if (metadata.chunkState == ChunkState_DIRTY) {
      commitRes = ErrorCode_CHUNK_NOT_CLEAN;
    } else if (metadata.commitVer < commitOp.commitVer) {
      metadata.commitVer = commitOp.commitVer;
      commitRes = ErrorCode_SUCCESS;
    } else {
      commitRes = ErrorCode_CHUNK_STALE_COMMIT;
    }

    if (metadata.commitVer == metadata.updateVer) {
      metadata.chunkState = ChunkState_COMMIT;
      metadata.chainVer = commitOp.commitChainVer;
    }

    if (content == default(tBytes))
      removeChunk = true;

    // since the client may expect commitVer to be monitonically increasing without gaps,
    // send commitOp.commitVer instead of metadata.commitVer as result,
    send commitOp.from, eCommitOpResult, (commitWork = commitOp.commitWork, status = commitRes,
      updateVer = metadata.updateVer, commitVer = commitOp.commitVer, chainVer = commitOp.key.vChainId.chainVer, removeChunk = removeChunk);

    if (removeChunk) {
      assert commitOp.commitVer == metadata.updateVer && metadata.chunkState == ChunkState_COMMIT,
        format("{0}: try to commit removed chunk but found uncommitted chunk state: {1}", this, metadata);
      print format("{0}: remove chunk committed: {1}", this, metadata);
      goto RemoveChunkCommitted;
    }
  }

  start state Init {
    entry (args: (chunkId: tChunkId, chunkSize: int)) {
      var i: int;

      while (i < args.chunkSize) {
        content += (i, 0);
        i = i + 1;
      }

      metadata = CreateChunkMetadata(args.chunkId, args.chunkSize);

      goto WaitForOps;
    }
  }

  state WaitForOps {

    on eShutDown goto Offline with (from: machine) {
      print format("{0} with chunk id {1} is going to offline", this, metadata.chunkId);
    }

    on eCommitOp do processCommitOp;

    on eReadOp do (readOp: tReadOp) {
      var dataBytes: tBytes;
      var i: int;

      if (!readOp.readWork.readUncommitted && metadata.updateVer != metadata.commitVer) {
        send readOp.from, eReadOpResult, (readWork = readOp.readWork, status = ErrorCode_CHUNK_NOT_COMMIT,
          chunkMetadata = metadata, dataBytes = default(tBytes));
        return;
      }

      i = 0;
      while (i < readOp.length && readOp.offset + i < sizeof(content)) {
        dataBytes += (i, content[readOp.offset + i]);
        i = i + 1;
      }

      send readOp.from, eReadOpResult,
        (readWork = readOp.readWork, status = ErrorCode_SUCCESS, chunkMetadata = metadata, dataBytes = dataBytes);
    }

    on eGetChunkSnapshot do (getSnapshot: tGetChunkSnapshot) {
      var chunkSnapshot: tChunkSnapshot;
      chunkSnapshot = (metadata = metadata, content = content);
      send getSnapshot.from, eGetChunkSnapshotResult, (status = ErrorCode_SUCCESS, chunkSnapshot = chunkSnapshot);
    }

    on eWriteOpBegin do (writeBegin: tWriteOpBegin) {
      var writeWork: tWriteWork;
      var writeOp: tWriteOp;

      writeWork = writeBegin.writeWork;

      print format("{0}: write begin: metadata {1}, content {2}", this, metadata, content);

      if (metadata.chunkState == ChunkState_DIRTY && !writeBegin.fullChunkReplace) {
        send writeBegin.from, eWriteOpFinishResult, (writeWork = writeWork, status = ErrorCode_CHUNK_NOT_CLEAN,
          updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = content);
        return;
      }

      if (writeBegin.key.vChainId.chainVer < metadata.chainVer) {
        send writeBegin.from, eWriteOpFinishResult, (writeWork = writeWork, status = ErrorCode_CHAIN_VERION_MISMATCH,
          updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = content);
        return;
      }

      if (writeBegin.updateVer > 0 || writeBegin.fullChunkReplace) {
        if (writeBegin.fullChunkReplace) {
          // full chunk update during syncing
          assert writeBegin.key.vChainId.chainVer >= metadata.chainVer;
          metadata.commitVer = Max(0, writeBegin.updateVer - 1);
        } else if (writeBegin.updateVer <= metadata.commitVer) {
          send writeBegin.from, eWriteOpFinishResult, (writeWork = writeWork, status = ErrorCode_CHUNK_COMMITTED_UPDATE,
            updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = content);
          return;
        } else if (writeBegin.updateVer <= metadata.updateVer) {
          send writeBegin.from, eWriteOpFinishResult, (writeWork = writeWork, status = ErrorCode_CHUNK_STALE_UPDATE,
            updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = content);
          return;
        } else if (writeBegin.updateVer == metadata.updateVer + 1) {
          // normal update
        } else {
          // writeBegin.updateVer > metadata.updateVer + 1
          send writeBegin.from, eWriteOpFinishResult, (writeWork = writeWork, status = ErrorCode_CHUNK_MISSING_UPDATE,
            updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = content);
          return;
        }

        metadata.updateVer = writeBegin.updateVer;
      } else {
        assert writeBegin.updateVer == 0;
        metadata.updateVer = metadata.updateVer + 1;
      }

      metadata.chunkState = ChunkState_DIRTY;
      metadata.chainVer = writeBegin.chainVer;
      metadata.debugInfo.lastWriteReqTag = writeBegin.tag;

      writeOp = (from = writeBegin.from, writeWork = writeWork, key = writeWork.key,
        offset = writeWork.offset, length = writeWork.length, dataBytes = writeWork.dataBytes,
        fullChunkReplace = writeWork.fullChunkReplace, removeChunk = writeWork.removeChunk);
      goto DoWrite, writeOp;
    }
  }

  state DoWrite {
    defer eReadOp, eGetChunkSnapshot;
    ignore eCommitOp, eWriteOpBegin;

    entry (writeOp: tWriteOp) {
      var i: int;
      var writeFinish: tWriteOpFinish;

      assert metadata.chunkState == ChunkState_DIRTY, format("chunk not in dirty state: {0}", metadata);
      assert writeOp.offset + writeOp.length <= metadata.chunkSize,
        format("write range condition failed: {0}", writeOp);

      print format("{0}: before write: metadata {1}, content {2}", this, metadata, content);

      // TODO: compare checksum of the chunk before changing the chunk content to ensure the update is not applied out-of-order

      if (writeOp.removeChunk) {
        content = default(tBytes);
      } else if (writeOp.fullChunkReplace) {
        assert (writeOp.offset == 0 && writeOp.length == metadata.chunkSize) || (writeOp.offset == 0 && writeOp.length == 0 && writeOp.dataBytes == default(tBytes)),
          format("full chunk replace condition failed: {0}", writeOp);
        content = writeOp.dataBytes;
      } else {
        assert writeOp.offset + writeOp.length <= sizeof(content),
          format("{0}: writeOp.offset:{1} + writeOp.length:{2} < sizeof(content):{3}", this, writeOp.offset, writeOp.length, sizeof(content));
        i = 0;
        while (i < writeOp.length) {
          content[writeOp.offset + i] = writeOp.dataBytes[i];
          i = i + 1;
        }
      }

      print format("{0}: after write: metadata {1}, content {2}", this, metadata, content);

      writeFinish = (from = writeOp.from, writeWork = writeOp.writeWork, key = writeOp.writeWork.key);
      goto FinishWrite, writeFinish;
    }

    on eShutDown goto Offline with (from: machine) {
      print format("{0} with chunk id {1} is going to offline", this, metadata.chunkId);
    }
  }

  state FinishWrite {
    defer eReadOp, eGetChunkSnapshot;
    ignore eCommitOp, eWriteOpBegin;

    entry (writeFinish: tWriteOpFinish) {
      assert metadata.chunkState == ChunkState_DIRTY;
      print format("{0}: write finish: metadata {1}, content {2}", this, metadata, content);

      metadata.chunkState = ChunkState_CLEAN;

      send writeFinish.from, eWriteOpFinishResult, (writeWork = writeFinish.writeWork, status = ErrorCode_SUCCESS,
        updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = content);
      goto WaitForOps;
    }

    on eShutDown goto Offline with (from: machine) {
      print format("{0} with chunk id {1} is going to offline", this, metadata.chunkId);
    }
  }

  state Offline {
    ignore eReadOp, eGetChunkSnapshot, eCommitOp, eWriteOpBegin, eShutDown;

    entry {
      print format("{0} with chunk id {1} is offline", this, metadata.chunkId);
    }

    on eRestart goto WaitForOps;
  }

  state RemoveChunkCommitted {
    ignore eShutDown, eRestart;

    on eCommitOp do (commitOp: tCommitOp) {
      send commitOp.from, eCommitOpResult, (commitWork = commitOp.commitWork, status = ErrorCode_CHUNK_NOT_FOUND,
        updateVer = metadata.updateVer, commitVer = commitOp.commitVer, chainVer = commitOp.key.vChainId.chainVer, removeChunk = true);
    }

    on eReadOp do (readOp: tReadOp) {
      send readOp.from, eReadOpResult, (readWork = readOp.readWork, status = ErrorCode_CHUNK_NOT_FOUND,
        chunkMetadata = metadata, dataBytes = default(tBytes));
    }

    on eGetChunkSnapshot do (getSnapshot: tGetChunkSnapshot) {
      var chunkSnapshot: tChunkSnapshot;
      chunkSnapshot = (metadata = default(tChunkMetadata), content = default(tBytes));
      send getSnapshot.from, eGetChunkSnapshotResult, (status = ErrorCode_CHUNK_NOT_FOUND, chunkSnapshot = chunkSnapshot);
    }

    on eWriteOpBegin do (writeBegin: tWriteOpBegin) {
      send writeBegin.from, eWriteOpFinishResult, (writeWork = writeBegin.writeWork, status = ErrorCode_CHUNK_NOT_FOUND,
        updateVer = metadata.updateVer, commitVer = metadata.commitVer, chainVer = metadata.chainVer, currentChunkContent = default(tBytes));
    }
  }
}
