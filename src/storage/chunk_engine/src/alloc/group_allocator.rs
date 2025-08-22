use std::sync::Arc;

use super::super::*;

pub struct GroupAllocator {
    pub(super) allocated_groups: ShardsSet<GroupId>,
    pub(super) unallocated_groups: ShardsSet<GroupId>,
    pub(super) next_group_id: GroupId,
    pub(super) counter: Arc<AllocatorCounter>,
}

#[derive(Debug, Clone, Copy)]
pub enum AllocateTask {
    None,
    Allocate(GroupId),
    Deallocate(GroupId),
}

impl GroupAllocator {
    pub fn init(counter: Arc<AllocatorCounter>) -> Self {
        Self {
            allocated_groups: Default::default(),
            unallocated_groups: Default::default(),
            next_group_id: Default::default(),
            counter,
        }
    }

    pub fn allocate(&mut self, clusters: &Clusters, allow_to_allocate: bool) -> Result<GroupId> {
        if let Some(&group_id) = self.allocated_groups.iter().next() {
            self.allocated_groups.remove(&group_id);
            Ok(group_id)
        } else if allow_to_allocate {
            let group_id = self.get_unallocated_group_id();
            tracing::info!("allocate group slow path {:?}", group_id);
            let result = clusters.allocate(group_id);
            if let Err(err) = result {
                self.unallocated_groups.insert(group_id);
                return Err(err);
            }
            self.counter.allocate_group();
            Ok(group_id)
        } else {
            Err(Error::NoSpace)
        }
    }

    pub fn deallocate(&mut self, group_id: GroupId) {
        self.allocated_groups.insert(group_id);
    }

    fn get_unallocated_group_id(&mut self) -> GroupId {
        if let Some(&group_id) = self.unallocated_groups.iter().next() {
            self.unallocated_groups.remove(&group_id);
            group_id
        } else {
            let group_id = self.next_group_id;
            self.next_group_id = self.next_group_id.plus_one();
            group_id
        }
    }

    pub fn get_allocate_task(&mut self, min_remain: usize, max_remain: usize) -> AllocateTask {
        if self.allocated_groups.len() < min_remain {
            AllocateTask::Allocate(self.get_unallocated_group_id())
        } else if self.allocated_groups.len() > max_remain {
            let group_id = *self.allocated_groups.iter().next().unwrap();
            self.allocated_groups.remove(&group_id);
            AllocateTask::Deallocate(group_id)
        } else {
            AllocateTask::None
        }
    }

