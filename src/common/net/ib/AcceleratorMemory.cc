#include "AcceleratorMemory.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fmt/format.h>
#include <folly/FileUtil.h>
#include <folly/logging/xlog.h>
#include <sys/stat.h>
#include <tuple>
#include <unistd.h>

#ifdef HF3FS_GDR_ENABLED
#include <cuda_runtime.h>
#endif

#include "common/monitor/Recorder.h"

namespace hf3fs::net {

namespace {

monitor::CountRecorder gdrMemRegistered("common.ib.gdr_mem_registered", {}, false);
monitor::CountRecorder gdrMemCached("common.ib.gdr_mem_cached", {}, false);
monitor::CountRecorder gdrMemCacheHit("common.ib.gdr_mem_cache_hit", {}, false);
monitor::CountRecorder gdrMemCacheMiss("common.ib.gdr_mem_cache_miss", {}, false);
monitor::CountRecorder gdrMemCacheExpired("common.ib.gdr_mem_cache_expired", {}, false);
monitor::CountRecorder gdrMemCacheInvalidate("common.ib.gdr_mem_cache_invalidate", {}, false);

// Access flags for GDR memory registration
constexpr int kGDRAccessFlags =
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_RELAXED_ORDERING;

}  // namespace

// AcceleratorMemoryRegion implementation

AcceleratorMemoryRegion::~AcceleratorMemoryRegion() { deregister(); }

AcceleratorMemoryRegion::AcceleratorMemoryRegion(AcceleratorMemoryRegion &&other) noexcept
    : desc_(other.desc_),
      mrs_(other.mrs_),
      registered_(other.registered_) {
  other.mrs_.fill(nullptr);
  other.registered_ = false;
}

AcceleratorMemoryRegion &AcceleratorMemoryRegion::operator=(AcceleratorMemoryRegion &&other) noexcept {
  if (this != &other) {
    deregister();
    desc_ = other.desc_;
    mrs_ = other.mrs_;
    registered_ = other.registered_;
    other.mrs_.fill(nullptr);
    other.registered_ = false;
  }
  return *this;
}

Result<std::unique_ptr<AcceleratorMemoryRegion>> AcceleratorMemoryRegion::create(
    const AcceleratorMemoryDescriptor &desc,
    const GDRConfig &config) {
  if (!desc.isValid()) {
    return makeError(StatusCode::kInvalidArg, "Invalid GPU memory descriptor");
  }

  // Check alignment if configured
  if (config.verify_alignment()) {
    auto alignment = reinterpret_cast<uintptr_t>(desc.devicePtr) % config.required_alignment();
    if (alignment != 0) {
      XLOGF(WARN,
            "GPU memory address {} is not aligned to {} bytes (offset: {})",
            desc.devicePtr,
            config.required_alignment(),
            alignment);
    }
  }

  auto region = std::make_unique<AcceleratorMemoryRegion>();
  region->desc_ = desc;

  auto result = region->registerWithDevices(config);
  if (!result) {
    return makeError(result.error());
  }

  gdrMemRegistered.addSample(desc.size);
  return std::move(region);
}

ibv_mr *AcceleratorMemoryRegion::getMR(int devId) const {
  if (devId < 0 || static_cast<size_t>(devId) >= mrs_.size()) {
    return nullptr;
  }
  return mrs_[devId];
}

std::optional<uint32_t> AcceleratorMemoryRegion::getRkey(int devId) const {
  auto mr = getMR(devId);
  if (mr) {
    return mr->rkey;
  }
  return std::nullopt;
}

bool AcceleratorMemoryRegion::getAllRkeys(std::array<uint32_t, IBDevice::kMaxDeviceCnt> &rkeys) const {
  bool hasAny = false;
  for (size_t i = 0; i < mrs_.size(); ++i) {
    if (mrs_[i]) {
      rkeys[i] = mrs_[i]->rkey;
      hasAny = true;
    } else {
      rkeys[i] = 0;
    }
  }
  return hasAny;
}

Result<Void> AcceleratorMemoryRegion::registerWithDevices(const GDRConfig &config) {
  (void)config;
  if (!IBManager::initialized()) {
    return makeError(RPCCode::kIBDeviceNotInitialized, "IB not initialized");
  }

  size_t registeredCount = 0;
  for (const auto &dev : IBDevice::all()) {
    if (dev->id() >= IBDevice::kMaxDeviceCnt) {
      XLOGF(ERR, "Device ID {} exceeds maximum {}", dev->id(), IBDevice::kMaxDeviceCnt);
      continue;
    }

    // For GDR, we use ibv_reg_mr with the GPU pointer directly
    // The underlying driver (nvidia_peermem) handles the GPU memory
    auto mr = dev->regMemory(desc_.devicePtr, desc_.size, kGDRAccessFlags);
    if (!mr) {
      XLOGF(WARN,
            "Failed to register GPU memory {} (size: {}) with IB device {}",
            desc_.devicePtr,
            desc_.size,
            dev->name());
      // Continue trying other devices
      continue;
    }

    mrs_[dev->id()] = mr;
    ++registeredCount;
    XLOGF(DBG,
          "Registered GPU memory {} (size: {}) with IB device {}, lkey: {}, rkey: {}",
          desc_.devicePtr,
          desc_.size,
          dev->name(),
          mr->lkey,
          mr->rkey);
  }

  if (registeredCount == 0) {
    return makeError(StatusCode::kIOError, "Failed to register GPU memory with any IB device");
  }

  registered_ = true;
  return Void{};
}

void AcceleratorMemoryRegion::deregister() {
  if (!registered_) {
    return;
  }

  for (size_t i = 0; i < mrs_.size(); ++i) {
    if (mrs_[i]) {
      auto dev = IBDevice::get(i);
      if (dev) {
        dev->deregMemory(mrs_[i]);
      }
      mrs_[i] = nullptr;
    }
  }

  if (desc_.size > 0) {
    gdrMemRegistered.addSample(-static_cast<int64_t>(desc_.size));
  }

  registered_ = false;
  XLOGF(DBG, "Deregistered GPU memory region {}", desc_.devicePtr);
}

// AcceleratorMemoryRegionCache implementation

AcceleratorMemoryRegionCache::AcceleratorMemoryRegionCache(const GDRConfig &config)
    : config_(config) {}

AcceleratorMemoryRegionCache::~AcceleratorMemoryRegionCache() { clear(); }

Result<std::shared_ptr<AcceleratorMemoryRegion>> AcceleratorMemoryRegionCache::getOrCreate(
    const AcceleratorMemoryDescriptor &desc) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = cache_.find(desc.devicePtr);
  if (it != cache_.end()) {
    auto cached = it->second.lock();
    if (!cached) {
      cache_.erase(it);
      gdrMemCached.addSample(-1);
      gdrMemCacheExpired.addSample(1);
    } else if (cached->size() == desc.size && cached->deviceId() == desc.deviceId) {
      gdrMemCacheHit.addSample(1);
      return cached;
    } else {
      cache_.erase(it);
      gdrMemCached.addSample(-1);
    }
  }
  gdrMemCacheMiss.addSample(1);

