use super::super::*;
use std::collections::hash_map::Entry;

use std::sync::atomic::Ordering;
use std::sync::Arc;

pub struct ChunkAllocator {
    pub full_groups: ShardsSet<GroupId>,
    pub active_groups: ShardsMap<GroupId, GroupState>,
    pub(super) active_levels: [ShardsSet<GroupId>; GroupState::LEVELS],
    pub(super) frozen_groups: ShardsMap<GroupId, GroupState>,
    pub(super) group_allocator: GroupAllocator,
    pub(super) position_rc: ShardsMap<Position, u32>,
    pub(super) counter: Arc<AllocatorCounter>,
}

impl ChunkAllocator {
    pub fn with_chunk_size(chunk_size: Size) -> Self {
        let counter = Arc::new(AllocatorCounter::new(chunk_size));
        Self {
            full_groups: Default::default(),
            active_groups: Default::default(),
            active_levels: Default::default(),
            frozen_groups: Default::default(),
            group_allocator: GroupAllocator::init(counter.clone()),
            position_rc: Default::default(),
            counter,
        }
    }

    pub fn load(
        mut it: RocksDBIterator,
        counter: Arc<AllocatorCounter>,
        chunk_size: Size,
    ) -> Result<Self> {
        let mut full_groups = ShardsSet::with_capacity(4096);
        let mut active_groups = ShardsMap::with_capacity(4096);
        let frozen_groups = ShardsMap::with_capacity(4096);
        let mut active_levels = std::array::from_fn(|_| ShardsSet::with_capacity(4096));

        let mut allocated_groups = ShardsSet::with_capacity(4096);
        let mut unallocated_groups = ShardsSet::with_capacity(4096);
        let mut current = GroupId::new(chunk_size, 0, 0);

        let mut allocated_count: u64 = 0;
        let mut reserved_count: u64 = 0;

        let prefix = MetaKey::group_bits_chunk_size_prefix(current);
        it.iterate(prefix, |key, value| {
            let group_id = MetaKey::parse_group_bits_key(key)?;
            let group_state = GroupState::from(value)?;

            assert!(
                current <= group_id,
                "current {current:?} > next {group_id:?}"
            );
            while current < group_id {
                unallocated_groups.insert(current);
                current.next();
            }
            current.next();

            allocated_count += GroupState::TOTAL_BITS as u64;
            if group_state.is_empty() {
                allocated_groups.insert(group_id);
                reserved_count += GroupState::TOTAL_BITS as u64;
            } else if group_state.is_full() {
                full_groups.insert(group_id);
            } else {
                reserved_count += GroupState::TOTAL_BITS as u64 - group_state.count() as u64;
                active_levels[group_state.level() as usize].insert(group_id);
                active_groups.insert(group_id, group_state);
            }

            Ok(())
        })?;

        counter.init(allocated_count, reserved_count);
        let chunk_allocator = ChunkAllocator {
            full_groups,
            active_groups,
            active_levels,
            frozen_groups,
            counter: counter.clone(),
            group_allocator: GroupAllocator {
                allocated_groups,
                unallocated_groups,
                next_group_id: current,
                counter,
            },
            position_rc: ShardsMap::with_capacity(1 << 20),
        };

        Ok(chunk_allocator)
    }

    pub fn allocate(&mut self, clusters: &Clusters, allow_to_allocate: bool) -> Result<Position> {
        if !self.active_groups.is_empty() {
            for level in (0..GroupState::LEVELS).rev() {
                let set = &mut self.active_levels[level];
                if let Some(&group_id) = set.iter().next() {
                    let state = self.active_groups.get_mut(&group_id).unwrap();
                    let index = state.allocate().unwrap();
                    if state.is_full() {
                        self.full_groups.insert(group_id);
                        self.active_groups.remove(&group_id);
                        set.remove(&group_id);
                    } else if state.level() != level as u32 {
                        set.remove(&group_id);
                        self.active_levels[level + 1].insert(group_id);
                    }
                    let pos = Position::new(group_id, index);
                    self.reference(pos, true);
                    self.counter.allocate_chunk();
                    return Ok(pos);
                }
            }
        }

        let group_id = self.group_allocator.allocate(clusters, allow_to_allocate)?;
        self.counter.allocate_chunk();
        let state = match self.active_groups.entry(group_id) {
            Entry::Occupied(_) => panic!("should not be active groups: {:?}", group_id),
            Entry::Vacant(entry) => entry.insert(GroupState::empty()),
        };
        let index = state.allocate().unwrap();
        self.active_levels[state.level() as usize].insert(group_id);
        let pos = Position::new(group_id, index);
        self.reference(pos, true);
        Ok(pos)
    }

    pub fn reference(&mut self, pos: Position, first_ref: bool) {
        let group_id = pos.group_id();
        if let Some(state) = self.active_groups.get_mut(&group_id) {
            assert!(state.check(pos.index()), "ref pos failed: {:?}", pos);
        } else if let Some(state) = self.frozen_groups.get_mut(&group_id) {
            assert!(state.check(pos.index()), "ref pos failed: {:?}", pos);
        } else {
            assert!(self.full_groups.contains(&group_id));
        }

        let rc = match self.position_rc.entry(pos) {
            Entry::Occupied(mut occupied_entry) => {
                let rc = occupied_entry.get_mut();
                *rc += 1;
                *rc
            }
            Entry::Vacant(vacant_entry) => {
                self.counter.position_count.fetch_add(1, Ordering::AcqRel);
                vacant_entry.insert(1);
                1
            }
        };
        self.counter.position_rc.fetch_add(1, Ordering::AcqRel);

        if first_ref {
            assert!(rc == 1, "should be first ref to pos {:?}, rc {}", pos, rc);
        }
    }

