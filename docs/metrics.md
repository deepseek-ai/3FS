# Metric System of 3FS

## Type of Metrics

3FS calculates metrics in each service and store metrics to ClickHouse through monitor service. There are some types of metrics in 3FS, and the metric may or may not be reset after each report.

There are many types of metrics:

| Type | Class | Storage Table | Description |
|------|-------|---------------|-------------|
| value | `ValueRecorder` | 3fs.counters | Set by code and stored to ClickHouse directly. E.g. capacity |
| count | `CountRecorder` | 3fs.counters | Increase by code and usually reset to zero after each report. E.g., report read counter every second mean read IOPS.<br><br>It is also be used to track current on-flight operations, which is increased at the beginning and is decreased after the operation finished, rather than reset automatically after report. |
| distribution | `DistributionRecorder` | 3fs.distributions | Used to calculate P90, P99 and etc for the period between two reports. |
| latency | `LatencyRecorder` | 3fs.distributions | Special distribution implementation, record latency with unit ns (nanosecond). |

## List of Metrics

Following is a partial list of the metrics and their meanings:

| Metric Name | Type         | Reset After Report | Description |
|-----------------------------------------|--------------|--------------------|-------------|
| fuse.dirty_inodes                       | value        | Y | Current number of dirty inodes |
| fuse.op                                 | count      | Y | Number of client operations; instance field is the operation name |
| fuse.piov.bw                            | count      | Y | Number of bytes written by the fuse client to the storage service |
| fuse.write.latency                      | latency      | Y | Write operation latency |
| fuse.write.size                         | distribution | Y | Write operation size |
| meta_server.batch_op_size               | count      | Y | Batch operation count of metadata (sync + close + setattr) |
| meta_server.dist_set_map                | count      | Y | Number of set disk server map operations |
| meta_server.op_code                     | count      | Y | Number of times a specific error code appears, tag is the specific error code |
| meta_server.op_failed                   | count      | Y | Number of failed operations |
| meta_server.op_idempotent               | count      | Y | Number of idempotent operations executed |
| meta_server.op_duplicated               | count      | Y | Number of duplicated operations |
| meta_server.op_running                  | count      | N | Number of running operations |
| meta_server.op_total                    | count      | Y | Total number of operations |
| meta_server.op_latency                  | latency      | Y | Metadata operation latency (nanoseconds) |
| meta_server.open_write                  | count      | Y | Number of open/create file operations |
| meta_server.stat_dir                    | count      | Y | Number of directory stat operations executed |
| meta_server.stat_file                   | count      | Y | Number of file stat operations executed |
| meta_server.stat_symlink                | count      | Y | Number of symbolic link stat operations executed |
| meta_server.auth_failed                 | count      | Y | Number of authentication failures |
| storage.check_disk.current              | count      | N | Number of currently running disk check operations |
| storage.check_disk.total                | count      | Y | Total number of disk check operations |
| storage.chunk_engine.<br>allocate_times     | count      | Y | Number of chunks created |
| storage.chunk_engine.<br>checksum_combine   | count      | Y | Checksum and merge count (includes checksum and merge, and append operations) |
| storage.chunk_engine.<br>checksum_recalculate | count  | Y | Checksum recalculation count |
| storage.chunk_engine.<br>copy_on_write_read_bytes | count | Y | Number of bytes read by COW (Copy-on-Write) |
| storage.chunk_engine.<br>copy_on_write_read_times | count | Y | Number of times COW read data |
| storage.chunk_engine.<br>copy_on_write_times| count    | Y | Number of COW executions |
| storage.chunk_engine.new                | value        | N | Current number of chunk_engines? (Need to check the code) |
| storage.chunk_engine.<br>pwrite_times       | count      | Y | Number of pwrite calls |
| storage.chunk_engine.<br>safe_write_direct_append | count | Y | Number of direct append writes during safe_write |
| storage.chunk_engine.<br>safe_write_indirect_append | count | Y | Number of indirect append writes during safe_write |
| storage.chunk_remove.times              | count      | Y | Number of chunk delete operations |
| storage.chunk_write.times               | count      | Y | Number of chunk write operations |
| storage.disk_info.available             | value        | N | Disk available capacity (target unused space + reserved space + file system available space, bytes) |
| storage.disk_info.capacity              | value        | N | Total file system capacity (bytes) |
| storage.disk_info.read_only             | value        | N | Whether the file system is read-only (1: read-only, implemented by writing .hf3fs_check file in the file system root directory) |
| storage.disk_info.free                  | value        | N | File system available space (bytes) |
| storage.do_commit.current               | count      | N | Number of IOs currently being committed |
| storage.do_commit.total                 | count      | Y | Total number of committed IOs |
| storage.do_commit.fails                 | count      | Y | Number of failed commit IOs |
| storage.do_commit.succ_latency          | latency      | Y | Success commit latency |
| storage.do_commit.fail_latency          | latency      | Y | Failure commit latency |
| storage.do_query.num_chunks             | count      | Y | Number of chunks in query results |
| storage.do_query.total                  | count      | Y | Total number of chunk query operations |
| storage.do_query.current                | count      | N | Number of current chunk query operations |
| storage.do_query.succ_latency           | latency      | Y | Success query latency |
| storage.do_query.fail_latency           | latency      | Y | Failure query latency |
| storage.do_remove.current               | count      | N | Number of chunk remove operations |
| storage.do_remove.fails                 | count      | Y | Number of failed chunk remove operations |
| storage.do_remove.num_chunks            | count      | Y | Number of chunks removed |
| storage.do_remove.total                 | count      | Y | Total number of chunk remove operations |
| storage.do_remove.succ_latency          | latency      | Y | Success remove latency |
| storage.do_remove.fail_latency          | latency      | Y | Failure remove latency |
| storage.do_update.current               | count      | N | Number of current update chunk operations |
| storage.do_update.total                 | count      | Y | Total number of update chunk operations |
| storage.do_update.fails                 | count      | Y | Number of failed update chunk operations |
| storage.do_update.succ_latency          | latency      | Y | Success update latency |
| storage.do_update.fail_latency          | latency      | Y | Failure update latency |
| storage.remove_range.current            | count      | N | Number of current range remove chunk operations |
| storage.remove_range.fails              | count      | Y | Number of failed range remove chunk operations |
| storage.remove_range.total              | count      | Y | Total number of range remove chunk operations |
| storage.remove_range.succ_latency       | latency      | Y | Success remove range latency |
| storage.remove_range.fail_latency       | latency      | Y | Failure remove range latency |
| storage.req_remove_chunks.current       | count      | N | Number of current remove chunk requests |
| storage.req_remove_chunks.total         | count      | Y | Total number of remove chunk requests |
| storage.req_update.bytes                | count      | Y | Number of chunk update bytes |
| storage.req_update.current              | count      | N | Number of current chunk update requests |
| storage.req_update.fails                | count      | Y | Number of failed chunk update requests |
| storage.req_update.total                | count      | Y | Total number of chunk update requests |
| storage.req_write.bytes                 | count      | Y | Number of bytes written by chunk write requests |
| storage.req_write.current               | count      | N | Number of current chunk write requests |
| storage.req_write.total                 | count      | Y | Total number of chunk write requests |
| storage.engine_commit.current           | count      | N | Number of current commit operations |
| storage.engine_commit.total             | count      | Y | Total number of commit operations |
| storage.engine_commit.fails             | count      | Y | Number of failed commit operations |
| storage.engine_update.current           | count      | N | Number of current chunk update operations |
| storage.engine_update.total             | count      | Y | Total number of chunk update operations |
| storage.engine_update.fails             | count      | Y | Number of failed chunk update operations |
| storage.forward.write_bytes             | count      | Y | Number of forwarded write bytes |
| storage.forward.syncing_bytes           | count      | Y | Number of forwarded sync bytes |
| storage.reliable_forward.current        | count      | N | Number of current forward operations |
| storage.reliable_forward.total          | count      | Y | Total number of forward operations |
| storage.reliable_forward.fails          | count      | Y | Number of failed forward operations |
| storage.target.used_size                | value        | N | Target used space (bytes) |
| storage.target.reserved_size            | value        | N | Target reserved space (bytes) |
| storage.target.unrecycled_size          | value        | N | Target unrecycled space (bytes) |
| storage.target_state                    | value        | N | Target state (0:invalid 1:uptodate 2:online 4:offline) |
| storage.write.bytes                     | count      | Y | Total bytes written (includes write and update operations) |
