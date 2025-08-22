type tBytes = seq[int];

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

fun Append(a: tBytes, b: tBytes): tBytes {
  var i: int;
  i = 0;
  while (i < sizeof(b)) {
    a += (sizeof(a), b[i]);
    i = i + 1;
  }
  return a;
}

enum tWROpCode {
  WROpCode_INVALID       = 100,
  WROpCode_SEND          = 101,
  WROpCode_SEND_WITH_IMM = 102
}

enum tWCOpCode {
  WCOpCode_INVALID       = 200,
  WCOpCode_SEND          = 201,
  WCOpCode_RECV          = 202,
  WCOpCode_RECV_WITH_IMM = 203 // no such opcode in ibv APIs, this is added to indicate a recv completion with wc_flags = IBV_WC_WITH_IMM
}

fun ConvertWRToWCOpCode(opcode: tWROpCode): tWCOpCode {
  if (opcode == WROpCode_SEND) return WCOpCode_RECV;
  if (opcode == WROpCode_SEND_WITH_IMM) return WCOpCode_RECV_WITH_IMM;
  return WCOpCode_INVALID;
}

enum tStatus {
  Status_OK,
  Status_ERR,
  Status_AGAIN
}

type tXmitPacket    = (opcode: tWROpCode, payload: tBytes, length: int, imm: int);
type tGetPacketResp = (from: QueuePair, status: tStatus, packet: tXmitPacket);

event ePutPacket: tXmitPacket;
event eGetPacket:  Network;
event eGetPacketResp: tGetPacketResp;
event eWaitConnected: machine;
event eWaitConnectedResp;
event eNextExchangeIter;

machine Network {
  var qps: seq[QueuePair];
  var user: machine;

  start state Init {
    entry (args: (sock: RDMASocket, peer: RDMASocket)) {
      send args.sock, eConnect, this;
      receive {
        case eConnectResp: (qp: QueuePair) { qps += (0, qp); }
      }

      send args.peer, eConnect, this;
      receive {
        case eConnectResp: (qp: QueuePair) { qps += (1, qp); }
      }

      print format("network connected {0}", qps);

      if (user != null)
        send user, eWaitConnectedResp;

      goto ExchangePackets;
    }

    on eWaitConnected do (from: machine) {
      user = from;
    }
  }

  state ExchangePackets {
    entry {
      var i: int;
      var n: int;
      i = 0;
      while (i < sizeof(qps)) {
        // exchange a nondeterministic number of packets between 1..4
        n = choose(3) + 1;
        while (n > 0) {
          send qps[i], eGetPacket, this;
          n = n - 1;
        }
        i = i + 1;
      }
    }

    on eWaitConnected do (from: machine) {
      send from, eWaitConnectedResp;
    }

    on eGetPacketResp do (resp: tGetPacketResp) {
      var i: int;

      if (resp.status == Status_OK) {
        i = 0;
        while (i < 2) {
          if (qps[i] != resp.from)
            break;
          i = i + 1;
        }
        send qps[i], ePutPacket, resp.packet;
      }

      send this, eNextExchangeIter;
    }

    on eNextExchangeIter goto ExchangePackets;
  }
}

type tWorkComplete  = (wrIdx: int, opcode: tWCOpCode, payload: tBytes, length: int, imm: int, status: tStatus);
type tWorkRequest   = (wrIdx: int, opcode: tWROpCode, payload: tBytes, length: int, imm: int);

event ePostRecv:  tWorkRequest;
event ePostSend:  tWorkRequest;
event ePollRecvCQ: RDMASocket;
event ePollSendCQ: RDMASocket;

