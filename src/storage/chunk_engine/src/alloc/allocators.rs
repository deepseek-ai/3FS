use super::super::*;
use std::path::Path;
use std::sync::Arc;

#[derive(Clone)]
pub struct Allocators {
    pub vec: [Arc<Allocator>; CHUNK_SIZE_NUMBER],
    meta_store: Arc<MetaStore>,
}

impl Allocators {
    pub fn new(path: &Path, create: bool, meta_store: Arc<MetaStore>) -> Result<Self> {
        let mut allocators = vec![];
        for i in 0..CHUNK_SIZE_NUMBER {
            let chunk_size = CHUNK_SIZE_SMALL * (1 << i);
            let allocator = Self::create(path, create, &meta_store, chunk_size)?;
            allocators.push(allocator);
        }

        Ok(Self {
            vec: [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10].map(|i| allocators[i].clone()),
            meta_store,
        })
    }

    fn create(
        path: &Path,
        create: bool,
        meta_store: &Arc<MetaStore>,
        chunk_size: Size,
    ) -> Result<Arc<Allocator>> {
        let cluster_config = ClustersConfig {
            path: path.join(chunk_size.to_string()),
            chunk_size,
            create,
        };
        let clusters = Clusters::open(&cluster_config)?;
        let allocator = Allocator::load(clusters, meta_store.iterator())?;
        tracing::info!("Allocator {:?} is created...", allocator.clusters);
        Result::Ok(allocator)
    }

    pub fn select_by_pos(&self, pos: Position) -> Result<&Arc<Allocator>> {
        let chunk_size = pos.chunk_size();
        if chunk_size.is_power_of_two()
            && CHUNK_SIZE_SMALL <= chunk_size
            && chunk_size <= CHUNK_SIZE_ULTRA
        {
            Ok(&self.vec[chunk_size.trailing_zeros() as usize - CHUNK_SIZE_SHIFT])
        } else {
            Err(Error::InvalidArg(format!(
                "select allocator invalid pos: {pos:?}"
            )))
        }
    }

    pub fn select_by_size(&self, size: Size) -> Result<&Arc<Allocator>> {
        if size <= CHUNK_SIZE_SMALL {
            Ok(&self.vec[0])
        } else if size <= CHUNK_SIZE_ULTRA {
            Ok(&self.vec[size.next_power_of_two().trailing_zeros() as usize - CHUNK_SIZE_SHIFT])
        } else {
            Err(Error::InvalidArg(format!(
                "select allocator invalid size: {size:?}"
            )))
        }
    }

    pub fn allocate(&self, size: Size, allow_to_allocate: bool) -> Result<Chunk> {
        let allocator = self.select_by_size(size)?;
        allocator.allocate(allow_to_allocate)
    }

    pub fn allocate_groups(
        &self,
        min_remain: usize,
        max_remain: usize,
        batch_size: usize,
        allocate_ultra_groups: bool,
    ) -> usize {
        let mut finish = 0usize;
        for allocator in &self.vec {
            let is_ultra = allocator.clusters.chunk_size > CHUNK_SIZE_LARGE;
            if is_ultra != allocate_ultra_groups {
                continue;
            }
            for _ in 0..batch_size {
                match allocator.do_allocate_task(min_remain, max_remain, &self.meta_store) {
                    Ok(AllocateTask::None) => break,
                    Ok(_) => {
                        finish += 1;
                        continue;
                    }
                    Err(_) => break,
                }
            }
        }
        finish
    }

    pub fn used_size(&self) -> UsedSize {
        self.vec
            .iter()
            .map(|allocator| allocator.counter.used_size())
            .sum()
    }

    pub fn get_allocate_tasks(&self, max_reserved: u64) -> tinyvec::ArrayVec<[GroupId; 3]> {
        self.vec
            .iter()
            .filter_map(|allocator| allocator.get_compact_task(max_reserved))
            .collect()
    }

    pub fn finish_compact_task(&self, group_id: GroupId) {
        self.select_by_pos(Position::new(group_id, 0))
            .unwrap()
            .finish_compact_task(group_id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_allocators() {
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

        assert_eq!(
            allocators
                .select_by_pos(Position::new(GroupId::new(CHUNK_SIZE_NORMAL, 0, 0), 0))
                .unwrap()
                .clusters
                .chunk_size,
            CHUNK_SIZE_NORMAL
        );

        assert_eq!(
            allocators
                .select_by_size(CHUNK_SIZE_SMALL)
                .unwrap()
                .clusters
                .chunk_size,
            CHUNK_SIZE_SMALL
        );

        assert_eq!(
            allocators
                .select_by_size(CHUNK_SIZE_SMALL + 1)
                .unwrap()
                .clusters
                .chunk_size,
            CHUNK_SIZE_SMALL * 2,
        );

        assert_eq!(
            allocators
                .select_by_size(CHUNK_SIZE_NORMAL)
                .unwrap()
                .clusters
                .chunk_size,
            CHUNK_SIZE_NORMAL
        );

        assert_eq!(
            allocators
                .select_by_size(CHUNK_SIZE_NORMAL + 1)
                .unwrap()
                .clusters
                .chunk_size,
            CHUNK_SIZE_NORMAL * 2,
        );

        let used_size = allocators.used_size();
        assert_eq!(used_size.allocated_size, 0);
        assert_eq!(used_size.reserved_size, 0);

        assert!(allocators
            .select_by_pos(Position::new(GroupId::new(CHUNK_SIZE_ULTRA, 0, 0), 0))
            .is_ok());
        assert!(allocators
            .select_by_pos(Position::new(GroupId::new(Size::gibibyte(1), 0, 0), 0))
            .is_err());
        assert!(allocators.select_by_size(Size::gibibyte(1)).is_err());
    }
}