    pub fn dereference(&mut self, pos: Position) {
        self.counter.position_rc.fetch_sub(1, Ordering::AcqRel);
        let count = self.position_rc.get_mut(&pos).unwrap();
        *count -= 1;
        if *count == 0 {
            self.counter.position_count.fetch_sub(1, Ordering::AcqRel);
            self.position_rc.remove(&pos);
            self.deallocate(pos);
        }
    }

    pub fn deallocate(&mut self, pos: Position) {
        let group_id = pos.group_id();
        if let Some(state) = self.active_groups.get_mut(&group_id) {
            let level = state.level();
            state.deallocate(pos.index()).unwrap();
            if state.is_empty() {
                self.active_groups.remove(&group_id);
                self.active_levels[level as usize].remove(&group_id);
                self.group_allocator.deallocate(group_id);
            } else if state.level() != level {
                self.active_levels[level as usize].remove(&group_id);
                self.active_levels[level as usize - 1].insert(group_id);
            }
        } else if let Some(state) = self.frozen_groups.get_mut(&group_id) {
            state.deallocate(pos.index()).unwrap();
            if state.is_empty() {
                self.frozen_groups.remove(&group_id);
                self.group_allocator.deallocate(group_id);
            }
        } else if self.full_groups.contains(&group_id) {
            let mut state = GroupState::full();
            state.deallocate(pos.index()).unwrap();
            self.active_levels[state.level() as usize].insert(group_id);
            self.active_groups.insert(group_id, state);
            self.full_groups.remove(&group_id);
        } else {
            unreachable!(
                "deallocate position failed! not found this position: {:?}",
                pos
            );
        }
        self.counter.deallocate_chunk();
    }

    pub fn get_compact_task(&mut self, max_reserved: u64) -> Option<GroupId> {
        let reserved = self.counter.reserved_chunks();
        if reserved <= max_reserved {
            return None;
        }

        for set in &mut self.active_levels {
            if let Some(&group_id) = set.iter().next() {
                set.remove(&group_id);
                let state = self.active_groups.remove(&group_id).unwrap();
                self.frozen_groups.insert(group_id, state);
                return Some(group_id);
            }
        }

        None
    }

    pub fn finish_compact_task(&mut self, group_id: GroupId) {
        if let Some(state) = self.frozen_groups.remove(&group_id) {
            self.active_levels[state.level() as usize].insert(group_id);
            self.active_groups.insert(group_id, state);
            tracing::info!("finish compact task and move back {:?}", group_id);
        } else {
            tracing::info!("finish compact task successful!");
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_chunk_allocator() {
        let dir = tempfile::tempdir().unwrap();

        let config = ClustersConfig {
            path: dir.path().into(),
            chunk_size: CHUNK_SIZE_NORMAL,
            create: true,
        };

        let clusters = Clusters::open(&config).unwrap();
        let mut chunk_allocator = ChunkAllocator::with_chunk_size(CHUNK_SIZE_NORMAL);
        assert!(chunk_allocator.active_groups.is_empty());
        assert!(chunk_allocator
            .active_levels
            .iter()
            .all(|set| set.is_empty()));
        assert!(chunk_allocator.full_groups.is_empty());

        let one_level_count = GroupState::TOTAL_BITS / GroupState::LEVELS;
        for i in 0..(one_level_count - 1) {
            let pos = chunk_allocator.allocate(&clusters, true).unwrap();
            assert_eq!(pos, Position::new(GroupId::default(), i as _));
        }
        assert_eq!(chunk_allocator.active_groups.len(), 1);
        assert_eq!(chunk_allocator.active_levels[0].len(), 1);

        let pos = chunk_allocator.allocate(&clusters, true).unwrap();
        assert_eq!(
            pos,
            Position::new(GroupId::default(), one_level_count as u8 - 1)
        );
        assert_eq!(chunk_allocator.active_groups.len(), 1);
        assert_eq!(chunk_allocator.active_levels[0].len(), 0);
        assert_eq!(chunk_allocator.active_levels[1].len(), 1);

        let used_size = chunk_allocator.counter.used_size();
        assert_eq!(
            used_size.allocated_size,
            CHUNK_SIZE_NORMAL * GroupState::TOTAL_BITS
        );
        assert_eq!(
            used_size.reserved_size,
            CHUNK_SIZE_NORMAL * (GroupState::TOTAL_BITS - one_level_count)
        );

        for i in one_level_count..GroupState::TOTAL_BITS {
            let pos = chunk_allocator.allocate(&clusters, true).unwrap();
            assert_eq!(pos, Position::new(GroupId::default(), i as _));
        }
        assert!(chunk_allocator.active_groups.is_empty());
        assert!(chunk_allocator
            .active_levels
            .iter()
            .all(|set| set.is_empty()));
        assert_eq!(chunk_allocator.full_groups.len(), 1);
    }

    #[test]
    #[should_panic(expected = "not found this position")]
    fn test_chunk_invalid_deallocate() {
        let mut allocator = ChunkAllocator::with_chunk_size(CHUNK_SIZE_NORMAL);
        allocator.deallocate(Position::default());
    }
}
