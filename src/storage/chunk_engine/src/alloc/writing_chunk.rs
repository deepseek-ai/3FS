use crate::{Bytes, Chunk, ChunkArc, ChunkMeta};
use dashmap::DashMap;
use std::{collections::HashMap, sync::Arc};

pub struct WritingHolder {
    pub chunk: Chunk,
    pub abort: bool,
}

pub type WritingList = DashMap<Bytes, HashMap<Bytes, WritingHolder>>;

pub struct WritingChunk {
    pub chunk_id: Bytes,
    pub chunk: Chunk,
    pub list: Arc<WritingList>,
    pub prefix_len: u32,
    pub is_remove: bool,
    pub commit_succ: bool,
}

impl WritingChunk {
    pub fn meta(&self) -> &ChunkMeta {
        self.chunk.meta()
    }

    pub fn set_committed(&mut self) {
        self.chunk.set_committed();
    }

    pub fn commit_succ(&mut self) {
        self.commit_succ = true;
    }
}

impl Drop for WritingChunk {
    fn drop(&mut self) {
        let prefix = &self.chunk_id[..self.prefix_len as usize];
        if let Some(mut map) = self.list.get_mut(prefix) {
            if self.commit_succ {
                if map.remove(&self.chunk_id).is_some() {
                    return;
                }
            } else if let Some(holder) = map.get_mut(&self.chunk_id) {
                holder.abort = true;
                return;
            }
        }
        panic!("chunk id {:?} is not in the writing list!", self.chunk_id);
    }
}

impl From<&WritingChunk> for ChunkArc {
    fn from(chunk: &WritingChunk) -> Self {
        Arc::new(chunk.chunk.clone())
    }
}

impl std::fmt::Debug for WritingChunk {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("WritingChunk")
            .field("chunk_id", &self.chunk_id)
            .field("chunk", &self.chunk)
            .field("is_remove", &self.is_remove)
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use crate::*;
    use std::sync::Arc;

    fn test_writing_chunk_not_in_list(has_list: bool, commit_succ: bool) {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path();

        let meta_config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: path.join("meta"),
                create: true,
                ..Default::default()
            },
            ..Default::default()
        };

        let meta_store = Arc::new(MetaStore::open(&meta_config).unwrap());
        let allocators = Allocators::new(path, true, meta_store).unwrap();
        let chunk = allocators.allocate(CHUNK_SIZE_NORMAL, true).unwrap();

        let writing_list: Arc<WritingList> = Default::default();
        if has_list {
            writing_list
                .entry(Bytes::from(b"te".as_slice()))
                .or_default();
        }
        let writing_chunk = WritingChunk {
            chunk_id: b"test".as_ref().into(),
            chunk,
            list: writing_list.clone(),
            prefix_len: 2,
            is_remove: false,
            commit_succ,
        };
        println!("{:#?}", writing_chunk);
    }

    #[test]
    #[should_panic(expected = "chunk id [116, 101, 115, 116] is not in the writing list!")]
    fn test_writing_chunk_not_in_list_1() {
        test_writing_chunk_not_in_list(false, false);
    }

    #[test]
    #[should_panic(expected = "chunk id [116, 101, 115, 116] is not in the writing list!")]
    fn test_writing_chunk_not_in_list_2() {
        test_writing_chunk_not_in_list(true, false);
    }

    #[test]
    #[should_panic(expected = "chunk id [116, 101, 115, 116] is not in the writing list!")]
    fn test_writing_chunk_not_in_list_3() {
        test_writing_chunk_not_in_list(true, true);
    }
}