  // Evict if needed
  evictIfNeeded();

  // Create new region
  auto result = AcceleratorMemoryRegion::create(desc, config_);
  if (!result) {
    return makeError(result.error());
  }

  auto region = std::shared_ptr<AcceleratorMemoryRegion>(std::move(*result));
  cache_[desc.devicePtr] = region;
  gdrMemCached.addSample(1);

  return region;
}

void AcceleratorMemoryRegionCache::invalidate(void *devicePtr) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(devicePtr);
  if (it != cache_.end()) {
    cache_.erase(it);
    gdrMemCached.addSample(-1);
    gdrMemCacheInvalidate.addSample(1);
  }
}

void AcceleratorMemoryRegionCache::clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  auto count = cache_.size();
  cache_.clear();
  if (count > 0) {
    gdrMemCached.addSample(-static_cast<int64_t>(count));
  }
}

size_t AcceleratorMemoryRegionCache::size() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cache_.size();
}

void AcceleratorMemoryRegionCache::evictIfNeeded() {
  for (auto it = cache_.begin(); it != cache_.end();) {
    if (it->second.expired()) {
      it = cache_.erase(it);
      gdrMemCached.addSample(-1);
      gdrMemCacheExpired.addSample(1);
    } else {
      ++it;
    }
  }

  // Simple LRU-like eviction: if cache is full, remove oldest entries.
  while (cache_.size() >= config_.max_cached_regions()) {
    // Remove first entry (arbitrary in unordered_map, but simple)
    auto it = cache_.begin();
    if (it != cache_.end()) {
      cache_.erase(it);
      gdrMemCached.addSample(-1);
    }
  }
}

