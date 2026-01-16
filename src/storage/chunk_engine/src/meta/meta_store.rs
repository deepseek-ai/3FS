use std::{cell::RefCell, collections::HashMap, ops::DerefMut};

use super::super::*;
use byteorder::{ByteOrder, LittleEndian};
use derse::{Deserialize, DownwardBytes, Serialize};

#[derive(Debug, Default, Clone)]
pub struct MetaStoreConfig {
    pub rocksdb: RocksDBConfig,
    pub prefix_len: usize,
}

pub struct MetaStore {
    rocksdb: RocksDB,
    config: MetaStoreConfig,
}

impl MetaStore {
    thread_local! {
        static BYTES: RefCell<DownwardBytes> = RefCell::new(DownwardBytes::with_capacity(Size::MB.into()));
    }

    pub fn open(config: &MetaStoreConfig) -> Result<Self> {
        let rocksdb = RocksDB::open::<MetaMergeOp>(&config.rocksdb)?;

        let mut this = MetaStore {
            rocksdb,
            config: config.clone(),
        };

        this.update_used_size_if_need()?;

        Ok(this)
    }

    pub fn get_chunk_meta(&self, chunk_id: &[u8]) -> Result<Option<ChunkMeta>> {
        let chunk_meta_key = MetaKey::chunk_meta_key(chunk_id);
        let value = self.rocksdb.get(chunk_meta_key)?;

        if let Some(value) = value {
            Ok(Some(
                ChunkMeta::deserialize(value.as_ref()).map_err(Error::SerializationError)?,
            ))
        } else {
            Ok(None)
        }
    }

