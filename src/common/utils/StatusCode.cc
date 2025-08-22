#include "StatusCode.h"

#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <cerrno>

namespace hf3fs::StatusCode {

std::string_view toString(status_code_t code) {
  switch (code) {
#define RAW_STATUS(name, ...) \
  case k##name:               \
    return #name;
#define STATUS(ns, name, ...) \
  case ns##Code::k##name:     \
    return #ns "::" #name;
#include "StatusCodeDetails.h"
#undef RAW_STATUS
#undef STATUS
  };
  return "UnknownStatusCode";
}

StatusCodeType typeOf(status_code_t code) {
  switch (code) {
#define RAW_STATUS(name, ...) \
  case k##name:               \
    return StatusCodeType::Common;
#define STATUS(ns, name, ...) \
  case ns##Code::k##name:     \
    return StatusCodeType::ns;
#include "StatusCodeDetails.h"
#undef RAW_STATUS
#undef STATUS
  };
  return StatusCodeType::Invalid;
}

int toErrno(status_code_t code) {
  //  XLOGF(DBG, "error code to convert {}", code);

  if (StatusCode::typeOf(code) == StatusCodeType::RPC) {
    return EREMOTEIO;
  }

  switch (code) {
    case ClientAgentCode::kTooManyOpenFiles:
      return EMFILE;

    case StatusCode::kInvalidArg:
      return EINVAL;
    case StatusCode::kNotImplemented:
      return ENOSYS;
    case StatusCode::kNotEnoughMemory:
      return ENOMEM;
    case StatusCode::kAuthenticationFail:
      return EPERM;
    case StatusCode::kReadOnlyMode:
      return EROFS;
    case MetaCode::kRequestCanceled:
    case StorageClientCode::kRequestCanceled:
      // NOTE: use EINTR instead of ECANCELED
      return EINTR;
    case StorageClientCode::kNoSpace:
      return ENOSPC;

    case MetaCode::kNotFound:
      return ENOENT;
    case MetaCode::kNotEmpty:
      return ENOTEMPTY;
    case MetaCode::kNotDirectory:
      return ENOTDIR;
    case MetaCode::kTooManySymlinks:
      return ELOOP;
    case MetaCode::kIsDirectory:
      return EISDIR;
    case MetaCode::kExists:
      return EEXIST;
    case MetaCode::kNoPermission:
      return EPERM;
    case MetaCode::kInconsistent:
      return EIO;
    case MetaCode::kNotFile:
      return EBADF;
    case MetaCode::kMoreChunksToRemove:
      return EAGAIN;
    case MetaCode::kNameTooLong:
      return ENAMETOOLONG;
    case MetaCode::kNoLock:
      return ENOLCK;
    case MetaCode::kFileTooLarge:
      return EFBIG;

    case ClientAgentCode::kHoleInIoOutcome:
      return ENODATA;
    case ClientAgentCode::kOperationDisabled:
      return EOPNOTSUPP;
    case ClientAgentCode::kIovNotRegistered:
      return EACCES;
    case ClientAgentCode::kIovShmFail:
      return EACCES;

    default:
      return EIO;
  }
}
}  // namespace hf3fs::StatusCode
