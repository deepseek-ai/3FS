/* Storage Client */

type tWriteArgs      = (from: machine, chunkId: tChunkId, offset: int, length: int, dataBytes: tBytes);
type tReadArgs       = (from: machine, chunkId: tChunkId, offset: int, length: int);
type tWriteRes       = (status: tErrorCode, chunkId: tChunkId, commitVer: tChunkVer);
type tReadRes        = (status: tErrorCode, chunkId: tChunkId, chunkMetadata: tChunkMetadata, dataBytes: tBytes);

event eSubmitWrite   : tWriteArgs;
event eSubmitRead    : tReadArgs;
event eWriteComplete : tWriteRes;
event eReadComplete  : tReadRes;
event eWaitConnected : machine;
event eClientConnected;

machine StorageClient {
  var clientId: tNodeId;
  var mgmtService: MgmtService;
  var mgmtClient: MgmtClient;
  // var timer: Timer;

  var routingVer: tRoutingVer;
  var replicaChains: tReplicaChainMap;

  var nextRequestId: tRequestId;

  var clientUsers: set[machine];
  var submittedWrites: map[tMessageTag, tWriteArgs];
  var submittedReads: map[tMessageTag, tReadArgs];
  var inflightWriteReqs: map[tMessageTag, tWriteReq];
  var inflightReadReqs: map[tMessageTag, tReadReq];

  fun newMessageTag(): tMessageTag {
    nextRequestId = nextRequestId + 1;
    return (nodeId = clientId, requestId = nextRequestId);
  }

  fun calcGlobalKeyFromChunkId(chunkId: tChainId): tGlobalKey {
    var chainIds: seq[tChainId];
    var targetChain: tChainId;
    var replicaChain: tReplicaChain;

    chainIds = keys(replicaChains);
    targetChain = chainIds[chunkId % sizeof(chainIds)];
    replicaChain = replicaChains[targetChain];

    return (vChainId = replicaChain.vChainId, chunkId = chunkId);
  }

  fun processRoutingInfo(routingInfo: tRoutingInfo) {
    var newRoutingVer: tRoutingVer;
    var newReplicaChains: tReplicaChainMap;
    var replicaChain: tReplicaChain;
    var targetId: tTargetId;
    var chainId: tChainId;
    var nodeId: tNodeId;
    var services: seq[StorageService];

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

    foreach (chainId in keys(replicaChains)) {
      if (!(chainId in newReplicaChains))
        replicaChains -= (chainId);
    }

    foreach (chainId in keys(newReplicaChains)) {
      replicaChains[chainId] = newReplicaChains[chainId];
      replicaChain = replicaChains[chainId];

      print format("{0}: new replica chain {1}, targets: {2}, services: {3}",
        this, newReplicaChains[chainId].vChainId, newReplicaChains[chainId].targets, newReplicaChains[chainId].services);
    }
  }

  // fun onSendHeartbeatEvent(heartbeatConns: tHeartbeatConns) {
  //   send heartbeatConns.mgmtService, eRegisterClientMsg, (from = heartbeatConns.mgmtClient, nodeId = clientId, storageClient = this);
  // }

  fun chooseServingTarget(replicaChain: tReplicaChain): tTargetId {
    var targetId: tTargetId;
    var servingTargetIds: set[tTargetId];

    targetId = replicaChain.targets[0];

    if (replicaChain.states[targetId] == PublicTargetState_SERVING) {
      return targetId;
    }

    return 0;
  }

  fun sendWriteReq(writeReq: tWriteReq) {
    var replicaChain: tReplicaChain;
    var targetId: tTargetId;
    var targetService: StorageService;

    // get the latest chain and update versioned chain id
    replicaChain = replicaChains[writeReq.key.vChainId.chainId];
    writeReq.key.vChainId = replicaChain.vChainId;

    targetId = chooseServingTarget(replicaChain);
    if (targetId > 0) {
      print format("{0}: send write request #{1}: {2}", this, writeReq.retries, writeReq);
      targetService = replicaChain.services[targetId];
      send targetService, eWriteReq, writeReq;
    }
  }

  fun reissueWriteReq(reqTag: tMessageTag) {
    inflightWriteReqs[reqTag].retries = inflightWriteReqs[reqTag].retries + 1;
    sendWriteReq(inflightWriteReqs[reqTag]);
  }

  fun sendReadReq(readReq: tReadReq) {
    var replicaChain: tReplicaChain;
    var targetId: tTargetId;
    var targetService: StorageService;

    // get the latest chain and update versioned chain id
    replicaChain = replicaChains[readReq.key.vChainId.chainId];
    readReq.key.vChainId = replicaChain.vChainId;

    targetId = chooseServingTarget(replicaChain);
    if (targetId > 0) {
      print format("{0}: send read request #{1}: {2}", this, readReq.retries, readReq);
      targetService = replicaChain.services[targetId];
      send targetService, eReadReq, readReq;
    }
  }

  fun reissueReadReq(reqTag: tMessageTag) {
    sendReadReq(inflightReadReqs[reqTag]);
    inflightReadReqs[reqTag].retries = inflightReadReqs[reqTag].retries + 1;
  }

  fun processInflightWriteReqs() {
    var oldChainId: tVersionedChainId;
    var newChainId: tVersionedChainId;
    var writeReq: tWriteReq;

    foreach (writeReq in values(inflightWriteReqs)) {
      oldChainId = writeReq.key.vChainId;
      newChainId = replicaChains[oldChainId.chainId].vChainId;
      if (oldChainId != newChainId) {
        print format("{0}: chain version updated: {1} --> {2}, reissuing request {3}", this, oldChainId, newChainId, writeReq);
        reissueWriteReq(writeReq.tag);
      }
    }
  }

  fun processInflightReadReqs() {
    var oldChainId: tVersionedChainId;
    var newChainId: tVersionedChainId;
    var readReq: tReadReq;

    foreach (readReq in values(inflightReadReqs)) {
      oldChainId = readReq.key.vChainId;
      newChainId = replicaChains[oldChainId.chainId].vChainId;
      if (oldChainId != newChainId) {
        print format("{0}: chain version updated: {1} --> {2}, reissuing request {3}", this, oldChainId, newChainId, readReq);
        reissueReadReq(readReq.tag);
      }
    }
  }

  start state Init {
    ignore eSendHeartbeat;

    entry (args: (clientId: tNodeId, mgmtService: MgmtService)) {
      clientId = args.clientId;
      mgmtService = args.mgmtService;
      mgmtClient = new MgmtClient((nodeId = clientId, clientHost = this, mgmtService = mgmtService, sendHeartbeats = false));
      // timer = new Timer(this);
    }

    on eWaitConnected do (user: machine) {
      clientUsers += (user);
    }

    // on eSendHeartbeat do onSendHeartbeatEvent;

    on eNewRoutingInfo goto WaitForReqs with processRoutingInfo;
  }

  state WaitForReqs {
    ignore eSendHeartbeat;

    entry {
      var user: machine;
      foreach (user in clientUsers) {
        send user, eClientConnected;
      }
      // StartTimer(timer);
    }

    on eWaitConnected do (user: machine) {
      send user, eClientConnected;
    }

    on eShutDown goto Stopped with (from: machine) {
      print format("{0} is going to shutdown", this);
      send mgmtClient, eShutDown, this;
    }

    // on eSendHeartbeat do onSendHeartbeatEvent;

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      processRoutingInfo(routingInfo);
      processInflightWriteReqs();
      processInflightReadReqs();
    }

    // on eTimeOut do {
    //   processInflightWriteReqs();
    //   processInflightReadReqs();
    //   StartTimer(timer);
    // }

    on eSubmitWrite do (writeArgs: tWriteArgs) {
      var writeReq: tWriteReq;

      writeReq = (from = this,
        retries = 1,
        tag = newMessageTag(),
        key = calcGlobalKeyFromChunkId(writeArgs.chunkId),
        updateVer = 0,
        commitChainVer = 0,
        fullChunkReplace = false,
        removeChunk = writeArgs.dataBytes == default(tBytes),
        fromClient = true,
        offset = writeArgs.offset, length = writeArgs.length,
        dataBytes = writeArgs.dataBytes);

      sendWriteReq(writeReq);

      submittedWrites += (writeReq.tag, writeArgs);
      inflightWriteReqs += (writeReq.tag, writeReq);
    }

    on eSubmitRead do (readArgs: tReadArgs) {
      var readReq: tReadReq;

      readReq = (from = this,
        retries = 1,
        tag = newMessageTag(),
        key = calcGlobalKeyFromChunkId(readArgs.chunkId),
        offset = readArgs.offset, length = readArgs.length);

      sendReadReq(readReq);

      submittedReads += (readReq.tag, readArgs);
      inflightReadReqs += (readReq.tag, readReq);
    }

    on eWriteResp do (writeResp: tWriteResp) {
      if (!(writeResp.tag in inflightWriteReqs)) {
        print format("{0}: got response for completed write request: {1}", this, writeResp.key);
        return;
      }

      if (writeResp.status == ErrorCode_CHAIN_VERION_MISMATCH) {
        print format("{0}: retry write request: {1}", this, writeResp.key);
        reissueWriteReq(writeResp.tag);
        return;
      }

      print format("{0}: write response {1}", this, writeResp);

      send submittedWrites[writeResp.tag].from, eWriteComplete, (status = writeResp.status, chunkId = writeResp.key.chunkId, commitVer = writeResp.commitVer);

      submittedWrites -= (writeResp.tag);
      inflightWriteReqs -= (writeResp.tag);
    }

    on eReadResp do (readResp: tReadResp) {
      if (!(readResp.tag in inflightReadReqs)) {
        return;
      }

      if ((readResp.status == ErrorCode_CHAIN_VERION_MISMATCH) || (readResp.status == ErrorCode_CHUNK_NOT_COMMIT)) {
        print format("{0}: retry read request: {1}", this, readResp.key);
        reissueReadReq(readResp.tag);
        return;
      }

      print format("{0}: read response {1}", this, readResp);

      send submittedReads[readResp.tag].from, eReadComplete, (status = readResp.status, chunkId = readResp.key.chunkId,
        chunkMetadata = readResp.chunkMetadata, dataBytes = readResp.dataBytes);

      submittedReads -= (readResp.tag);
      inflightReadReqs -= (readResp.tag);
    }
  }

  state Stopped {
    ignore eReadResp, eWriteResp, eSendHeartbeat, eNewRoutingInfo, eTimeOut;

    entry {
      // CancelTimer(timer);
      print format("{0} stopped", this);
    }
  }
}
