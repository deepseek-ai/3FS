test tcPingPong [main = PingPongTest]:
  assert RecvComplete, NoDuplicatePostedBuffers, AllIterationsProcessed in
  (union RDMANetwork, { PingPongServer, PingPongClient, PingPongTest });

test tcOneWay [main = OneWayCommunication]:
  assert RecvComplete, NoDuplicatePostedBuffers, AllIterationsProcessed in
  (union RDMANetwork, { OneWayReceiver, OneWaySender, OneWayCommunication });

test tcTwoWay [main = TwoWayCommunication]:
  assert RecvComplete, NoDuplicatePostedBuffers, AllIterationsProcessed in
  (union RDMANetwork, { TwoWaySenderReceiver, TwoWayCommunication });