machine QueuePair {
  var maxNumSendWRs: int;
  var maxNumRecvWRs: int;
  var postedRecvWRs: seq[tWorkRequest];
  var postedSendWRs: seq[tWorkRequest];
  var sendCompQueue: seq[tWorkComplete];
  var recvCompQueue: seq[tWorkComplete];
  var outboundQueue: seq[tXmitPacket];
  var inboundQueue:  seq[tXmitPacket];

  // users waiting on events
  var network: Network;
  var sockPollSendCQ: RDMASocket;
  var sockPollRecvCQ: RDMASocket;
  var pendingGetPkt: int;
  var pendingPollSend: int;
  var pendingPollRecv: int;

  fun PushPacketToNetwork(net: Network) {
    var wr: tWorkRequest;
    var wc: tWorkComplete;
    var packet: tXmitPacket;

    wr = postedSendWRs[0];
    postedSendWRs -= (0);
    print format("{0} -sizeof postedSendWRs {1}", this, sizeof(postedSendWRs));

    wc = (wrIdx   = wr.wrIdx,
          opcode  = WCOpCode_SEND,
          payload = wr.payload,
          length  = wr.length,
          imm     = wr.imm,
          status  = Status_OK);
    sendCompQueue += (sizeof(sendCompQueue), wc);
    print format("{0} +sizeof sendCompQueue {1}", this, sizeof(sendCompQueue));

    if (pendingPollSend > 0) {
      NotifySendCQ(sockPollSendCQ);
      pendingPollSend = pendingPollSend - 1;
      if (pendingPollSend == 0)
        sockPollSendCQ = default(RDMASocket);
    }

    packet = (opcode = wr.opcode,
              payload = wr.payload,
              length = wr.length,
              imm = wr.imm);
    send net, eGetPacketResp, (from = this, status = Status_OK, packet = packet);
  }

  fun NotifyRecvCQ(sock: RDMASocket) {
    var wc: tWorkComplete;
    wc = recvCompQueue[0];
    recvCompQueue -= (0);
    print format("{0} -sizeof recvCompQueue {1}", this, sizeof(recvCompQueue));
    send sock, ePollRecvCQReturn, wc;
  }

  fun NotifySendCQ(sock: RDMASocket) {
    var wc: tWorkComplete;
    wc = sendCompQueue[0];
    sendCompQueue -= (0);
    print format("{0} -sizeof sendCompQueue {1}", this, sizeof(sendCompQueue));
    send sock, ePollSendCQReturn, wc;
  }

  start state Init {
    entry (args: (maxNumSendWRs: int, maxNumRecvWRs: int)) {
      print format("qp init start {0}", this);
      maxNumSendWRs = args.maxNumSendWRs;
      maxNumRecvWRs = args.maxNumRecvWRs;
      print format("qp init done {0}", this);
      goto WaitForEvents;
    }
  }

  state WaitForEvents {
    on ePostRecv do (wr: tWorkRequest) {
      assert sizeof(postedRecvWRs) < maxNumRecvWRs;
      postedRecvWRs += (sizeof(postedRecvWRs), wr);
      print format("{0} +sizeof postedRecvWRs {1}", this, sizeof(postedRecvWRs));
    }

    on ePostSend do (wr: tWorkRequest) {
      assert sizeof(postedSendWRs) < maxNumSendWRs;
      postedSendWRs += (sizeof(postedSendWRs), wr);
      print format("{0} +sizeof postedSendWRs {1}", this, sizeof(postedSendWRs));

      if (pendingGetPkt > 0) {
        PushPacketToNetwork(network);
        pendingGetPkt = pendingGetPkt - 1;
        if (pendingGetPkt == 0)
          network = default(Network);
      }
    }

    on ePutPacket do (packet: tXmitPacket) {
      var wr: tWorkRequest;
      var wc: tWorkComplete;
      var i: int;

      assert sizeof(postedRecvWRs) > 0, "error: receive not ready";
      wr = postedRecvWRs[0];
      postedRecvWRs -= (0);
      print format("{0} -sizeof postedRecvWRs {1}", this, sizeof(postedRecvWRs));

      assert packet.length <= wr.length;

      wc = (wrIdx   = wr.wrIdx,
            opcode  = ConvertWRToWCOpCode(packet.opcode),
            payload = wr.payload,
            length  = packet.length,
            imm     = packet.imm,
            status  = Status_OK);

      while (i < packet.length) {
        wc.payload[i] = packet.payload[i];
        i = i + 1;
      }

      recvCompQueue += (sizeof(recvCompQueue), wc);
      print format("{0} +sizeof recvCompQueue {1}", this, sizeof(recvCompQueue));

      if (pendingPollRecv > 0) {
        NotifyRecvCQ(sockPollRecvCQ);
        pendingPollRecv = pendingPollRecv - 1;
        if (pendingPollRecv == 0)
          sockPollRecvCQ = default(RDMASocket);
      }
    }

    on eGetPacket do (net: Network) {
      if (sizeof(postedSendWRs) == 0) {
        // send net, eGetPacketResp, (from = this, status = Status_AGAIN, packet = default(tXmitPacket));
        pendingGetPkt = pendingGetPkt + 1;
        if (pendingGetPkt > 1)
          assert network == net;
        else
          network = net;
      } else {
        PushPacketToNetwork(net);
      }
    }

    on ePollRecvCQ do (sock: RDMASocket) {
      if (sizeof(recvCompQueue) == 0) {
        pendingPollRecv = pendingPollRecv + 1;
        if (pendingPollRecv > 1)
          assert sockPollRecvCQ == sock;
        else
          sockPollRecvCQ = sock;
      } else {
        NotifyRecvCQ(sock);
      }
    }

    on ePollSendCQ do (sock: RDMASocket) {
      if (sizeof(sendCompQueue) == 0) {
        pendingPollSend = pendingPollSend + 1;
        if (pendingPollSend > 1)
          assert sockPollSendCQ == sock;
        else
          sockPollSendCQ = sock;
      } else {
        NotifySendCQ(sock);
      }
    }
  }
}

