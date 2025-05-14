use super::super::*;
use lockmap::{EntryByRef, LockMap};
use serde::Deserialize;
use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::ops::Deref;
use std::path::PathBuf;

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};

#[derive(Debug, Default, Deserialize)]
pub struct EngineConfig {
    pub path: PathBuf,
    pub create: bool,
    pub prefix_len: usize,
}

#[derive(Clone)]
pub struct Engine {
    pub meta_store: Arc<MetaStore>,
    pub allocators: Allocators,
    pub meta_cache: Arc<LockMap<Bytes, ChunkArc>>,
    pub workers: Arc<Mutex<Vec<Worker>>>,
    pub allow_to_allocate: Arc<AtomicBool>,
    pub metrics: Arc<Metrics>,
    pub prefix_len: usize,
    pub writing_list: Arc<WritingList>,
}

impl Engine {
    pub fn open(config: &EngineConfig) -> Result<Self> {
        let meta_config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: config.path.join("meta"),
                create: config.create,
                ..Default::default()
            },
            prefix_len: config.prefix_len,
        };

        let mut meta_store = MetaStore::open(&meta_config)?;
        let uncommitted_chunks = meta_store.occupy_uncommitted_positions()?;
        let meta_store = Arc::new(meta_store);
        let allocators = Allocators::new(&config.path, config.create, meta_store.clone())?;
        let meta_cache = Arc::new(LockMap::with_capacity_and_shard_amount(1 << 20, 256));

        let engine = Self {
            meta_store: meta_store.clone(),
            allocators,
            meta_cache,
            workers: Default::default(),
            allow_to_allocate: Arc::new(AtomicBool::new(true)),
            metrics: Default::default(),
            prefix_len: config.prefix_len,
            writing_list: Default::default(),
        };

        if !uncommitted_chunks.is_empty() {
            // resume writing chunks in memory.
            for (chunk_id, meta, _) in &uncommitted_chunks {
                let old_chunk = engine.get(chunk_id)?;

                let prefix: Bytes = chunk_id[..engine.prefix_len].into();
                let mut writing_list = engine.writing_list.entry(prefix).or_default();
                let allocator = engine.allocators.select_by_pos(meta.pos)?;
                let chunk = allocator.reference(meta.clone(), old_chunk.is_none());
                writing_list.insert(chunk_id.clone(), WritingHolder { chunk, abort: true });
            }
            meta_store.vacate_uncommitted_positions(uncommitted_chunks)?;
        }

        engine.upgrade_version()?;

        Ok(engine)
    }

    pub fn used_size(&self) -> UsedSize {
        self.allocators.used_size()
    }

    pub fn allocate_groups(
        &self,
        min_remain: usize,
        max_remain: usize,
        batch_size: usize,
    ) -> usize {
        self.allocators.allocate_groups(
            if self.allow_to_allocate.load(Ordering::Acquire) {
                min_remain
            } else {
                0
            },
            max_remain,
            batch_size,
            false,
        )
    }

    pub fn allocate_ultra_groups(
        &self,
        min_remain: usize,
        max_remain: usize,
        batch_size: usize,
    ) -> usize {
        self.allocators.allocate_groups(
            if self.allow_to_allocate.load(Ordering::Acquire) {
                min_remain
            } else {
                0
            },
            max_remain,
            batch_size,
            true,
        )
    }

    pub fn compact_groups(&self, max_reserved: u64) -> usize {
        let group_ids = self.allocators.get_allocate_tasks(max_reserved);
        if group_ids.is_empty() {
            return 0;
        }

        let mut finish = 0usize;
        for group_id in group_ids {
            let mut it = self.meta_store.iterator();
            let result = it.iterate(
                MetaKey::group_to_chunks_key_prefix(group_id),
                |_, chunk_id| self.move_chunk(chunk_id).map(|_| ()),
            );
            match result {
                Ok(_) => finish += 1,
                Err(e) => tracing::error!("compact group {:?} failed: {:?}", group_id, e),
            }
            self.allocators.finish_compact_task(group_id);
        }

        finish
    }

    pub fn start_allocate_workers(&self, num: usize) {
        let workers = (0..num)
            .map(|i| {
                let allocators = self.allocators.clone();
                WorkerBuilder::default()
                    .name(format!("Allocate{}", i))
                    .spawn(move || {
                        let finish = allocators.allocate_groups(1, 2, 2, false);
                        if finish != 0 {
                            WorkerState::Continue
                        } else {
                            WorkerState::Wait(std::time::Duration::from_millis(100))
                        }
                    })
            })
            .collect::<Vec<_>>();

        self.workers.lock().unwrap().extend(workers);
    }

    pub fn stop_and_join(&self) {
        let mut workers = self.workers.lock().unwrap();
        for mut worker in workers.drain(..) {
            worker.stop_and_join();
        }
    }

    pub fn set_allow_to_allocate(&self, val: bool) {
        self.allow_to_allocate.store(val, Ordering::Release)
    }

    pub fn speed_up_quit(&self) {
        // There is a memory leak and should only be called when the process exits.
        let _ = Arc::into_raw(self.meta_cache.clone());
        let _ = Arc::into_raw(self.writing_list.clone());
    }

    pub fn get(&self, chunk_id: &[u8]) -> Result<Option<ChunkArc>> {
        let mut entry = self.meta_cache.entry_by_ref(chunk_id);
        self.get_with_entry(chunk_id, &mut entry)
    }

    fn get_with_entry(
        &self,
        chunk_id: &[u8],
        entry: &mut EntryByRef<Bytes, [u8], ChunkArc>,
    ) -> Result<Option<ChunkArc>> {
        match entry.get() {
            Some(chunk) => Ok(Some(chunk.clone())),
            None => {
                let meta = self.meta_store.get_chunk_meta(chunk_id)?;
                if let Some(mut meta) = meta {
                    meta.set_default_etag_if_need();
                    let allocator = self.allocators.select_by_pos(meta.pos)?;
                    let chunk = Arc::new(allocator.reference(meta, true));
                    entry.insert(chunk.clone());
                    Ok(Some(chunk))
                } else {
                    Ok(None)
                }
            }
        }
    }

    pub fn batch_get(&self, chunk_ids: &BTreeSet<Bytes>) -> Result<BTreeMap<Bytes, ChunkArc>> {
        let mut chunks = BTreeMap::<Bytes, ChunkArc>::new();

        let mut entries = Vec::with_capacity(chunk_ids.len());
        for chunk_id in chunk_ids {
            let mut entry = self.meta_cache.entry_by_ref(chunk_id);
            if let Some(chunk) = self.get_with_entry(chunk_id, &mut entry)? {
                chunks.insert(chunk_id.clone(), chunk);
            }
            entries.push(entry);
        }

        Ok(chunks)
    }

    pub fn move_chunk(&self, chunk_id: &[u8]) -> Result<Option<ChunkArc>> {
        // 1. get chunk and keep reference.
        let old_chunk = match self.get(chunk_id)? {
            Some(chunk) => chunk,
            None => return Ok(None),
        };

        // 2. allocate new chunk.
        let new_chunk = old_chunk.copy_chunk()?;

        // 3. replace chunk.
        let mut entry = self.meta_cache.entry_by_ref(chunk_id);
        match entry.get() {
            Some(chunk) if chunk.meta() == old_chunk.meta() => {
                self.meta_store
                    .move_chunk(chunk_id, old_chunk.meta(), new_chunk.meta(), true)?;
                let new_chunk = Arc::new(new_chunk);
                entry.insert(new_chunk.clone());
                Ok(Some(new_chunk))
            }
            _ => Ok(None), // chunk is updated or deleted by other thread.
        }
    }

    pub fn write(
        &self,
        chunk_id: &[u8],
        data: &[u8],
        offset: u32,
        checksum: u32,
    ) -> Result<ChunkArc> {
        self.update(
            chunk_id,
            &mut UpdateReq {
                checksum,
                length: data.len() as _,
                offset,
                data: data.as_ptr() as u64,
                ..Default::default()
            },
        )
    }

    pub fn remove(&self, chunk_id: &[u8]) -> Result<ChunkArc> {
        self.update(
            chunk_id,
            &mut UpdateReq {
                is_remove: true,
                ..Default::default()
            },
        )
    }

    pub fn truncate(&self, chunk_id: &[u8], length: u32) -> Result<ChunkArc> {
        self.update(
            chunk_id,
            &mut UpdateReq {
                is_truncate: true,
                offset: length,
                ..Default::default()
            },
        )
    }

    fn update(&self, chunk_id: &[u8], req: &mut UpdateReq) -> Result<ChunkArc> {
        let chunk = self.update_chunk(chunk_id, req)?;
        self.commit_chunk(chunk, false)
    }

    pub fn update_chunk(&self, chunk_id: &[u8], req: &mut UpdateReq) -> Result<WritingChunk> {
        if chunk_id.len() < self.prefix_len {
            return Err(Error::InvalidArg(format!(
                "chunk id {:?} is too short for prefix {}",
                chunk_id, self.prefix_len
            )));
        }

        // 1. prepare update info.
        let data = if req.length != 0 {
            let data =
                unsafe { std::slice::from_raw_parts(req.data as *const _, req.length as usize) };
            let checksum = crc32c::crc32c(data);
            if req.without_checksum {
                req.checksum = checksum;
            } else if checksum != req.checksum {
                return Err(Error::ChecksumMismatch(format!(
                    "invalid checksum local {:08x} != remote {:08x}",
                    checksum, req.checksum
                )));
            }
            data
        } else {
            &[]
        };

        // 2. get old chunk.
        let old_chunk = self.get(chunk_id)?;
        let etag = match &old_chunk {
            Some(chunk) => {
                if req.create_new {
                    return Err(Error::ChunkAlreadyExists);
                }
                let meta = chunk.meta();
                req.out_commit_ver = meta.chunk_ver;
                req.out_chain_ver = meta.chain_ver;
                req.out_checksum = meta.checksum;
                meta.etag.as_slice()
            }
            None => {
                if req.is_remove && req.update_ver > 0 {
                    req.out_commit_ver = req.update_ver - 1;
                } else {
                    req.out_commit_ver = 0;
                }
                req.out_chain_ver = req.chain_ver;
                req.out_checksum = 0;
                req.out_non_existent = true;
                &[]
            }
        };

        // 3. check version.
        if req.chain_ver < req.out_chain_ver {
            return Err(Error::ChainVersionMismatch(format!(
                "req.chain_ver {} < meta.chain_ver {}",
                req.chain_ver, req.out_chain_ver
            )));
        }

        let new_chunk_ver = if req.is_syncing {
            req.update_ver
        } else if req.update_ver > 0 {
            if req.update_ver <= req.out_commit_ver {
                return Err(Error::ChunkCommittedUpdate(format!(
                    "committed update {} <= {}",
                    req.update_ver, req.out_commit_ver
                )));
            } else if req.update_ver > req.out_commit_ver + 1 {
                return Err(Error::ChunkMissingUpdate(format!(
                    "missing update {} > {} + 1",
                    req.update_ver, req.out_commit_ver
                )));
            }
            req.update_ver
        } else {
            req.out_commit_ver + 1
        };

        if !req.expected_tag.is_empty() && req.expected_tag != etag {
            return Err(Error::ChunkETagMismatch(format!(
                "expected {:?} != real {:?}",
                req.expected_tag, etag
            )));
        }

        // 4. do update.
        let mut new_chunk = match old_chunk {
            Some(old_chunk) if req.is_remove => old_chunk.as_ref().clone(),
            Some(old_chunk)
                if req.is_syncing
                    || (req.length > 0 && req.offset < old_chunk.meta().len)
                    || req.offset + req.length > old_chunk.capacity() =>
            {
                old_chunk.copy_on_write(
                    data,
                    req.offset,
                    req.checksum,
                    req.is_syncing,
                    self.allow_to_allocate.load(Ordering::Acquire),
                    &self.allocators,
                    &self.metrics,
                )?
            }
            Some(old_chunk) => {
                let mut new_chunk = old_chunk.as_ref().clone();
                new_chunk.safe_write(
                    data,
                    req.offset,
                    req.checksum,
                    req.is_truncate,
                    &self.metrics,
                )?;
                new_chunk
            }
            None => {
                let begin = std::time::Instant::now();
                let mut new_chunk = self.allocators.allocate(
                    Size::from(req.offset + req.length),
                    self.allow_to_allocate.load(Ordering::Acquire),
                )?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as _;
                self.metrics.allocate_times.fetch_add(1, Ordering::AcqRel);
                self.metrics
                    .allocate_latency
                    .fetch_add(latency, Ordering::AcqRel);
                new_chunk.safe_write(
                    data,
                    req.offset,
                    req.checksum,
                    req.is_truncate,
                    &self.metrics,
                )?;
                new_chunk
            }
        };

        req.out_commit_ver = new_chunk_ver;
        req.out_chain_ver = req.chain_ver;
        req.out_checksum = new_chunk.meta().checksum;
        new_chunk.update_meta(req.deref());

        let prefix: Bytes = chunk_id[..self.prefix_len].into();
        let mut writing_list = self.writing_list.entry(prefix).or_default();
        match writing_list.entry(chunk_id.into()) {
            std::collections::hash_map::Entry::Occupied(mut occupied_entry)
                if occupied_entry.get().abort =>
            {
                occupied_entry.insert(WritingHolder {
                    chunk: new_chunk.clone(),
                    abort: false,
                });
            }
            std::collections::hash_map::Entry::Occupied(occupied_entry) => {
                return Err(Error::InvalidArg(format!(
                    "chunk {:?} is in writing ({:?})",
                    chunk_id,
                    occupied_entry.get().chunk.meta(),
                )));
            }
            std::collections::hash_map::Entry::Vacant(vacant_entry) => {
                vacant_entry.insert(WritingHolder {
                    chunk: new_chunk.clone(),
                    abort: false,
                });
            }
        }
        drop(writing_list);

        self.meta_store
            .persist_writing_chunk(chunk_id, new_chunk.meta())?;

        Ok(WritingChunk {
            chunk_id: chunk_id.into(),
            chunk: new_chunk,
            list: self.writing_list.clone(),
            prefix_len: self.prefix_len as _,
            is_remove: req.is_remove,
            commit_succ: false,
        })
    }

    pub fn commit_chunk(&self, mut chunk: WritingChunk, sync: bool) -> Result<ChunkArc> {
        chunk.set_committed();
        let chunk_id = &chunk.chunk_id;
        let new_chunk: ChunkArc = (&chunk).into();

        if chunk.is_remove {
            self.meta_store.remove(chunk_id, new_chunk.meta(), sync)?;
            self.meta_cache.remove(chunk_id);
            chunk.commit_succ();
            return Ok(new_chunk);
        }

        // update rocksdb under lock protection.
        let mut entry = self.meta_cache.entry_by_ref(chunk_id);
        match self.get_with_entry(chunk_id, &mut entry)? {
            Some(old_chunk) => {
                self.meta_store
                    .move_chunk(chunk_id, old_chunk.meta(), new_chunk.meta(), sync)?;
            }
            None => {
                self.meta_store
                    .add_chunk(chunk_id, new_chunk.meta(), sync)?;
            }
        }
        entry.insert(new_chunk.clone());
        drop(entry);

        chunk.commit_succ();
        Ok(new_chunk)
    }

    pub fn commit_chunks(&self, mut chunks: Vec<WritingChunk>, sync: bool) -> Result<()> {
        let chunk_ids = chunks
            .iter()
            .map(|c| c.chunk_id.clone())
            .collect::<BTreeSet<_>>();
        if chunk_ids.len() != chunks.len() {
            return Err(Error::InvalidArg("same chunk id in the batch!".into()));
        }

        // lock it.
        let mut entries = HashMap::with_capacity(chunk_ids.len());
        for chunk_id in &chunk_ids {
            let mut entry = self.meta_cache.entry_by_ref(chunk_id);
            self.get_with_entry(chunk_id, &mut entry)?;
            entries.insert(chunk_id.clone(), entry);
        }

        let mut write_batch = RocksDB::new_write_batch();
        for chunk in &mut chunks {
            chunk.set_committed();
            if chunk.is_remove {
                self.meta_store
                    .remove_mut(&chunk.chunk_id, chunk.meta(), &mut write_batch)?;
            } else {
                match entries.get_mut(&chunk.chunk_id).unwrap().get() {
                    Some(old_chunk) => self.meta_store.move_chunk_mut(
                        &chunk.chunk_id,
                        old_chunk.meta(),
                        chunk.meta(),
                        &mut write_batch,
                    )?,
                    None => self.meta_store.add_chunk_mut(
                        &chunk.chunk_id,
                        chunk.meta(),
                        &mut write_batch,
                    )?,
                }
            }
        }
        self.meta_store.write(write_batch, sync)?;

        for chunk in &mut chunks {
            chunk.commit_succ();
            if chunk.is_remove {
                entries.get_mut(&chunk.chunk_id).unwrap().remove();
            } else {
                entries
                    .get_mut(&chunk.chunk_id)
                    .unwrap()
                    .insert(chunk.deref().into());
            }
        }
        Ok(())
    }

    pub fn query_uncommitted_chunks(&self, prefix: &[u8]) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let writing_list = self.writing_list.entry(prefix.into()).or_default();
        let mut uncommitted_chunks = Vec::with_capacity(writing_list.len());
        for (chunk_id, holder) in writing_list.iter() {
            uncommitted_chunks.push((chunk_id.clone(), holder.chunk.meta().clone()));
        }
        Ok(uncommitted_chunks)
    }

    pub fn handle_uncommitted_chunks(
        &self,
        prefix: &[u8],
        chain_ver: u32,
    ) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let mut writing_list = self.writing_list.entry(prefix.into()).or_default();

        let mut chunks = Vec::with_capacity(writing_list.len());
        let mut uncommitted_chunks = Vec::with_capacity(writing_list.len());
        // commit it!
        for (chunk_id, holder) in writing_list.iter_mut() {
            holder.chunk.set_chain_ver(chain_ver);
            let chunk = holder.chunk.clone();
            chunks.push(WritingChunk {
                chunk_id: chunk_id.clone(),
                chunk,
                list: self.writing_list.clone(),
                prefix_len: self.prefix_len as _,
                is_remove: false,
                commit_succ: false,
            });
            uncommitted_chunks.push((chunk_id.clone(), holder.chunk.meta().clone()));
        }
        drop(writing_list);
        self.commit_chunks(chunks, true)?;
        Ok(uncommitted_chunks)
    }

    pub fn query_chunks(
        &self,
        begin: impl AsRef<[u8]>,
        end: impl AsRef<[u8]>,
        max_count: u64,
    ) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let mut chunks = self.meta_store.query_chunks(begin, end, max_count)?;
        for (_, meta) in &mut chunks {
            meta.set_default_etag_if_need();
        }
        Ok(chunks)
    }

    // returns an unordered list of chunks (including chunks being written).
    pub fn query_all_chunks(&self, prefix: &[u8]) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let (mut writing_list, it) = match self.writing_list.entry(prefix.into()) {
            dashmap::Entry::Occupied(occupied_entry) => {
                let mut map = HashMap::new();
                for (chunk_id, holder) in occupied_entry.get() {
                    map.insert(chunk_id.clone(), holder.chunk.meta().clone());
                }
                (map, self.meta_store.iterator())
            }
            dashmap::Entry::Vacant(_) => (Default::default(), self.meta_store.iterator()),
        };

        let mut chunks =
            self.meta_store
                .query_chunks_from_iterator(it, prefix, prefix, u64::MAX)?;
        for (chunk_id, meta) in &mut chunks {
            meta.set_default_etag_if_need();
            if let Some(writing) = writing_list.remove(chunk_id) {
                *meta = writing;
            }
        }
        chunks.extend(writing_list);
        Ok(chunks)
    }

    pub fn query_chunks_by_timestamp(
        &self,
        prefix: &[u8],
        begin: u64,
        end: u64,
        max_count: u64,
    ) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let chunk_ids = self
            .meta_store
            .query_chunks_by_timestamp(prefix, begin, end, max_count)?;
        let mut chunks = Vec::with_capacity(chunk_ids.len());
        for chunk_id in chunk_ids {
            if let Some(chunk) = self.get(&chunk_id)? {
                let mut meta = chunk.meta().clone();
                meta.set_default_etag_if_need();
                chunks.push((chunk_id, meta));
            }
        }
        Ok(chunks)
    }

    pub fn batch_remove(
        &self,
        begin: impl AsRef<[u8]>,
        end: impl AsRef<[u8]>,
        max_count: u64,
    ) -> Result<u64> {
        let chunks = self.meta_store.query_chunks(begin, end, max_count)?;
        let mut offset = 0;

        const BATCH_SIZE: Size = Size::mebibyte(1);
        let mut write_batch = RocksDB::new_write_batch();
        for (index, (chunk_id, meta)) in chunks.iter().enumerate() {
            if write_batch.size_in_bytes() >= BATCH_SIZE.0 as _ {
                self.meta_store.write(write_batch, true)?;
                write_batch = RocksDB::new_write_batch();
                for (chunk_id, _) in &chunks[offset..index] {
                    self.meta_cache.remove(chunk_id);
                }
                offset = index;
            }
            self.meta_store
                .remove_mut(&chunk_id, &meta, &mut write_batch)?;
        }
        if !write_batch.is_empty() {
            self.meta_store.write(write_batch, true)?;
            for (chunk_id, _) in &chunks[offset..] {
                self.meta_cache.remove(chunk_id);
            }
        }
        Ok(chunks.len() as _)
    }

    pub fn upgrade_version(&self) -> Result<()> {
        let version = self.meta_store.get_version()?;
        let mut new_version = version;

        if new_version < MetaStore::V1_FIX_TIMESTAMP {
            // fix timestamp format.
            tracing::info!(
                "upgrade version: {} -> {}",
                new_version,
                MetaStore::V1_FIX_TIMESTAMP
            );
            let mut write_batch = RocksDB::new_write_batch();
            self.meta_store
                .remove_range_mut(MetaKey::TIMESTAMP_KEY_PREFIX, &mut write_batch)?;
            self.meta_store.write(write_batch, true)?;
            new_version = MetaStore::V1_FIX_TIMESTAMP;
        }

        if version < new_version {
            self.meta_store.set_version(new_version)?;
            tracing::info!("upgrade version {} -> {} successful!", version, new_version);
        }
        Ok(())
    }
}

