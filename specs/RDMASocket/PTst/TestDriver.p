fun ConvertToInt4Bytes(n: int): tBytes {
  var bytes: tBytes;
  var i: int;
  i = 0;
  while (i < 4) {
    bytes += (i, n % 256);
    n = n / 256;
    i = i + 1;
  }
  assert n == 0;
  return bytes;
}

fun Convert4BytesToInt(bytes: tBytes): int {
  var n: int;
  var i: int;
  var b: int;
  assert sizeof(bytes) >= 4;
  n = 0;
  i = 0;
  b = 1;
  while (i < 4) {
    n = n + bytes[i] * b;
    i = i + 1;
    b = b * 256;
  }
  return n;
}

fun RecvBytes(user: machine, socket: RDMASocket, length: int): tBytes {
  var result: tBytes;
  var i: int;

  send socket, eRecvBytes, (from = user, length = length);
  receive {
    case eRecvBytesResp: (bytes: tBytes) {
      assert sizeof(bytes) == length;
      result = bytes;
    }
  }

  return result;
}

fun RecvMessage(user: machine, socket: RDMASocket): tBytes {
  var header: tBytes;
  var message: tBytes;
  var msgLen: int;
  header = RecvBytes(user, socket, 4);
  msgLen = Convert4BytesToInt(header);
  print format("{0} try to receive message of length {1}", user, msgLen);
  message = RecvBytes(user, socket, msgLen);
  print format("{0} received message {1}", user, message);
  return message;
}

fun SendMessage(user: machine, socket: RDMASocket, message: tBytes) {
  var header: tBytes;
  var response: tBytes;
  header = ConvertToInt4Bytes(sizeof(message));
  response = Append(header, message);
  send socket, eSendBytes, response;
  print format("{0} sent message {1}", user, message);
}

type tSystemConfig = (
  bufSize: int,
  bufNum: int,
  numRecvBeforeAck: int,
  numIters: int,
  numSenders: int
);

type tNetworkSystem = (
  sock: RDMASocket,
  peer: RDMASocket,
  net: Network
);

fun CreateRDMASocketPair(user: machine, config: tSystemConfig): tNetworkSystem {
  var system: tNetworkSystem;
  system.sock = new RDMASocket((sockId = 1, bufSize = config.bufSize, bufNum = config.bufNum, numRecvBeforeAck = config.numRecvBeforeAck));
  system.peer = new RDMASocket((sockId = 2, bufSize = config.bufSize, bufNum = config.bufNum, numRecvBeforeAck = config.numRecvBeforeAck));
  system.net  = new Network((sock = system.sock, peer = system.peer));
  print format("network system created {0}", system);
  send system.net, eWaitConnected, user;
  receive {
    case eWaitConnectedResp: { }
  }
  print format("network system connected {0}", system);
  return system;
}

/* Ping-pong server and client */

machine PingPongServer {
  var socket: RDMASocket;

  start state Init {
    entry (args: (socket: RDMASocket)) {
      print format("server init {0}", this);
      socket = args.socket;
      print format("server started {0}", this);
      goto ProcessPing;
    }
  }

  state ProcessPing {
    entry {
      var message: tBytes;

      message = RecvMessage(this, socket);
      if (sizeof(message) == 0) // client disconnected
        goto Stopped;

      SendMessage(this, socket, message);
      goto ProcessPing;
    }
  }

  state Stopped {
    entry {
      print format("{0} stopped", this);
    }
  }
}

machine PingPongClient {
  var config: tSystemConfig;
  var socket: RDMASocket;
  var server: PingPongServer;

  var currIter: int;
  var message: tBytes;
  var response: tBytes;

  start state Init {
    entry (args: (config: tSystemConfig, socket: RDMASocket, server: PingPongServer)) {
      print format("client init {0}", this);

      config = args.config;
      socket = args.socket;
      server = args.server;
      currIter = 0;

      print format("client init done {0}", this);
      goto SendPing;
    }
  }

  state SendPing {
    entry {
      var msgLen: int;

      currIter = currIter + 1;
      msgLen = choose(config.bufSize * config.bufNum * 2) + 1;
      message = InitBytes(msgLen, currIter % 256);
      print format("#{0} message {1}", currIter, message);

      SendMessage(this, socket, message);
      goto WaitPong;
    }
  }

  state WaitPong {
    entry {
      var i: int;

      response = RecvMessage(this, socket);
      assert sizeof(message) == sizeof(response);

      i = 0;
      while (i < sizeof(response)) {
        assert response[i] == message[i] && message[i] == currIter % 256;
        i = i + 1;
      }

      if (currIter < config.numIters)
        goto SendPing;
      else
        goto Stopped;
    }
  }

  state Stopped {
    entry {
      SendMessage(this, socket, default(tBytes)); // disconnect
      print format("{0} stopped", this);
    }
  }
}

machine PingPongTest {
  start state Init {
    entry {
      var config: tSystemConfig;
      var system: tNetworkSystem;
      var server: PingPongServer;
      var client: PingPongClient;

      print format("test init {0}", this);

      config = (bufSize = 16, bufNum = 10, numRecvBeforeAck = 4, numIters = 10, numSenders = 2);
      announce eSystemConfig, (config = config,);

      system = CreateRDMASocketPair(this, config);
      server = new PingPongServer((socket = system.peer,));
      client = new PingPongClient((config = config, socket = system.sock, server = server));

      print format("test init done {0}", this);
    }
  }
}