type tTaggedBuffer = (bufIdx: int, payload: tBytes, length: int);

event ePollRecvCQReturn: tWorkComplete;
event ePollSendCQReturn: tWorkComplete;
event eRecvBytes: (from: machine, length: int);
event eSendBytes: tBytes;
event eRecvBytesResp: tBytes;
event eConnect: Network;
event eConnectResp: QueuePair;
event eNextPollCQIter;

machine RDMASocket {
  var qp: QueuePair;
  var sockId: int;
  var bufSize: int;
  var bufNum: int;  // assume the numbers of local and remote send/recv buffers are the same
  var flowCtrlBufNum: int;

  var unusedSendBufs: seq[tTaggedBuffer];
  var remotePostedBufNum: int;
  var effectiveSendBufNum: int;
  var numRecvBeforeAck: int;
  var numRecvSinceLastAck: int;

  var bytesToSend: tBytes;
  var bytesRecved: tBytes;

  // pending recv
  var userWaited: machine;
  var recvedData: tBytes;
  var recvLength: int;

  var pendingPollSendCQ: int;
  var pendingPollRecvCQ: int;

  start state Init {
    entry (args: (sockId: int, bufSize: int, bufNum: int, numRecvBeforeAck: int)) {
      print format("socket init start {0}", this);

      flowCtrlBufNum = (args.bufNum + args.numRecvBeforeAck - 1) / args.numRecvBeforeAck;
      qp = new QueuePair((
        maxNumSendWRs = args.bufNum + flowCtrlBufNum,
        maxNumRecvWRs = args.bufNum + flowCtrlBufNum));

      sockId = args.sockId;
      bufSize = args.bufSize;
      bufNum = args.bufNum;

      remotePostedBufNum = args.bufNum;
      effectiveSendBufNum = args.bufNum;
      numRecvBeforeAck = args.numRecvBeforeAck;
      numRecvSinceLastAck = 0;

      print format("socket init done {0} with qp {1}", this, qp);
      goto BeforeConnect;
    }
  }

  state BeforeConnect {
    entry {
      var i: int;
      print format("post {0} recv buffers in {1}", bufNum, this);

      i = 0;
      while (i < bufNum + flowCtrlBufNum) {
        send qp, ePostRecv, (wrIdx = sockId * bufNum * 2 + i, opcode = WROpCode_INVALID, payload = InitBytes(bufSize, 0), length = bufSize, imm = 0);
        i = i + 1;
      }

      print format("create {0} send buffers", effectiveSendBufNum);

      i = 0;
      while (i < effectiveSendBufNum) {
        unusedSendBufs += (i, (bufIdx = sockId * bufNum * 2 + i, payload = InitBytes(bufSize, 0), length = bufSize));
        i = i + 1;
      }

      goto WaitConnect;
    }
  }

  state WaitConnect {
    on eConnect do (net: Network) {
      print format("{0} connected to {1}", this, net);
      send net, eConnectResp, qp;
      goto PollCQEvents;
    }
  }

  state PollCQEvents {
    entry {
      var i: int;
      var sendBuf: tTaggedBuffer;

      print format("{0} sizeof(bytesToSend) {1} && sizeof(unusedSendBufs) {2} && remotePostedBufNum {3}",
        this, sizeof(bytesToSend), sizeof(unusedSendBufs), remotePostedBufNum);

      if (sizeof(bytesToSend) > 0 && remotePostedBufNum == 0) {
        print format("{0}: remote side not posted any recv buffer", this);
      }

      if (sizeof(bytesToSend) > 0 && sizeof(unusedSendBufs) == 0) {
        print format("{0}: local side does not have send buffer", this);
      }

      while (sizeof(bytesToSend) > 0 && sizeof(unusedSendBufs) > 0 && remotePostedBufNum > 0) {

        remotePostedBufNum = remotePostedBufNum - 1;
        sendBuf = unusedSendBufs[0];
        unusedSendBufs -= (0);

        i = 0;
        while (i < bufSize && sizeof(bytesToSend) > 0) {
          sendBuf.payload[i] = bytesToSend[0];
          bytesToSend -= (0);
          i = i + 1;
        }

        send qp, ePostSend, (wrIdx = sendBuf.bufIdx, opcode = WROpCode_SEND, payload = sendBuf.payload, length = i, imm = 0);
      }

      if (pendingPollRecvCQ < bufNum + flowCtrlBufNum) {
        i = 0;
        while (i < bufNum + flowCtrlBufNum - pendingPollRecvCQ) {
          send qp, ePollRecvCQ, this;
          pendingPollRecvCQ = pendingPollRecvCQ + 1;
        }
      }

      if (pendingPollSendCQ < bufNum) {
        i = 0;
        while (i < bufNum - pendingPollSendCQ) {
          send qp, ePollSendCQ, this;
          pendingPollSendCQ = pendingPollSendCQ + 1;
        }
      }
    }

    on ePollRecvCQReturn do (wc: tWorkComplete) {
      var i: int;

      if (wc.status == Status_OK) {
        assert wc.opcode == WCOpCode_RECV_WITH_IMM || wc.opcode == WCOpCode_RECV;

        if (wc.opcode == WCOpCode_RECV_WITH_IMM) {
          remotePostedBufNum = remotePostedBufNum + wc.imm;
          print format("{0} received flow control packet with imm {1}, remotePostedBufNum {2}", this, wc.imm, remotePostedBufNum);
        } else if (wc.opcode == WCOpCode_RECV) {
          i = 0;
          while (i < wc.length) {
            bytesRecved += (sizeof(bytesRecved), wc.payload[i]);
            i = i + 1;
          }

          print format("recv cq returned, user {0} waited", userWaited);

          if (userWaited != null) {
            print format("recvLength {0}, sizeof(recvedData) {1}, sizeof(bytesRecved) {2}",
              recvLength, sizeof(recvedData), sizeof(bytesRecved));

            i = 0;
            while (sizeof(recvedData) < recvLength && sizeof(bytesRecved) > 0) {
              recvedData += (sizeof(recvedData), bytesRecved[0]);
              bytesRecved -= (0);
              i = i + 1;
            }

            print format("copy recv data, copy length {0}, recvLength {1}, recvedData {2}", i, recvLength, recvedData);

            if (sizeof(recvedData) == recvLength) {
              send userWaited, eRecvBytesResp, recvedData;
              userWaited = default(machine);
              recvedData = default(tBytes);
              recvLength = 0;
            }
          }
        }

        send qp, ePostRecv, (wrIdx = wc.wrIdx, opcode = WROpCode_INVALID, payload = wc.payload, length = bufSize, imm = 0);

        if (wc.opcode == WCOpCode_RECV) {
          numRecvSinceLastAck = numRecvSinceLastAck + 1;
          if (numRecvSinceLastAck == numRecvBeforeAck) {
            send qp, ePostSend, (wrIdx = -1, opcode = WROpCode_SEND_WITH_IMM, payload = default(tBytes), length = 0, imm = numRecvSinceLastAck);
            numRecvSinceLastAck = 0;
          }
        }

        assert pendingPollRecvCQ > 0;
        pendingPollRecvCQ = pendingPollRecvCQ - 1;
        send this, eNextPollCQIter;
      } else if (wc.status != Status_AGAIN) {
        assert false, "Unexpected wc status";
      }
    }

    on ePollSendCQReturn do (wc: tWorkComplete) {
      var sendBuf: tTaggedBuffer;
      if (wc.status == Status_OK) {
        if (wc.opcode == WCOpCode_SEND) {
          if (wc.wrIdx >= 0) {
            sendBuf = (bufIdx = wc.wrIdx, payload = wc.payload, length = bufSize);
            unusedSendBufs += (sizeof(unusedSendBufs), sendBuf);
          }
        } else {
          assert false, "Unexpected wc opcode";
        }

        assert pendingPollSendCQ > 0;
        pendingPollSendCQ = pendingPollSendCQ - 1;
        send this, eNextPollCQIter;
      } else if (wc.status != Status_AGAIN) {
        assert false, "Unexpected wc status";
      }
    }

    on eRecvBytes do (args: (from: machine, length: int)) {
      var i: int;

      print format("{0} requested to receive {1} bytes, sizeof(bytesRecved) {2}", args.from, args.length, sizeof(bytesRecved));

      i = 0;
      while (i < args.length && sizeof(bytesRecved) > 0) {
        recvedData += (i, bytesRecved[0]);
        bytesRecved -= (0);
        i = i + 1;
      }

      if (sizeof(recvedData) == args.length) {
        send args.from, eRecvBytesResp, recvedData;
        recvedData = default(tBytes);
      } else {
        userWaited = args.from;
        recvLength = args.length;
      }
      send this, eNextPollCQIter;
    }

    on eSendBytes do (bytes: tBytes) {
      var i: int;
      i = 0;
      while (i < sizeof(bytes)) {
        bytesToSend += (sizeof(bytesToSend), bytes[i] % 256);
        i = i + 1;
      }
      send this, eNextPollCQIter;
    }

    on eNextPollCQIter goto PollCQEvents;
  }
}
