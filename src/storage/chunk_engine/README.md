# chunk-engine

### Design

1. The entire Chunk Engine can be divided into two components:
   1. **Allocator**: Responsible for allocating/reclaiming chunks and modifying memory states.
   2. **MetaStore**: Responsible for persisting allocation/reclamation events.
2. Workflow for writing a new chunk:
   1. The **Allocator** assigns a new chunk position, pointing to a disk space (purely in-memory operation).
   2. Write data to this chunk position. If a power failure or write failure occurs at this stage, no existing data is affected.
   3. Generate corresponding chunk metadata and persist it alongside the allocation event to the **MetaStore**. Using RocksDB's WriteBatch ensures **atomic** updates—the entire write operation either succeeds or fails, with no intermediate states.
3. Maintaining the Allocator's in-memory state:
   1. At startup, the Allocator **quickly** loads all allocation information from RocksDB.
   2. Allocation is performed in-memory first, followed by persistence. If a failure occurs before persistence, the allocation event is lost.
   3. Reclamation first persists the event to disk, then modifies the memory state. Even if a chunk deletion event is persisted, the chunk remains readable as long as memory holds its reference.
   4. This ensures conflict-free read/write operations: a read operation acquires a chunk reference, guaranteeing the chunk's validity until the read completes.
4. Use `Arc` to manage ownership of chunk position:
   1. For allocation, returns an `Arc<ChunkPos>`. If persistence fails, the position is automatically released when the `Arc` is dropped.
   2. Read operations also return an `Arc<ChunkPos>`, ensuring safe data access even during concurrent writes or deletions.

### Allocator

Storage hierarchy:

1. **Chunk**: Basic data unit, currently proposed as 64KB, 512KB, and 4MB.
2. **Group**: Each group contains 256 chunks (16MB, 128MB, or 1GB depending on chunk size).
3. **File**: For 512KB chunks, a single file (~120GB) contains ~960 groups.
4. **Disk**: Single disk capacity of 30TB, divided into 256 files per chunk size.
5. **Node**: A single node contains 10–20 disks.

This configuration supports up to ~1.2 billion chunks and ~5 million groups per machine.

Implementation details:
1. Each group uses a 256-bit bitset (4 `uint64_t`) to track allocation status.
2. Maintain three in-memory structures:
   - `allocated_groups`: Groups with allocated space but no chunks assigned.
   - `unallocated_groups`: Groups without allocated space.
   - `active_groups`: Map of `<group_id, group_state>` tracking allocation status.
3. Chunk allocation workflow:
   1. Prioritize finding free slots in `active_groups` using **`__builtin_ctz`** for fast bitwise operations.
   2. If `active_groups` is empty, acquire a new group from `allocated_groups`.
   3. If `allocated_groups` is empty, fetch a group from `unallocated_groups` and allocate disk space synchronously.
4. Background threads:
   - **`allocate_thread`**: Maintains `active_groups` within a target size range to ensure in-memory allocation efficiency.
   - **`compact_thread`**: Periodically scans `active_groups`, migrates all chunks from selected groups, releases space, and returns groups to `allocated_groups`.

### MetaStore

Persists three mappings:
1. **`chunk_id -> chunk_meta`**: Metadata includes chunk location, length, hash, version, etc., serialized using **`derse`**.
2. **`group_id -> group_state`**: Tracks chunk allocation status within groups, leveraging RocksDB's **MergeOp** for atomic updates.
3. **`chunk_pos -> chunk_id`**: Maps physical positions to chunk IDs, used by `compact_thread` during chunk migration.

### Chunk Engine

1. **MetaCache**: Maintains an in-memory `chunk_id -> chunk_info` mapping, where `chunk_info` includes `chunk_meta` and `Arc<ChunkPos>`.
2. **Read operation**: Returns `chunk_info`. The `Arc<ChunkPos>` ensures safe data access until the read completes.
3. **Write operation workflow**:
   1. Query `MetaCache` to retrieve the current `chunk_info`.
   2. Invoke `Allocator::allocate()` to obtain a new chunk position.
   3. Read existing chunk data, write it to the new chunk position, append the new write request, and generate `new_chunk_info`.
   4. Persist `new_chunk_info` to the **MetaStore** along with a release record for the original chunk position.
