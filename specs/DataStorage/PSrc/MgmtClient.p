type tHeartbeatConns = (mgmtClient: MgmtClient, mgmtService: MgmtService);
event eSendHeartbeat: tHeartbeatConns;
event eNewRoutingInfo: tRoutingInfo;

machine MgmtClient {
  var nodeId: tNodeId;
  var clientHost: machine;
  var mgmtService: MgmtService;
  var sendHeartbeats: bool;
  var timer: Timer;
  var nextRequestId: tRequestId;
  var routingInfo: tRoutingInfo;

  fun newMessageTag(): tMessageTag {
    nextRequestId = nextRequestId + 1;
    return (nodeId = nodeId, requestId = nextRequestId);
  }

  start state Init {
    entry (args: (nodeId: tNodeId, clientHost: machine, mgmtService: MgmtService, sendHeartbeats: bool)) {
      print format("{0} init: {1}", this, args);
      nodeId = args.nodeId;
      clientHost = args.clientHost;
      mgmtService = args.mgmtService;
      sendHeartbeats = args.sendHeartbeats;
      timer = CreateTimer(this);
      goto SendHeartbeats;
    }
  }

  state SendHeartbeats {
    entry {
      if (sendHeartbeats) {
        print format("{0} of {1} sends heartbeat to {2}", this, clientHost, mgmtService);
        send clientHost, eSendHeartbeat, (mgmtClient = this, mgmtService = mgmtService);
      }
      send mgmtService, eGetRoutingInfoReq, (from = this, tag = newMessageTag(), routingVer = routingInfo.routingVer);
      StartTimer(timer);
    }

    on eTimeOut goto SendHeartbeats;

    on eShutDown goto Offline with (from: machine) {
      print format("{0} of node {1} is going to shutdown", this, nodeId);
      CancelTimer(timer);
    }

    on eGetRoutingInfoResp do (getRoutingInfoResp: tGetRoutingInfoResp) {
      var latestRoutingInfo: tRoutingInfo;

      latestRoutingInfo = getRoutingInfoResp.routingInfo;

      if (getRoutingInfoResp.status == ErrorCode_SUCCESS &&
          routingInfo.routingVer < latestRoutingInfo.routingVer)
      {
        print format("{0}: routing info version {1} is greater than: {2}", this, latestRoutingInfo.routingVer, routingInfo.routingVer);
        routingInfo = latestRoutingInfo;
        send clientHost, eNewRoutingInfo, routingInfo;
      }
    }
  }

  state Offline {
    ignore eTimeOut, eShutDown, eGetRoutingInfoResp;

    entry {
      print format("{0} #{1} is offline, client host: {2}", this, nodeId, clientHost);
      routingInfo = default(tRoutingInfo);
    }

    on eRestart goto SendHeartbeats with (from: machine) {
      print format("{0} #{1} is restarted by {2}", this, nodeId, from);
    }
  }
}
