type tSystemConfig = (
  chunkSize: int,
  numChains: int,
  numReplicas: int,
  numStorageServices: int,
  failStorageServices: int,
  failDetectionMaxAttempts: int,
  numClients: int,
  numIters: int
);

type tStorageSystem = (
  mgmt: MgmtService,
  storages: tStorageServiceMap,
  clients: tTestClientMap
);

fun BuildNodeTargetMap(chunkSize: int, numNodes: int, numTargetsPerNode: int)
  : tGlobalTargetMap
{
  var nodeId: tNodeId;
  var targetId: tTargetId;
  var storageTarget: StorageTarget;
  var localTargets: tLocalTargetMap;
  var nodeTargets: tGlobalTargetMap;

  assert numTargetsPerNode < 100;

  nodeId = 1;
  while (nodeId <= numNodes) {
    localTargets = default(tLocalTargetMap);
    targetId = nodeId * 100 + 1;

    while (sizeof(localTargets) < numTargetsPerNode) {
      storageTarget = new StorageTarget((targetId = targetId, chunkSize = chunkSize));
      localTargets += (targetId, storageTarget);
      targetId = targetId + 1;
    }

    nodeTargets += (nodeId, localTargets);
    nodeId = nodeId + 1;
  }

  return nodeTargets;
}

fun BuildRepliaChainMap(numChains: int, numReplicas: int, nodeTargets: tGlobalTargetMap)
  : tReplicaChainMap
{
  var vChainId: tVersionedChainId;
  var targetId: tTargetId;
  var nodeId: tNodeId;
  var replicaChain: tReplicaChain;
  var replicaChains: tReplicaChainMap;
  var serviceNodeIds: seq[tNodeId];
  var n: int;

  n = 0;
  serviceNodeIds = keys(nodeTargets);
  vChainId = (chainId = 1, chainVer = 1);

  while (vChainId.chainId <= numChains) {
    replicaChain = default(tReplicaChain);
    replicaChain.vChainId = vChainId;

    while (sizeof(replicaChain.targets) < numReplicas) {
      nodeId = serviceNodeIds[n % sizeof(serviceNodeIds)];
      targetId = keys(nodeTargets[nodeId])[0];
      nodeTargets[nodeId] -= (targetId);
      print format("chain {0} added target {1} from node {2}", vChainId.chainId, targetId, nodeId);
      replicaChain.targets += (sizeof(replicaChain.targets), targetId);
      replicaChain.nodes += (targetId, nodeId);
      replicaChain.states += (targetId, PublicTargetState_SERVING);
      n = n + 1;
    }

    print format("create new replica chain: {0}", replicaChain);
    replicaChains += (vChainId.chainId, replicaChain);
    vChainId.chainId = vChainId.chainId + 1;
  }

  return replicaChains;
}

fun CreateStorageServices(nodeTargets: tGlobalTargetMap, mgmtService: MgmtService)
  : tStorageServiceMap
{
  var nodeId: tNodeId;
  var localTargets: tLocalTargetMap;
  var service: StorageService;
  var storageServices: tStorageServiceMap;

  foreach (nodeId in keys(nodeTargets)) {
    service = new StorageService((nodeId = nodeId, localTargets = nodeTargets[nodeId], mgmtService = mgmtService));
    storageServices += (nodeId, service);
  }

  return storageServices;
}

fun CreateTestClients(numClients: int, numChains: int, numIters: int, failStorageServices: int, mgmtService: MgmtService, storageServices: tStorageServiceMap, systemMonitor: SystemMonitor)
  : tTestClientMap
{
  var clientId: tNodeId;
  var client: TestClient;
  var testClients: tTestClientMap;

  clientId = 1;
  while (clientId <= numClients) {
    client = new TestClient((
      clientId = clientId,
      chunkIdBegin = 789001,
      chunkIdEnd = 789000 + numChains * 2,
      numIters = numIters,
      failStorageServices = failStorageServices,
      mgmtService = mgmtService,
      storageServices = storageServices,
      systemMonitor = systemMonitor));
    testClients += (clientId, client);
    clientId = clientId + 1;
  }

  return testClients;
}

