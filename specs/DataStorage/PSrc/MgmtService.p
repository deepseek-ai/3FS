enum tPublicTargetState {
  PublicTargetState_INVALID = 0,  // invalid state
  PublicTargetState_SERVING = 1,  // online and serving client requests
  PublicTargetState_LASTSRV = 2,  // offline but it was the last serving target
  PublicTargetState_SYNCING = 4,  // online and syncing updates
  PublicTargetState_WAITING = 8,  // online and waiting to join the chain
  PublicTargetState_OFFLINE = 16  // crashed or stopped
}

fun IsActiveTargetState(targetState: tPublicTargetState): bool {
  return targetState == PublicTargetState_SERVING || targetState == PublicTargetState_SYNCING;
}

fun AllPublicTargetStates(): map[tPublicTargetState, string] {
  var states: map[tPublicTargetState, string];
  states += (PublicTargetState_SERVING, "SERVING");
  states += (PublicTargetState_LASTSRV, "LASTSRV");
  states += (PublicTargetState_SYNCING, "SYNCING");
  states += (PublicTargetState_WAITING, "WAITING");
  states += (PublicTargetState_OFFLINE, "OFFLINE");
  return states;
}

fun PublicTargetStateToString(x: int): string {
  var states: map[tPublicTargetState, string];  
  var s: tPublicTargetState;
  var y: int;
  var str: string;

  states = AllPublicTargetStates();
  y = x;

  while (y > 0) {
    if (str != "") {
      str = str + "+";
    };

    foreach (s in keys(states)) {
      if (BitwiseAnd(y, (s to int)) == (s to int)) {
        str = str + states[s];
        y = y - (s to int);
        break;
      }
    }
  }

  return str + format("({0})", x);
}

fun PublicTargetStatesToString(targetStates: map[tTargetId, tPublicTargetState]): string {
  var targetId: tTargetId;
  var str: string;

  foreach (targetId in keys(targetStates)) {
    if (str != "") str = str + ", ";
    str = str + format("<{0}->{1}>", targetId, PublicTargetStateToString(targetStates[targetId] to int));
  }

  return str;
}

type tLocalTargetMap    = map[tTargetId, StorageTarget];
type tGlobalTargetMap   = map[tNodeId, tLocalTargetMap];
type tReplicaChainMap   = map[tChainId, tReplicaChain];
type tStorageClientMap  = map[tNodeId, StorageClient];
type tStorageServiceMap = map[tNodeId, StorageService];

type tRoutingVer  = int;
type tRoutingInfo = (routingVer: tRoutingVer, replicaChains: tReplicaChainMap, storageServices: tStorageServiceMap, offlineServices: set[tNodeId]);

type tGetRoutingInfoReq   = (from: machine, tag: tMessageTag, routingVer: tRoutingVer);
type tGetRoutingInfoResp  = (tag: tMessageTag, status: tErrorCode, routingInfo: tRoutingInfo);
event eGetRoutingInfoReq  : tGetRoutingInfoReq;
event eGetRoutingInfoResp : tGetRoutingInfoResp;

type tUpdateTargetStateMsg   = (from: machine, tag: tMessageTag, routingVer: tRoutingVer, nodeId: tNodeId, targetStates: tLocalTargetStateMap, localTargets: tLocalTargetMap, storageService: StorageService);
event eUpdateTargetStateMsg  : tUpdateTargetStateMsg;

type tRegisterClientMsg   = (from: machine, nodeId: tNodeId, storageClient: StorageClient);
event eRegisterClientMsg  : tRegisterClientMsg;

event eStopFindNewFailures : int;

event eStartNextHeartbeatRound;

// DONE: remove failed storage targets from replication chains
// DONE: re-send pending write requests to successor
// DONE: let failed targets resync and return
// TODO: allow targets moved from one node to another
// TODO: leader election among multiple mgmt services
// TODO: create C++ interfaces from the spec

