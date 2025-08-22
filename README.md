#  Fire-Flyer File System

[![Build](https://github.com/deepseek-ai/3fs/actions/workflows/build.yml/badge.svg)](https://github.com/deepseek-ai/3fs/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/LICENSE-MIT-blue.svg)](LICENSE)

The Fire-Flyer File System (3FS) is a high-performance distributed file system designed to address the challenges of AI training and inference workloads. It leverages modern SSDs and RDMA networks to provide a shared storage layer that simplifies development of distributed applications. Key features and benefits of 3FS include:

- Performance and Usability
  - **Disaggregated Architecture** Combines the throughput of thousands of SSDs and the network bandwidth of hundreds of storage nodes, enabling applications to access storage resource in a locality-oblivious manner.
  - **Strong Consistency** Implements Chain Replication with Apportioned Queries (CRAQ) for strong consistency, making application code simple and easy to reason about.
  - **File Interfaces** Develops stateless metadata services backed by a transactional key-value store (e.g., FoundationDB). The file interface is well known and used everywhere. There is no need to learn a new storage API.

- Diverse Workloads
  - **Data Preparation** Organizes outputs of data analytics pipelines into hierarchical directory structures and manages a large volume of intermediate outputs efficiently.
  - **Dataloaders** Eliminates the need for prefetching or shuffling datasets by enabling random access to training samples across compute nodes.
  - **Checkpointing** Supports high-throughput parallel checkpointing for large-scale training.
  - **KVCache for Inference** Provides a cost-effective alternative to DRAM-based caching, offering high throughput and significantly larger capacity.

## Documentation

* [Design Notes](docs/design_notes.md)
* [Setup Guide](deploy/README.md)
* [USRBIO API Reference](src/lib/api/UsrbIo.md)
* [P Specifications](./specs/README.md)

## Performance

### 1. Peak throughput

The following figure demonstrates the throughput of read stress test on a large 3FS cluster. This cluster consists of 180 storage nodes, each equipped with 2×200Gbps InfiniBand NICs and sixteen 14TiB NVMe SSDs. Approximately 500+ client nodes were used for the read stress test, with each client node configured with 1x200Gbps InfiniBand NIC. The final aggregate read throughput reached approximately 6.6 TiB/s with background traffic from training jobs.

![Large block read throughput under stress test on a 180-node cluster](docs/images/peak_throughput.jpg)

To benchmark 3FS, please use our [fio engine for USRBIO](benchmarks/fio_usrbio/README.md).

### 2. GraySort

We evaluated [smallpond](https://github.com/deepseek-ai/smallpond) using the GraySort benchmark, which measures sort performance on large-scale datasets. Our implementation adopts a two-phase approach: (1) partitioning data via shuffle using the prefix bits of keys, and (2) in-partition sorting. Both phases read/write data from/to 3FS.

The test cluster comprised 25 storage nodes (2 NUMA domains/node, 1 storage service/NUMA, 2×400Gbps NICs/node) and 50 compute nodes (2 NUMA domains, 192 physical cores, 2.2 TiB RAM, and 1×200 Gbps NIC/node). Sorting 110.5 TiB of data across 8,192 partitions completed in 30 minutes and 14 seconds, achieving an average throughput of *3.66 TiB/min*.

![](docs/images/gray_sort_server.png)
![](docs/images/gray_sort_client.png)

### 3. KVCache

KVCache is a technique used to optimize the LLM inference process. It avoids redundant computations by caching the key and value vectors of previous tokens in the decoder layers.
The top figure demonstrates the read throughput of all KVCache clients (1×400Gbps NIC/node), highlighting both peak and average values, with peak throughput reaching up to 40 GiB/s. The bottom figure presents the IOPS of removing ops from garbage collection (GC) during the same time period.

![KVCache Read Throughput](./docs/images/kvcache_read_throughput.png)
![KVCache GC IOPS](./docs/images/kvcache_gc_iops.png)

## Check out source code

Clone 3FS repository from GitHub:

	git clone https://github.com/deepseek-ai/3fs

When `deepseek-ai/3fs` has been cloned to a local file system, run the
following commands to check out the submodules:

```bash
cd 3fs
git submodule update --init --recursive
./patches/apply.sh
```

## Install dependencies

Install dependencies:

```bash
# for Ubuntu 20.04.
apt install cmake libuv1-dev liblz4-dev liblzma-dev libdouble-conversion-dev libdwarf-dev libunwind-dev \
  libaio-dev libgflags-dev libgoogle-glog-dev libgtest-dev libgmock-dev clang-format-14 clang-14 clang-tidy-14 lld-14 \
  libgoogle-perftools-dev google-perftools libssl-dev libclang-rt-14-dev gcc-10 g++-10 libboost1.71-all-dev build-essential

# for Ubuntu 22.04.
apt install cmake libuv1-dev liblz4-dev liblzma-dev libdouble-conversion-dev libdwarf-dev libunwind-dev \
  libaio-dev libgflags-dev libgoogle-glog-dev libgtest-dev libgmock-dev clang-format-14 clang-14 clang-tidy-14 lld-14 \
  libgoogle-perftools-dev google-perftools libssl-dev gcc-12 g++-12 libboost-all-dev build-essential

# for openEuler 2403sp1
yum install cmake libuv-devel lz4-devel xz-devel double-conversion-devel libdwarf-devel libunwind-devel \
    libaio-devel gflags-devel glog-devel gtest-devel gmock-devel clang-tools-extra clang lld \
    gperftools-devel gperftools openssl-devel gcc gcc-c++ boost-devel

# for OpenCloudOS 9 and TencentOS 4
dnf install epol-release wget git meson cmake perl lld gcc gcc-c++ autoconf lz4 lz4-devel xz xz-devel \
    double-conversion-devel libdwarf-devel libunwind-devel libaio-devel gflags-devel glog-devel \
    libuv-devel gmock-devel gperftools gperftools-devel openssl-devel boost-static boost-devel mono-devel \
    libevent-devel libibverbs-devel numactl-devel python3-devel
```

Install other build prerequisites:

- [`libfuse`](https://github.com/libfuse/libfuse/releases/tag/fuse-3.16.1) 3.16.1 or newer version
- [FoundationDB](https://apple.github.io/foundationdb/getting-started-linux.html) 7.1 or newer version
- [Rust](https://www.rust-lang.org/tools/install) toolchain: minimal 1.75.0, recommended 1.85.0 or newer version (latest stable version) 

## Build 3FS

- Build 3FS in `build` folder:

    ```
    cmake -S . -B build -DCMAKE_CXX_COMPILER=clang++-14 -DCMAKE_C_COMPILER=clang-14 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    cmake --build build -j 32
    ```
- Build 3FS use Docker
  - For TencentOS-4:  `docker pull docker.io/tencentos/tencentos4-deepseek3fs-build:latest`
  - For OpenCloudOS-9:  `docker pull docker.io/opencloudos/opencloudos9-deepseek3fs-build:latest`
  
## Run a test cluster

Follow instructions in [setup guide](deploy/README.md) to run a test cluster.

## Report Issues

Please visit https://github.com/deepseek-ai/3fs/issues to report issues.