    pub fn finish_allocate_task(&mut self, task: AllocateTask, succ: bool) {
        match (task, succ) {
            (AllocateTask::Allocate(group_id), true) => {
                self.counter.allocate_group();
                self.allocated_groups.insert(group_id)
            }
            (AllocateTask::Deallocate(group_id), true) => {
                self.counter.deallocate_group();
                self.unallocated_groups.insert(group_id)
            }
            (AllocateTask::Allocate(group_id), false) => self.unallocated_groups.insert(group_id),
            (AllocateTask::Deallocate(group_id), false) => self.allocated_groups.insert(group_id),
            _ => false,
        };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_group_allocator() {
        let dir = tempfile::tempdir().unwrap();

        let config = ClustersConfig {
            path: dir.path().into(),
            chunk_size: CHUNK_SIZE_NORMAL,
            create: true,
        };

        let clusters = Clusters::open(&config).unwrap();
        let counter = Arc::new(AllocatorCounter::new(CHUNK_SIZE_NORMAL));
        let mut group_allocator = GroupAllocator::init(counter);

        let group_id_1 = group_allocator.allocate(&clusters, true).unwrap();
        assert_eq!(group_id_1, GroupId::default());
        assert_eq!(group_allocator.next_group_id, group_id_1.plus_one());
        assert!(group_allocator.allocated_groups.is_empty());
        assert!(group_allocator.unallocated_groups.is_empty());

        let group_id_2 = group_allocator.allocate(&clusters, true).unwrap();
        assert_eq!(group_id_1.plus_one(), group_id_2);
        assert_eq!(group_allocator.next_group_id, group_id_2.plus_one());
        assert!(group_allocator.allocated_groups.is_empty());
        assert!(group_allocator.unallocated_groups.is_empty());

        group_allocator.deallocate(group_id_1);
        assert_eq!(group_allocator.next_group_id, group_id_2.plus_one());
        assert_eq!(group_allocator.allocated_groups.len(), 1);
        assert!(group_allocator.unallocated_groups.is_empty());

        let group_id_3 = group_allocator.allocate(&clusters, true).unwrap();
        assert_eq!(group_id_1, group_id_3);
        assert_eq!(group_allocator.next_group_id, group_id_2.plus_one());
        assert!(group_allocator.allocated_groups.is_empty());
        assert!(group_allocator.unallocated_groups.is_empty());

        group_allocator.allocate(&clusters, false).unwrap_err();
    }

    #[test]
    fn test_group_allocator_task() {
        let counter = Arc::new(AllocatorCounter::new(CHUNK_SIZE_NORMAL));
        let mut group_allocator = GroupAllocator::init(counter);
        assert!(group_allocator.allocated_groups.is_empty());
        assert!(group_allocator.unallocated_groups.is_empty());
        assert_eq!(group_allocator.next_group_id.cluster(), 0);

        let task = group_allocator.get_allocate_task(2, 4);
        assert!(matches!(task, AllocateTask::Allocate(_)));
        assert!(group_allocator.allocated_groups.is_empty());
        assert!(group_allocator.unallocated_groups.is_empty());
        assert_eq!(group_allocator.next_group_id.cluster(), 1);

        group_allocator.finish_allocate_task(task, false);
        let task = group_allocator.get_allocate_task(2, 4);
        assert!(matches!(task, AllocateTask::Allocate(_)));
        assert!(group_allocator.allocated_groups.is_empty());
        assert!(group_allocator.unallocated_groups.is_empty());
        assert_eq!(group_allocator.next_group_id.cluster(), 1);

        group_allocator.finish_allocate_task(task, true);
        assert_eq!(group_allocator.allocated_groups.len(), 1);
        assert_eq!(group_allocator.unallocated_groups.len(), 0);
        assert_eq!(group_allocator.next_group_id.cluster(), 1);

        let task = group_allocator.get_allocate_task(2, 4);
        assert!(matches!(task, AllocateTask::Allocate(_)));
        group_allocator.finish_allocate_task(task, true);
        assert_eq!(group_allocator.allocated_groups.len(), 2);
        assert_eq!(group_allocator.unallocated_groups.len(), 0);
        assert_eq!(group_allocator.next_group_id.cluster(), 2);

        let task = group_allocator.get_allocate_task(2, 4);
        assert!(matches!(task, AllocateTask::None));
        group_allocator.finish_allocate_task(task, true);
        assert_eq!(group_allocator.allocated_groups.len(), 2);
        assert_eq!(group_allocator.unallocated_groups.len(), 0);
        assert_eq!(group_allocator.next_group_id.cluster(), 2);

        let task = group_allocator.get_allocate_task(3, 4);
        assert!(matches!(task, AllocateTask::Allocate(_)));
        group_allocator.finish_allocate_task(task, false);
        assert_eq!(group_allocator.allocated_groups.len(), 2);
        assert_eq!(group_allocator.unallocated_groups.len(), 1);
        assert_eq!(group_allocator.next_group_id.cluster(), 3);

        let task = group_allocator.get_allocate_task(1, 1);
        assert!(matches!(task, AllocateTask::Deallocate(_)));
        group_allocator.finish_allocate_task(task, false);
        assert_eq!(group_allocator.allocated_groups.len(), 2);
        assert_eq!(group_allocator.unallocated_groups.len(), 1);
        assert_eq!(group_allocator.next_group_id.cluster(), 3);
    }
}