fun SetUpStorageSystem(testDriver: machine, config: tSystemConfig) {
  var numTargetsPerNode: int;
  var nodeTargets: tGlobalTargetMap;
  var replicaChains: tReplicaChainMap;
  var storageServices: tStorageServiceMap;
  var storageService: StorageService;
  var mgmtService: MgmtService;
  var testClients: tTestClientMap;
  var storageSystem: tStorageSystem;
  var systemMonitor: SystemMonitor;

  print format("system config: {0}", config);
  announce eSystemConfig, (config = config,);

  assert config.failStorageServices <= config.numStorageServices;
  assert config.numStorageServices >= config.numReplicas;
  assert config.numChains * config.numReplicas % config.numStorageServices == 0;
  assert config.chunkSize > config.numClients * config.numIters;
  numTargetsPerNode = config.numChains * config.numReplicas / config.numStorageServices;

  nodeTargets = BuildNodeTargetMap(config.chunkSize, config.numStorageServices, numTargetsPerNode);
  print format("init nodeTargets {0}", nodeTargets);

  replicaChains = BuildRepliaChainMap(config.numChains, config.numReplicas, nodeTargets);
  print format("init replicaChains {0}", replicaChains);

  mgmtService = new MgmtService((nodeId = 9001, maxAttempts = config.failDetectionMaxAttempts,
    numStorageServices = config.numStorageServices, replicaChains = replicaChains));

  storageServices = CreateStorageServices(nodeTargets, mgmtService);
  systemMonitor = new SystemMonitor((nodeId = 9002, numClients = config.numClients, mgmtService = mgmtService, storageServices = storageServices));
  testClients = CreateTestClients(config.numClients, config.numChains, config.numIters, config.failStorageServices, mgmtService, storageServices, systemMonitor);

  storageSystem = (mgmt = mgmtService, storages = storageServices, clients = testClients);
  announce eStorageSystem, (system = storageSystem,);
}

fun InitBytes(size: int, value: int): tBytes {
  var i: int;
  var bytes: tBytes;
  i = 0;
  while (i < size) {
    bytes += (i, value);
    i = i + 1;
  }
  return bytes;
}

/* Service Monitor */

event eRestart: machine;
event eStarted: machine;
event eStartUp: machine;

event eShutDown: machine;
event eStopped: machine;

machine SystemMonitor {
  var nodeId: tNodeId;
  var numClients: int;
  var mgmtService: MgmtService;
  var storageServices: tStorageServiceMap;
  var failStorageServices: int;
  var mgmtClient: MgmtClient;
  var timer: Timer;

  var stoppedClients: set[tNodeId];
  var offlineTargets: set[tTargetId];
  var offlineServices: set[tNodeId];
  var restartedServices: set[tNodeId];

  fun processRoutingInfo(routingInfo: tRoutingInfo) {
    var replicaChain: tReplicaChain;
    var targetId: tTargetId;

    restartedServices = default(set[tNodeId]);

    foreach (replicaChain in values(routingInfo.replicaChains)) {
      print format("{0}: replication chain: {1}", this, ReplicaChainToString(replicaChain));
      foreach (targetId in replicaChain.targets) {
        if (replicaChain.states[targetId] == PublicTargetState_OFFLINE ||
            replicaChain.states[targetId] == PublicTargetState_LASTSRV)
        {
          if (!(targetId in offlineTargets)) {
            offlineTargets += (targetId);
            offlineServices += (replicaChain.nodes[targetId]);
          }
        }
        else if (replicaChain.states[targetId] == PublicTargetState_SERVING ||
                 replicaChain.states[targetId] == PublicTargetState_SYNCING ||
                 replicaChain.states[targetId] == PublicTargetState_WAITING)
        {
          if (targetId in offlineTargets) {
            offlineTargets -= (targetId);
            offlineServices -= (replicaChain.nodes[targetId]);
          }
        }
      }
    }
  }

  fun restartOfflineServices() {
    var nodeId: tNodeId;

    foreach (nodeId in offlineServices) {
      send storageServices[nodeId], eRestart, this;
      restartedServices += (nodeId);
    }
  }

  start state Init {
    entry (args: (nodeId: tNodeId, numClients: int, mgmtService: MgmtService, storageServices: tStorageServiceMap)) {
      nodeId = args.nodeId;
      numClients = args.numClients;
      mgmtService = args.mgmtService;
      storageServices = args.storageServices;
      // failStorageServices = args.failStorageServices;
      mgmtClient = new MgmtClient((nodeId = nodeId, clientHost = this, mgmtService = mgmtService, sendHeartbeats = false));
      timer = CreateTimer(this);
      goto WaitUntilTestDone;
    }
  }

  state WaitUntilTestDone {
    ignore eSendHeartbeat;

    entry {
      print format("wait until test done: offlineTargets {0}, offlineServices {1}, restartedServices {2}, stoppedClients {3}",
        offlineTargets, offlineServices, restartedServices, stoppedClients);
      StartTimer(timer);
    }

    // on eSendHeartbeat do (heartbeatConns: tHeartbeatConns) {
    //   send heartbeatConns.mgmtService, eRegisterClientMsg, (from = heartbeatConns.mgmtClient, nodeId = nodeId, storageClient = this);
    // }

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      processRoutingInfo(routingInfo);
      if (sizeof(offlineServices) > 0) {
        StartTimer(timer);
      }
    }

    on eTimeOut do {
      restartOfflineServices();
      if (sizeof(offlineServices) > 0) {
        StartTimer(timer);
      } else {
        CancelTimer(timer);
      }
    }

    on eTestClientDone do (clientId: tNodeId) {
      stoppedClients += (clientId);
      if (sizeof(stoppedClients) == numClients) {
        print format("all test clients stopped");
        send mgmtService, eStopFindNewFailures, 1;
        goto WaitUntilSyncDone;
      }
    }
  }

  state WaitUntilSyncDone {
    ignore eSendHeartbeat;

    entry {
      print format("wait until sync done: offlineTargets {0}, offlineServices {1}, restartedServices {2}, stoppedClients {3}",
        offlineTargets, offlineServices, restartedServices, stoppedClients);
      StartTimer(timer);
    }

    on eNewRoutingInfo do (routingInfo: tRoutingInfo) {
      processRoutingInfo(routingInfo);
      if (sizeof(offlineServices) > 0) {
        StartTimer(timer);
      } else {
        goto ShutdownSystem;
      }
    }

    on eTimeOut do {
      restartOfflineServices();
      if (sizeof(offlineServices) > 0) {
        StartTimer(timer);
      } else {
        goto ShutdownSystem;
      }
    }
  }

  state ShutdownSystem {
    ignore eSendHeartbeat, eNewRoutingInfo, eTimeOut;

    entry {
      var storageService: StorageService;

      print format("{0}: all done, restartedServices: {1}", this, restartedServices);
      announce eStopMonitorTargetStates;
      CancelTimer(timer);

      send mgmtClient, eShutDown, this;
      send mgmtService, eShutDown, this;
      receive {
        case eStopped: (mgmt: machine) {
          assert mgmt == mgmtService;
        }
      }

      foreach (storageService in values(storageServices)) {
        send storageService, eShutDown, this;
      }
    }
  }
}

