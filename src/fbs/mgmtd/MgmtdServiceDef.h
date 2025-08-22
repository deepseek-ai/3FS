#include "fbs/macros/SerdeDef.h"

DEFINE_SERDE_SERVICE_METHOD(Mgmtd, getPrimaryMgmtd, GetPrimaryMgmtd, 1)
// 2 is deprecated
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, heartbeat, Heartbeat, 3)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, registerNode, RegisterNode, 4)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, getRoutingInfo, GetRoutingInfo, 5)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, setConfig, SetConfig, 6)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, getConfig, GetConfig, 7)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, setChainTable, SetChainTable, 8)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, enableNode, EnableNode, 9)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, disableNode, DisableNode, 10)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, extendClientSession, ExtendClientSession, 11)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, listClientSessions, ListClientSessions, 12)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, setNodeTags, SetNodeTags, 13)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, unregisterNode, UnregisterNode, 14)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, setChains, SetChains, 15)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, setUniversalTags, SetUniversalTags, 16)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, getUniversalTags, GetUniversalTags, 17)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, getConfigVersions, GetConfigVersions, 18)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, getClientSession, GetClientSession, 19)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, rotateLastSrv, RotateLastSrv, 20)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, listOrphanTargets, ListOrphanTargets, 21)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, setPreferredTargetOrder, SetPreferredTargetOrder, 22)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, rotateAsPreferredOrder, RotateAsPreferredOrder, 23)
DEFINE_SERDE_SERVICE_METHOD(Mgmtd, updateChain, UpdateChain, 24)

#include "fbs/macros/Undef.h"
