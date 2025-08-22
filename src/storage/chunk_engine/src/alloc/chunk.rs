use super::super::*;
use lazy_static::lazy_static;
use rand::Rng;
use std::cell::RefCell;

use std::sync::atomic::Ordering;
use std::sync::Arc;

pub struct Chunk {
    meta: ChunkMeta,
    allocator: Arc<Allocator>,
}

pub type ChunkArc = Arc<Chunk>;

lazy_static! {
    static ref ZERO: AlignedBuffer = {
        let mut vec = create_aligned_buf(CHUNK_SIZE_ULTRA);
        vec.fill(0);
        vec
    };
}

impl Chunk {
    thread_local! {
        static BUFFER: RefCell<AlignedBuffer> = RefCell::new(create_aligned_buf(CHUNK_SIZE_ULTRA));
    }

    pub fn new(meta: ChunkMeta, allocator: Arc<Allocator>) -> Self {
        Self { meta, allocator }
    }

    pub fn meta(&self) -> &ChunkMeta {
        &self.meta
    }

    pub fn capacity(&self) -> u32 {
        self.meta.pos.chunk_size().into()
    }

    pub fn update_meta(&mut self, req: &UpdateReq) {
        self.meta.chunk_ver = req.out_commit_ver;
        self.meta.chain_ver = req.chain_ver;
        self.meta.last_request_id = req.last_request_id;
        self.meta.last_client_low = req.last_client_low;
        self.meta.last_client_high = req.last_client_high;
        if req.desired_tag.is_empty() {
            let r: u64 = rand::thread_rng().gen();
            self.meta.etag = ETag::from(format!("{:X}", r).as_bytes());
        } else {
            self.meta.etag = req.desired_tag.into();
        }
        self.meta.uncommitted = true;
        self.meta.timestamp = ChunkMeta::now();
    }

    pub fn set_chain_ver(&mut self, chain_ver: u32) {
        self.meta.chain_ver = chain_ver;
    }

    pub fn set_committed(&mut self) {
        self.meta.uncommitted = false;
    }

    pub fn copy_chunk(&self) -> Result<Chunk> {
        // 1. allocate new chunk.
        let mut new_chunk = self.allocator.allocate(true)?;

        // 2. copy meta.
        new_chunk.meta = ChunkMeta {
            pos: new_chunk.meta.pos,
            etag: Default::default(),
            ..self.meta
        };

        // 3. copy data.
        Self::BUFFER.with(|v| {
            let mut vec = v.borrow_mut();
            let len = self.meta.len.next_multiple_of(ALIGN_SIZE.into());
            let buf = &mut vec[..len as usize]; // aligned.
            self.pread(buf, 0)?;
            new_chunk.pwrite(buf, 0)?;
            Result::Ok(())
        })?;

        Ok(new_chunk)
    }

    pub fn copy_on_write(
        &self,
        data: &[u8],
        offset: u32,
        checksum: u32,
        is_syncing: bool,
        allow_to_allocate: bool,
        allocators: &Allocators,
        metrics: &Metrics,
    ) -> Result<Chunk> {
        // 1. allocate new chunk.
        let new_len = std::cmp::max(self.meta.len, offset + data.len() as u32);
        let begin = std::time::Instant::now();
        let mut new_chunk = allocators.allocate(Size::from(new_len), allow_to_allocate)?;
        let begin2 = std::time::Instant::now();
        let latency = begin2.duration_since(begin).as_micros() as _;
        metrics.allocate_times.fetch_add(1, Ordering::AcqRel);
        metrics
            .allocate_latency
            .fetch_add(latency, Ordering::AcqRel);
        metrics.copy_on_write_times.fetch_add(1, Ordering::AcqRel);

        // 2. write data.
        let skip_read = is_syncing || (offset == 0 && data.len() >= self.meta.len as usize);
        let checksum = Self::BUFFER.with(|v| {
            let mut vec = v.borrow_mut();
            if !skip_read {
                // aligned read.
                let len = self.meta.len.next_multiple_of(ALIGN_SIZE.into());
                let begin = std::time::Instant::now();
                self.pread(&mut vec[..len as usize], 0)?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as _;
                metrics
                    .copy_on_write_read_times
                    .fetch_add(1, Ordering::AcqRel);
                metrics
                    .copy_on_write_read_bytes
                    .fetch_add(len as _, Ordering::AcqRel);
                metrics
                    .copy_on_write_read_latency
                    .fetch_add(latency, Ordering::AcqRel);
            }

            // aligned write.
            if skip_read && is_aligned_io(data, offset) {
                let begin = std::time::Instant::now();
                new_chunk.pwrite(data, offset)?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as _;
                metrics.pwrite_times.fetch_add(1, Ordering::AcqRel);
                metrics.pwrite_latency.fetch_add(latency, Ordering::AcqRel);
            } else {
                if self.meta.len < offset {
                    vec[self.meta.len as usize..offset as usize].fill(0);
                }
                vec[offset as usize..][..data.len()].copy_from_slice(data);
                let len = new_len.next_multiple_of(ALIGN_SIZE.into());
                let begin = std::time::Instant::now();
                new_chunk.pwrite(&vec[..len as usize], 0)?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as _;
                metrics.pwrite_times.fetch_add(1, Ordering::AcqRel);
                metrics.pwrite_latency.fetch_add(latency, Ordering::AcqRel);
            };

            Result::Ok(if skip_read {
                metrics.checksum_reuse.fetch_add(1, Ordering::AcqRel);
                checksum
            } else {
                metrics.checksum_recalculate.fetch_add(1, Ordering::AcqRel);
                crc32c::crc32c(&vec[..new_len as usize])
            })
        })?;
        let latency = std::time::Instant::now().duration_since(begin2).as_micros() as _;
        metrics
            .copy_on_write_latency
            .fetch_add(latency, Ordering::AcqRel);

        // 3. copy meta.
        new_chunk.meta.len = if is_syncing {
            offset + data.len() as u32
        } else {
            new_len
        };
        new_chunk.meta.checksum = checksum;

        Ok(new_chunk)
    }