/* One-way communication */

machine OneWayReceiver {
  var socket: RDMASocket;

  start state Init {
    entry (args: (socket: RDMASocket)) {
      print format("receiver init {0}", this);
      socket = args.socket;
      print format("receiver started {0}", this);
      goto Receiving;
    }
  }

  state Receiving {
    entry {
      var message: tBytes;

      message = RecvMessage(this, socket);

      if (sizeof(message) == 0) // client disconnected
        goto Stopped;
      else
        goto Receiving;
    }
  }

  state Stopped {
    entry {
      print format("{0} stopped", this);
    }
  }
}

machine OneWaySender {
  var config: tSystemConfig;
  var socket: RDMASocket;
  var receiver: OneWayReceiver;

  var currIter: int;
  var message: tBytes;
  var response: tBytes;

  start state Init {
    entry (args: (config: tSystemConfig, socket: RDMASocket, receiver: OneWayReceiver)) {
      print format("sender init {0}", this);

      config = args.config;
      socket = args.socket;
      receiver = args.receiver;
      currIter = 0;

      print format("sender init done {0}", this);
      goto Sending;
    }
  }

  state Sending {
    entry {
      var msgLen: int;

      currIter = currIter + 1;
      msgLen = choose(config.bufSize * config.bufNum * 2) + 1;
      message = InitBytes(msgLen, currIter % 256);
      print format("#{0} message {1}", currIter, message);

      SendMessage(this, socket, message);

      if (currIter < config.numIters)
        goto Sending;
      else
        goto Stopped;
    }
  }

  state Stopped {
    entry {
      SendMessage(this, socket, default(tBytes)); // disconnect
      print format("{0} stopped", this);
    }
  }
}

machine OneWayCommunication {
  start state Init {
    entry {
      var config: tSystemConfig;
      var system: tNetworkSystem;
      var receiver: OneWayReceiver;
      var sender: OneWaySender;

      config = (bufSize = 16, bufNum = 10, numRecvBeforeAck = 4, numIters = 10, numSenders = 1);
      announce eSystemConfig, (config = config,);

      system = CreateRDMASocketPair(this, config);
      receiver = new OneWayReceiver((socket = system.peer,));
      sender = new OneWaySender((config = config, socket = system.sock, receiver = receiver));
    }
  }
}

/* Two-way communication */

event eSetPeer: (peer: TwoWaySenderReceiver);
event eNextSendAndRecvIter;

machine TwoWaySenderReceiver {
  var config: tSystemConfig;
  var socket: RDMASocket;
  var peer: TwoWaySenderReceiver;

  var currIter: int;
  var recvLen: int;
  var message: tBytes;
  var response: tBytes;

  fun ProcessRecv (bytes: tBytes) {
    if (recvLen < 0) {
      assert sizeof(bytes) == 4;
      recvLen = Convert4BytesToInt(bytes);
      if (recvLen > 0)
        send socket, eRecvBytes, (from = this, length = recvLen);
    } else {
      assert sizeof(bytes) == recvLen;
      recvLen = -1;
      send socket, eRecvBytes, (from = this, length = 4);
    }
  }

  start state Init {
    entry (args: (config: tSystemConfig, socket: RDMASocket)) {
      print format("two-way sender/receiver init {0}", this);

      config = args.config;
      socket = args.socket;
      currIter = 0;
      recvLen = -1;

      print format("two-way sender/receiver init done {0}", this);
      send socket, eRecvBytes, (from = this, length = 4);
      goto WaitPeer;
    }
  }

  state WaitPeer {
    on eSetPeer goto SendAndRecv with (args: (peer: TwoWaySenderReceiver)) {
      peer = args.peer;
    }

    on eRecvBytesResp do ProcessRecv;
  }

  state SendAndRecv {
    entry {
      var msgLen: int;

      currIter = currIter + 1;
      msgLen = choose(config.bufSize * config.bufNum * 2) + 1;
      message = InitBytes(msgLen, currIter % 256);
      print format("#{0} message {1}", currIter, message);

      SendMessage(this, socket, message);

      if (currIter == config.numIters)
        goto StopSend;
      else
        send this, eNextSendAndRecvIter;
    }

    on eNextSendAndRecvIter goto SendAndRecv;

    on eRecvBytesResp do ProcessRecv;
  }

  state StopSend {
    entry {
      SendMessage(this, socket, default(tBytes)); // disconnect
      print format("{0} stopped sending", this);
    }

    on eRecvBytesResp do ProcessRecv;
  }
}

machine TwoWayCommunication {
  start state Init {
    entry {
      var config: tSystemConfig;
      var system: tNetworkSystem;
      var first: TwoWaySenderReceiver;
      var second: TwoWaySenderReceiver;

      config = (bufSize = 16, bufNum = 10, numRecvBeforeAck = 4, numIters = 10, numSenders = 2);
      announce eSystemConfig, (config = config,);

      system = CreateRDMASocketPair(this, config);
      first = new TwoWaySenderReceiver((config = config, socket = system.peer));
      second = new TwoWaySenderReceiver((config = config, socket = system.sock));

      send first, eSetPeer, (peer = second,);
      send second, eSetPeer, (peer = first,);
    }
  }
}
