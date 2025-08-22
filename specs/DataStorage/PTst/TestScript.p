// no failure

test tcOneClientWriteNoFailure [main = OneClientWriteNoFailure]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { OneClientWriteNoFailure };

test tcTwoClientsWriteNoFailure [main = TwoClientsWriteNoFailure]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { TwoClientsWriteNoFailure };

test tcThreeClientsWriteNoFailure [main = ThreeClientsWriteNoFailure]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { ThreeClientsWriteNoFailure };

// unreliable failure detector

test tcOneClientWriteUnreliableDetector [main = OneClientWriteUnreliableDetector]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated in
  union StorageSystem, { OneClientWriteUnreliableDetector };

test tcTwoClientsWriteUnreliableDetector [main = TwoClientsWriteUnreliableDetector]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated in
  union StorageSystem, { TwoClientsWriteUnreliableDetector };

// with failures

test tcOneClientWriteWithFailure [main = OneClientWriteWithFailure]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { OneClientWriteWithFailure };

test tcTwoClientsWriteWithFailure [main = TwoClientsWriteWithFailure]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { TwoClientsWriteWithFailure };

test tcOneClientWriteWithFailures [main = OneClientWriteWithFailures]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { OneClientWriteWithFailures };

test tcTwoClientsWriteWithFailures [main = TwoClientsWriteWithFailures]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { TwoClientsWriteWithFailures };

// short chain

test tcOneClientWriteShortChainWithFailure [main = OneClientWriteShortChainWithFailure]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { OneClientWriteShortChainWithFailure };

test tcTwoClientsWriteShortChainWithFailures [main = TwoClientsWriteShortChainWithFailures]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { TwoClientsWriteShortChainWithFailures };

// long chain

test tcTwoClientsWriteLongChainWithFailures [main = TwoClientsWriteLongChainWithFailures]:
  assert WriteComplete, MonotoneIncreasingVersionNumber, AllReplicasOnChainUpdated, AllReplicasInServingState in
  union StorageSystem, { TwoClientsWriteLongChainWithFailures };
