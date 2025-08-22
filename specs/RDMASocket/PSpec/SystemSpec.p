spec RecvComplete observes eSendBytes, eRecvBytes, eRecvBytesResp {
  var pendingRecv: int;
  var pendingRecvBytes: int;
  var sentBytes: int;
  var recvBytes: int;

  fun AddSendBytes(bytes: tBytes) {
    sentBytes = sentBytes + sizeof(bytes);
  }

  fun AddRecvBytes(args: (from: machine, length: int)) {
    pendingRecv = pendingRecv + 1;
    pendingRecvBytes = pendingRecvBytes + args.length;
  }

  start hot state NoPendingRecv {
    entry{
      assert pendingRecvBytes == 0, format("{0} pending recv bytes not equal to zero", pendingRecvBytes);
      assert recvBytes <= sentBytes, format("error: {0} recv bytes > {1} sent bytes", recvBytes, sentBytes);

      if (recvBytes == sentBytes) {
        goto AllDataRecved;
      }
    }

    on eSendBytes do AddSendBytes;

    on eRecvBytes goto PendingRecv with AddRecvBytes;
  }

  hot state PendingRecv {

    on eSendBytes do AddSendBytes;

    on eRecvBytes do AddRecvBytes;

    on eRecvBytesResp do (bytes: tBytes) {
      recvBytes = recvBytes + sizeof(bytes);
      pendingRecv = pendingRecv - 1;
      pendingRecvBytes = pendingRecvBytes - sizeof(bytes);
      if (pendingRecv == 0)
        goto NoPendingRecv;
    }
  }

  cold state AllDataRecved {
    entry {
      print format("all data received");
    }

    on eSendBytes do AddSendBytes;

    on eRecvBytes goto PendingRecv with AddRecvBytes;
  }
}

spec NoDuplicatePostedBuffers observes ePostSend, ePostRecv, ePollSendCQReturn, ePollRecvCQReturn {
  var postedRecvBufs: set[int];
  var postedSendBufs: set[int];

  start state Init {
    on ePostRecv do (wr: tWorkRequest) {
      assert wr.wrIdx >= 0, format("buffer index {0} < 0", wr.wrIdx);
      assert !(wr.wrIdx in postedRecvBufs), format("buffer with index {0} already posted", wr.wrIdx);
      postedRecvBufs += (wr.wrIdx);
    }

    on ePostSend do (wr: tWorkRequest) {
      if (wr.wrIdx >= 0) {
        assert !(wr.wrIdx in postedSendBufs), format("buffer with index {0} already posted", wr.wrIdx);
        postedSendBufs += (wr.wrIdx);
      } else {
        assert wr.opcode == WROpCode_SEND_WITH_IMM && wr.imm > 0;
      }
    }

    on ePollRecvCQReturn do (wc: tWorkComplete) {
      assert wc.wrIdx >= 0, format("buffer index {0} < 0", wc.wrIdx);
      assert wc.wrIdx in postedRecvBufs, format("unexpected buffer index {0} returned", wc.wrIdx);
      postedRecvBufs -= (wc.wrIdx);
    }

    on ePollSendCQReturn do (wc: tWorkComplete) {
      if (wc.wrIdx >= 0) {
        assert wc.wrIdx in postedSendBufs, format("unexpected buffer index {0} returned", wc.wrIdx);
        postedSendBufs -= (wc.wrIdx);
      } else {
        assert wc.opcode == WCOpCode_SEND && wc.imm > 0;
      }
    }
  }
}

event eSystemConfig: (config: tSystemConfig);

spec AllIterationsProcessed observes eSendBytes, eRecvBytesResp, eSystemConfig {
  var config: tSystemConfig;
  var sendIters: tBytes;
  var recvIters: tBytes;

  fun CheckStopCondition(iters: tBytes): bool {
    var i: int;
    i = 0;
    while (i < sizeof(iters)) {
      if (iters[i] != config.numIters) {
        return false;
      }
      i = i + 1;
    }
    return true;
  }

  fun UpdateIters(iters: tBytes, expected: int): tBytes {
    var i: int;
    i = 0;
    while (i < sizeof(iters)) {
      if (expected == iters[i] + 1) {
        iters[i] = iters[i] + 1;
        return iters;
      }
      i = i + 1;
    }
    print format("failed to update iters to {0}", expected);
    return iters;
  }

  start state Init {
    on eSystemConfig goto Communicating with (args: (config: tSystemConfig)) {
      var i: int;
      i = 0;
      config = args.config;
      while (i < config.numSenders) {
        print format("i {0}/{1}", i, config.numSenders);
        sendIters += (i, 0);
        recvIters += (i, 0);
        i = i + 1;
      }
    }
  }

  hot state Communicating {
    on eSendBytes do (bytes: tBytes) {
      sendIters = UpdateIters(sendIters, bytes[sizeof(bytes)-1]);
      if (CheckStopCondition(sendIters) && CheckStopCondition(recvIters)) {
        goto Done;
      }
    }

    on eRecvBytesResp do (bytes: tBytes) {
      recvIters = UpdateIters(recvIters, bytes[sizeof(bytes)-1]);
      if (CheckStopCondition(sendIters) && CheckStopCondition(recvIters)) {
        goto Done;
      }
    }
  }

  cold state Done {
    ignore eSendBytes, eRecvBytesResp;
    entry {
      print format("all iterations processed");
    }
  }
}