/* Test Client */

// DONE: write different part of the chunk for each write to detect any error
// DONE: stop a storage service more than once during test (stop it when it's syncing)
// TODO: [new test] stop mgmt client of an alive storage service to simulate network partition
// TODO: [new test] shut down storage service and then restart
// TODO: [new test] make storage service crash during syncing

type tTestClientMap = map[tNodeId, TestClient];
// type tTestStatus = (nodeId: tNodeId, done: bool);
event eTestClientDone : tNodeId;
// event eTestStatusReq  : tTestStatus;
// event eTestStatusResp : tTestStatus;

machine TestClient {
  var clientId: tNodeId;
  var chunkIdBegin: tChainId;
  var chunkIdEnd: tChainId;
  var numIters: int;
  var failStorageServices: int;

  var storageClient: StorageClient;
  var storageServices: tStorageServiceMap;
  var systemMonitor: SystemMonitor;

  var nextWritePos: int;
  var currIter: int;
  var currChunkId: tChunkId;
  var lastChunkVer: map[tChunkId, tChunkVer];

  fun CreateNewWrite(chunkId: tChunkId, offset: int, length: int, value: int): tWriteArgs {
    var dataBytes: tBytes;
    var writeArgs: tWriteArgs;

    dataBytes = InitBytes(length, value);
    print format("data bytes size {0}", sizeof(dataBytes));

    writeArgs = (from = this, chunkId = chunkId, offset = offset, length = sizeof(dataBytes), dataBytes = dataBytes);
    print format("{0}: created a new write: {1}", this, writeArgs);

    return writeArgs;
  }

  fun CreateNewRemove(chunkId: tChunkId): tWriteArgs {
    var writeArgs: tWriteArgs;

    writeArgs = (from = this, chunkId = chunkId, offset = 0, length = 0, dataBytes = default(tBytes));
    print format("{0}: created a new remove: {1}", this, writeArgs);

    return writeArgs;
  }

  fun CreateNewRead(chunkId: tChunkId, offset: int, length: int): tReadArgs {
    var readArgs: tReadArgs;

    readArgs = (from = this, chunkId = chunkId, offset = offset, length = length);
    print format("{0}: created a new read: {1}", this, readArgs);

    return readArgs;
  }

  start state Init {
    // defer eTestStatusReq;

    entry (args: (clientId: tNodeId, chunkIdBegin: int, chunkIdEnd: int, numIters: int, failStorageServices: int, mgmtService: MgmtService, storageServices: tStorageServiceMap, systemMonitor: SystemMonitor)) {
      assert args.chunkIdBegin < args.chunkIdEnd;
      clientId = args.clientId + 8000;
      chunkIdBegin = args.chunkIdBegin;
      chunkIdEnd = args.chunkIdEnd;
      numIters = args.numIters;
      failStorageServices = args.failStorageServices;
      storageServices = args.storageServices;
      systemMonitor = args.systemMonitor;
      nextWritePos = 0;
      storageClient = new StorageClient((clientId = clientId, mgmtService = args.mgmtService));
      send storageClient, eWaitConnected, this;
    }

    on eClientConnected goto SendingWriteReq;
  }

  state SendingWriteReq {
    entry {
      var offset: int;
      var length: int;
      var machineToFail: machine;

/*        ---------------------------------------------------------------------
                                      currChunkId
          ---------------------------------------------------------------------
              client 8001   |    client 8002   |    client 8003   |    ......
          ---------------------------------------------------------------------
           <currIter> bytes | <currIter> bytes | <currIter> bytes |    ......
          ---------------------------------------------------------------------
              ^             ^
              |             |
    offset----|<---length-->|
*/

      currChunkId = chunkIdBegin + nextWritePos / numIters;
      currIter = nextWritePos % numIters + 1;
      offset = (clientId - 8001) * numIters + currIter - 1;
      length = numIters - currIter + 1;
      nextWritePos = nextWritePos + 1;

      if (!(currChunkId in lastChunkVer))
        lastChunkVer += (currChunkId, 0);

      send storageClient, eSubmitWrite, CreateNewWrite(currChunkId, offset, length, currIter);

      if (failStorageServices > 0 && choose()) {
        machineToFail = choose(values(storageServices));
        send machineToFail, eShutDown, machineToFail;
        failStorageServices = failStorageServices - 1;
      }
    }

    on eWriteComplete do (writeRes: tWriteRes) {
      assert writeRes.status == ErrorCode_SUCCESS, format("error: {0}", writeRes);
      // assert lastChunkVer[writeRes.chunkId] < writeRes.commitVer,
      //   format("error: last chunk version {0} >= commit version {1}", lastChunkVer[writeRes.chunkId], writeRes.commitVer);

      // lastChunkVer[writeRes.chunkId] = writeRes.commitVer;

      if (nextWritePos >= numIters * (chunkIdEnd - chunkIdBegin)) {
        goto Done;
      } else if (nextWritePos % numIters == numIters / 2) {
        goto SendingRemoveReq;
      } else {
        goto SendingWriteReq;
      }
    }

    // on eTestStatusReq do (from: machine) {
    //   send from, eTestStatusResp, (nodeId = clientId, nextWritePos = nextWritePos, done = false);
    // }
  }

  state SendingReadReq {
    entry {
      var offset: int;
      var length: int;

      offset = (clientId - 101) * numIters;
      length = numIters;

      send storageClient, eSubmitRead, CreateNewRead(currChunkId, offset, length);
    }

    on eReadComplete do (readRes: tReadRes) {
      var i: int;

      if (readRes.status == ErrorCode_CHUNK_NOT_FOUND) {
        print format("{0} chunk {1} removed by other client, re-create the chunk", this, currChunkId);
        goto SendingWriteReq;
        return;
      }

      if (readRes.status == ErrorCode_TARGET_OFFLINE) {
        goto SendingReadReq;
        return;
      }

      assert readRes.status == ErrorCode_SUCCESS, format("readRes.status {0}", readRes.status);
      assert readRes.chunkId == currChunkId, format("readRes.chunkId {0} != currChunkId {1}", readRes.chunkId, currChunkId);
      // assert lastChunkVer[currChunkId] <= readRes.chunkMetadata.commitVer,
      //   format("lastChunkVer[currChunkId:{0}] {1} > readRes.chunkMetadata.commitVer {2}",
      //     currChunkId, lastChunkVer[currChunkId], readRes.chunkMetadata.commitVer);

      // if (lastChunkVer[currChunkId] == readRes.chunkMetadata.commitVer) {
        while (i < sizeof(readRes.dataBytes)) {
          assert readRes.dataBytes[i] <= Min(currIter, i + 1),
            format("readRes.dataBytes[i:{0}] {1} != {2}, nextWritePos {3}, currIter {4}",
              i, readRes.dataBytes[i], Min(currIter, i + 1), nextWritePos, currIter);
          i = i + 1;
        }
      // }

      if (nextWritePos % numIters == 0) {
        goto SendingRemoveReq;
      } else {
        goto SendingWriteReq;
      }
    }

    // on eTestStatusReq do (from: machine) {
    //   send from, eTestStatusResp, (nodeId = clientId, nextWritePos = nextWritePos, done = false);
    // }
  }

  state SendingRemoveReq {
    entry {
      send storageClient, eSubmitWrite, CreateNewRemove(currChunkId);
    }

    on eWriteComplete do (writeRes: tWriteRes) {
      assert writeRes.status == ErrorCode_SUCCESS, format("error: {0}", writeRes);
      // assert lastChunkVer[writeRes.chunkId] < writeRes.commitVer,
      //   format("error: last chunk version {0} >= commit version {1}", lastChunkVer[writeRes.chunkId], writeRes.commitVer);

      // lastChunkVer -= (writeRes.chunkId);

      // check if the chunk removed or re-created
      send storageClient, eSubmitRead, CreateNewRead(currChunkId, 0, numIters);
      receive {
        case eReadComplete: (readRes: tReadRes) {
          if (readRes.status == ErrorCode_CHUNK_NOT_FOUND) {
            print format("Chunk {0} removed, result: {1}", currChunkId, readRes);
          } else {
            print format("Chunk {0} re-created, result: {1}", currChunkId, readRes);
          }
        }
      }

      if (nextWritePos < numIters * (chunkIdEnd - chunkIdBegin)) {
        goto SendingWriteReq;
      } else {
        goto Done;
      }
    }

    // on eTestStatusReq do (from: machine) {
    //   send from, eTestStatusResp, (nodeId = clientId, nextWritePos = nextWritePos, done = false);
    // }
  }

  state Done {
    entry {
      print format("{0}: all done", this);
      send systemMonitor, eTestClientDone, clientId;
      send storageClient, eShutDown, this;
    }

    // on eTestStatusReq do (from: machine) {
    //   send from, eTestStatusResp, (nodeId = clientId, nextWritePos = nextWritePos, done = true);
    // }
  }
}

