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
            for pending in &uncommitted_chunks {
                let chunk_id = &pending.chunk_id;
                let meta = &pending.meta;
                let old_chunk = engine.get(chunk_id)?;

                let prefix: Bytes = chunk_id[..engine.prefix_len].into();
                let mut writing_list = engine.writing_list.entry(prefix).or_default();
                let allocator = engine.allocators.select_by_pos(meta.pos)?;
                let chunk = allocator.reference(meta.clone(), old_chunk.is_none());
                writing_list.insert(
                    chunk_id.clone(),
                    WritingHolder {
                        chunk,
                        abort: true,
                        is_remove: pending.is_remove,
                    },
                );
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

    /// Intentionally leaks Arc pointers to avoid shutdown overhead.
    ///
    /// # Safety
    ///
    /// This function **must only be called** when the process is guaranteed to exit immediately afterwards.
    /// Calling it in any other context (e.g., during a restart, in a test) will cause permanent memory leaks
    /// of the Arc-managed data. This is an internal method intended to be used exactly once in the process exit path.
    pub unsafe fn speed_up_quit(&self) {
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
            Some(chunk) if Arc::ptr_eq(chunk, &old_chunk) => {
                self.meta_store.move_chunk(
                    chunk_id,
                    old_chunk.meta(),
                    new_chunk.meta(),
                    false,
                    true,
                )?;
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
        if req.is_syncing && req.offset != 0 {
            // A syncing write replaces the whole chunk: the caller-provided
            // checksum covers the full payload, and copy_on_write persists it
            // without reading the bytes before `offset`.
            return Err(Error::InvalidArg(format!(
                "chunk id {:?} syncing chunk must start from offset 0",
                chunk_id
            )));
        }

        // 1. prepare update info.
        let data = if req.length != 0 {
            // SAFETY: req.data pointer must be valid and remain live for the duration of this function.
            // The slice is consumed synchronously and immediately within safe_write/copy_on_write calls
            // (lines 386-394, 398-404, 418-424). If writes become asynchronous in the future,
            // this would require refactoring to take ownership of the data (e.g., via Vec<u8>).
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
        // A shrink needs a new slot so a later append cannot overwrite a pinned version.
        let mut new_chunk = match old_chunk {
            Some(old_chunk) if req.is_remove => old_chunk.as_ref().clone(),
            Some(old_chunk)
                if req.is_syncing
                    || ((req.is_truncate || req.length > 0) && req.offset < old_chunk.meta().len)
                    || req.offset + req.length > old_chunk.capacity() =>
            {
                old_chunk.copy_on_write(
                    data,
                    req.offset,
                    req.checksum,
                    req.is_syncing,
                    req.is_truncate,
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
        let entry = writing_list.entry(chunk_id.into());
        let replaces_remove = if let std::collections::hash_map::Entry::Occupied(entry) = &entry {
            let holder = entry.get();
            if !holder.abort {
                return Err(Error::InvalidArg(format!(
                    "chunk {:?} is in writing ({:?})",
                    chunk_id,
                    holder.chunk.meta(),
                )));
            }
            holder.is_remove
        } else {
            false
        };

        // Publish changes to remove intent only after the journal put succeeds.
        let persist_before_publish = req.is_remove || replaces_remove;
        if persist_before_publish {
            self.meta_store
                .persist_writing_chunk(chunk_id, new_chunk.meta(), req.is_remove)?;
        }
        entry.insert_entry(WritingHolder {
            chunk: new_chunk.clone(),
            abort: false,
            is_remove: req.is_remove,
        });
        drop(writing_list);

        if !persist_before_publish {
            self.meta_store
                .persist_writing_chunk(chunk_id, new_chunk.meta(), false)?;
        }

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
            let mut entry = self.meta_cache.entry_by_ref(chunk_id);
            match self.get_with_entry(chunk_id, &mut entry)? {
                Some(old_chunk) => {
                    self.meta_store
                        .remove(&chunk.chunk_id, old_chunk.meta(), sync)?
                }
                None => self.meta_store.remove_writing_chunk(chunk_id, sync)?,
            }
            entry.remove();
            drop(entry);
            chunk.commit_succ();
            return Ok(new_chunk);
        }

        // update rocksdb under lock protection.
        let mut entry = self.meta_cache.entry_by_ref(chunk_id);
        match self.get_with_entry(chunk_id, &mut entry)? {
            Some(old_chunk) => {
                self.meta_store.move_chunk(
                    chunk_id,
                    old_chunk.meta(),
                    new_chunk.meta(),
                    true,
                    sync,
                )?;
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
                match entries.get_mut(&chunk.chunk_id).unwrap().get() {
                    Some(old_chunk) => self.meta_store.remove_mut(
                        &chunk.chunk_id,
                        old_chunk.meta(),
                        &mut write_batch,
                    )?,
                    None => self
                        .meta_store
                        .remove_writing_chunk_mut(&chunk.chunk_id, &mut write_batch),
                }
            } else {
                match entries.get_mut(&chunk.chunk_id).unwrap().get() {
                    Some(old_chunk) => self.meta_store.move_chunk_mut(
                        &chunk.chunk_id,
                        old_chunk.meta(),
                        chunk.meta(),
                        true,
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
                is_remove: holder.is_remove,
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
        let mut chunk_ids = self
            .meta_store
            .query_chunks(begin, end, max_count)?
            .into_iter()
            .map(|(chunk_id, _)| chunk_id)
            .collect::<Vec<Bytes>>();

        const BATCH_SIZE: usize = 4096;
        for batch in chunk_ids.chunks_mut(BATCH_SIZE) {
            batch.sort(); // acquire locks in sequence to avoid deadlocks.

            let mut write_batch = RocksDB::new_write_batch();
            let mut entries = Vec::with_capacity(batch.len());
            for chunk_id in batch.iter() {
                let mut entry = self.meta_cache.entry_by_ref(chunk_id);
                match self.get_with_entry(chunk_id, &mut entry)? {
                    Some(chunk) => {
                        self.meta_store
                            .remove_mut(chunk_id, chunk.meta(), &mut write_batch)?;
                    }
                    None => {
                        // The chunk has been deleted by another thread. Still count it as a successfully deleted one.
                    }
                }
                entries.push(entry);
            }

            self.meta_store.write(write_batch, true)?;

            for mut entry in entries {
                entry.remove();
            }
        }

        Ok(chunk_ids.len() as _)
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

            // SAFETY: This is an exceptional case for testing the function itself.
            // In normal usage, speed_up_quit() should ONLY be called immediately before process exit.
            // This test intentionally violates that contract to verify the function's behavior,
            // but this will cause memory leaks that persist for the remainder of the test process.
            unsafe { engine.speed_up_quit(); }
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
            let data = vec![i as u8; i * 1024];
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
        for i in 1..=N {
            assert!(engine.get(&i.to_be_bytes()).unwrap().is_none());
        }
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
    fn test_engine_truncate_shrink_preserves_reader() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        let cases = [
            (8192, 4096usize),
            (5000, 100),
            (12, 4),
            (12, 0),
            (usize::from(CHUNK_SIZE_SMALL) + 1, 4097),
        ];
        for (old_len, new_len) in cases {
            let chunk_id = &new_len.to_le_bytes();
            let original = vec![0xa5; old_len];
            engine
                .write(chunk_id, &original, 0, crc32c::crc32c(&original))
                .unwrap();
            let v1 = engine.get(chunk_id).unwrap().unwrap();

            let v2 = engine.truncate(chunk_id, new_len as u32).unwrap();
            assert_chunk_data(&v1, &original);
            let aligned_len = new_len.next_multiple_of(ALIGN_SIZE.into());
            let mut truncated = vec![0; aligned_len];
            v2.pread(&mut truncated, 0).unwrap();

            let appended = b"append";
            let v3 = engine
                .write(chunk_id, appended, new_len as u32, crc32c::crc32c(appended))
                .unwrap();
            assert_chunk_data(&v1, &original);
            assert_chunk_data(&v2, &original[..new_len]);
            assert_eq!(&truncated[..new_len], &original[..new_len]);
            assert!(truncated[new_len..].iter().all(|&byte| byte == 0));
            assert_ne!(v2.meta().pos, v1.meta().pos);
            assert_eq!(v2.capacity(), u32::from(CHUNK_SIZE_SMALL));

            let mut expected = original[..new_len].to_vec();
            expected.extend_from_slice(appended);
            assert_chunk_data(&v3, &expected);
        }
    }

    #[test]
    fn test_engine_truncate_extend_in_place() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        for (old_len, new_len) in [(4096, 8192usize), (12, 100)] {
            let chunk_id = &new_len.to_le_bytes();
            let mut expected = vec![0xa5; old_len];
            let v1 = engine
                .write(chunk_id, &expected, 0, crc32c::crc32c(&expected))
                .unwrap();

            let v2 = engine.truncate(chunk_id, new_len as u32).unwrap();
            assert_eq!(v1.meta().pos, v2.meta().pos);
            assert_chunk_data(&v1, &expected);
            expected.resize(new_len, 0);
            assert_chunk_data(&v2, &expected);
        }
    }

    #[track_caller]
    fn assert_chunk_data(chunk: &Chunk, expected: &[u8]) {
        assert_eq!(chunk.meta().len as usize, expected.len());
        let mut actual = vec![0; expected.len()];
        chunk.pread(&mut actual, 0).unwrap();
        assert_eq!(actual.as_slice(), expected);
        assert_eq!(chunk.meta().checksum, crc32c::crc32c(&actual));
    }

    /// Overwrite the whole on-disk slot of `chunk` behind the engine's back.
    fn corrupt_chunk_slot(chunk: &Chunk) {
        let mut buf = create_aligned_buf(CHUNK_SIZE_SMALL);
        buf.fill(0xAA);
        let FdAndOffset { fd, offset } = chunk.fd_and_offset();
        let written = unsafe {
            libc::pwrite(
                fd,
                buf.as_ptr() as *const libc::c_void,
                buf.len(),
                offset as libc::off_t,
            )
        };
        assert_eq!(
            written as usize,
            buf.len(),
            "failed to corrupt chunk slot: {:?}",
            std::io::Error::last_os_error()
        );
    }

    #[test]
    fn test_engine_copy_on_write_detects_corrupted_old_data() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        let chunk_id = b"cow-corrupt";
        let data = vec![0xA5u8; 4096];
        let checksum = crc32c::crc32c(&data);
        engine.write(chunk_id, &data, 0, checksum).unwrap();

        let chunk = engine.get(chunk_id).unwrap().unwrap();
        corrupt_chunk_slot(&chunk);

        // A partial overwrite merges the old chunk data into a replacement.
        // It must fail instead of silently persisting the corrupted bytes.
        let new_data = b"overwrite";
        let mut req = UpdateReq {
            length: new_data.len() as _,
            offset: 0,
            data: new_data.as_ptr() as _,
            checksum: crc32c::crc32c(new_data),
            ..Default::default()
        };
        let err = engine.update_chunk(chunk_id, &mut req).unwrap_err();
        assert!(
            matches!(err, Error::ChecksumMismatch(_)),
            "expected ChecksumMismatch, got {err:?}"
        );

        // The committed metadata must stay untouched after the rejected write.
        let meta = engine.get(chunk_id).unwrap().unwrap().meta().clone();
        assert_eq!(meta.len as usize, data.len());
        assert_eq!(meta.checksum, checksum);
    }

    #[test]
    fn test_engine_truncate_detects_corrupted_old_data() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        let chunk_id = b"truncate-corrupt";
        let data = vec![0x5Au8; 8192];
        let checksum = crc32c::crc32c(&data);
        engine.write(chunk_id, &data, 0, checksum).unwrap();

        let chunk = engine.get(chunk_id).unwrap().unwrap();
        corrupt_chunk_slot(&chunk);

        // Shrinking truncate also copies old chunk bytes into a new slot; it
        // must verify the stored checksum before doing so.
        let err = engine.truncate(chunk_id, 5000).unwrap_err();
        assert!(
            matches!(err, Error::ChecksumMismatch(_)),
            "expected ChecksumMismatch, got {err:?}"
        );

        let meta = engine.get(chunk_id).unwrap().unwrap().meta().clone();
        assert_eq!(meta.len as usize, data.len());
        assert_eq!(meta.checksum, checksum);
    }

    #[test]
    fn test_engine_rejects_syncing_write_with_nonzero_offset() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        let chunk_id = b"syncing-offset";
        let data = b"hi!";
        let mut req = UpdateReq {
            is_syncing: true,
            offset: 1,
            length: data.len() as _,
            data: data.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let err = engine.update_chunk(chunk_id, &mut req).unwrap_err();
        assert!(
            matches!(err, Error::InvalidArg(_)),
            "syncing writes must start at offset 0, got {err:?}"
        );

        // Offset 0 remains supported for syncing writes.
        let mut req = UpdateReq {
            is_syncing: true,
            offset: 0,
            length: data.len() as _,
            data: data.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let chunk = engine.update_chunk(chunk_id, &mut req).unwrap();
        engine.commit_chunk(chunk, false).unwrap();
        let chunk = engine.get(chunk_id).unwrap().unwrap();
        let mut buf = vec![0u8; data.len()];
        chunk.pread(&mut buf, 0).unwrap();
        assert_eq!(buf.as_slice(), data);
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

    fn check_uncommitted_remove_recovery(reopen: bool) {
        use derse::Serialize;

        let dir = tempfile::tempdir().unwrap();
        let mut config = EngineConfig {
            path: dir.path().into(),
            create: true,
            prefix_len: 1,
        };
        let engine = Engine::open(&config).unwrap();
        config.create = false;

        let existing: &[u8] = b"p/existing";
        let missing: &[u8] = b"p/missing";
        let empty_write: &[u8] = b"p/empty";
        let data = b"kept until commit";
        engine
            .write(existing, data, 0, crc32c::crc32c(data))
            .unwrap();

        for id in [existing, missing, empty_write] {
            let mut req = UpdateReq {
                is_remove: true,
                update_ver: 2,
                ..Default::default()
            };
            drop(engine.update_chunk(id, &mut req).unwrap());
        }

        // A retry can replace an aborted removal with an ordinary zero-length write.
        let chunk = engine
            .update_chunk(empty_write, &mut UpdateReq::default())
            .unwrap();
        {
            let legacy_bytes: derse::DownwardBytes = chunk.meta().serialize().unwrap();
            let key = MetaKey::writing_chunk_key(empty_write);
            let mut journal = engine.meta_store.iterator();
            journal.seek(&key).unwrap();
            assert_eq!(journal.key(), Some(key.as_ref()));
            assert_eq!(journal.value(), Some(legacy_bytes.as_slice()));
        }
        drop(chunk);

        let engine = if reopen {
            drop(engine);
            Engine::open(&config).unwrap()
        } else {
            engine
        };
        assert_eq!(engine.get(existing).unwrap().unwrap().meta().chunk_ver, 1);
        assert!(engine.get(missing).unwrap().is_none());
        assert!(engine.get(empty_write).unwrap().is_none());
        assert_eq!(engine.query_uncommitted_chunks(b"p").unwrap().len(), 3);

        let recovered = engine.handle_uncommitted_chunks(b"p", 7).unwrap();
        assert_eq!(recovered.len(), 3);
        assert!(recovered.iter().all(|(_, meta)| meta.chain_ver == 7));
        assert!(engine.get(existing).unwrap().is_none());
        assert!(engine.get(missing).unwrap().is_none());
        assert!(engine.query_uncommitted_chunks(b"p").unwrap().is_empty());
        assert_eq!(engine.used_size().position_count, 1);
        drop(engine);

        let engine = Engine::open(&config).unwrap();
        assert!(engine.handle_uncommitted_chunks(b"p", 8).unwrap().is_empty());
        let chunks = engine.query_chunks(b"p", b"p", u64::MAX).unwrap();
        assert_eq!(chunks.len(), 1);
        assert_eq!(chunks[0].0.as_slice(), empty_write);
        assert_eq!(chunks[0].1.len, 0);
        assert_eq!(chunks[0].1.chain_ver, 7);
        assert!(!chunks[0].1.uncommitted);
        assert_eq!(
            engine.meta_store.query_used_size(b"p").unwrap(),
            u64::from(CHUNK_SIZE_SMALL)
        );
    }

    #[test]
    fn test_engine_recovers_aborted_uncommitted_removes() {
        check_uncommitted_remove_recovery(false);
    }

    #[test]
    fn test_engine_recovers_uncommitted_removes_after_reopen() {
        check_uncommitted_remove_recovery(true);
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

    #[test]
    fn test_move_chunk_preserves_etag() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        let chunk_id = b"etag-relocation";
        let data = b"etag payload";
        let mut req = UpdateReq {
            length: data.len() as _,
            data: data.as_ptr() as _,
            checksum: crc32c::crc32c(data),
            desired_tag: b"my-client-tag",
            ..Default::default()
        };
        let writing = engine.update_chunk(chunk_id, &mut req).unwrap();
        engine.commit_chunk(writing, false).unwrap();
        assert_eq!(
            engine
                .get(chunk_id)
                .unwrap()
                .unwrap()
                .meta()
                .etag
                .as_slice(),
            b"my-client-tag".as_slice()
        );

        // Compaction relocation copies the chunk into a new slot; it must not
        // drop the client-visible etag, otherwise CAS writers would fail.
        let moved = engine.move_chunk(chunk_id).unwrap().unwrap();
        assert_eq!(
            moved.meta().etag.as_slice(),
            b"my-client-tag".as_slice(),
            "compaction relocation must preserve the client-visible etag"
        );
        assert_eq!(
            engine
                .get(chunk_id)
                .unwrap()
                .unwrap()
                .meta()
                .etag
                .as_slice(),
            b"my-client-tag".as_slice()
        );
    }

    #[test]
    fn test_move_chunk_preserves_pending_writing_chunk_journal() {
        let dir = tempfile::tempdir().unwrap();
        let config = EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };
        let engine = Engine::open(&config).unwrap();

        // Case 1: in-place append (v2.pos == v1.pos) followed by compaction move_chunk + crash.
        let chunk_id = b"chunk01";
        let v1_bytes = b"hello world!";
        let mut req1 = UpdateReq {
            length: v1_bytes.len() as _,
            data: v1_bytes.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let v1 = engine.update(chunk_id, &mut req1).unwrap();
        assert_eq!(v1.meta().len as usize, v1_bytes.len());

        let v2_bytes = b"second-write-payload!";
        let mut req2 = UpdateReq {
            length: v2_bytes.len() as _,
            data: v2_bytes.as_ptr() as _,
            offset: v1_bytes.len() as _,
            without_checksum: true,
            ..Default::default()
        };
        let v2_writing = engine.update_chunk(chunk_id, &mut req2).unwrap();
        let expected_v2_payload = [v1_bytes.as_slice(), v2_bytes.as_slice()].concat();
        assert_eq!(v2_writing.meta().pos, v1.meta().pos);
        assert_eq!(v2_writing.meta().len as usize, expected_v2_payload.len());

        // Background compaction relocates committed v1 while v2 is still staged/uncommitted.
        let moved = engine.move_chunk(chunk_id).unwrap();
        assert!(moved.is_some());
        assert_eq!(moved.unwrap().meta().len as usize, v1_bytes.len());

        // Timestamp index must remain intact after compaction relocation.
        let by_ts = engine
            .query_chunks_by_timestamp(&[], 0, u64::MAX, 100)
            .unwrap();
        assert!(by_ts.iter().any(|(id, _)| id.as_ref() == chunk_id));

        // Case 2: COW overwrite (v2.pos != v1.pos) followed by compaction move_chunk + crash.
        let cow_chunk_id = b"chunk02";
        let cow_v1 = b"initial-cow-v1";
        let mut cow_req1 = UpdateReq {
            length: cow_v1.len() as _,
            data: cow_v1.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        let cow_c1 = engine.update(cow_chunk_id, &mut cow_req1).unwrap();

        let cow_v2 = b"overwritten-cow-v2-data!";
        let mut cow_req2 = UpdateReq {
            length: cow_v2.len() as _,
            data: cow_v2.as_ptr() as _,
            offset: 0,
            without_checksum: true,
            ..Default::default()
        };
        let cow_v2_writing = engine.update_chunk(cow_chunk_id, &mut cow_req2).unwrap();
        assert_ne!(cow_v2_writing.meta().pos, cow_c1.meta().pos);

        let cow_moved = engine.move_chunk(cow_chunk_id).unwrap();
        assert!(cow_moved.is_some());

        // Case 3: normal commit after move_chunk must still clear writing_chunk_key.
        let committed_id = b"chunk03";
        let c3_v1 = b"commit-after-move-v1";
        let mut c3_req1 = UpdateReq {
            length: c3_v1.len() as _,
            data: c3_v1.as_ptr() as _,
            without_checksum: true,
            ..Default::default()
        };
        engine.update(committed_id, &mut c3_req1).unwrap();
        let c3_v2 = b"commit-after-move-v2!";
        let mut c3_req2 = UpdateReq {
            length: c3_v2.len() as _,
            data: c3_v2.as_ptr() as _,
            offset: 0,
            without_checksum: true,
            ..Default::default()
        };
        let c3_writing = engine.update_chunk(committed_id, &mut c3_req2).unwrap();
        assert!(engine.move_chunk(committed_id).unwrap().is_some());
        engine.commit_chunk(c3_writing, true).unwrap();

        // Simulate crash before chunk01/chunk02 v2 commits locally, then reopen and recover.
        drop(v2_writing);
        drop(cow_v2_writing);
        drop(engine);

        let reopen_cfg = EngineConfig {
            path: dir.path().into(),
            create: false,
            ..Default::default()
        };
        let engine = Engine::open(&reopen_cfg).unwrap();
        let uncommitted = engine.query_uncommitted_chunks(&[]).unwrap();
        assert_eq!(
            uncommitted.len(),
            2,
            "pending v2 journals were erased by move_chunk!"
        );

        engine.handle_uncommitted_chunks(&[], 2).unwrap();

        let recovered1 = engine.get(chunk_id).unwrap().unwrap();
        assert_eq!(recovered1.meta().len as usize, expected_v2_payload.len());
        let mut buf1 = vec![0u8; expected_v2_payload.len()];
        recovered1.pread(&mut buf1, 0).unwrap();
        assert_eq!(buf1, expected_v2_payload);
        assert_eq!(recovered1.meta().checksum, crc32c::crc32c(&buf1));

        let recovered2 = engine.get(cow_chunk_id).unwrap().unwrap();
        assert_eq!(recovered2.meta().len as usize, cow_v2.len());
        let mut buf2 = vec![0u8; cow_v2.len()];
        recovered2.pread(&mut buf2, 0).unwrap();
        assert_eq!(buf2, cow_v2);
        assert_eq!(recovered2.meta().checksum, crc32c::crc32c(&buf2));

        let recovered3 = engine.get(committed_id).unwrap().unwrap();
        let mut buf3 = vec![0u8; c3_v2.len()];
        recovered3.pread(&mut buf3, 0).unwrap();
        assert_eq!(buf3, c3_v2);

        // Second reopen verifies RocksDB metadata & group bitmaps remain fully consistent.
        drop(recovered1);
        drop(recovered2);
        drop(recovered3);
        drop(engine);

        let engine = Engine::open(&reopen_cfg).unwrap();
        assert!(engine.query_uncommitted_chunks(&[]).unwrap().is_empty());
        assert_eq!(
            engine.get(chunk_id).unwrap().unwrap().meta().len as usize,
            expected_v2_payload.len()
        );
        assert_eq!(
            engine.get(cow_chunk_id).unwrap().unwrap().meta().len as usize,
            cow_v2.len()
        );
    }

    #[test]
    fn test_syncing_shrink_allocates_target_size_and_zero_pads_tail() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        // Poison the thread-local Chunk::BUFFER with bytes from an unrelated chunk.
        let large_size = 256 * 1024usize;
        let donor = vec![0xABu8; large_size - 17];
        engine
            .write(b"donor", &donor, 0, crc32c::crc32c(&donor))
            .unwrap();

        let big_payload = vec![0x55u8; large_size];
        let big_crc = crc32c::crc32c(&big_payload);
        let old_small = engine.write(b"sync-shrink-small", &big_payload, 0, big_crc).unwrap();
        let old_empty = engine.write(b"sync-shrink-empty", &big_payload, 0, big_crc).unwrap();
        assert_eq!(old_small.capacity(), large_size as u32);
        assert_eq!(old_empty.capacity(), large_size as u32);
        let used_before = engine.meta_store.query_used_size(&[]).unwrap();

        // Syncing replacement with a small unaligned payload must allocate the
        // target size class (64 KiB, not 256 KiB) and zero-pad the aligned tail.
        let tiny = b"tiny";
        let mut req = UpdateReq {
            is_syncing: true,
            update_ver: 5,
            offset: 0,
            length: tiny.len() as _,
            data: tiny.as_ptr() as _,
            checksum: crc32c::crc32c(tiny),
            ..Default::default()
        };
        let synced_small = engine.update(b"sync-shrink-small", &mut req).unwrap();
        assert_eq!(synced_small.capacity(), u32::from(CHUNK_SIZE_SMALL));
        assert_eq!(synced_small.meta().len as usize, tiny.len());
        assert_eq!(synced_small.meta().checksum, crc32c::crc32c(tiny));
        let mut aligned = create_aligned_buf(ALIGN_SIZE);
        aligned.fill(0xEE);
        synced_small.pread(&mut aligned, 0).unwrap();
        assert_eq!(&aligned[..tiny.len()], tiny);
        assert!(
            aligned[tiny.len()..].iter().all(|&b| b == 0),
            "syncing write leaked stale thread-local buffer bytes into aligned tail"
        );

        // Syncing replacement with an empty payload must allocate 64 KiB and
        // must not flush stale buffer bytes to disk.
        engine.metrics.pwrite_times.swap(0, Ordering::AcqRel);
        let mut empty_req = UpdateReq {
            is_syncing: true,
            update_ver: 5,
            offset: 0,
            length: 0,
            checksum: 0,
            ..Default::default()
        };
        let synced_empty = engine.update(b"sync-shrink-empty", &mut empty_req).unwrap();
        assert_eq!(synced_empty.capacity(), u32::from(CHUNK_SIZE_SMALL));
        assert_eq!(synced_empty.meta().len, 0);
        assert_eq!(synced_empty.meta().checksum, 0);
        assert_eq!(
            engine.metrics.pwrite_times.load(Ordering::Acquire),
            0,
            "zero-length syncing write should not issue disk pwrite"
        );

        let expected_reclaimed = 2 * (large_size as u64 - u64::from(CHUNK_SIZE_SMALL));
        assert_eq!(
            engine.meta_store.query_used_size(&[]).unwrap(),
            used_before - expected_reclaimed
        );

        // Disabling new group allocation must not fail a syncing shrink from an
        // unallocated/full larger size class when a 64 KiB slot is available.
        let huge = vec![0x77u8; usize::from(CHUNK_SIZE_NORMAL)];
        let old_huge = engine
            .write(b"sync-shrink-no-space", &huge, 0, crc32c::crc32c(&huge))
            .unwrap();
        assert_eq!(old_huge.capacity(), u32::from(CHUNK_SIZE_NORMAL));
        // Exhaust remaining slots in the 512 KiB group so allocating another
        // 512 KiB chunk would fail once allow_to_allocate is false.
        let mut fillers = Vec::new();
        for _ in 0..(GroupState::TOTAL_BITS - 1) {
            fillers.push(
                engine
                    .allocators
                    .allocate(CHUNK_SIZE_NORMAL, true)
                    .unwrap(),
            );
        }
        engine.set_allow_to_allocate(false);

        let ok_data = b"ok";
        let mut shrink_req = UpdateReq {
            is_syncing: true,
            update_ver: 9,
            offset: 0,
            length: ok_data.len() as _,
            data: ok_data.as_ptr() as _,
            checksum: crc32c::crc32c(ok_data),
            ..Default::default()
        };
        let synced_ok = engine
            .update(b"sync-shrink-no-space", &mut shrink_req)
            .unwrap();
        assert_eq!(synced_ok.capacity(), u32::from(CHUNK_SIZE_SMALL));
        assert_chunk_data(&synced_ok, ok_data);
        drop(fillers);
    }

    #[test]
    fn test_unaligned_writes_zero_pad_aligned_tail() {
        let dir = tempfile::tempdir().unwrap();
        let engine = Engine::open(&EngineConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        })
        .unwrap();

        // Populate thread-local Chunk::BUFFER with non-zero bytes from another chunk.
        let donor = vec![0xCDu8; 8190];
        engine
            .write(b"donor", &donor, 0, crc32c::crc32c(&donor))
            .unwrap();

        let assert_aligned_tail_zero = |chunk: &Chunk, expected: &[u8]| {
            assert_chunk_data(chunk, expected);
            let aligned_len = expected.len().next_multiple_of(ALIGN_SIZE.into());
            let mut buf = create_aligned_buf(Size::from(aligned_len));
            buf.fill(0xEE);
            chunk.pread(&mut buf[..aligned_len], 0).unwrap();
            assert_eq!(&buf[..expected.len()], expected);
            assert!(
                buf[expected.len()..aligned_len].iter().all(|&b| b == 0),
                "stale thread-local buffer bytes leaked into aligned tail"
            );
        };

        // 1. Fresh chunk (safe_write indirect append).
        let c1 = engine
            .write(b"tail", b"abc", 0, crc32c::crc32c(b"abc"))
            .unwrap();
        assert_aligned_tail_zero(&c1, b"abc");

        // Re-poison Chunk::BUFFER before each path.
        engine
            .write(b"donor2", &donor, 0, crc32c::crc32c(&donor))
            .unwrap();

        // 2. In-place indirect append (safe_write).
        let c2 = engine
            .write(b"tail", b"def", 3, crc32c::crc32c(b"def"))
            .unwrap();
        assert_aligned_tail_zero(&c2, b"abcdef");

        engine
            .write(b"donor3", &donor, 0, crc32c::crc32c(&donor))
            .unwrap();

        // 3. Full overwrite via copy_on_write (skip_read = true).
        let full = b"0123456789";
        let c3 = engine
            .write(b"tail", full, 0, crc32c::crc32c(full))
            .unwrap();
        assert_aligned_tail_zero(&c3, full);

        engine
            .write(b"donor4", &donor, 0, crc32c::crc32c(&donor))
            .unwrap();

        // 4. Partial overwrite extending chunk via copy_on_write (skip_read = false).
        let ext = b"XXXXXXXXXX";
        let c4 = engine
            .write(b"tail", ext, 4, crc32c::crc32c(ext))
            .unwrap();
        assert_aligned_tail_zero(&c4, b"0123XXXXXXXXXX");
    }
}
