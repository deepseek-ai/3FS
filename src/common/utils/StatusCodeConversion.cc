#include "StatusCodeConversion.h"

namespace hf3fs {

Status StatusCodeConversion::convertToStorageClientCode(Status status) {
  switch (status.code()) {
    case StatusCode::kInvalidArg:
      return status.convert(StorageClientCode::kInvalidArg);
    case RPCCode::kTimeout:
      return status.convert(StorageClientCode::kTimeout);
    case RPCCode::kSendFailed:
    case RPCCode::kRequestRefused:
      return status.convert(StorageClientCode::kCommError);
    case RPCCode::kVerifyRequestFailed:
    case RPCCode::kVerifyResponseFailed:
      return status.convert(StorageClientCode::kProtocolMismatch);
    case StorageCode::kChunkMetadataNotFound:
      return status.convert(StorageClientCode::kChunkNotFound);
    case StorageCode::kChunkReadFailed:
      return status.convert(StorageClientCode::kRemoteIOError);
    case StorageCode::kChecksumMismatch:
      return status.convert(StorageClientCode::kChecksumMismatch);
    case StorageCode::kChainVersionMismatch:
      return status.convert(StorageClientCode::kRoutingVersionMismatch);
    case StorageCode::kChunkNotCommit:
      return status.convert(StorageClientCode::kChunkNotCommit);
    case StorageCode::kChannelIsLocked:
      return status.convert(StorageClientCode::kResourceBusy);
    case StatusCode::kReadOnlyMode:
      return status.convert(StorageClientCode::kReadOnlyServer);
  }

  if (StatusCode::typeOf(status.code()) == StatusCodeType::StorageClient) {
    return status;
  } else {
    return status.convert(StorageClientCode::kServerError);
  }
}

}  // namespace hf3fs
