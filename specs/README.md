# P specifications

## Build prerequisites

Follow the [official guide](https://p-org.github.io/P/getstarted/install/) to install the [P](https://github.com/p-org/P) framework.

Or if `dotnet` has been installed, run the following command to store the `p` command.
```
dotnet tool restore
```

## Run tests

A helper script [`RunTests.ps1`](RunTests.ps1), implemented in [PowerShell](https://learn.microsoft.com/en-us/powershell/scripting/install/installing-powershell), is used to run tests and summarize the results.

[`DataStorage`](DataStorage) specifies the CRAQ implementation in 3FS.

```powershell
PS > cd DataStorage
PS > ..\RunTests.ps1

...

-----------------------
Summary of test results
-----------------------
[02/26/2025 10:57:58] Elapsed time: 372.4s

test                                       status       seed schedules seconds min avg max
----                                       ------       ---- --------- ------- --- --- ---
tcOneClientWriteNoFailure[0]               pass   1402445568        10 15.8     -1  -1  -1
tcTwoClientsWriteNoFailure[0]              pass    189933208        10 23.6     -1  -1  -1
tcThreeClientsWriteNoFailure[0]            pass   3060254145        10 40.7     -1  -1  -1
tcOneClientWriteUnreliableDetector[0]      pass   2016460916        10 17.7     -1  -1  -1
tcTwoClientsWriteUnreliableDetector[0]     pass     18777396        10 24.7     -1  -1  -1
tcOneClientWriteWithFailure[0]             pass   2559323541        10 15.7     -1  -1  -1
tcTwoClientsWriteWithFailure[0]            pass   1199246267        10 29.9     -1  -1  -1
tcOneClientWriteWithFailures[0]            pass    672618818        10 15.4     -1  -1  -1
tcTwoClientsWriteWithFailures[0]           pass   1908913074        10 32.3     -1  -1  -1
tcOneClientWriteShortChainWithFailure[0]   pass   3031701162        10 6.3      -1  -1  -1
tcTwoClientsWriteShortChainWithFailures[0] pass   2907349611        10 16.6     -1  -1  -1
tcTwoClientsWriteLongChainWithFailures[0]  pass    260515276        10 67.0     -1  -1  -1

[02/26/2025 10:57:58] All tests passed
```

[`RDMASocket`](RDMASocket) verifies the RDMA socket implementation in 3FS.

```powershell
PS > cd RDMASocket
PS > ..\RunTests.ps1

...


-----------------------
Summary of test results
-----------------------
[02/26/2025 11:19:22] Elapsed time: 40.6s

test          status       seed schedules seconds min avg max
----          ------       ---- --------- ------- --- --- ---
tcPingPong[0] pass   3776118231        10 9.8      -1  -1  -1
tcOneWay[0]   pass    200216558        10 3.6      -1  -1  -1
tcTwoWay[0]   pass   1923093627        10 7.1      -1  -1  -1

[02/26/2025 11:19:22] All tests passed
```

[`Timer`](Timer) includes modified portions of the following open-source project:
  - The [original implementation](https://github.com/p-org/P/tree/master/Tutorial/Common/Timer) of `Timer` is part of [P tutorials](https://p-org.github.io/P/tutsoutline/) licensed under MIT License.
