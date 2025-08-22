use super::super::Size;
use std::alloc::Layout;

pub const ALIGN_SIZE: Size = Size::new(4096);

pub struct AlignedBuffer(&'static mut [u8]);

impl AlignedBuffer {
    pub fn new(size: usize) -> Self {
        Self(unsafe {
            let size = std::cmp::max(size, 1).next_multiple_of(ALIGN_SIZE.into());
            let layout = Layout::from_size_align_unchecked(size, ALIGN_SIZE.into());
            let ptr = std::alloc::alloc(layout);
            std::slice::from_raw_parts_mut(ptr, size)
        })
    }
}

impl Drop for AlignedBuffer {
    fn drop(&mut self) {
        unsafe {
            let layout = Layout::from_size_align_unchecked(self.0.len(), ALIGN_SIZE.into());
            std::alloc::dealloc(self.0.as_mut_ptr(), layout);
        }
    }
}

impl std::ops::Deref for AlignedBuffer {
    type Target = [u8];

    #[inline(always)]
    fn deref(&self) -> &Self::Target {
        self.0
    }
}

impl std::ops::DerefMut for AlignedBuffer {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.0
    }
}

pub fn create_aligned_buf(size: Size) -> AlignedBuffer {
    AlignedBuffer::new(size.into())
}

pub fn is_aligned_buf(data: &[u8]) -> bool {
    data.as_ptr() as u64 % ALIGN_SIZE.0 == 0 && data.len() as u64 % ALIGN_SIZE.0 == 0
}

pub fn is_aligned_len(len: u32) -> bool {
    len % ALIGN_SIZE.0 as u32 == 0
}

pub fn is_aligned_io(data: &[u8], offset: u32) -> bool {
    is_aligned_buf(data) && is_aligned_len(offset)
}
