#include "SysvShm.h"

namespace hf3fs {
SysvShm::SysvShm() = default;

SysvShm::~SysvShm() {
  if (auto r = detach(); r.hasError()) {
    XLOGF(WARN, "Error when destruct a shm: {}", r.error());
  }
  if (auto r = remove(); r.hasError()) {
    XLOGF(WARN, "Error when destruct a shm: {}", r.error());
  }
}

Result<std::shared_ptr<SysvShm>> SysvShm::create(const Path &path, size_t size, int flags) {
  struct SysvShmDerived : SysvShm {};

  std::shared_ptr<SysvShm> shm = std::make_shared<SysvShmDerived>();
  key_t key = ::ftok(path.c_str(), 0);
  if (key == -1) {
    return MAKE_ERROR_F(StatusCode::kOSError, "Fail to call ftok({}): {} {}", path, errno, std::strerror(errno));
  }
  shm->id_ = ::shmget(key, size, flags);
  if (shm->id_ == -1) {
    return MAKE_ERROR_F(StatusCode::kOSError, "Fail to call shmget: {} {}", errno, std::strerror(errno));
  }
  if (size == 0) {
    auto r = shm->getStat();
    RETURN_ON_ERROR(r);
    size = r->shm_segsz;
  }
  if (size == 0) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "size should be positive");
  }
  auto *ptr = (uint8_t *)::shmat(shm->id_, nullptr, 0);
  if (ptr == reinterpret_cast<uint8_t *>(-1)) {
    return MAKE_ERROR_F(StatusCode::kOSError, "Fail to call shmat: {} {}", errno, std::strerror(errno));
  }
  shm->ptr_ = ptr;
  shm->size_ = size;
  XLOGF(DBG3, "Create SysvShm path={} size={} flags={} id={} ptr={}", path, size, flags, shm->id_, fmt::ptr(shm->ptr_));
  return std::move(shm);
}

std::span<uint8_t> SysvShm::getBuf() const {
  if (ptr_) {
    return {ptr_, size_};
  }
  return {};
}

Result<shmid_ds> SysvShm::getStat() const {
  if (id_ == -1) {
    return MAKE_ERROR_F(StatusCode::kInvalidArg, "Shm not inited");
  }
  shmid_ds shmInfo;
  if (::shmctl(id_, IPC_STAT, &shmInfo) == -1) {
    return MAKE_ERROR_F(StatusCode::kOSError, "Fail to get stat of shm {}: {} {}", id_, errno, std::strerror(errno));
  }
  return shmInfo;
}

Result<Void> SysvShm::detach() {
  if (!ptr_) {
    return Void{};
  }
  if (::shmdt(ptr_) == -1) {
    return MAKE_ERROR_F(StatusCode::kOSError, "Fail to detach shm {}: {} {}", id_, errno, std::strerror(errno));
  }
  XLOGF(DBG3, "Detach shm {} size {}", id_, size_);
  ptr_ = nullptr;
  return Void{};
}

Result<Void> SysvShm::remove() {
  if (id_ < 0 || removed_) {
    return Void{};
  }
  if (::shmctl(id_, IPC_RMID, nullptr) == -1) {
    return MAKE_ERROR_F(StatusCode::kOSError, "Fail to remove shm {}: {} {}", id_, errno, std::strerror(errno));
  }
  XLOGF(DBG3, "Remove shm {} size {}", id_, size_);
  removed_ = true;
  return Void{};
}

}  // namespace hf3fs