// GDRManager implementation

GDRManager &GDRManager::instance() {
  static GDRManager instance;
  return instance;
}

GDRManager::~GDRManager() { shutdown(); }

Result<Void> GDRManager::init(const GDRConfig &config) {
  if (initialized_.load()) {
    return Void{};
  }

  config_ = config;

  // Parse HF3FS_GDR_ENABLED env var
  const char *gdrEnabledEnv = std::getenv("HF3FS_GDR_ENABLED");
  if (gdrEnabledEnv) {
    if (std::string(gdrEnabledEnv) == "0") {
      XLOGF(INFO, "GDR disabled by HF3FS_GDR_ENABLED=0");
      return Void{};  // Return early, don't initialize
    }
    // "1" means require GDR - will fail later if detection fails
  }

  // Parse HF3FS_GDR_FALLBACK env var
  fallbackMode_ = FallbackMode::Auto;  // default
  const char *fallbackEnv = std::getenv("HF3FS_GDR_FALLBACK");
  if (fallbackEnv) {
    std::string fallback(fallbackEnv);
    if (fallback == "host") {
      fallbackMode_ = FallbackMode::Host;
    } else if (fallback == "fail") {
      fallbackMode_ = FallbackMode::Fail;
    }
    // "auto" or unrecognized → default Auto
  }

  if (!config_.enabled()) {
    XLOGF(INFO, "GDR support is disabled by configuration");
    return Void{};
  }

  // Detect GPU devices
  auto detectResult = detectGpuDevices();
  if (!detectResult) {
    XLOGF(WARN, "Failed to detect GPU devices: {}", detectResult.error().message());
    // Continue without GDR support
    return Void{};
  }

  if (gpuDevices_.empty()) {
    XLOGF(INFO, "No GPU devices found, GDR support unavailable");
    return Void{};
  }

  // Setup GPU to IB device mapping
  auto mappingResult = setupGpuIBMapping();
  if (!mappingResult) {
    XLOGF(WARN, "Failed to setup GPU-IB mapping: {}", mappingResult.error().message());
  }

  // Initialize region cache
  regionCache_ = std::make_unique<AcceleratorMemoryRegionCache>(config_);

  initialized_.store(true);
  XLOGF(INFO, "GDR support initialized with {} GPU device(s)", gpuDevices_.size());

  return Void{};
}

void GDRManager::shutdown() {
  if (!initialized_.load()) {
    return;
  }

  if (regionCache_) {
    regionCache_->clear();
    regionCache_.reset();
  }

  gpuDevices_.clear();
  gpuToIBMapping_.clear();
  initialized_.store(false);

  XLOGF(INFO, "GDR support shut down");
}

bool GDRManager::isGdrSupported(int deviceId) const {
  if (!initialized_.load()) {
    return false;
  }

  for (const auto &dev : gpuDevices_) {
    if (dev.deviceId == deviceId) {
      return dev.gdrSupported;
    }
  }
  return false;
}

std::optional<uint8_t> GDRManager::getBestIBDevice(int gpuDeviceId) const {
  auto it = gpuToIBMapping_.find(gpuDeviceId);
  if (it != gpuToIBMapping_.end()) {
    return it->second;
  }
  // Return first available IB device as fallback
  if (!IBDevice::all().empty()) {
    return IBDevice::all()[0]->id();
  }
  return std::nullopt;
}

