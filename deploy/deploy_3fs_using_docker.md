# 3FS Docker Deployment Guide

## Table of Contents
1. [Prerequisites](#prerequisites)
2. [Build Docker Images](#build-docker-images)
3. [Deployment Steps](#deployment-steps)
   - [ClickHouse Service](#clickhouse-service)
   - [FoundationDB Service](#foundationdb-service)
   - [Monitor Service](#monitor-service)
   - [Management Service](#management-service)
   - [Metadata Service](#metadata-service)
   - [Storage Services](#storage-services)
4. [Post-Deployment Configuration](#post-deployment-configuration)
5. [FUSE Client Setup](#fuse-client-setup)

## Prerequisites <a name="prerequisites"></a>

### Hardware Requirements
| Node      | OS           | IP Address   | Memory  | Storage          | Networking | RDMA port|
|-----------|--------------|--------------|---------|------------------|------------|----------|
| Meta      | Ubuntu 20.04 | 192.168.1.1  | 128GB   | -                | RoCE       | mlx5_2   |
| Storage1  | Ubuntu 20.04 | 192.168.1.2  | 128GB   | 2×7TB NVMe SSD   | RoCE       | mlx5_2   |
| Storage2  | Ubuntu 20.04 | 192.168.1.3  | 128GB   | 2×7TB NVMe SSD   | RoCE       | mlx5_2   |

> **RDMA Configuration**
> 1. Assign IP addresses to RDMA NICs. Multiple RDMA NICs (InfiniBand or RoCE) are supported on each node.
> 2. Check RDMA connectivity between nodes using `ib_write_bw`.


## Build Docker Images <a name="build-docker-images"></a>

1. Clone repository and checkout source code:
   ```bash
   git clone https://github.com/your-org/3fs.git
   cd 3fs
   git submodule update --init --recursive
   ./patches/apply.sh
   ```

2. Build base environment:
   ```bash
   ./build.sh docker-ubuntu2004
   ```

3. Build component images:
   ```bash
   docker build -t hf3fs-monitor -f dockerfile/monitor.ubuntu2004.Dockerfile .
   docker build -t hf3fs-mgmtd -f dockerfile/mgmtd.ubuntu2004.Dockerfile .
   docker build -t hf3fs-meta -f dockerfile/meta.ubuntu2004.Dockerfile .
   docker build -t hf3fs-storage -f dockerfile/storage.ubuntu2004.Dockerfile .
   docker build -t hf3fs-fuse -f dockerfile/fuse.ubuntu2004.Dockerfile .
   ```

## Deployment Steps <a name="deployment-steps"></a>

### ClickHouse Service <a name="clickhouse-service"></a>
```bash
# Pull official image
docker pull clickhouse/clickhouse-server:25.3.1.2703

# Start container
docker run -d \
  -p19000:9000 \
  -e CLICKHOUSE_PASSWORD=3fs \
  --name clickhouse-server \
  --ulimit nofile=262144:262144 \
  clickhouse/clickhouse-server:25.3.1.2703

# Copy 3fs-monitor.sql to clickhouse-server docker container
docker cp 3fs-monitor.sql  clickhouse-server:/root

# Login clickhouse-server docker container and import the SQL file into ClickHouse:
docker exec -it clickhouse-server /bin/bash
clickhouse-client -n < /root/3fs-monitor.sql
```

### FoundationDB Service <a name="foundationdb-service"></a>
```bash
# Pull official image
docker pull foundationdb/foundationdb:7.3.63

# Start container
docker run -d \
  --privileged \
  --network host \
  -e FDB_NETWORKING_MODE=host \
  --name foundationdb-server \
  foundationdb/foundationdb:7.3.63

# Configure FoundationDB
docker exec -it foundationdb-server /bin/sh -c "echo 'export PUBLIC_IP=192.168.1.1' > /var/fdb/.fdbenv"
docker restart foundationdb-server

# Initialize database
docker exec foundationdb-server /usr/bin/fdbcli -C /var/fdb/fdb.cluster --exec 'configure new single ssd'

# Check status
docker exec foundationdb-server /usr/bin/fdbcli -C /var/fdb/fdb.cluster --exec 'status'
```

### Monitor Service <a name="monitor-service"></a>
```bash
docker run --name hf3fs-monitor \
  --privileged \
  --network host \
  -d --restart always \
  --env CLICKHOUSE_DB=3fs \
  --env CLICKHOUSE_HOST=192.168.1.1 \
  --env CLICKHOUSE_PASSWD=3fs \
  --env CLICKHOUSE_PORT=19000 \
  --env CLICKHOUSE_USER=default \
  --env DEVICE_FILTER=mlx5_2 \
  hf3fs-monitor:latest
```

### Management Service <a name="management-service"></a>
```bash
docker run --name hf3fs-mgmtd \
  --privileged \
  --network host \
  -d --restart always \
  --env CLUSTER_ID=stage \
  --env FDB_CLUSTER=docker:docker@192.168.1.1:4500 \
  --env MGMTD_SERVER_ADDRESSES=RDMA://192.168.1.1:8000 \
  --env MGMTD_NODE_ID=1 \
  --env DEVICE_FILTER=mlx5_2 \
  --env REMOTE_IP=192.168.1.1:10000 \
  hf3fs-mgmtd:latest
```

### Metadata Service <a name="metadata-service"></a>
```bash
docker run --name hf3fs-meta \
  --privileged \
  -d --restart always \
  --network host \
  --env CLUSTER_ID=stage \
  --env FDB_CLUSTER=docker:docker@192.168.1.1:4500 \
  --env MGMTD_SERVER_ADDRESSES=RDMA://192.168.1.1:8000 \
  --env META_NODE_ID=100 \
  --env DEVICE_FILTER=mlx5_2 \
  --env REMOTE_IP=192.168.1.1:10000 \
  hf3fs-meta:latest
```

### Storage Services <a name="storage-services"></a>

#### Storage Node Preparation
```bash
# Format and mount SSDs
mkdir -p /storage/data{0..1}
for i in {0..1}; do
  mkfs.xfs -L data${i} -s size=4096 /dev/nvme${i}n1
  mount -o noatime,nodiratime -L data${i} /storage/data${i}
  mkdir -p /storage/data${i}/3fs
done
```

#### Storage1 Deployment
```bash
docker run --name hf3fs-storage \
  --privileged \
  -d --restart always \
  --network host \
  -v /storage:/storage \
  --env CLUSTER_ID=stage \
  --env FDB_CLUSTER=docker:docker@192.168.1.1:4500 \
  --env MGMTD_SERVER_ADDRESSES=RDMA://192.168.1.1:8000 \
  --env STORAGE_NODE_ID=10001 \
  --env TARGET_PATHS='/storage/data0/3fs,/storage/data1/3fs' \
  --env DEVICE_FILTER=mlx5_2 \
  --env REMOTE_IP=192.168.1.1:10000 \
  hf3fs-storage:latest
```

#### Storage2 Deployment
```bash
docker run --name hf3fs-storage \
  --privileged \
  -d --restart always \
  --network host \
  -v /storage:/storage \
  --env CLUSTER_ID=stage \
  --env FDB_CLUSTER=docker:docker@192.168.1.1:4500 \
  --env MGMTD_SERVER_ADDRESSES=RDMA://192.168.1.1:8000 \
  --env STORAGE_NODE_ID=10002 \
  --env TARGET_PATHS='/storage/data0/3fs,/storage/data1/3fs' \
  --env DEVICE_FILTER=mlx5_2 \
  --env REMOTE_IP=192.168.1.1:10000 \
  hf3fs-storage:latest
```

## Post-Deployment Configuration <a name="post-deployment-configuration"></a>

1. Generate cluster configuration:
```bash
pip install -r deploy/data_placement/requirements.txt
python3 deploy/data_placement/src/model/data_placement.py \
  -ql -relax -type CR --num_nodes 2 --replication_factor 2 --min_targets_per_disk 6

python3 deploy/data_placement/src/setup/gen_chain_table.py \
  --chain_table_type CR --node_id_begin 10001 --node_id_end 10002 \
  --num_disks_per_node 2 --num_targets_per_disk 6 \
  --target_id_prefix 1 --chain_id_prefix 9 \
  --incidence_matrix_path output/DataPlacementModel-v_2-b_6-r_6-k_2-λ_6-lb_1-ub_0/incidence_matrix.pickle
```

2. Transfer configuration files:
```bash
docker cp output/create_target_cmd.txt hf3fs-mgmtd:/opt/3fs/etc/
docker cp output/generated_chains.csv hf3fs-mgmtd:/opt/3fs/etc/
docker cp output/generated_chain_table.csv hf3fs-mgmtd:/opt/3fs/etc/
```

3. Configure administrative access:

```bash
docker exec -it hf3fs-mgmtd /bin/bash
```
The admin token is printed to the console, save it to /opt/3fs/etc/token.txt.
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "user-add --root --admin 0 root"
```

4. Initialize storage targets:
```bash
/opt/3fs/bin/admin_cli --cfg /opt/3fs/etc/admin_cli.toml \
  --config.user_info.token $(<"/opt/3fs/etc/token.txt") \
  < /opt/3fs/etc/create_target_cmd.txt
```

5. Upload chain configuration:
```bash
/opt/3fs/bin/admin_cli --cfg /opt/3fs/etc/admin_cli.toml \
  --config.user_info.token $(<"/opt/3fs/etc/token.txt") \
  "upload-chains /opt/3fs/etc/generated_chains.csv"

/opt/3fs/bin/admin_cli --cfg /opt/3fs/etc/admin_cli.toml \
  --config.user_info.token $(<"/opt/3fs/etc/token.txt") \
  "upload-chain-table --desc stage 1 /opt/3fs/etc/generated_chain_table.csv"
```

6. List chains and chain tables to check if they have been correctly uploaded
```bash
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "list-chains"
/opt/3fs/bin/admin_cli -cfg /opt/3fs/etc/admin_cli.toml "list-chain-tables"
```

## FUSE Client Setup on Meta Node <a name="fuse-client-setup"></a>
```bash
docker run --name hf3fs-fuse \
  --privileged \
  -d --restart always \
  --network host \
  --env CLUSTER_ID=stage \
  --env FDB_CLUSTER=docker:docker@192.168.1.1:4500 \
  --env MGMTD_SERVER_ADDRESSES=RDMA://192.168.1.1:8000 \
  --env REMOTE_IP=192.168.1.1:10000 \
  --env DEVICE_FILTER=mlx5_2 \
  --env TOKEN=${TOKEN} \
  hf3fs-fuse:latest

# Login hf3fs-fuse Docker container
docker exec -it hf3fs-fuse /bin/bash

# Verify mount
mount | grep '/3fs/stage'
```
