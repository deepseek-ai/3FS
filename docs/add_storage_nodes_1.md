# Add New Storage Nodes to a Chain Table -- 1

In this article, we will show you how to add a new **storage node** to a deployed 3FS cluster.

## Reasons and Methods For Expanding a Cluster

There are some reasons you want to add new storage services to a 3FS cluster:

1. Capacity utilization is high.
1. Cluster performance hits bottleneck.
1. Replacing broken nodes.

There are 3 ways to increase the storage capacity or performance limit:

1. **Add new storage nodes to a chain table**. This will increase both the capacity and performance limit, and all data accessing to existing files will be benefited.
2. **Add new disks to every node in a chain table**. This will increase capacity. But it won't be benefit to performance if current performance bottleneck is storage networking.
3. **Add a new chain table consists of new storage nodes**. This will increase both the capacity and performance limit, but the new nodes only serve newly created directories.

Adding new storage nodes to a chain table fits most scenarios in production. The reasons are:

1. Applications are mostly designed to created new files in pre-configured directories.
1. As the applications scaling out, networking performance requirement increase simultaneously.
1. When a cluster deployed, engineers usually use up all the hardwares capacity, e.g. PCI-e slots.

In the following parts, we will describe the procedure to adding new storage nodes to a chain table.

## Adding New Storage Nodes to a Chain Table