Result<Void> GDRManager::detectGpuDevices() {
  gpuDevices_.clear();

#ifdef HF3FS_GDR_ENABLED
  int deviceCount = 0;
  cudaError_t err = cudaGetDeviceCount(&deviceCount);
  if (err != cudaSuccess) {
    if (err == cudaErrorNoDevice) {
      XLOGF(INFO, "No CUDA devices found");
      cudaGetLastError();  // Clear CUDA error state
      return Void{};
    }
    return makeError(StatusCode::kIOError, fmt::format("cudaGetDeviceCount failed: {}", cudaGetErrorString(err)));
  }

  if (deviceCount <= 0) {
    XLOGF(INFO, "No CUDA devices found");
    return Void{};
  }

  bool peermemLoaded = false;
  int fd = open("/sys/module/nvidia_peermem", O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    peermemLoaded = true;
    close(fd);
  }
  XLOGF_IF(WARN, !peermemLoaded, "nvidia_peermem module not loaded, GDR registration may fail at runtime");

  gpuDevices_.reserve(deviceCount);
  for (int deviceId = 0; deviceId < deviceCount; ++deviceId) {
    cudaDeviceProp prop;
    err = cudaGetDeviceProperties(&prop, deviceId);
    if (err != cudaSuccess) {
      XLOGF(WARN, "cudaGetDeviceProperties({}) failed: {}", deviceId, cudaGetErrorString(err));
      cudaGetLastError();
      continue;
    }

    AcceleratorDeviceInfo info;
    info.deviceId = deviceId;
    info.totalMemory = prop.totalGlobalMem;
    info.computeCapabilityMajor = prop.major;
    info.computeCapabilityMinor = prop.minor;
    info.gdrSupported = true;

    int value = 0;
    if (cudaDeviceGetAttribute(&value, cudaDevAttrPciDomainId, deviceId) == cudaSuccess) {
      info.pciDomainId = value;
    }
    if (cudaDeviceGetAttribute(&value, cudaDevAttrPciBusId, deviceId) == cudaSuccess) {
      info.pciBusId = value;
    }
    if (cudaDeviceGetAttribute(&value, cudaDevAttrPciDeviceId, deviceId) == cudaSuccess) {
      info.pciDeviceId = value;
    }

    char busId[32] = {0};
    if (cudaDeviceGetPCIBusId(busId, sizeof(busId), deviceId) == cudaSuccess) {
      info.uuid = busId;
    } else {
      info.uuid = std::to_string(deviceId);
    }

    gpuDevices_.push_back(std::move(info));
  }

  XLOGF(INFO, "Detected {} CUDA device(s) for GDR", gpuDevices_.size());
  return Void{};
#else
  XLOGF(DBG, "GDR disabled at build time, skipping GPU device detection");
  return Void{};
#endif
}

namespace {

// IB device topology info resolved from sysfs
struct IBDeviceTopology {
  uint8_t devId;
  std::string name;
  int pciDomain = -1;
  int pciBus = -1;
  int pciDevice = -1;
  int numaNode = -1;
};

// Read a single integer from a sysfs file, return -1 on failure
int readSysfsInt(const std::string &path) {
  std::string content;
  if (!folly::readFile(path.c_str(), content)) {
    return -1;
  }
  try {
    return std::stoi(content);
  } catch (...) {
    return -1;
  }
}

// Parse PCI BDF from sysfs device symlink target
// e.g. /sys/class/infiniband/mlx5_0/device -> ../../../0000:3b:00.0
// Returns {domain, bus, device} or {-1,-1,-1} on failure
std::tuple<int, int, int> parseIBDevicePciBdf(const std::string &ibDevName) {
  std::string devicePath = fmt::format("/sys/class/infiniband/{}/device", ibDevName);
  char buf[PATH_MAX] = {};
  ssize_t len = readlink(devicePath.c_str(), buf, sizeof(buf) - 1);
  if (len <= 0) {
    return {-1, -1, -1};
  }
  // The symlink target basename is the PCI BDF, e.g. "0000:3b:00.0"
  std::string target(buf, len);
  auto pos = target.rfind('/');
  std::string bdf = (pos != std::string::npos) ? target.substr(pos + 1) : target;

  int domain = 0, bus = 0, dev = 0, func = 0;
  if (sscanf(bdf.c_str(), "%x:%x:%x.%x", &domain, &bus, &dev, &func) >= 3) {
    return {domain, bus, dev};
  }
  return {-1, -1, -1};
}

// Compute affinity score between a GPU and an IB device.
// Higher score = closer topology = better affinity.
//   3: same PCI domain + same bus (on the same PCIe switch)
//   2: same PCI domain, different bus
//   1: different domain but same NUMA node
//   0: no known affinity
int computeAffinityScore(const AcceleratorDeviceInfo &gpu, const IBDeviceTopology &ib) {
  if (gpu.pciDomainId >= 0 && ib.pciDomain >= 0 && gpu.pciDomainId == ib.pciDomain) {
    if (gpu.pciBusId >= 0 && ib.pciBus >= 0 && gpu.pciBusId == ib.pciBus) {
      return 3;  // Same PCIe switch
    }
    return 2;  // Same domain, different bus
  }
  if (gpu.numaNode >= 0 && ib.numaNode >= 0 && gpu.numaNode == ib.numaNode) {
    return 1;  // Same NUMA node
  }
  return 0;
}

}  // namespace