machine MgmtService {
  var nodeId: tNodeId;
  var nextRequestId: tRequestId;
  var routingVer: tRoutingVer;

  var numStorageServices: int;
  // var mgmtClients: set[machine];
  var fullReplicaChains: tReplicaChainMap;
  // var knownStorageClients: tStorageClientMap;
  var knownStorageServices: tStorageServiceMap;
  var nodeTargetStates: map[tNodeId, tLocalTargetStateMap];
  var storageTargets: map[tTargetId, StorageTarget]; // for debug only
  var delayedRoutingReqs: map[(machine, tRoutingVer), tGetRoutingInfoReq];

  // num of ping attempts made
  var numAttempts: int;
  var maxAttempts: int;
  var stopFindNewFailures: int;
  // set of offline storage services
  var offlineStorageServices: set[tNodeId];
  // nodes that have responded in the current round
  var aliveStorageServices: set[tNodeId];
  // timer to wait for responses from nodes
  var timer: Timer;

  fun newMessageTag(): tMessageTag {
    nextRequestId = nextRequestId + 1;
    return (nodeId = nodeId, requestId = nextRequestId);
  }

  // fun registerClient(registerClientMsg: tRegisterClientMsg) {
  //   var nodeId: tNodeId;
  //   var storageClient: StorageClient;

  //   nodeId = registerClientMsg.nodeId;
  //   storageClient = registerClientMsg.storageClient;

  //   assert !(nodeId in knownStorageClients && knownStorageClients[nodeId] != storageClient);

  //   knownStorageClients[nodeId] = storageClient;
  //   mgmtClients += (registerClientMsg.from);

  //   print format("added client {0}", nodeId);
  // }

  fun updateLocalTargetState(nodeId: tNodeId, localTargetStates: tLocalTargetStateMap, localTargets: tLocalTargetMap) {
    var targetId: tTargetId;

    if (!(nodeId in nodeTargetStates)) {
      nodeTargetStates += (nodeId, default(tLocalTargetStateMap));
    }

    foreach (targetId in keys(nodeTargetStates[nodeId])) {
      if (!(targetId in localTargetStates)) {
        nodeTargetStates[nodeId] -= (targetId);
      }
    }

    foreach (targetId in keys(localTargetStates)) {
      nodeTargetStates[nodeId][targetId] = localTargetStates[targetId];
      storageTargets[targetId] = localTargets[targetId];
    }
  }

  fun setLocalTargetState(nodeId: tNodeId, targetState: tLocalTargetState) {
    var targetId: tTargetId;

    if (!(nodeId in nodeTargetStates)) {
      nodeTargetStates += (nodeId, default(tLocalTargetStateMap));
    }

    foreach (targetId in keys(nodeTargetStates[nodeId])) {
      nodeTargetStates[nodeId][targetId] = targetState;
    }
  }

  fun processUpdateTargetStateMsg(updateTargetStateMsg: tUpdateTargetStateMsg) {
    // mgmtClients += (updateTargetStateMsg.from);
    updateLocalTargetState(updateTargetStateMsg.nodeId, updateTargetStateMsg.targetStates, updateTargetStateMsg.localTargets);
  }

  fun appendTargetToChain(replicaChain: tReplicaChain, targetId: tTargetId, nodeId: tNodeId, targetState: tPublicTargetState): tReplicaChain {
    if (targetId in replicaChain.targets)
      return replicaChain;

    replicaChain.targets += (sizeof(replicaChain.targets), targetId);
    replicaChain.states += (targetId, targetState);
    replicaChain.nodes += (targetId, nodeId);
    replicaChain.services += (targetId, knownStorageServices[nodeId]);

    return replicaChain;
  }

  fun extendChain(chain: tReplicaChain, other: tReplicaChain): tReplicaChain {
    var targetId: tTargetId;

    foreach (targetId in other.targets) {
      if (targetId in chain.targets) continue;
      chain = appendTargetToChain(chain, targetId, other.nodes[targetId], other.states[targetId]);
    }

    return chain;
  }

  fun updatePublicTargetState(
    replicaChain: tReplicaChain,
    chainId: tChainId,
    targetId: tTargetId,
    expectedLocalState: tLocalTargetState,
    fromPublicState: int,
    toPublicState: tPublicTargetState): tReplicaChain
  {
    var nodeId: tNodeId;
    var currentLocalState: tLocalTargetState;
    var currentPublicState: tPublicTargetState;

    nodeId = fullReplicaChains[chainId].nodes[targetId];
    currentLocalState = nodeTargetStates[nodeId][targetId];
    currentPublicState = fullReplicaChains[chainId].states[targetId];

    if (currentLocalState == expectedLocalState) {

      if (BitwiseAnd(currentPublicState to int, fromPublicState to int) > 0) {
        replicaChain = appendTargetToChain(replicaChain, targetId, nodeId, toPublicState);

        if (fullReplicaChains[chainId].states[targetId] != toPublicState) {
          replicaChain.vChainId.chainVer = replicaChain.vChainId.chainVer + 1;
          routingVer = routingVer + 1;

          print format("chain {0}, {1} #{2}: public state updated {3} ==> {4}, local state: {5}, routing version: {6}",
            replicaChain.vChainId, storageTargets[targetId], targetId,
            PublicTargetStateToString(fromPublicState to int),
            PublicTargetStateToString(toPublicState to int),
            LocalTargetStateToString(currentLocalState),
            routingVer);
        } else {
          print format("chain {0}, {1} #{2}: public state untouched, from state {3}, to state {4}, local state: {5}",
            chainId, storageTargets[targetId], targetId,
            PublicTargetStateToString(fromPublicState to int),
            PublicTargetStateToString(toPublicState to int),
            LocalTargetStateToString(expectedLocalState));
        }
      }
    }

    return replicaChain;
  }

  /* transitions of public target states

  <up-to-date>
            serving   syncing   waiting   lastsrv   offline
  serving     y
  syncing     y
  waiting                         y
  lastsrv     y
  offline                         y

  <online>
            serving   syncing   waiting   lastsrv   offline
  serving     y
  syncing               c         c
  waiting               c         c
  lastsrv     y
  offline                         y

  <offline>
            serving   syncing   waiting   lastsrv   offline
  serving                                   c         c
  syncing                                             y
  waiting                                             y
  lastsrv                                   y
  offline                                             y

  */

  fun updateOneReplicaChain(chainId: tChainId): tReplicaChain {
    var states: map[tPublicTargetState, string];
    var targetsGroupbyState: map[tPublicTargetState, tReplicaChain];
    var updatedReplicaChain: tReplicaChain;
    var targetId: tTargetId;
    var targetState: tPublicTargetState;

    states = AllPublicTargetStates();

    foreach (targetState in keys(states)) {
      targetsGroupbyState[targetState] = default(tReplicaChain);
      targetsGroupbyState[targetState].vChainId.chainId = chainId;
    }

    // state transitions to serving

    foreach (targetId in fullReplicaChains[chainId].targets) {
      targetsGroupbyState[PublicTargetState_SERVING] = updatePublicTargetState(
        targetsGroupbyState[PublicTargetState_SERVING],
        chainId, targetId,
        LocalTargetState_UPTODATE,
        (PublicTargetState_SERVING to int) + (PublicTargetState_SYNCING to int) + (PublicTargetState_LASTSRV to int),
        PublicTargetState_SERVING);
    }

    foreach (targetId in fullReplicaChains[chainId].targets) {
      targetsGroupbyState[PublicTargetState_SERVING] = updatePublicTargetState(
        targetsGroupbyState[PublicTargetState_SERVING],
        chainId, targetId,
        LocalTargetState_ONLINE,
        (PublicTargetState_SERVING to int) + (PublicTargetState_LASTSRV to int),
        PublicTargetState_SERVING);
    }

    // state transitions to lastsrv

    foreach (targetId in fullReplicaChains[chainId].targets) {
      if (sizeof(targetsGroupbyState[PublicTargetState_SERVING].targets) == 0 &&
          sizeof(targetsGroupbyState[PublicTargetState_LASTSRV].targets) == 0) {
        targetsGroupbyState[PublicTargetState_LASTSRV] = updatePublicTargetState(
          targetsGroupbyState[PublicTargetState_LASTSRV],
          chainId, targetId,
          LocalTargetState_OFFLINE,
          PublicTargetState_SERVING to int,
          PublicTargetState_LASTSRV);
      }

      targetsGroupbyState[PublicTargetState_LASTSRV] = updatePublicTargetState(
        targetsGroupbyState[PublicTargetState_LASTSRV],
        chainId, targetId,
        LocalTargetState_OFFLINE,
        PublicTargetState_LASTSRV to int,
        PublicTargetState_LASTSRV);
    }

    // state transitions to syncing

    foreach (targetId in fullReplicaChains[chainId].targets) {
      if (sizeof(targetsGroupbyState[PublicTargetState_SERVING].targets) > 0) {
        targetsGroupbyState[PublicTargetState_SYNCING] = updatePublicTargetState(
          targetsGroupbyState[PublicTargetState_SYNCING],
          chainId, targetId,
          LocalTargetState_ONLINE,
          PublicTargetState_SYNCING to int,
          PublicTargetState_SYNCING);
      }
    }

    foreach (targetId in fullReplicaChains[chainId].targets) {
      if (sizeof(targetsGroupbyState[PublicTargetState_SERVING].targets) > 0 &&
          sizeof(targetsGroupbyState[PublicTargetState_SYNCING].targets) == 0) {
        targetsGroupbyState[PublicTargetState_SYNCING] = updatePublicTargetState(
          targetsGroupbyState[PublicTargetState_SYNCING],
          chainId, targetId,
          LocalTargetState_ONLINE,
          PublicTargetState_WAITING to int,
          PublicTargetState_SYNCING);
      }
    }

    // state transitions to waiting

    foreach (targetId in fullReplicaChains[chainId].targets) {
      if (sizeof(targetsGroupbyState[PublicTargetState_SERVING].targets) == 0) {
        targetsGroupbyState[PublicTargetState_WAITING] = updatePublicTargetState(
          targetsGroupbyState[PublicTargetState_WAITING],
          chainId, targetId,
          LocalTargetState_ONLINE,
          PublicTargetState_SYNCING to int,
          PublicTargetState_WAITING);
      }

      if (!(targetId in targetsGroupbyState[PublicTargetState_SYNCING].targets)) {
        targetsGroupbyState[PublicTargetState_WAITING] = updatePublicTargetState(
          targetsGroupbyState[PublicTargetState_WAITING],
          chainId, targetId,
          LocalTargetState_ONLINE,
          PublicTargetState_WAITING to int,
          PublicTargetState_WAITING);
      }

      targetsGroupbyState[PublicTargetState_WAITING] = updatePublicTargetState(
        targetsGroupbyState[PublicTargetState_WAITING],
        chainId, targetId,
        LocalTargetState_UPTODATE,
        (PublicTargetState_OFFLINE to int) + (PublicTargetState_WAITING to int),
        PublicTargetState_WAITING);

      targetsGroupbyState[PublicTargetState_WAITING] = updatePublicTargetState(
        targetsGroupbyState[PublicTargetState_WAITING],
        chainId, targetId,
        LocalTargetState_ONLINE,
        PublicTargetState_OFFLINE to int,
        PublicTargetState_WAITING);
    }

    // state transitions to offline

    foreach (targetId in fullReplicaChains[chainId].targets) {
      if (!(targetId in targetsGroupbyState[PublicTargetState_LASTSRV].targets)) {
        targetsGroupbyState[PublicTargetState_OFFLINE] = updatePublicTargetState(
          targetsGroupbyState[PublicTargetState_OFFLINE],
          chainId, targetId,
          LocalTargetState_OFFLINE,
          PublicTargetState_SERVING to int,
          PublicTargetState_OFFLINE);
      }

      targetsGroupbyState[PublicTargetState_OFFLINE] = updatePublicTargetState(
        targetsGroupbyState[PublicTargetState_OFFLINE],
        chainId, targetId,
        LocalTargetState_OFFLINE,
        (PublicTargetState_SYNCING to int) + (PublicTargetState_WAITING to int) + (PublicTargetState_OFFLINE to int),
        PublicTargetState_OFFLINE);
    }

    // print format("chain {0}, targets group by state: {1}", chainId, targetsGroupbyState);

    updatedReplicaChain.vChainId = fullReplicaChains[chainId].vChainId;

    foreach (targetState in keys(states)) {
      updatedReplicaChain = extendChain(updatedReplicaChain, targetsGroupbyState[targetState]);
      updatedReplicaChain.vChainId.chainVer = updatedReplicaChain.vChainId.chainVer + targetsGroupbyState[targetState].vChainId.chainVer;
    }

    return updatedReplicaChain;
  }

  fun updateRoutingInfo() {
    var updatedReplicaChain: tReplicaChain;
    var localTargetStates: map[tTargetId, tLocalTargetState];
    var chainId: tChainId;
    var targetId: tTargetId;
    var prevRoutingVer: tRoutingVer;

    prevRoutingVer = routingVer;

    foreach (chainId in keys(fullReplicaChains)) {
      localTargetStates = default(map[tTargetId, tLocalTargetState]);

      foreach (targetId in fullReplicaChains[chainId].targets) {
        nodeId = fullReplicaChains[chainId].nodes[targetId];
        localTargetStates += (targetId, nodeTargetStates[nodeId][targetId]);
      }

      print format("start to update chain {0}, public states: {1}, local states: {2}",
        fullReplicaChains[chainId].vChainId,
        PublicTargetStatesToString(fullReplicaChains[chainId].states),
        LocalTargetStatesToString(localTargetStates));

      updatedReplicaChain = updateOneReplicaChain(chainId);

      if (updatedReplicaChain.vChainId != fullReplicaChains[chainId].vChainId) {
        print format("replication chain updated: {0}, updated states: {1}, services: {2}",
          updatedReplicaChain.vChainId,
          PublicTargetStatesToString(updatedReplicaChain.states),
          updatedReplicaChain.services);
      }

      assert PublicTargetState_SERVING in values(updatedReplicaChain.states) ||
             PublicTargetState_LASTSRV in values(updatedReplicaChain.states),
        format("no serving target: {0}", ReplicaChainToString(updatedReplicaChain));
      assert sizeof(updatedReplicaChain.targets) == sizeof(fullReplicaChains[chainId].targets),
        format("updated chain {0} has different number of targets {1} than the old chain {2}",
          chainId, updatedReplicaChain.targets, fullReplicaChains[chainId].targets);

      fullReplicaChains[chainId] = updatedReplicaChain;
    }

    if (routingVer != prevRoutingVer) {
      print format("routing info updated to version {0}, process delayed routing queries: {1}", routingVer, delayedRoutingReqs);
      processDelayedRoutingReqs();
    }
  }

  fun processDelayedRoutingReqs() {
    var getRoutingInfo: tGetRoutingInfoReq;

    foreach (getRoutingInfo in values(delayedRoutingReqs)) {
      delayedRoutingReqs -= (getRoutingInfo.from, getRoutingInfo.routingVer);
      replyWithRoutingInfo(getRoutingInfo);
    }
  }

  fun replyWithRoutingInfo(getRoutingInfo: tGetRoutingInfoReq) {
    var routingInfo: tRoutingInfo;

    if (getRoutingInfo.routingVer == routingVer) {
      if (!((getRoutingInfo.from, getRoutingInfo.routingVer) in delayedRoutingReqs))
        delayedRoutingReqs += ((getRoutingInfo.from, getRoutingInfo.routingVer), getRoutingInfo);
      return;
    }

    routingInfo = (
      routingVer = routingVer,
      replicaChains = fullReplicaChains,
      storageServices = knownStorageServices,
      offlineServices = offlineStorageServices);
    send getRoutingInfo.from, eGetRoutingInfoResp, (tag = getRoutingInfo.tag, status = ErrorCode_SUCCESS, routingInfo = routingInfo);
  }

  fun computeOfflineStorageServices() : set[tNodeId] {
    var nodeId: tNodeId;
    var servicesOffline: set[tNodeId];

    if (stopFindNewFailures == 2) {
      return servicesOffline;
    } else if (stopFindNewFailures == 1 && sizeof(knownStorageServices) == sizeof(aliveStorageServices)) {
      // wait until all storage services are alive and then stop finding new failures
      stopFindNewFailures = 2;
      return servicesOffline;
    }

    foreach (nodeId in keys(knownStorageServices)) {
      if (!(nodeId in aliveStorageServices)) {
        servicesOffline += (nodeId);
      }
    }

    return servicesOffline;
  }

  start state Init {
    entry (args: (nodeId: tNodeId, maxAttempts: int, numStorageServices: int, replicaChains: tReplicaChainMap)) {
      nodeId = args.nodeId;
      numStorageServices = args.numStorageServices;
      fullReplicaChains = args.replicaChains;
      routingVer = 10001;
      maxAttempts = args.maxAttempts;
      timer = CreateTimer(this);
      goto Bootstrap;
    }
  }

  state Bootstrap {
    defer eGetRoutingInfoReq, eRegisterClientMsg;

    on eUpdateTargetStateMsg do (updateTargetStateMsg: tUpdateTargetStateMsg) {
      var nodeId: tNodeId;

      processUpdateTargetStateMsg(updateTargetStateMsg);

      knownStorageServices[updateTargetStateMsg.nodeId] = updateTargetStateMsg.storageService;

      if (sizeof(knownStorageServices) == numStorageServices) {
        foreach(nodeId in keys(knownStorageServices)) {
          aliveStorageServices += (nodeId);
        }

        updateRoutingInfo();

        print format("mgmt service started");

        goto WaitForHeartbeats;
      }
    }
  }

  state WaitForHeartbeats {
    entry {
      // start wait timer to wait for responses
      StartTimer(timer);
    }

    on eGetRoutingInfoReq do replyWithRoutingInfo;

    // on eRegisterClientMsg do registerClient;

    on eStopFindNewFailures do (value: int) {
      stopFindNewFailures = value;
    }

    on eUpdateTargetStateMsg do (updateTargetStateMsg: tUpdateTargetStateMsg) {
      if (updateTargetStateMsg.routingVer < routingVer) {
        print format("#{0}: ignore stale heartbeat (routingVer < {1}): {2} ", numAttempts, routingVer, updateTargetStateMsg);
        return;
      }

      processUpdateTargetStateMsg(updateTargetStateMsg);
      aliveStorageServices += (updateTargetStateMsg.nodeId);
      print format("#{0}: {1} added to aliveStorageServices {2}", numAttempts, updateTargetStateMsg.nodeId, aliveStorageServices);
    }

    on eTimeOut do {
      var nodeId: tNodeId;

      // one more attempt finished
      numAttempts = numAttempts + 1;
      print format("#{0}: aliveStorageServices: {1}", numAttempts, aliveStorageServices);

      if (numAttempts < maxAttempts) {
        // send this, eStartNextHeartbeatRound;
        StartTimer(timer);
        return;
      }

      // set storage targets to offline state
      offlineStorageServices = computeOfflineStorageServices();

      foreach (nodeId in offlineStorageServices) {
        print format("detected node {0} {1} is down, set its targets offline: {2}",
          nodeId, knownStorageServices[nodeId], keys(nodeTargetStates[nodeId]));
        setLocalTargetState(nodeId, LocalTargetState_OFFLINE);
      }

      updateRoutingInfo();

      // lets reset and restart the failure detection
      aliveStorageServices = default(set[tNodeId]);
      numAttempts = 0;
      StartTimer(timer);
      // send this, eStartNextHeartbeatRound;
    }

    // on eStartNextHeartbeatRound goto WaitForHeartbeats;

    on eShutDown goto Offline with (from: machine) {
      print format("{0} is going to shutdown", this);
      send from, eStopped, this;
    }
  }

  state Offline {
    // detection has finish, these are all delayed responses and must be ignored
    ignore eGetRoutingInfoReq, eUpdateTargetStateMsg, eRegisterClientMsg, eTimeOut, eStartNextHeartbeatRound;

    entry {
      var client: machine;
      var service: StorageService;

      print format("stop failure detection");
      CancelTimer(timer);
    }
  }
}
