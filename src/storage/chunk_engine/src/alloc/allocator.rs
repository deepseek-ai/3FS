use super::super::*;
use std::sync::{Arc, Mutex};

pub struct Allocator {
    allocator: Mutex<ChunkAllocator>,
    pub counter: Arc<AllocatorCounter>,
    pub clusters: Clusters,
}

impl Allocator {
    pub fn load(clusters: Clusters, it: RocksDBIterator) -> Result<Arc<Allocator>> {
        let counter = Arc::new(AllocatorCounter::new(clusters.chunk_size));
        Ok(Arc::new(Self {
            allocator: Mutex::new(ChunkAllocator::load(
                it,
                counter.clone(),
                clusters.chunk_size,
            )?),
            counter,
            clusters,
        }))
    }

    pub fn allocate(self: &Arc<Self>, allow_to_allocate: bool) -> Result<Chunk> {
        let this = self.as_ref();
        let mut allocator = this.allocator.lock().unwrap();
        allocator
            .allocate(&this.clusters, allow_to_allocate)
            .map(|pos| {
                Chunk::new(
                    ChunkMeta {
                        pos,
                        ..Default::default()
                    },
                    self.clone(),
                )
            })
    }

    pub fn reference(self: &Arc<Self>, meta: ChunkMeta, first_ref: bool) -> Chunk {
        let mut allocator = self.allocator.lock().unwrap();
        allocator.reference(meta.pos, first_ref);
        Chunk::new(meta, self.clone())
    }

    pub fn dereference(&self, pos: Position) {
        let mut allocator = self.allocator.lock().unwrap();
        allocator.dereference(pos)
    }

    pub fn get_allocate_task(&self, min_remain: usize, max_remain: usize) -> AllocateTask {
        let mut allocator = self.allocator.lock().unwrap();
        allocator
            .group_allocator
            .get_allocate_task(min_remain, max_remain)
    }

    pub fn finish_allocate_task(&self, task: AllocateTask, succ: bool) {
        let mut allocator = self.allocator.lock().unwrap();
        allocator.group_allocator.finish_allocate_task(task, succ);
    }

    pub fn do_allocate_task(
        &self,
        min_remain: usize,
        max_remain: usize,
        meta_store: &MetaStore,
    ) -> Result<AllocateTask> {
        let task = self.get_allocate_task(min_remain, max_remain);

        let result = match task {
            AllocateTask::None => return Ok(task),
            AllocateTask::Allocate(group_id) => (|| {
                self.clusters.allocate(group_id)?;
                meta_store.allocate_group(group_id)
            })(),
            AllocateTask::Deallocate(group_id) => (|| {
                tracing::warn!("deallocate group: {:?}", group_id);
                meta_store.remove_group(group_id)?;
                self.clusters.deallocate(group_id)
            })(),
        };

        self.finish_allocate_task(task, result.is_ok());

        result?;
        Ok(task)
    }

    pub fn get_compact_task(&self, max_reserved: u64) -> Option<GroupId> {
        let mut allocator = self.allocator.lock().unwrap();
        allocator.get_compact_task(max_reserved)
    }

    pub fn finish_compact_task(&self, group_id: GroupId) {
        let mut allocator = self.allocator.lock().unwrap();
        allocator.finish_compact_task(group_id)
    }
}

impl Drop for Allocator {
    fn drop(&mut self) {
        tracing::info!("Allocator {:?} is dropping...", self.clusters);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_allocator() {
        use rand::seq::SliceRandom;
        let dir = tempfile::tempdir().unwrap();

        let cluster_config = ClustersConfig {
            path: dir.path().into(),
            chunk_size: CHUNK_SIZE_NORMAL,
            create: true,
        };
        let clusters = Clusters::open(&cluster_config).unwrap();

        let meta_store_config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: dir.path().join("meta"),
                create: true,
                ..Default::default()
            },
            ..Default::default()
        };
        let meta_store = Arc::new(MetaStore::open(&meta_store_config).unwrap());

        let allocator = Allocator::load(clusters, meta_store.iterator()).unwrap();