Result<Void> GDRManager::setupGpuIBMapping() {
  // Setup mapping between GPU devices and IB devices based on PCIe topology.
  // For each GPU, find the IB device with the shortest PCIe path by comparing
  // PCI domain/bus (same PCIe switch) and NUMA node from sysfs.

  const auto &ibDevices = IBDevice::all();
  if (ibDevices.empty()) {
    XLOGF(WARN, "No IB devices available for GPU-IB mapping");
    return Void{};
  }

  // Resolve topology for each IB device from sysfs
  std::vector<IBDeviceTopology> ibTopo;
  ibTopo.reserve(ibDevices.size());
  bool hasTopology = false;

  for (const auto &ibDev : ibDevices) {
    IBDeviceTopology topo;
    topo.devId = ibDev->id();
    topo.name = ibDev->name();

    auto [domain, bus, dev] = parseIBDevicePciBdf(ibDev->name());
    topo.pciDomain = domain;
    topo.pciBus = bus;
    topo.pciDevice = dev;

    if (domain >= 0) {
      // Read NUMA node from the PCI device sysfs entry
      std::string numaPath = fmt::format("/sys/bus/pci/devices/{:04x}:{:02x}:{:02x}.0/numa_node", domain, bus, dev);
      topo.numaNode = readSysfsInt(numaPath);
      hasTopology = true;
    }

    XLOGF(DBG,
          "IB device {} PCI {:04x}:{:02x}:{:02x} NUMA {}",
          topo.name,
          std::max(0, topo.pciDomain),
          std::max(0, topo.pciBus),
          std::max(0, topo.pciDevice),
          topo.numaNode);
    ibTopo.push_back(std::move(topo));
  }

  // Read GPU NUMA nodes from sysfs if not already populated
  for (auto &gpu : gpuDevices_) {
    if (gpu.numaNode < 0 && gpu.pciBusId >= 0) {
      std::string numaPath = fmt::format("/sys/bus/pci/devices/{}/numa_node", gpu.pciBdf());
      gpu.numaNode = readSysfsInt(numaPath);
    }
  }

  // For each GPU, pick the IB device with the best affinity score
  for (const auto &gpu : gpuDevices_) {
    int bestScore = -1;
    uint8_t bestIbId = ibTopo[0].devId;

    for (const auto &ib : ibTopo) {
      int score = hasTopology ? computeAffinityScore(gpu, ib) : -1;
      if (score > bestScore) {
        bestScore = score;
        bestIbId = ib.devId;
      }
    }

    gpuToIBMapping_[gpu.deviceId] = bestIbId;
    XLOGF(INFO, "GPU {} (PCI {}) → IB device {} (score {})", gpu.deviceId, gpu.pciBdf(), bestIbId, bestScore);
  }

  if (!hasTopology && !gpuDevices_.empty()) {
    // Fallback: round-robin when no sysfs topology is available
    XLOGF(WARN, "No PCIe topology from sysfs, using round-robin GPU-IB mapping");
    for (size_t i = 0; i < gpuDevices_.size(); ++i) {
      gpuToIBMapping_[gpuDevices_[i].deviceId] = ibDevices[i % ibDevices.size()]->id();
    }
  }

  XLOGF(INFO, "GPU-IB mapping established for {} GPU(s)", gpuToIBMapping_.size());
  return Void{};
}

// detectMemoryType implementation

MemoryType detectMemoryType(const void *ptr) {
  if (!ptr) {
    return MemoryType::Unknown;
  }

#ifdef HF3FS_GDR_ENABLED
  cudaPointerAttributes attrs;
  cudaError_t err = cudaPointerGetAttributes(&attrs, ptr);

  if (err != cudaSuccess) {
    // Not a CUDA-known pointer, treat as host memory
    cudaGetLastError();  // Clear the error
    return MemoryType::Host;
  }

  switch (attrs.type) {
    case cudaMemoryTypeDevice:
      return MemoryType::Device;
    case cudaMemoryTypeManaged:
      return MemoryType::Managed;
    case cudaMemoryTypeHost:
      // Check if it's pinned (page-locked) host memory
      if (attrs.devicePointer != nullptr) {
        return MemoryType::Pinned;
      }
      return MemoryType::Host;
    default:
      return MemoryType::Host;
  }
#else
  // Without GDR support, all pointers are treated as host memory
  return MemoryType::Host;
#endif
}

}  // namespace hf3fs::net