    pub fn query_chunks(
        &self,
        begin: impl AsRef<[u8]>,
        end: impl AsRef<[u8]>,
        max_count: u64,
    ) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let it = self.iterator();
        self.query_chunks_from_iterator(it, begin, end, max_count)
    }

    pub fn query_chunks_from_iterator(
        &self,
        mut it: RocksDBIterator,
        begin: impl AsRef<[u8]>,
        end: impl AsRef<[u8]>,
        max_count: u64,
    ) -> Result<Vec<(Bytes, ChunkMeta)>> {
        let mut out = Vec::<(Bytes, ChunkMeta)>::with_capacity(4096);

        let end_key = MetaKey::chunk_meta_key(end.as_ref());
        it.seek(&end_key)?;

        if it.key() == Some(end_key.as_ref()) {
            it.next(); // [begin, end)
        }

        for _ in 0..max_count {
            if !it.valid() {
                break;
            }

            if it.key().unwrap()[0] != MetaKey::CHUNK_META_KEY_PREFIX {
                break;
            }

            let chunk_id = MetaKey::parse_chunk_meta_key(it.key().unwrap());
            if begin.as_ref() <= chunk_id.as_ref() {
                let chunk_meta = ChunkMeta::deserialize(it.value().unwrap())
                    .map_err(Error::SerializationError)?;
                out.push((chunk_id, chunk_meta))
            } else {
                break;
            }

            it.next();
        }

        Ok(out)
    }

    pub fn query_chunks_by_timestamp(
        &self,
        prefix: &[u8],
        begin: u64,
        end: u64,
        max_count: u64,
    ) -> Result<Vec<Bytes>> {
        let mut it = self.iterator();
        let mut out = Vec::<Bytes>::with_capacity(4096);

        let begin_key = MetaKey::timestamp_key_filter(prefix, begin);
        it.seek(&begin_key)?;

        for _ in 0..max_count {
            if !it.valid() {
                break;
            }

            let key = it.key().unwrap();
            if key[0] != MetaKey::TIMESTAMP_KEY_PREFIX {
                break;
            }
            if key.len() <= prefix.len() || &key[1..1 + self.config.prefix_len] != prefix {
                break;
            }

            let (timestamp, chunk_id) = MetaKey::parse_timestamp_key(key, self.config.prefix_len)?;
            if timestamp < end {
                out.push(chunk_id)
            } else {
                break;
            }

            it.next();
        }

        Ok(out)
    }

    #[inline(always)]
    pub fn write(&self, write_batch: rocksdb::WriteBatch, sync: bool) -> Result<()> {
        self.rocksdb.write(write_batch, sync)
    }

    pub fn add_chunk(&self, chunk_id: &[u8], chunk_meta: &ChunkMeta, sync: bool) -> Result<()> {
        let mut write_batch = RocksDB::new_write_batch();
        self.add_chunk_mut(chunk_id, chunk_meta, &mut write_batch)?;
        self.write(write_batch, sync)
    }

    pub fn add_chunk_mut(
        &self,
        chunk_id: &[u8],
        chunk_meta: &ChunkMeta,
        write_batch: &mut rocksdb::WriteBatch,
    ) -> Result<()> {
        // 1. add chunk meta.
        let chunk_meta_key = MetaKey::chunk_meta_key(chunk_id);
        Self::with_tls_bytes(|bytes| {
            chunk_meta
                .serialize_to(bytes)
                .map_err(Error::SerializationError)?;
            write_batch.put(chunk_meta_key, &bytes[..]);
            Ok(())
        })?;

        // 2. add pos->chunk map.
        let pos_to_chunk_key = MetaKey::pos_to_chunk_key(chunk_meta.pos);
        write_batch.put(pos_to_chunk_key, chunk_id);

        // 3. update group bits.
        let group_bits_key = MetaKey::group_bits_key(chunk_meta.pos.group_id());
        Self::with_tls_bytes(|bytes| {
            MergeState::acquire(chunk_meta.pos.index())
                .serialize_to(bytes)
                .map_err(Error::SerializationError)?;
            write_batch.merge(group_bits_key, &bytes[..]);
            Ok(())
        })?;

        // 4. update used size.
        self.update_used_size(chunk_id, chunk_meta.pos.chunk_size().0 as i64, write_batch)?;

        // 5. add timestamp->chunk map.
        let timestamp_key =
            MetaKey::timestamp_key(chunk_meta.timestamp, chunk_id, self.config.prefix_len);
        write_batch.put(timestamp_key, chunk_id);

        // 6. remove writing chunk log.
        self.remove_writing_chunk_mut(chunk_id, write_batch);

        Ok(())
    }

    pub fn move_chunk(
        &self,
        chunk_id: &[u8],
        old_meta: &ChunkMeta,
        new_meta: &ChunkMeta,
        sync: bool,
    ) -> Result<()> {
        let mut write_batch = RocksDB::new_write_batch();
        self.move_chunk_mut(chunk_id, old_meta, new_meta, &mut write_batch)?;
        self.write(write_batch, sync)
    }

    pub fn move_chunk_mut(
        &self,
        chunk_id: &[u8],
        old_meta: &ChunkMeta,
        new_meta: &ChunkMeta,
        write_batch: &mut rocksdb::WriteBatch,
    ) -> Result<()> {
        // 1. change chunk meta.
        let chunk_meta_key = MetaKey::chunk_meta_key(chunk_id);
        Self::with_tls_bytes(|bytes| {
            new_meta
                .serialize_to(bytes)
                .map_err(Error::SerializationError)?;
            write_batch.put(chunk_meta_key, bytes.as_slice());
            Ok(())
        })?;

        if old_meta.pos != new_meta.pos {
            // 2. remove old pos->chunk map.
            let old_pos = old_meta.pos;
            let pos_to_chunk_key = MetaKey::pos_to_chunk_key(old_pos);
            write_batch.delete(pos_to_chunk_key);

            let group_bits_key = MetaKey::group_bits_key(old_pos.group_id());
            Self::with_tls_bytes(|bytes| {
                MergeState::release(old_pos.index())
                    .serialize_to(bytes)
                    .map_err(Error::SerializationError)?;
                write_batch.merge(group_bits_key, &bytes[..]);
                Ok(())
            })?;

            // 3. add new pos->chunk map.
            let pos_to_chunk_key = MetaKey::pos_to_chunk_key(new_meta.pos);
            write_batch.put(pos_to_chunk_key, chunk_id);

            let group_bits_key = MetaKey::group_bits_key(new_meta.pos.group_id());
            Self::with_tls_bytes(|bytes| {
                MergeState::acquire(new_meta.pos.index())
                    .serialize_to(bytes)
                    .map_err(Error::SerializationError)?;
                write_batch.merge(group_bits_key, &bytes[..]);
                Ok(())
            })?;

            // 4. update used size.
            self.update_used_size(
                chunk_id,
                new_meta.pos.chunk_size().0 as i64 - old_pos.chunk_size().0 as i64,
                write_batch,
            )?;
        }

        // 5. update timestamp->chunk map.
        self.check_chunk_id(chunk_id)?;
        let timestamp_key =
            MetaKey::timestamp_key(new_meta.timestamp, chunk_id, self.config.prefix_len);
        write_batch.put(timestamp_key, []);
        let timestamp_key =
            MetaKey::timestamp_key(old_meta.timestamp, chunk_id, self.config.prefix_len);
        write_batch.delete(timestamp_key);

        // 6. remove writing chunk log.
        self.remove_writing_chunk_mut(chunk_id, write_batch);

        Ok(())
    }

    pub fn remove(&self, chunk_id: &[u8], chunk_meta: &ChunkMeta, sync: bool) -> Result<()> {
        let mut write_batch = RocksDB::new_write_batch();
        self.remove_mut(chunk_id, chunk_meta, &mut write_batch)?;
        self.write(write_batch, sync)
    }

    pub fn remove_mut(
        &self,
        chunk_id: &[u8],
        chunk_meta: &ChunkMeta,
        write_batch: &mut rocksdb::WriteBatch,
    ) -> Result<()> {
        // 1. delete chunk meta.
        let chunk_meta_key = MetaKey::chunk_meta_key(chunk_id);
        write_batch.delete(chunk_meta_key);

        // 2. delete pos->chunk map.
        let pos_to_chunk_key = MetaKey::pos_to_chunk_key(chunk_meta.pos);
        write_batch.delete(pos_to_chunk_key);

        // 3. release position.
        let group_bits_key = MetaKey::group_bits_key(chunk_meta.pos.group_id());
        Self::with_tls_bytes(|bytes| {
            MergeState::release(chunk_meta.pos.index())
                .serialize_to(bytes)
                .map_err(Error::SerializationError)?;
            write_batch.merge(group_bits_key, &bytes[..]);
            Ok(())
        })?;

        // 4. update used size.
        self.update_used_size(
            chunk_id,
            -(chunk_meta.pos.chunk_size().0 as i64),
            write_batch,
        )?;

        // 5. delete timestamp->chunk map.
        let timestamp_key =
            MetaKey::timestamp_key(chunk_meta.timestamp, chunk_id, self.config.prefix_len);
        write_batch.delete(timestamp_key);

        // 6. remove writing chunk log.
        self.remove_writing_chunk_mut(chunk_id, write_batch);

        Ok(())
    }

    pub fn allocate_group(&self, group_id: GroupId) -> Result<()> {
        let group_bits_key = MetaKey::group_bits_key(group_id);
        self.rocksdb
            .put(group_bits_key, GroupState::empty().as_bytes(), true)
    }

    pub fn remove_group(&self, group_id: GroupId) -> Result<()> {
        let group_bits_key = MetaKey::group_bits_key(group_id);
        self.rocksdb.delete(group_bits_key, true)
    }

    pub fn iterator(&self) -> RocksDBIterator {
        self.rocksdb.new_iterator()
    }

    fn update_used_size(
        &self,
        chunk_id: &[u8],
        diff: i64,
        write_batch: &mut rocksdb::WriteBatch,
    ) -> Result<()> {
        self.check_chunk_id(chunk_id)?;
        let used_size_key = MetaKey::used_size_key(&chunk_id[..self.config.prefix_len]);
        write_batch.merge(used_size_key, diff.to_le_bytes());
        Ok(())
    }

    pub fn persist_writing_chunk(&self, chunk_id: &[u8], chunk_meta: &ChunkMeta) -> Result<()> {
        let chunk_meta_key = MetaKey::writing_chunk_key(chunk_id);
        Self::with_tls_bytes(|bytes| {
            chunk_meta
                .serialize_to(bytes)
                .map_err(Error::SerializationError)?;
            self.rocksdb.put(chunk_meta_key, &bytes[..], true)
        })
    }

    pub fn remove_writing_chunk(&self, chunk_id: &[u8], sync: bool) -> Result<()> {
        let chunk_meta_key = MetaKey::writing_chunk_key(chunk_id);
        self.rocksdb.delete(chunk_meta_key, sync)
    }

    pub fn remove_writing_chunk_mut(&self, chunk_id: &[u8], write_batch: &mut rocksdb::WriteBatch) {
        write_batch.delete(MetaKey::writing_chunk_key(chunk_id));
    }

    pub fn occupy_uncommitted_positions(&mut self) -> Result<Vec<(Bytes, ChunkMeta, bool)>> {
        let mut prefix_len = 0;
        std::mem::swap(&mut self.config.prefix_len, &mut prefix_len);
        let list = self.query_uncommitted_chunks(&[])?;
        std::mem::swap(&mut self.config.prefix_len, &mut prefix_len);

        let mut uncommitted_chunks = vec![];
        let mut write_batch = RocksDB::new_write_batch();
        let mut count = 0;
        for (chunk_id, writing_meta) in list {
            let pos = writing_meta.pos;
            match self.get_chunk_meta(&chunk_id)? {
                Some(meta) if meta.pos == writing_meta.pos => {
                    uncommitted_chunks.push((chunk_id, writing_meta, false));
                }
                _ => {
                    uncommitted_chunks.push((chunk_id.clone(), writing_meta, true));

                    count += 1;
                    let pos_to_chunk_key = MetaKey::pos_to_chunk_key(pos);
                    write_batch.put(pos_to_chunk_key, chunk_id);

                    let group_bits_key = MetaKey::group_bits_key(pos.group_id());
                    Self::with_tls_bytes(|bytes| {
                        MergeState::acquire(pos.index())
                            .serialize_to(bytes)
                            .map_err(Error::SerializationError)?;
                        write_batch.merge(group_bits_key, &bytes[..]);
                        Ok(())
                    })?;
                }
            }
        }
        if !uncommitted_chunks.is_empty() {
            self.write(write_batch, true)?;
            tracing::info!("occupy {} positions for writing chunks", count);
        }
        Ok(uncommitted_chunks)
    }

    pub fn vacate_uncommitted_positions(
        &self,
        uncommitted_chunks: Vec<(Bytes, ChunkMeta, bool)>,
    ) -> Result<()> {
        let mut write_batch = RocksDB::new_write_batch();
        let mut count = 0;
        for (_, chunk_meta, occupied) in uncommitted_chunks {
            if !occupied {
                continue;
            }

            count += 1;
            let pos_to_chunk_key = MetaKey::pos_to_chunk_key(chunk_meta.pos);
            write_batch.delete(pos_to_chunk_key);

            let group_bits_key = MetaKey::group_bits_key(chunk_meta.pos.group_id());
            Self::with_tls_bytes(|bytes| {
                MergeState::release(chunk_meta.pos.index())
                    .serialize_to(bytes)
                    .map_err(Error::SerializationError)?;
                write_batch.merge(group_bits_key, &bytes[..]);
                Ok(())
            })?;
        }
        self.write(write_batch, true)?;
        tracing::info!("vacate {} positions for writing chunks", count);
        Ok(())
    }

    fn query_uncommitted_chunks(&self, prefix: &[u8]) -> Result<Vec<(Bytes, ChunkMeta)>> {
        self.check_prefix(prefix)?;

        let mut it = self.iterator();
        let mut out = Vec::<(Bytes, ChunkMeta)>::with_capacity(4096);

        let end_key = MetaKey::writing_chunk_key(prefix);
        it.seek(&end_key)?;

        if it.key() == Some(end_key.as_ref()) {
            it.next(); // [begin, end)
        }

        loop {
            if !it.valid() {
                break;
            }

            if it.key().unwrap()[0] != MetaKey::WRITING_CHUNK_KEY_PREFIX {
                break;
            }

            let chunk_id = MetaKey::parse_writing_chunk_key(it.key().unwrap())?;
            if prefix <= chunk_id.as_ref() {
                let chunk_meta = ChunkMeta::deserialize(it.value().unwrap())
                    .map_err(Error::SerializationError)?;
                out.push((chunk_id, chunk_meta))
            } else {
                break;
            }

            it.next();
        }

        Ok(out)
    }

    fn check_chunk_id(&self, chunk_id: &[u8]) -> Result<()> {
        let prefix_len = self.config.prefix_len;
        if chunk_id.len() < prefix_len {
            return Err(Error::InvalidArg(format!(
                "chunk_id.len() < prefix len: {:?}, {}",
                chunk_id, prefix_len
            )));
        }
        Ok(())
    }

    fn check_prefix(&self, prefix: &[u8]) -> Result<()> {
        let prefix_len = self.config.prefix_len;
        if prefix.len() != prefix_len {
            return Err(Error::InvalidArg(format!(
                "prefix.len() != prefix len: {:?}, {}",
                prefix, prefix_len
            )));
        }
        Ok(())
    }

    pub fn query_used_size(&self, prefix: &[u8]) -> Result<u64> {
        self.check_prefix(prefix)?;

        let used_size_key = MetaKey::used_size_key(prefix);
        let value = self.rocksdb.get(used_size_key)?;
        if let Some(size) = value {
            if size.len() != std::mem::size_of::<u64>() {
                Err(Error::InvalidArg(format!(
                    "invalid size length: {:?}",
                    size.as_ref()
                )))
            } else {
                Ok(LittleEndian::read_u64(size.as_ref()))
            }
        } else {
            Ok(0)
        }
    }

    fn with_tls_bytes<F, R>(f: F) -> R
    where
        F: FnOnce(&mut DownwardBytes) -> R,
    {
        Self::BYTES.with(|v| {
            let mut bytes = v.borrow_mut();
            let result = f(bytes.deref_mut());
            bytes.clear_and_shrink_to(Size::MB.into());
            result
        })
    }

    fn update_used_size_if_need(&mut self) -> Result<()> {
        let old_len = match self.rocksdb.get(MetaKey::used_size_prefix_len_key())? {
            Some(value) => {
                if value.len() != std::mem::size_of::<u32>() {
                    return Err(Error::InvalidArg(format!(
                        "invalid used size prefix length: {:?}",
                        value.as_ref()
                    )));
                }
                LittleEndian::read_u32(value.as_ref()) as usize
            }
            None => 0,
        };

        let prefix_len = self.config.prefix_len;
        if old_len == prefix_len {
            return Ok(());
        }

        let mut map = HashMap::<Bytes, u64>::new();
        if prefix_len == 0 {
            map.insert(Bytes::new(), 0);
        }
        let mut it = self.iterator();
        it.iterate(MetaKey::chunk_meta_key_prefix(), |key, value| {
            let mut chunk_id = MetaKey::parse_chunk_meta_key(key);
            chunk_id.resize(prefix_len, 0);
            let chunk_meta = ChunkMeta::deserialize(value).map_err(Error::SerializationError)?;
            let chunk_size = chunk_meta.pos.chunk_size().0;
            map.entry(chunk_id)
                .and_modify(|v| *v += chunk_size)
                .or_insert(chunk_size);
            Ok(())
        })?;

        let mut write_batch = RocksDB::new_write_batch();
        write_batch.put(
            MetaKey::used_size_prefix_len_key(),
            (prefix_len as u32).to_le_bytes(),
        );
        for (prefix, size) in map {
            write_batch.put(MetaKey::used_size_key(&prefix), size.to_le_bytes())
        }
        self.write(write_batch, true)
    }

    pub const V1_FIX_TIMESTAMP: u8 = 1;
    pub const LATEST_VERSION: u8 = Self::V1_FIX_TIMESTAMP;

    pub fn get_version(&self) -> Result<u8> {
        match self.rocksdb.get(MetaKey::version_key())? {
            Some(value) if !value.is_empty() => Ok(value[0]),
            Some(value) => Err(Error::InvalidArg(format!(
                "invalid version: {:?}",
                value.as_ref()
            ))),
            None => Ok(0),
        }
    }

    pub fn set_version(&self, version: u8) -> Result<()> {
        self.rocksdb.put(MetaKey::version_key(), [version], true)
    }

    pub fn remove_range_mut(
        &self,
        prefix: u8,
        write_batch: &mut rocksdb::WriteBatch,
    ) -> Result<()> {
        if prefix == MetaKey::CHUNK_META_KEY_PREFIX
            || prefix == MetaKey::GROUP_BITS_KEY_PREFIX
            || prefix == MetaKey::POS_TO_CHUNK_KEY_PREFIX
            || prefix == MetaKey::USED_SIZE_KEY_PREFIX
            || prefix == MetaKey::USED_SIZE_PREFIX_LEN_KEY
        {
            return Err(Error::InvalidArg(format!(
                "invalid remove range: {}",
                prefix
            )));
        }

        write_batch.delete_range(&[prefix], &[prefix + 1]);
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_meta_store_normal() {
        let dir = tempfile::tempdir().unwrap();

        let config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: dir.path().into(),
                create: true,
                ..Default::default()
            },
            ..Default::default()
        };

        let meta_store = MetaStore::open(&config).unwrap();

        let chunk_id = "1000".as_bytes();
        let chunk_meta = meta_store.get_chunk_meta(chunk_id).unwrap();
        assert!(chunk_meta.is_none());

        let chunk_meta_in = ChunkMeta {
            chunk_ver: 1,
            ..Default::default()
        };
        meta_store
            .add_chunk(chunk_id, &chunk_meta_in, false)
            .unwrap();

        let chunk_id = "1000".as_bytes();
        let chunk_meta_out = meta_store.get_chunk_meta(chunk_id).unwrap().unwrap();
        assert_eq!(chunk_meta_in, chunk_meta_out);
        assert_eq!(meta_store.query_chunks([], "100", 10).unwrap().len(), 1);

        meta_store.remove(chunk_id, &chunk_meta_out, false).unwrap();

        let mut write_batch = RocksDB::new_write_batch();
        meta_store
            .remove_range_mut(MetaKey::CHUNK_META_KEY_PREFIX, &mut write_batch)
            .unwrap_err();

        meta_store
            .rocksdb
            .put(MetaKey::version_key(), &[], false)
            .unwrap();
        meta_store.get_version().unwrap_err();
    }

    #[test]
    fn test_meta_get_set() {
        let dir = tempfile::tempdir().unwrap();

        let config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: dir.path().into(),
                create: true,
                ..Default::default()
            },
            ..Default::default()
        };

        let meta_store = MetaStore::open(&config).unwrap();

        let group_id = GroupId::default();
        let mut chunk_meta = ChunkMeta::default();
        for i in 0..128u32 {
            chunk_meta.pos = Position::new(group_id, 2 * i as u8);
            meta_store
                .add_chunk(&i.to_be_bytes(), &chunk_meta, false)
                .unwrap();
        }

        let vec = meta_store
            .query_chunks(10u32.to_be_bytes(), 20u32.to_be_bytes(), 30)
            .unwrap();
        assert_eq!(vec.len(), 10);
        assert_eq!(vec.first().unwrap().0.as_ref(), &19u32.to_be_bytes());
        assert_eq!(vec.last().unwrap().0.as_ref(), &10u32.to_be_bytes());

        let vec = meta_store
            .query_chunks(80u32.to_be_bytes(), 100u32.to_be_bytes(), 30)
            .unwrap();
        assert_eq!(vec.len(), 20);

        let mut it = meta_store.iterator();
        let mut count = 0;
        it.iterate(MetaKey::group_bits_key_prefix(), |_key, value| {
            count += 1;
            let bits = GroupState::from(value)?;
            assert_eq!(bits.count(), 128);
            for i in 0..128 {
                assert!(bits.check(i * 2));
                assert!(!bits.check(i * 2 + 1));
            }
            Ok(())
        })
        .unwrap();
        assert_eq!(count, 1);

        for i in 0..128u32 {
            chunk_meta.pos = Position::new(group_id, 1 + 2 * i as u8);
            meta_store
                .add_chunk(&i.to_be_bytes(), &chunk_meta, false)
                .unwrap();
        }

        let mut it = meta_store.iterator();
        let mut count = 0;
        it.iterate(MetaKey::group_bits_key_prefix(), |_key, value| {
            count += 1;
            let bits = GroupState::from(value)?;
            assert_eq!(bits.count(), 256);
            assert!(bits.is_full());
            Ok(())
        })
        .unwrap();
        assert_eq!(count, 1);
    }

    #[test]
    fn test_meta_store_open_failed() {
        let config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: "/proc/test".into(),
                create: true,
                ..Default::default()
            },
            ..Default::default()
        };

        assert!(MetaStore::open(&config).is_err());
    }

    #[test]
    fn test_meta_store_update_used_size() {
        let dir = tempfile::tempdir().unwrap();

        let config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: dir.path().into(),
                create: true,
                ..Default::default()
            },
            prefix_len: 4,
        };

        let meta_store = MetaStore::open(&config).unwrap();

        let chunk_id = [0, 1, 2, 3];
        let group_id = GroupId::default();
        let chunk_meta = ChunkMeta {
            pos: Position::new(group_id, 0_u8),
            ..Default::default()
        };
        meta_store
            .add_chunk(&chunk_id[..3], &chunk_meta, false)
            .unwrap_err();
        meta_store.add_chunk(&chunk_id, &chunk_meta, false).unwrap();

        meta_store.query_used_size(&chunk_id[..3]).unwrap_err();
        assert_eq!(
            meta_store.query_used_size(&chunk_id).unwrap(),
            CHUNK_SIZE_NORMAL
        );
        assert_eq!(meta_store.query_used_size(&0u32.to_le_bytes()).unwrap(), 0);

        meta_store
            .query_chunks_by_timestamp(&0u32.to_le_bytes(), 0, u64::MAX, u64::MAX)
            .unwrap();

        meta_store.remove(&chunk_id, &chunk_meta, false).unwrap();
        assert_eq!(meta_store.query_used_size(&chunk_id).unwrap(), 0);

        let key = MetaKey::used_size_key(&chunk_id);
        meta_store.rocksdb.put(key, [], false).unwrap();
        meta_store.query_used_size(&chunk_id).unwrap_err();

        meta_store
            .rocksdb
            .put(MetaKey::used_size_prefix_len_key(), [233], false)
            .unwrap();
        drop(meta_store);
        assert!(MetaStore::open(&config).is_err());
    }

    #[test]
    fn test_meta_store_update_used_size_prefix_len() {
        let dir = tempfile::tempdir().unwrap();

        let mut config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: dir.path().into(),
                create: true,
                ..Default::default()
            },
            prefix_len: 0,
        };

        const N: u64 = 1024;
        let start = ChunkMeta::now();
        let meta_store = MetaStore::open(&config).unwrap();
        for i in 0..N {
            let chunk_id = i.to_le_bytes();
            let id = i as u8;
            let chunk_size = if id % 2 == 0 {
                CHUNK_SIZE_NORMAL
            } else {
                CHUNK_SIZE_SMALL
            };

            let pos = Position::new(GroupId::new(chunk_size, 0, 0), id);
            let meta = ChunkMeta {
                pos,
                ..Default::default()
            };
            meta_store.add_chunk(&chunk_id, &meta, false).unwrap();
        }

        let size = meta_store.query_used_size(&[]).unwrap();
        assert_eq!(size, N / 2 * (CHUNK_SIZE_NORMAL.0 + CHUNK_SIZE_SMALL.0));

        let mut write_batch = RocksDB::new_write_batch();
        write_batch.put("m", "m");
        meta_store.write(write_batch, false).unwrap();

        let end = ChunkMeta::now();
        let vec = meta_store
            .query_chunks_by_timestamp(&[], 0, start, u64::MAX)
            .unwrap();
        assert!(vec.is_empty());
        let vec = meta_store
            .query_chunks_by_timestamp(&[], start, end + 1, u64::MAX)
            .unwrap();
        assert_eq!(vec.len(), N as usize);

        drop(meta_store);

        config.prefix_len = 1;
        let meta_store = MetaStore::open(&config).unwrap();
        for i in 0..=u8::MAX {
            let size = meta_store.query_used_size(&[i]).unwrap();
            if i % 2 == 0 {
                assert_eq!(size, N / 256 * CHUNK_SIZE_NORMAL.0);
            } else {
                assert_eq!(size, N / 256 * CHUNK_SIZE_SMALL.0);
            }
        }

        for i in 0..N {
            let chunk_id = i.to_le_bytes();
            let meta = meta_store.get_chunk_meta(&chunk_id).unwrap().unwrap();
            meta_store.remove(&chunk_id, &meta, false).unwrap();
        }
        for i in 0..=u8::MAX {
            let size = meta_store.query_used_size(&[i]).unwrap();
            assert_eq!(size, 0);
        }

        drop(meta_store);
        config.prefix_len = 0;
        let meta_store = MetaStore::open(&config).unwrap();
        let size = meta_store.query_used_size(&[]).unwrap();
        assert_eq!(size, 0);
    }
}