        for _ in 0..10000 {
            let chunk = allocator.allocate(true).unwrap();
            assert_eq!(chunk.meta().pos, Position::new(GroupId::default(), 0));
        }

        const N: usize = 1000;
        let mut chunks = vec![];
        for _ in 0..N {
            let chunk = allocator.allocate(true).unwrap();
            chunks.push(std::sync::Arc::new(chunk));
        }

        {
            let allocator = allocator.allocator.lock().unwrap();
            assert_eq!(allocator.full_groups.len(), N / 256);
            assert_eq!(allocator.active_groups.len(), 1);
            assert_eq!(
                allocator.active_groups.iter().next().unwrap().1.count() as usize,
                N % 256
            );
        }

        const T: usize = 8;
        (0..T)
            .map(|i| {
                let chunks = chunks.clone();
                std::thread::spawn(move || {
                    let mut vec = create_aligned_buf(ALIGN_SIZE);
                    vec.fill(0);
                    for chunk in chunks.iter() {
                        if chunk.meta().pos.index() as usize % T == i {
                            vec.fill(chunk.meta().pos.index());
                            chunk.pwrite(&vec[..], 0).unwrap();
                        }
                    }
                })
            })
            .collect::<Vec<_>>()
            .into_iter()
            .for_each(|t| t.join().unwrap());

        chunks.shuffle(&mut rand::thread_rng());

        (0..T)
            .map(|i| {
                let chunks = chunks.clone();
                std::thread::spawn(move || {
                    let mut buf = [0u8; 8];
                    for chunk in chunks.iter() {
                        if chunk.meta().pos.index() as usize % T == i {
                            assert!(chunk.pread(&mut buf, 0).is_ok());
                            assert_eq!(buf, [chunk.meta().pos.index(); 8]);
                        }
                    }
                })
            })
            .collect::<Vec<_>>()
            .into_iter()
            .for_each(|t| t.join().unwrap());

        chunks.clear();

        {
            let allocator = allocator.allocator.lock().unwrap();
            assert!(allocator.full_groups.is_empty());
            assert!(allocator.active_groups.is_empty());
        }
    }

    #[test]
    fn test_allocator_do_allocate_task() {
        let dir = tempfile::tempdir().unwrap();
        const S: Size = CHUNK_SIZE_NORMAL;

        let cluster_config = ClustersConfig {
            path: dir.path().into(),
            chunk_size: S,
            create: true,
        };
        let clusters = Clusters::open(&cluster_config).unwrap();

        let meta_store_config = MetaStoreConfig {
            rocksdb: RocksDBConfig {
                path: dir.path().join("meta"),
                create: true,
                ..Default::default()
            },
            ..Default::default()
        };
        let meta_store = Arc::new(MetaStore::open(&meta_store_config).unwrap());

        let allocator = Allocator::load(clusters, meta_store.iterator()).unwrap();

        for _ in 0..4 {
            assert!(matches!(
                allocator.do_allocate_task(4, 8, &meta_store).unwrap(),
                AllocateTask::Allocate(_)
            ));
        }
        assert!(matches!(
            allocator.do_allocate_task(4, 8, &meta_store).unwrap(),
            AllocateTask::None
        ));

        let s = allocator.counter.used_size();
        assert_eq!(s.allocated_size, S * GroupState::TOTAL_BITS as u64 * 4);
        assert_eq!(s.reserved_size, S * GroupState::TOTAL_BITS as u64 * 4);

        for _ in 2..4 {
            assert!(matches!(
                allocator.do_allocate_task(1, 2, &meta_store).unwrap(),
                AllocateTask::Deallocate(_)
            ));
        }
        assert!(matches!(
            allocator.do_allocate_task(1, 2, &meta_store).unwrap(),
            AllocateTask::None
        ));

        let s = allocator.counter.used_size();
        assert_eq!(s.allocated_size, S * GroupState::TOTAL_BITS as u64 * 2);
        assert_eq!(s.reserved_size, S * GroupState::TOTAL_BITS as u64 * 2);
    }
}