    pub fn safe_write(
        &mut self,
        data: &[u8],
        offset: u32,
        checksum: u32,
        truncate: bool,
        metrics: &Metrics,
    ) -> Result<()> {
        if truncate && offset < self.meta.len {
            metrics
                .safe_write_truncate_shorten
                .fetch_add(1, Ordering::AcqRel);
            metrics.checksum_recalculate.fetch_add(1, Ordering::AcqRel);
            return Self::BUFFER.with(|v| {
                // aligned read.
                let mut vec = v.borrow_mut();
                let len = offset.next_multiple_of(ALIGN_SIZE.into());
                self.pread(&mut vec[..len as usize], 0)?;
                self.meta.len = offset;
                self.meta.checksum = crc32c::crc32c(&vec[..offset as usize]);
                Result::Ok(())
            });
        }

        if is_aligned_len(self.meta.len)
            && is_aligned_len(offset)
            && (data.is_empty() || is_aligned_buf(data))
        {
            // already aligned.
            if offset > self.meta.len {
                let padding = (offset - self.meta.len) as usize;
                let begin = std::time::Instant::now();
                self.pwrite(&ZERO[..padding], self.meta.len)?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as _;
                metrics.pwrite_times.fetch_add(1, Ordering::AcqRel);
                metrics.pwrite_latency.fetch_add(latency, Ordering::AcqRel);
                self.meta.len = offset;
                self.meta.checksum = crc32c::crc32c_append(self.meta.checksum, &ZERO[..padding]);
                metrics
                    .safe_write_truncate_extend
                    .fetch_add(1, Ordering::AcqRel);
                metrics.checksum_combine.fetch_add(1, Ordering::AcqRel);
            }

            if !data.is_empty() {
                assert!(offset == self.meta.len);
                let begin = std::time::Instant::now();
                self.pwrite(data, offset)?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as u64;
                metrics.pwrite_times.fetch_add(1, Ordering::AcqRel);
                metrics.pwrite_latency.fetch_add(latency, Ordering::AcqRel);
                self.meta.len = offset + data.len() as u32;
                self.meta.checksum =
                    crc32c::crc32c_combine(self.meta.checksum, checksum, data.len());
                metrics
                    .safe_write_direct_append
                    .fetch_add(1, Ordering::AcqRel);
                metrics.checksum_combine.fetch_add(1, Ordering::AcqRel);
            }
        } else if self.meta.len < offset + data.len() as u32 {
            // copy to buffer and write.
            assert!(self.meta.len <= offset);
            Self::BUFFER.with(|v| {
                let mut vec = v.borrow_mut();
                let start = self.meta.len & !(ALIGN_SIZE.0 as u32 - 1);
                if start != self.meta.len {
                    metrics
                        .safe_write_read_tail_times
                        .fetch_add(1, Ordering::AcqRel);
                    metrics
                        .safe_write_read_tail_bytes
                        .fetch_add(ALIGN_SIZE.0, Ordering::AcqRel);
                    self.pread(&mut vec[start as usize..][..ALIGN_SIZE.into()], start)?;
                }
                if self.meta.len < offset {
                    metrics
                        .safe_write_truncate_extend
                        .fetch_add(1, Ordering::AcqRel);
                    vec[self.meta.len as usize..offset as usize].fill(0);
                }
                vec[offset as usize..][..data.len()].copy_from_slice(data);
                let new_len = offset as usize + data.len();
                let begin = std::time::Instant::now();
                self.pwrite(
                    &vec[start as usize..new_len.next_multiple_of(ALIGN_SIZE.into())],
                    start,
                )?;
                let latency = std::time::Instant::now().duration_since(begin).as_micros() as _;
                metrics.pwrite_times.fetch_add(1, Ordering::AcqRel);
                metrics.pwrite_latency.fetch_add(latency, Ordering::AcqRel);
                self.meta.checksum = crc32c::crc32c_append(
                    self.meta.checksum,
                    &vec[self.meta.len as usize..new_len],
                );
                metrics
                    .safe_write_indirect_append
                    .fetch_add(1, Ordering::AcqRel);
                metrics.checksum_combine.fetch_add(1, Ordering::AcqRel);
                self.meta.len = new_len as u32;
                Result::Ok(())
            })?;
        } else {
            assert!(data.is_empty());
        }
        Ok(())
    }

    pub fn pread(&self, buf: &mut [u8], offset: u32) -> Result<()> {
        self.allocator.clusters.pread(self.meta.pos, buf, offset)
    }

    pub(super) fn pwrite(&self, buf: &[u8], offset: u32) -> Result<()> {
        self.allocator.clusters.pwrite(self.meta.pos, buf, offset)
    }

    pub fn fd_and_offset(&self) -> FdAndOffset {
        self.allocator.clusters.fd_and_offset(self.meta.pos)
    }
}

impl Clone for Chunk {
    fn clone(&self) -> Self {
        self.allocator.reference(self.meta.clone(), false)
    }
}

impl Drop for Chunk {
    fn drop(&mut self) {
        self.allocator.dereference(self.meta.pos);
    }
}

impl std::fmt::Debug for Chunk {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        std::fmt::Debug::fmt(&self.meta, f)
    }
}