impl Drop for Engine {
    fn drop(&mut self) {
        self.stop_and_join();
    }
}

impl std::fmt::Debug for Engine {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Engine").finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicBool, Ordering};
    use std::sync::Arc;

    #[test]
    fn test_engine_normal() {
        let dir = tempfile::tempdir().unwrap();

        let chunk_id = "chunk01".as_bytes();
        let bytes = "hello world!".as_bytes();

        let mut config: EngineConfig = toml::from_str(&format!(
            r#"
                path = {:?}
                create = true
                prefix_len = 0
            "#,
            dir.path()
        ))
        .unwrap();

        {
            let engine = Engine::open(&config).unwrap();
            let _ = format!("{:?}", engine);
            assert!(engine.get(chunk_id).unwrap().is_none());
            assert_eq!(engine.used_size(), UsedSize::default());
            assert!(engine.move_chunk(chunk_id).is_ok());

            let allocator = engine.allocators.select_by_size(CHUNK_SIZE_NORMAL).unwrap();
            for _ in 0..1000 {
                let chunk = allocator.allocate(true).unwrap();
                assert_eq!(chunk.meta().pos, Position::default());
            }

            let s = CHUNK_SIZE_NORMAL * GroupId::COUNT;
            assert_eq!(engine.used_size().allocated_size, s);
            assert_eq!(engine.used_size().reserved_size, s);

            engine.speed_up_quit();
        }

        {
            let engine = Engine::open(&config).unwrap();
            assert!(engine.get(chunk_id).unwrap().is_none());
            assert_eq!(engine.used_size(), UsedSize::default());
            assert!(engine.write(chunk_id, bytes, 0, 0).is_err());

            let checksum = crc32c::crc32c(bytes);
            let chunk = engine.write(chunk_id, bytes, 0, checksum).unwrap();
            assert_eq!(chunk.meta().pos.chunk_size(), CHUNK_SIZE_SMALL);
            assert_eq!(chunk.meta().pos.group(), 0);
            assert_eq!(chunk.meta().pos.cluster(), 0);
            assert_eq!(chunk.meta().chunk_ver, 1);

            let s = CHUNK_SIZE_SMALL * GroupId::COUNT;
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL);

            let chunk = engine.get(chunk_id).unwrap().unwrap();
            assert_eq!(chunk.meta().chunk_ver, 1);
            assert_eq!(chunk.meta().len, 12);

            let mut buf = [0u8; 12];
            assert!(chunk.pread(&mut buf, 0).is_ok());
            assert_eq!(buf, bytes);
            assert_eq!(chunk.meta().checksum, crc32c::crc32c(&buf));

            let fd_and_offset = chunk.fd_and_offset();
            assert_ne!(fd_and_offset.fd, 0);
            assert_eq!(fd_and_offset.offset, chunk.meta().pos.offset());
        }

