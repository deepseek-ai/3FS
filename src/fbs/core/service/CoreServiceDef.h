#include "fbs/macros/SerdeDef.h"

DEFINE_SERDE_SERVICE_METHOD(Core, echo, Echo, 1)
DEFINE_SERDE_SERVICE_METHOD(Core, getConfig, GetConfig, 2)
DEFINE_SERDE_SERVICE_METHOD(Core, renderConfig, RenderConfig, 3)
DEFINE_SERDE_SERVICE_METHOD(Core, hotUpdateConfig, HotUpdateConfig, 4)
DEFINE_SERDE_SERVICE_METHOD(Core, getLastConfigUpdateRecord, GetLastConfigUpdateRecord, 5)
DEFINE_SERDE_SERVICE_METHOD(Core, shutdown, Shutdown, 6)

#include "fbs/macros/Undef.h"