// no failure

machine OneClientWriteNoFailure {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 0,
        failDetectionMaxAttempts = 11,
        numClients = 1,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine TwoClientsWriteNoFailure {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 0,
        failDetectionMaxAttempts = 11,
        numClients = 2,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine ThreeClientsWriteNoFailure {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 0,
        failDetectionMaxAttempts = 11,
        numClients = 3,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

// unreliable failure detector

machine OneClientWriteUnreliableDetector {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 1,
        failDetectionMaxAttempts = 7,
        numClients = 1,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine TwoClientsWriteUnreliableDetector {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 1,
        failDetectionMaxAttempts = 7,
        numClients = 2,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

// with failures

machine OneClientWriteWithFailure {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 1,
        failDetectionMaxAttempts = 11,
        numClients = 1,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine TwoClientsWriteWithFailure {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 1,
        failDetectionMaxAttempts = 11,
        numClients = 2,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine OneClientWriteWithFailures {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 3,
        failDetectionMaxAttempts = 11,
        numClients = 1,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine TwoClientsWriteWithFailures {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 3,
        numStorageServices = 3,
        failStorageServices = 3,
        failDetectionMaxAttempts = 11,
        numClients = 2,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

// short chain: two replicas

machine OneClientWriteShortChainWithFailure {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 2,
        numStorageServices = 2,
        failStorageServices = 1,
        failDetectionMaxAttempts = 11,
        numClients = 1,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

machine TwoClientsWriteShortChainWithFailures {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 2,
        numStorageServices = 2,
        failStorageServices = 2,
        failDetectionMaxAttempts = 11,
        numClients = 2,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}

// long chain: four replicas

machine TwoClientsWriteLongChainWithFailures {
  start state Init {
    entry {
      var config: tSystemConfig;
      config = (
        chunkSize = 16,
        numChains = 1,
        numReplicas = 4,
        numStorageServices = 4,
        failStorageServices = 2,
        failDetectionMaxAttempts = 23,
        numClients = 2,
        numIters = 2);
      SetUpStorageSystem(this, config);
    }
  }
}
