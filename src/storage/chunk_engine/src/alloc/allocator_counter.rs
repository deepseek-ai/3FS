use super::super::*;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Default)]
pub struct AllocatorCounter {
    pub chunk_size: Size,
    pub allocated_chunks: AtomicU64,
    pub reserved_chunks: AtomicU64,
    pub position_count: AtomicU64,
    pub position_rc: AtomicU64,
}

#[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
#[repr(C)]
pub struct UsedSize {
    pub allocated_size: Size,
    pub reserved_size: Size,
    pub position_count: u64,
    pub position_rc: u64,
}

impl std::iter::Sum for UsedSize {
    fn sum<I: Iterator<Item = Self>>(iter: I) -> Self {
        let mut s = UsedSize::default();
        for i in iter {
            s.allocated_size += i.allocated_size;
            s.reserved_size += i.reserved_size;
            s.position_count += i.position_count;
            s.position_rc += i.position_rc;
        }
        s
    }
}

impl AllocatorCounter {
    pub fn new(chunk_size: Size) -> Self {
        Self {
            chunk_size,
            ..Default::default()
        }
    }

    pub fn allocated_chunks(&self) -> u64 {
        self.allocated_chunks.load(Ordering::Acquire)
    }

    pub fn reserved_chunks(&self) -> u64 {
        self.reserved_chunks.load(Ordering::Acquire)
    }

    pub fn used_size(&self) -> UsedSize {
        UsedSize {
            allocated_size: self.allocated_chunks() * self.chunk_size,
            reserved_size: self.reserved_chunks() * self.chunk_size,
            position_count: self.position_count.load(Ordering::Acquire),
            position_rc: self.position_rc.load(Ordering::Acquire),
        }
    }

    pub fn init(&self, allocated_count: u64, reserved_count: u64) {
        self.allocated_chunks
            .store(allocated_count, Ordering::Release);
        self.reserved_chunks
            .store(reserved_count, Ordering::Release);
    }

    pub fn allocate_group(&self) {
        self.allocated_chunks
            .fetch_add(GroupState::TOTAL_BITS as u64, Ordering::SeqCst);
        self.reserved_chunks
            .fetch_add(GroupState::TOTAL_BITS as u64, Ordering::SeqCst);
    }

    pub fn deallocate_group(&self) {
        self.allocated_chunks
            .fetch_sub(GroupState::TOTAL_BITS as u64, Ordering::SeqCst);
        self.reserved_chunks
            .fetch_sub(GroupState::TOTAL_BITS as u64, Ordering::SeqCst);
    }

    pub fn allocate_chunk(&self) {
        self.reserved_chunks.fetch_sub(1, Ordering::SeqCst);
    }

    pub fn deallocate_chunk(&self) {
        self.reserved_chunks.fetch_add(1, Ordering::SeqCst);
    }
}
