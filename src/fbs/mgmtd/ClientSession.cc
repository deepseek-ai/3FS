#include "ClientSession.h"

#include "Rpc.h"

namespace hf3fs::flat {
ClientSession::ClientSession(const mgmtd::ExtendClientSessionReq &req, UtcTime now)
    : clientId(req.clientId),
      configVersion(req.configVersion),
      start(now),
      lastExtend(now),
      universalId(req.data.universalId),
      description(req.data.description),
      serviceGroups(req.data.serviceGroups),
      releaseVersion(req.data.releaseVersion),
      configStatus(req.configStatus),
      type(req.type),
      clientStart(req.clientStart) {}
}  // namespace hf3fs::flat