        {
            config.create = false;
            let engine = Engine::open(&config).unwrap();
            let chunk0 = engine.get(chunk_id).unwrap().unwrap();

            let s = CHUNK_SIZE_SMALL * GroupId::COUNT;
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL);

            let mut buf = [0u8; 12];
            assert!(chunk0.pread(&mut buf, 0).is_ok());
            assert_eq!(buf, bytes);
            assert_eq!(chunk0.meta().pos.chunk_size(), CHUNK_SIZE_SMALL);
            assert_eq!(chunk0.meta().pos.group(), 0);
            assert_eq!(chunk0.meta().pos.cluster(), 0);
            assert_eq!(chunk0.meta().checksum, crc32c::crc32c(&buf));

            let chunk1 = engine.move_chunk(chunk_id).unwrap().unwrap();
            let mut buf = [0u8; 12];
            assert!(chunk1.pread(&mut buf, 0).is_ok());
            assert_eq!(buf, bytes);
            assert_eq!(chunk1.meta().pos.chunk_size(), CHUNK_SIZE_SMALL);
            assert_eq!(chunk1.meta().pos.index(), 1);
            assert_eq!(chunk1.meta().checksum, crc32c::crc32c(&buf));
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL * 2);

            let allocator = engine.allocators.select_by_size(CHUNK_SIZE_SMALL).unwrap();
            assert_eq!(
                allocator.allocate(true).unwrap().meta().pos,
                Position::new(GroupId::new(CHUNK_SIZE_SMALL, 0, 0), 2)
            );
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL * 2);

            drop(chunk0);
            assert_eq!(
                allocator.allocate(true).unwrap().meta().pos,
                Position::new(GroupId::new(CHUNK_SIZE_SMALL, 0, 0), 0)
            );
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL);

            drop(chunk1); // still in cache.
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL);

            let chunk2 = engine
                .write(chunk_id, bytes, 12, crc32c::crc32c(bytes))
                .unwrap();
            assert_eq!(chunk2.meta().len, 24);
            assert_eq!(chunk2.meta().pos.index(), 1);
            assert_eq!(chunk2.meta().chunk_ver, 2);
            let mut buf = [0u8; 12];
            chunk2.pread(&mut buf, 12).unwrap();
            assert_eq!(buf, bytes);
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL);

            let empty = [0u8; 12];
            let chunk3 = engine
                .write(chunk_id, &empty, 0, crc32c::crc32c(&empty))
                .unwrap();
            assert_eq!(chunk3.meta().len, 24);
            assert_eq!(chunk3.meta().pos.index(), 0);
            assert_eq!(chunk3.meta().chunk_ver, 3);
            let mut buf = [0u8; 12];
            chunk3.pread(&mut buf, 0).unwrap();
            assert_eq!(buf, [0u8; 12]);
            chunk3.pread(&mut buf, 12).unwrap();
            assert_eq!(buf, bytes);
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL * 2);

            engine.remove(chunk_id).unwrap();
            engine.remove(chunk_id).unwrap();
            assert!(engine.get(chunk_id).unwrap().is_none());
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL * 2);

            let mut buf = [0u8; 12];
            chunk3.pread(&mut buf, 0).unwrap();
            assert_eq!(buf, [0u8; 12]);
            chunk3.pread(&mut buf, 12).unwrap();
            assert_eq!(buf, bytes);

            drop(chunk3);
            assert_eq!(engine.used_size().reserved_size, s - CHUNK_SIZE_SMALL);

            drop(chunk2);
            assert_eq!(engine.used_size().reserved_size, s);
        }

        {
            let engine = Engine::open(&config).unwrap();
            let s = CHUNK_SIZE_SMALL * GroupId::COUNT;
            assert_eq!(engine.used_size().reserved_size, s);

            engine.start_allocate_workers(2);
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
    }

    #[test]
    fn test_engine_list_chunks() {
        let dir = tempfile::tempdir().unwrap();

        // 1. prepare.
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        // 2. write chunks.
        let engine = Engine::open(&config).unwrap();
        const N: usize = 512;
        for i in 1..=N {
            let mut data = create_aligned_buf(Size::from(i * 1024));
            data.fill(i as u8);
            let checksum = crc32c::crc32c(&data);
            let chunk = engine
                .write(&i.to_be_bytes(), &data, i as u32 * 512, checksum)
                .unwrap();
            assert_eq!(chunk.meta().chunk_ver, 1);
        }

        // 3. list all chunks.
        drop(engine);
        let engine = Engine::open(&config).unwrap();
        let chunks = engine.query_chunks([], [], u64::MAX).unwrap();
        assert_eq!(chunks.len(), N);
        let mut buffer = create_aligned_buf(CHUNK_SIZE_LARGE);
        for (i, (chunk_id, chunk_meta)) in chunks.iter().enumerate() {
            let i = N - i; // reversed order.
            assert_eq!(&i.to_be_bytes(), &chunk_id[..]);
            let offset = i * 512;
            let data_len = i * 1024;
            let total = Size::from(offset + data_len);
            assert_eq!(chunk_meta.len, total);
            assert_eq!(
                chunk_meta.pos.chunk_size(),
                std::cmp::max(total.next_power_of_two(), CHUNK_SIZE_SMALL)
            );

            let chunk = engine.get(chunk_id).unwrap().unwrap();
            assert_eq!(chunk.meta(), chunk_meta);

            chunk.pread(&mut buffer[..total.into()], 0).unwrap();
            assert!(buffer[..offset].iter().all(|v| *v == 0));
            assert!(buffer[offset..total.into()].iter().all(|v| *v == i as u8));
        }

        let key = vec![1u8; 1 << 20];
        engine.write(&key, &[], 0, 0).unwrap();

        let count = engine.batch_remove([], [], u64::MAX).unwrap();
        assert_eq!(count, 1 + N as u64);
        let count = engine.batch_remove([], [], u64::MAX).unwrap();
        assert_eq!(count, 0);

        let chunks = engine.query_chunks([], [], u64::MAX).unwrap();
        assert!(chunks.is_empty());
    }

    #[test]
    fn test_engine_load() {
        let dir = tempfile::tempdir().unwrap();
        tracing_subscriber::fmt()
            .with_max_level(tracing::Level::INFO)
            .init();

        // 1. prepare.
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        // 2. write chunks.
        let engine = Engine::open(&config).unwrap();

        let mut data = create_aligned_buf(CHUNK_SIZE_SMALL);
        for i in 0..512u32 {
            data.fill(i as u8);
            let checksum = crc32c::crc32c(&data);
            let chunk = engine.write(&i.to_le_bytes(), &data, 0, checksum).unwrap();
            assert_eq!(chunk.meta().chunk_ver, 1);
        }

        let vec = engine.query_chunks([], [], 10000).unwrap();
        assert_eq!(vec.len(), 512);

        for i in 0..256u32 {
            engine.remove(&i.to_le_bytes()).unwrap();
        }

        let vec = engine.query_chunks([], [], 10000).unwrap();
        assert_eq!(vec.len(), 256);

        assert_eq!(engine.allocate_groups(0, 0, 8), 1);

        drop(engine);
        let engine = Engine::open(&config).unwrap();
        std::thread::sleep(std::time::Duration::from_millis(100));
        let mut buf = create_aligned_buf(CHUNK_SIZE_SMALL);
        for i in 0..512u32 {
            let chunk = engine.get(&i.to_le_bytes()).unwrap();
            if i < 256 {
                assert!(chunk.is_none());
            } else {
                let chunk = chunk.unwrap();
                chunk.pread(&mut buf, 0).unwrap();
                assert!(buf.iter().all(|&v| v == i as u8));
                assert_eq!(
                    chunk.meta().checksum,
                    crc32c::crc32c(&[i as u8; CHUNK_SIZE_SMALL.0 as _])
                );

                data.fill(!i as u8);
                let checksum = crc32c::crc32c(&data);
                let chunk = engine.write(&i.to_le_bytes(), &data, 0, checksum).unwrap();
                assert_eq!(chunk.meta().chunk_ver, 2);
            }
        }

        drop(engine);
        let engine = Engine::open(&config).unwrap();
        std::thread::sleep(std::time::Duration::from_millis(100));
        for i in 0..512u32 {
            let chunk = engine.get(&i.to_le_bytes()).unwrap();
            if i < 256 {
                assert!(chunk.is_none());
            } else {
                let chunk = chunk.unwrap();
                chunk.pread(&mut buf, 0).unwrap();
                assert!(buf.iter().all(|&v| v == !i as u8));
                assert_eq!(
                    chunk.meta().checksum,
                    crc32c::crc32c(&[!i as u8; CHUNK_SIZE_SMALL.0 as _])
                );
            }
        }

        let vec = engine.query_chunks([], [], 10000).unwrap();
        assert_eq!(vec.len(), 256);

        let vec = engine
            .query_chunks_by_timestamp(&[], 0, u64::MAX, 10000)
            .unwrap();
        assert_eq!(vec.len(), 256);
    }

    #[test]
    fn test_engine_compact() {
        let dir = tempfile::tempdir().unwrap();

        // 1. prepare.
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        // 2. write chunks.
        let engine = Engine::open(&config).unwrap();

        let mut data = create_aligned_buf(CHUNK_SIZE_SMALL);
        for i in 0..512u32 {
            data.fill(i as u8);
            let checksum = crc32c::crc32c(&data);
            let chunk = engine.write(&i.to_le_bytes(), &data, 0, checksum).unwrap();
            assert_eq!(chunk.meta().chunk_ver, 1);
        }

        engine.remove(&255u32.to_le_bytes()).unwrap();
        let chunk1 = engine.allocators.allocate(CHUNK_SIZE_SMALL, true);
        engine.remove(&511u32.to_le_bytes()).unwrap();
        let chunk2 = engine.allocators.allocate(CHUNK_SIZE_SMALL, true);

        // 3. remove some chunks.
        for i in 0..512u32 {
            if i % 2 == 0 {
                engine.remove(&i.to_le_bytes()).unwrap();
            }
        }
        assert_eq!(engine.used_size().allocated_size, CHUNK_SIZE_SMALL * 512);
        assert_eq!(engine.used_size().reserved_size, CHUNK_SIZE_SMALL * 256);
        assert_eq!(engine.allocate_groups(0, 0, 16), 0);

        // 4. start a bg read thread.
        let stop = Arc::new(AtomicBool::default());
        let stop_clone = stop.clone();
        let engine_clone = engine.clone();
        let thread = std::thread::spawn(move || {
            let engine = engine_clone;
            let stop = stop_clone;
            while !stop.load(Ordering::Acquire) {
                let mut buf = create_aligned_buf(CHUNK_SIZE_SMALL);
                for i in 0..512u32 {
                    let chunk = engine.get(&i.to_le_bytes()).unwrap();
                    if i % 2 == 0 || i == 255 || i == 511 {
                        assert!(chunk.is_none());
                    } else {
                        let chunk = chunk.unwrap();
                        chunk.pread(&mut buf, 0).unwrap();
                        assert!(buf.iter().all(|&v| v == i as u8));
                        assert_eq!(
                            chunk.meta().checksum,
                            crc32c::crc32c(&[i as u8; CHUNK_SIZE_SMALL.0 as _])
                        );
                    }
                }
            }
        });

        // 5. compact.
        assert_eq!(engine.compact_groups(10000), 0);
        assert_eq!(engine.compact_groups(0), 1);
        assert_eq!(engine.used_size().allocated_size, CHUNK_SIZE_SMALL * 512);
        assert_eq!(engine.used_size().reserved_size, CHUNK_SIZE_SMALL * 256);

        // 6. deallocate previous group.
        drop(chunk1);
        drop(chunk2);
        assert_eq!(engine.compact_groups(0), 1);
        assert_eq!(engine.allocate_groups(0, 0, 16), 1);
        assert_eq!(engine.used_size().allocated_size, CHUNK_SIZE_SMALL * 256);
        assert_eq!(engine.used_size().reserved_size, CHUNK_SIZE_SMALL * 2);
        assert_eq!(engine.compact_groups(1), 1);

        // 7. stop read chunks.
        stop.store(true, Ordering::SeqCst);
        thread.join().unwrap();
    }

    #[test]
    fn test_engine_truncate() {
        let dir = tempfile::tempdir().unwrap();

        // 1. prepare.
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        // 2. write chunks.
        let engine = Engine::open(&config).unwrap();
        let mut data = create_aligned_buf(CHUNK_SIZE_SMALL);
        for i in 0..512u32 {
            data.fill(i as u8);
            let checksum = crc32c::crc32c(&data);
            let chunk = engine.write(&i.to_le_bytes(), &data, 0, checksum).unwrap();
            assert_eq!(chunk.meta().chunk_ver, 1);
        }

        // 3. extend chunks.
        for i in 0..512u32 {
            let length = i * 131;
            let chunk = engine.write(&i.to_le_bytes(), &[], length, 0).unwrap();
            assert_eq!(chunk.meta().chunk_ver, 2);
            assert_eq!(
                chunk.meta().len,
                std::cmp::max(length, CHUNK_SIZE_SMALL.into())
            );
        }

        // 4. truncate and check.
        let mut buf = create_aligned_buf(CHUNK_SIZE_NORMAL);
        for i in 0..512u32 {
            let length = i * 137;
            let chunk = engine.truncate(&i.to_le_bytes(), length).unwrap();
            assert_eq!(chunk.meta().chunk_ver, 3);
            assert_eq!(chunk.meta().len, length);
            chunk
                .pread(
                    &mut buf[..length.next_multiple_of(ALIGN_SIZE.into()) as usize],
                    0,
                )
                .unwrap();

            let bound = std::cmp::min(length, CHUNK_SIZE_SMALL.into()) as usize;
            assert!(buf[..bound].iter().all(|v| *v == i as u8));
            assert!(buf[bound..length as usize].iter().all(|v| *v == 0));
            assert_eq!(
                chunk.meta().checksum,
                crc32c::crc32c(&buf[..length as usize])
            );
        }

        // 4. check version.
        let chunk_id = &511u32.to_le_bytes();
        let mut req = UpdateReq {
            update_ver: 4,
            ..Default::default()
        };
        let chunk = engine.update(chunk_id, &mut req).unwrap();
        assert_eq!(req.out_chain_ver, 0);
        assert_eq!(req.out_commit_ver, 4);
        assert_eq!(req.out_checksum, chunk.meta().checksum);

        engine.update(chunk_id, &mut req).unwrap_err();
        assert_eq!(req.out_chain_ver, 0);
        assert_eq!(req.out_commit_ver, 4);
        assert_eq!(req.out_checksum, chunk.meta().checksum);

        req.update_ver = 6;
        engine.update(chunk_id, &mut req).unwrap_err();
        assert_eq!(req.out_chain_ver, 0);
        assert_eq!(req.out_commit_ver, 4);
        assert_eq!(req.out_checksum, chunk.meta().checksum);

        req.is_syncing = true;
        engine.update(chunk_id, &mut req).unwrap();
        req.is_syncing = false;
        assert_eq!(req.out_chain_ver, 0);
        assert_eq!(req.out_commit_ver, 6);
        assert_eq!(req.out_checksum, 0);
        assert_eq!(req.length, 0);

        req.chain_ver = 1;
        req.update_ver = 7;
        let chunk = engine.update(chunk_id, &mut req).unwrap();
        assert_eq!(req.out_chain_ver, 1);
        assert_eq!(req.out_commit_ver, 7);
        assert_eq!(req.out_checksum, chunk.meta().checksum);

        req.chain_ver = 0;
        req.update_ver = 8;
        engine.update(chunk_id, &mut req).unwrap_err();
        assert_eq!(req.out_chain_ver, 1);
        assert_eq!(req.out_commit_ver, 7);
        assert_eq!(req.out_checksum, chunk.meta().checksum);

        assert_eq!(
            format!("{:?}", chunk.as_ref()),
            format!("{:?}", chunk.meta())
        );
    }

    #[test]
    fn test_engine_checksum() {
        let dir = tempfile::tempdir().unwrap();

        // 1. prepare.
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        // 2. write chunks.
        let engine = Engine::open(&config).unwrap();

        let data = b"etc";
        let checksum = crc32c::crc32c(data);
        engine.write(b"chunk", data, 0, checksum).unwrap();

        let data = b"zzz";
        let checksum = crc32c::crc32c(data);
        let chunk = engine.write(b"chunk", data, 3, checksum).unwrap();

        let mut buf = [0u8; 6];
        chunk.pread(&mut buf, 0).unwrap();
        assert_eq!(crc32c::crc32c(&buf), chunk.meta().checksum);

        // 3. write without checksum
        engine
            .update(
                b"chunk",
                &mut UpdateReq {
                    without_checksum: true,
                    length: data.len() as _,
                    offset: 6,
                    data: data.as_ptr() as u64,
                    expected_tag: unsafe {
                        std::mem::transmute::<&[u8], &[u8]>(chunk.meta().etag.as_slice())
                    },
                    desired_tag: b"desired_tag",
                    ..Default::default()
                },
            )
            .unwrap();

        let err = engine
            .update(
                b"chunk",
                &mut UpdateReq {
                    without_checksum: true,
                    length: data.len() as _,
                    offset: 6,
                    data: data.as_ptr() as u64,
                    expected_tag: b"mismatch",
                    ..Default::default()
                },
            )
            .unwrap_err();
        assert!(matches!(err, Error::ChunkETagMismatch(_)));

        let err = engine
            .update(
                b"chunk",
                &mut UpdateReq {
                    without_checksum: true,
                    length: data.len() as _,
                    offset: 6,
                    data: data.as_ptr() as u64,
                    create_new: true,
                    ..Default::default()
                },
            )
            .unwrap_err();
        assert!(matches!(err, Error::ChunkAlreadyExists));
    }

    #[test]
    fn test_set_allow_to_allocate() {
        let dir = tempfile::tempdir().unwrap();

        // 1. prepare.
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        // 2. write chunks.
        let engine = Engine::open(&config).unwrap();
        assert_eq!(engine.allocate_ultra_groups(0, 1, 1), 0);

        engine.set_allow_to_allocate(false);
        assert_eq!(engine.allocate_groups(1, 1, 1), 0);
        assert_eq!(engine.allocate_ultra_groups(0, 1, 1), 0);

        let data = b"etc";
        let checksum = crc32c::crc32c(data);
        let err = engine.write(b"chunk", data, 0, checksum).unwrap_err();
        assert!(matches!(err, Error::NoSpace));
    }

    #[test]
    fn test_remove_update() {
        let dir = tempfile::tempdir().unwrap();

        // 1. prepare.
        let data = b"etc";
        let checksum = crc32c::crc32c(data);
        {
            let config = EngineConfig {
                path: dir.path().into(),
                create: true,
                ..Default::default()
            };
            let engine = Engine::open(&config).unwrap();

            engine.write(b"chunk", data, 0, checksum).unwrap();
        }

        // 2. remove.
        {
            let config = EngineConfig {
                path: dir.path().into(),
                create: false,
                ..Default::default()
            };
            let engine = Engine::open(&config).unwrap();
            let mut req = UpdateReq {
                is_remove: true,
                is_syncing: true,
                ..Default::default()
            };
            engine.update(b"chunk", &mut req).unwrap();
            assert!(!req.out_non_existent);
            assert_eq!(req.out_checksum, checksum);
        }
    }

    #[test]
    fn test_engine_writing_list() {
        let dir = tempfile::tempdir().unwrap();

        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };
        let engine = Engine::open(&config).unwrap();

        let chunks = engine.query_all_chunks(&[]).unwrap();
        assert!(chunks.is_empty());

        let chunk_id = "chunk01".as_bytes();
        let bytes = "hello world!".as_bytes();
        let mut req = UpdateReq {
            length: bytes.len() as _,
            data: bytes.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let mut chunk = engine.update_chunk(chunk_id, &mut req).unwrap();
        assert!(!engine
            .writing_list
            .entry(Default::default())
            .or_default()
            .is_empty());

        let err = engine.update_chunk(chunk_id, &mut req).unwrap_err();
        assert!(matches!(err, Error::InvalidArg(_)));

        let chunks = engine.query_chunks([], [], u64::MAX).unwrap();
        assert!(chunks.is_empty());

        let chunks = engine.query_all_chunks(&[]).unwrap();
        assert_eq!(chunks.len(), 1);
        assert_eq!(&chunks[0].1, chunk.meta());

        chunk.chunk.set_chain_ver(3);
        let mut meta = chunk.meta().clone();
        engine.commit_chunks(vec![chunk], false).unwrap();
        assert!(engine
            .writing_list
            .entry(Default::default())
            .or_default()
            .is_empty());

        let chunks = engine.query_chunks([], [], u64::MAX).unwrap();
        assert_eq!(chunks.len(), 1);
        meta.uncommitted = false;
        assert_eq!(chunks[0].1, meta);

        let mut writing_chunks = vec![];
        let remove = engine
            .update_chunk(
                b"remove",
                &mut UpdateReq {
                    is_remove: true,
                    ..Default::default()
                },
            )
            .unwrap();
        writing_chunks.push(remove);

        let new_chunk = engine
            .update_chunk(
                b"new_chunk",
                &mut UpdateReq {
                    length: bytes.len() as _,
                    data: bytes.as_ptr() as _,
                    without_checksum: true,
                    ..Default::default()
                },
            )
            .unwrap();
        writing_chunks.push(new_chunk);

        let update = engine
            .update_chunk(
                chunk_id,
                &mut UpdateReq {
                    offset: bytes.len() as _,
                    length: bytes.len() as _,
                    data: bytes.as_ptr() as _,
                    without_checksum: true,
                    chain_ver: 3,
                    ..Default::default()
                },
            )
            .unwrap();
        writing_chunks.push(update);
        assert_eq!(
            engine
                .writing_list
                .entry(Default::default())
                .or_default()
                .len(),
            3
        );

        assert_eq!(engine.query_chunks([], [], u64::MAX).unwrap().len(), 1);
        let chunks = engine.query_all_chunks(&[]).unwrap();
        assert_eq!(chunks.len(), 3);
        for (id, meta) in chunks {
            if id == chunk_id {
                assert_eq!(meta.len as usize, bytes.len() * 2);
            } else if id.as_slice() == b"new_chunk" {
                assert_eq!(meta.len as usize, bytes.len());
            } else {
                assert_eq!(id.as_slice(), b"remove");
            }
        }

        engine.commit_chunks(writing_chunks, false).unwrap();
        assert!(engine
            .writing_list
            .entry(Default::default())
            .or_default()
            .is_empty());
    }

    #[test]
    fn test_engine_persist_writing_list() {
        let dir = tempfile::tempdir().unwrap();

        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };
        let engine = Engine::open(&config).unwrap();

        let chunk_id = "chunk01".as_bytes();
        let bytes = "hello world!".as_bytes();
        let mut req = UpdateReq {
            length: bytes.len() as _,
            data: bytes.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let chunk = engine.update_chunk(chunk_id, &mut req).unwrap();
        assert!(!engine
            .writing_list
            .entry(Default::default())
            .or_default()
            .is_empty());

        let mut chunks = engine.query_all_chunks(&[]).unwrap();
        assert_eq!(chunks.len(), 1);
        assert_eq!(&chunks[0].1, chunk.meta());
        assert!(chunk.meta().uncommitted);

        let uncommitted_chunks = engine.query_uncommitted_chunks(&[]).unwrap();
        assert_eq!(uncommitted_chunks.len(), 1);
        assert_eq!(chunks, uncommitted_chunks);

        drop(chunk);
        drop(engine);

        let engine = Engine::open(&config).unwrap();
        let uncommitted_chunks = engine.query_uncommitted_chunks(&[]).unwrap();
        assert_eq!(uncommitted_chunks.len(), 1);
        assert_eq!(chunks, uncommitted_chunks);

        assert!(engine.get(chunk_id).unwrap().is_none());
        let chain_ver = 233;
        let uncommitted_chunks = engine.handle_uncommitted_chunks(&[], chain_ver).unwrap();
        assert_eq!(uncommitted_chunks.len(), 1);
        for chunk in &mut chunks {
            chunk.1.chain_ver = chain_ver;
        }
        assert_eq!(chunks, uncommitted_chunks);

        let chunk = engine.get(chunk_id).unwrap().unwrap();
        assert!(!chunk.meta().uncommitted);
        assert_eq!(chunk.meta().chain_ver, chain_ver);
    }

    #[test]
    fn test_engine_persist_writing_list_2() {
        let dir = tempfile::tempdir().unwrap();

        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };
        let engine = Engine::open(&config).unwrap();

        let chunk_id = "chunk01".as_bytes();
        let bytes = "hello world!".as_bytes();
        let mut req = UpdateReq {
            length: bytes.len() as _,
            data: bytes.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let _ = engine.update(chunk_id, &mut req).unwrap();
        assert!(engine
            .writing_list
            .entry(Default::default())
            .or_default()
            .is_empty());

        let mut req = UpdateReq {
            length: bytes.len() as _,
            data: bytes.as_ptr() as _,
            offset: bytes.len() as _,
            without_checksum: true,
            ..Default::default()
        };
        let chunk = engine.update_chunk(chunk_id, &mut req).unwrap();
        assert!(!engine
            .writing_list
            .entry(Default::default())
            .or_default()
            .is_empty());

        let chunks = engine.query_all_chunks(&[]).unwrap();
        assert_eq!(chunks.len(), 1);
        assert_eq!(&chunks[0].1, chunk.meta());
        assert!(chunk.meta().uncommitted);

        let uncommitted_chunks = engine.query_uncommitted_chunks(&[]).unwrap();
        assert_eq!(uncommitted_chunks.len(), 1);
        assert_eq!(chunks, uncommitted_chunks);

        drop(chunk);

        let mut req = UpdateReq {
            length: bytes.len() as _,
            data: bytes.as_ptr() as _,
            offset: bytes.len() as _,
            without_checksum: true,
            ..Default::default()
        };
        let chunk = engine.update_chunk(chunk_id, &mut req).unwrap();
        drop(chunk);

        let mut req = UpdateReq {
            length: bytes.len() as _,
            data: bytes.as_ptr() as _,
            offset: bytes.len() as _,
            without_checksum: true,
            ..Default::default()
        };
        let chunk = engine.update_chunk(chunk_id, &mut req).unwrap();
        drop(chunk);

        drop(engine);

        let engine = Engine::open(&config).unwrap();
        let chunks = engine.query_uncommitted_chunks(&[]).unwrap();
        assert_eq!(chunks.len(), 1);

        engine.handle_uncommitted_chunks(&[], 2).unwrap();
        let chunk = engine.get(chunk_id).unwrap().unwrap();
        assert_eq!(chunk.meta().len as usize, bytes.len() * 2);
    }

    #[test]
    fn test_engine_concurrent_update_and_get() {
        let dir = tempfile::tempdir().unwrap();

        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };
        let engine = Engine::open(&config).unwrap();

        let chunk_id = "chunk01".as_bytes();
        let bytes = "hello world!".as_bytes();
        let checksum = crc32c::crc32c(bytes);

        let start = std::time::Instant::now();
        let duration = std::time::Duration::from_secs(2);
        let engine_clone = engine.clone();
        let commit_thread = std::thread::spawn(move || {
            while std::time::Instant::now().duration_since(start) < duration {
                engine.write(chunk_id, bytes, 0, checksum).unwrap();
                engine.remove(chunk_id).unwrap();
            }
        });

        let get_thread = std::thread::spawn(move || {
            while std::time::Instant::now().duration_since(start) < duration {
                let _ = engine_clone.get(chunk_id).unwrap();
            }
        });

        let _ = get_thread.join().unwrap();
        let _ = commit_thread.join().unwrap();
    }
}