In this chapter, we assume you have deployed a 3FS cluster with [m3fs](https://github.com/open3fs/m3fs). All the file path used are according to m3fs's design. This method also applicable even if you deployed 3FS cluster with other methods.

### Example Deployed Cluster

Here is a two-node cluster:

- **10.0.0.1**: monitor + mgmtd + meta + storage
- **10.0.0.2**: storage

`list-nodes` output:

```bash
$ /opt/3fs/admin_cli.sh list-nodes
Id     Type     Status               Hostname                 Pid  Tags  LastHeartbeatTime    ConfigVersion  ReleaseVersion
1      MGMTD    PRIMARY_MGMTD        iZwz9e4meqsilq7pd7gs5iZ  1    []    N/A                  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
100    META     HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5iZ  1    []    2025-06-03 02:56:37  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
10001  STORAGE  HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5iZ  1    []    2025-06-03 02:56:43  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
10002  STORAGE  HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5jZ  1    []    2025-06-03 02:56:43  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
```

Now we want to add a new node **10.0.0.3** to this chain table.

### Procedure Overview

1. Prepare new storage nodes.
1. Create new storage services.
1. Generate new data placement policy.
1. Migrate targets.

### Prepare New Storage Nodes

You should prepare new storage nodes firstly. And here are some requirements for the new nodes:

1. New nodes should have same disks setup as current nodes. Because 3FS enforces all storage nodes to use the same configuration, so the `target_paths` value is same on every storage node. Although you can disregard for performance and capacity consideration by only providing same target paths to storage service without backing them with appropriate disks, this is not recommended for production.
1. New nodes should have same or better networking setup. According to 3FS's design, FUSE clients assume every storage nodes can provide same IO bandwidth.
1. New nodes should have same or better CPU and memory setup.
1. New nodes should use same OS.

### Create New Storage Services

Because m3fs has not support adding new storage service yet, we need to do these steps manually. All these steps are executed on the new storage node 10.0.0.3 if not mentioned specially.

First, install docker:

```bash
apt -y update && apt install docker.io
```

Then, configure RDMA if necessary. Because we run this cluster on Aliyun's eRDMA instances, we need to configure like this:

```bash
rmmod erdma && modprobe erdma compat_mode=1
```

If you are using physical RDMA cards, you don't expect to do any configuration. If you are using RXE (soft RDMA), you may need to do like this:

```bash
root@open3fs-node2:/root# rdma link add eth0_rxe0 type rxe netdev eth0
root@open3fs-node2:/root# rdma link
link eth0_rxe0/1 state ACTIVE physical_state LINK_UP netdev eth0
```

Now, prepare binaries and configurations.

```bash
mkdir -p /opt/3fs/bin
scp 10.0.0.1:/opt/3fs/bin/* /opt/3fs/bin/

mkdir -p /opt/3fs/storage/{config.d,3fsdata/data0/3fs,log}
scp 10.0.0.1:/opt/3fs/storage/config.d/* /opt/3fs/storage/config.d/
```

Set a new `node_id` and assure this is unique and consecutive:

```bash
$ cat /opt/3fs/storage/config.d/storage_main_app.toml
allow_empty_node_id = true
node_id = 10003
```

Using **runlike** command to generate `docker run` command on any existing node:

```bash
$ runlike -p 3fs-storage
docker run --name=3fs-storage \
        --hostname=iZwz9e4meqsilq7pd7gs5iZ \
        --volume /opt/3fs/storage/log:/var/log/3fs \
        --volume /opt/3fs/storage/3fsdata:/mnt/3fsdata \
        --volume /opt/3fs/bin/ibdev2netdev:/usr/sbin/ibdev2netdev \
        --volume /etc/libibverbs.d/erdma.driver:/etc/libibverbs.d/erdma.driver \
        --volume /usr/lib/x86_64-linux-gnu/libibverbs/liberdma-rdmav34.so:/usr/lib/x86_64-linux-gnu/libibverbs/liberdma-rdmav34.so \
        --volume /dev:/dev \
        --volume /opt/3fs/storage/config.d:/opt/3fs/etc \
        --network=host \
        --privileged \
        --runtime=runc \
        --detach=true \
        open3fs/3fs:20250410 \
        /opt/3fs/bin/storage_main --launcher_cfg /opt/3fs/etc/storage_main_launcher.toml --app_cfg /opt/3fs/etc/storage_main_app.toml
```

Start storage service on new node 10.0.0.3 with above command. **Change the hostname argument to a unique hostname in the cluster**:

```bash
docker run --name=3fs-storage \
        --hostname=iZwz9e4meqsilq7pd7gs5kZ \ # This is unique in 3FS cluster, check with list-nodes
        --volume /opt/3fs/storage/log:/var/log/3fs \
        --volume /opt/3fs/storage/3fsdata:/mnt/3fsdata \
        --volume /opt/3fs/bin/ibdev2netdev:/usr/sbin/ibdev2netdev \
        --volume /etc/libibverbs.d/erdma.driver:/etc/libibverbs.d/erdma.driver \
        --volume /usr/lib/x86_64-linux-gnu/libibverbs/liberdma-rdmav34.so:/usr/lib/x86_64-linux-gnu/libibverbs/liberdma-rdmav34.so \
        --volume /dev:/dev \
        --volume /opt/3fs/storage/config.d:/opt/3fs/etc \
        --network=host \
        --privileged \
        --runtime=runc \
        --detach=true \
        open3fs/3fs:20250410 \
        /opt/3fs/bin/storage_main --launcher_cfg /opt/3fs/etc/storage_main_launcher.toml --app_cfg /opt/3fs/etc/storage_main_app.toml
```

Check the new storage node with `list-nodes` command:

```bash
$ /opt/3fs/admin_cli.sh list-nodes
Id     Type     Status               Hostname                 Pid  Tags  LastHeartbeatTime    ConfigVersion  ReleaseVersion
1      MGMTD    PRIMARY_MGMTD        iZwz9e4meqsilq7pd7gs5iZ  1    []    N/A                  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
100    META     HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5iZ  1    []    2025-06-03 03:46:48  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
10001  STORAGE  HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5iZ  1    []    2025-06-03 03:46:55  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
10002  STORAGE  HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5jZ  1    []    2025-06-03 03:46:54  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
10003  STORAGE  HEARTBEAT_CONNECTED  iZwz9e4meqsilq7pd7gs5kZ  1    []    2025-06-03 03:46:57  1(UPTODATE)    250228-dev-1-999999-ee9a5cee
```

### Generate New Data Placement Policy

After the new storage service online, you need to generate new data placement policy for the chain table.

We need to execute `data_placement.py` script in `3fs-mgmtd` container (node 10.0.0.1) with same `--replication_factor` and `--min_targets_per_disk` arguments (m3fs use `--replication_factor 2` and `--min_targets_per_disk 32` as default values), and providing previous model with `-m` argument (the *output/DataPlacementModel-xxx* file, you can regenerate this using same argument when you deploy the cluster). **Notice** to the `--num_nodes` argument.

```bash
docker exec 3fs-mgmtd python3 /opt/3fs/data_placement/src/model/data_placement.py \
   -ql -relax -type CR --num_nodes 3 \
   --replication_factor 2 --min_targets_per_disk 32 \
   -m output/DataPlacementModel-v_2-b_32-r_32-k_2-λ_32-lb_1-ub_0/incidence_matrix.pickle
```

This will generated a new data placement policy model:

```bash
2025-06-03 03:50:05.467 | SUCCESS  | __main__:run:148 - saved solution to: output/RebalanceTrafficModel-v_3-b_48-r_32-k_2-λ_16-lb_1-ub_0
```

You can check the data placement policy visually through the HTML file in the dir:

![Rebalanced Data Placement Policy](./images/rebalanced-placement-policy.png)

Now we can generate new chain table configuration with new model. Make sure `target_id_prefix` and `chain_id_prefix` are same with current setting.

```bash
docker exec 3fs-mgmtd python3 /opt/3fs/data_placement/src/setup/gen_chain_table.py \
   --chain_table_type CR --node_id_begin 10001 --node_id_end 10003 \
   --num_disks_per_node 1 --num_targets_per_disk 32 \
   --target_id_prefix 1 --chain_id_prefix 9 \
   --incidence_matrix_path output/RebalanceTrafficModel-v_3-b_48-r_32-k_2-λ_16-lb_1-ub_0/incidence_matrix.pickle
```

After generation, check the result as follows. Now the number of chains is increased to 48 (previous 32), each storage node still serves 16 chains, and each chain still has replicator set to 2.

```bash
$ docker exec 3fs-mgmtd cat /output/create_target_cmd.txt
create-target --node-id 10001 --disk-index 0 --target-id 101000100101 --chain-id 900100001  --use-new-chunk-engine
create-target --node-id 10002 --disk-index 0 --target-id 101000200101 --chain-id 900100001  --use-new-chunk-engine
create-target --node-id 10001 --disk-index 0 --target-id 101000100102 --chain-id 900100002  --use-new-chunk-engine
...
create-target --node-id 10003 --disk-index 0 --target-id 101000300131 --chain-id 900100047  --use-new-chunk-engine
create-target --node-id 10002 --disk-index 0 --target-id 101000200132 --chain-id 900100048  --use-new-chunk-engine
create-target --node-id 10003 --disk-index 0 --target-id 101000300132 --chain-id 900100048  --use-new-chunk-engine

$ docker exec 3fs-mgmtd cat /output/generated_chain_table.csv
ChainId
900100001
900100002
900100003
...
900100046
900100047
900100048

$ docker exec 3fs-mgmtd cat /output/generated_chains.csv
ChainId,TargetId,TargetId
900100001,101000100101,101000200101
900100002,101000100102,101000300101
900100003,101000100103,101000300102
...
900100046,101000200131,101000300130
900100047,101000100132,101000300131
900100048,101000200132,101000300132

$ docker exec 3fs-mgmtd cat /output/remove_target_cmd.txt
offline-target --node-id 10001 --target-id 101000100101
remove-target --node-id 10001 --target-id 101000100101
offline-target --node-id 10002 --target-id 101000200101
...

remove-target --node-id 10002 --target-id 101000200132
offline-target --node-id 10003 --target-id 101000300132
remove-target --node-id 10003 --target-id 101000300132
```

### Migrate Targets

Now we come to the most tricky part. We can not use the generated **create_target_cmd.txt** and **generated_chains.csv** directly. Because mgmtd is not allow to update existing chains:

```bash
$ /opt/3fs/admin_cli.sh "upload-chains /output/generated_chains.csv"
Encounter error: 5007(Mgmtd::InvalidChainTable)
Target mismatch of ChainId(900100002). old: [TargetId(101000100102),TargetId(101000200102)]. new: [TargetId(101000100102),TargetId(101000300101)]
```

To apply the new placement policy, we should manually migrate each chain to new targets, and make sure the data rebalance is finished correctly. The basic idea is:

1. Processing chains in *generated_chains.csv* from top to bottom.
1. For each chain, say chain A, checks the new targets assigned to the chain:
    1. If the target does not belongs another chain in previous placement policy, we can added it to chain A.
    1. If the target belongs to another chain B in previous placement policy, we should firstly remove it from chain B, recreate it, and then added it to chain A. But if the target is the last target of the chain B, we should process chain B firstly.


Let's take a chain as example.

```bash
$ docker exec -it 3fs-mgmtd cat /output/generated_chains.csv | head -n 17
ChainId,TargetId,TargetId
900100001,101000100101,101000200101
900100002,101000100102,101000300101
900100003,101000100103,101000300102
900100004,101000100104,101000300103
900100005,101000200102,101000300104
900100006,101000100105,101000200103
900100007,101000100106,101000300105
900100008,101000200104,101000300106
900100009,101000100107,101000300107
900100010,101000100108,101000200105
900100011,101000200106,101000300108
900100012,101000100109,101000300109
900100013,101000100110,101000300110
900100014,101000100111,101000200107
900100015,101000200108,101000300111
900100016,101000200109,101000300112
```

Above is the new generated chains. Assuming we have processed all chains above `900100005` and it is now we need to process chain `900100005`, it's targets changed from `101000100105,101000200105` to `101000200102,101000300104`. The new target `101000200102` belongs to another chain right now, while target `101000300104` is not used.

Following commands do the migration:

```bash
# Remove target 101000200105, so chain 900100005 downgrade to 1 replica.
/opt/3fs/admin_cli.sh offline-target --target-id 101000200105
/opt/3fs/admin_cli.sh update-chain --mode remove 900100005 101000200105

# Target 101000200102 belongs to chain 900100019 right now.
# Need to move target 101000200102 from chain 900100019 to 900100005.
# Firstly, remove target 101000200102 from chain 900100019
/opt/3fs/admin_cli.sh offline-target --target-id 101000200102
/opt/3fs/admin_cli.sh update-chain --mode remove 900100019 101000200102
/opt/3fs/admin_cli.sh remove-target --node-id 10002 --target-id 101000200102
# Secondly, recreate the target and add it to chain 900100005
/opt/3fs/admin_cli.sh create-target --node-id 10002 --disk-index 0 --target-id 101000200102 --chain-id 900100005  --use-new-chunk-engine
/opt/3fs/admin_cli.sh update-chain --mode add 900100005 101000200102

# Wait status of 101000200102 changed from SYNCING-ONLINE to SERVING-UPTODATE
# Now chain 900100005 consists of targets `101000100105,101000200102`

# Using offline or rotate to reorder targets in chain. Turn 101000100105 into TAIL and 101000200102 into HEAD.
/opt/3fs/admin_cli.sh offline-target --target-id 101000100105
# Delete TAIL 101000100105
/opt/3fs/admin_cli.sh update-chain --mode remove 900100005 101000100105

# Create new target 101000300104 to join chain 900100005
/opt/3fs/admin_cli.sh create-target --node-id 10003 --disk-index 0 --target-id 101000300104 --chain-id 900100005  --use-new-chunk-engine
/opt/3fs/admin_cli.sh update-chain --mode add 900100005 101000300104

# Now chain 900100005 consists of targets `101000200102,101000300104`
```

**NOTICE**: Check target status after every command execution, make sure target status are expectable.

```bash
/opt/3fs/admin_cli.sh list-chains
```

This is a error-prone procedure, so we wrote a script to generate these commands: [gen-migrate-cmds.py](https://github.com/open3fs/m3fs/blob/main/scripts/gen-migrate-cmds.py). After generating new data placement policy, run this script on mgmtd node:

```bash
python3 gen-migrate-cmds.py
```

After all chains being processed, we can apply the commands and chains output by **gen_chain_table.py**. These commands are idempotent for targets of the cluster.

```bash
docker exec -it 3fs-mgmtd cat /output/create_target_cmd.txt | /opt/3fs/admin_cli.sh
/opt/3fs/admin_cli.sh "upload-chains /output/generated_chains.csv"
/opt/3fs/admin_cli.sh "upload-chain-table --desc stage 1 /output/generated_chain_table.csv"
```

### IO Behavior When Migrating Targets

1. IO will hang up for few seconds when you adding target to a chain, removing target from a chain or offline a target.
1. Bandwidth will decrease before data rebalance finished.

## Conclusion

**Pros**:

1. Increase capacity and performance limit at same time.
1. Does not require existing nodes to have significant spare capacity, making it more practical for production environments.

**Cons**:

1. Removing targets from chains first temporarily reduces data reliability.
1. Operation is really complicated.

**NOTE: Make sure to always check chain and target status after every operation.**

## Reference

1. [Understanding 3FS data placement model -- part 1](https://blog.open3fs.com/2025/04/14/understanding-3fs-data-placement-model-part1.html)
1. [Understanding 3FS data placement model -- part 2](https://blog.open3fs.com/2025/04/28/understanding-3fs-data-placement-model-part2.html)
1. [Add a Storage Node to a Chain Table -- 1](https://blog.open3fs.com/2025/06/05/3FS-add-node-to-chaintable-1.html)
