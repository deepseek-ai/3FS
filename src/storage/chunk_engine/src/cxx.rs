use std::collections::BTreeSet;
use std::path::PathBuf;
use std::pin::Pin;
use std::sync::atomic::Ordering;
use std::sync::Arc;

use crate::*;
pub use ::cxx::CxxString;

fn create(path: &str, create: bool, prefix_len: usize, error: Pin<&mut CxxString>) -> *mut Engine {
    let config = EngineConfig {
        path: PathBuf::from(path),
        create,
        prefix_len,
    };
    match Engine::open(&config) {
        Ok(engine) => Box::into_raw(Box::new(engine)),
        Err(e) => {
            error.push_str(&e.to_string());
            std::ptr::null_mut()
        }
    }
}

fn release(_engine: Box<Engine>) {}

#[allow(dead_code)]
struct LogGuard(tracing_appender::non_blocking::WorkerGuard);

fn init_log(path: &str, error: Pin<&mut CxxString>) -> *mut LogGuard {
    match rolling_file::BasicRollingFileAppender::new(
        path,
        rolling_file::RollingConditionBasic::new().max_size(Size::mebibyte(500).into()),
        20,
    ) {
        Ok(file_appender) => {
            let (non_blocking, guard) = tracing_appender::non_blocking(file_appender);
            tracing_subscriber::fmt()
                .with_max_level(tracing::Level::INFO)
                .with_writer(non_blocking)
                .with_ansi(false)
                .init();
            Box::into_raw(Box::new(LogGuard(guard)))
        }
        Err(e) => {
            error.push_str(&e.to_string());
            std::ptr::null_mut()
        }
    }
}

impl Chunk {
    fn raw_meta(&self) -> &ffi::RawMeta {
        unsafe { std::mem::transmute(self.meta()) }
    }

    fn raw_etag(&self) -> &[u8] {
        &self.meta().etag
    }

    fn uncommitted(&self) -> bool {
        self.meta().uncommitted
    }
}

impl WritingChunk {
    fn raw_meta(&self) -> &ffi::RawMeta {
        self.chunk.raw_meta()
    }

    fn raw_etag(&self) -> &[u8] {
        self.chunk.raw_etag()
    }

    fn uncommitted(&self) -> bool {
        self.chunk.uncommitted()
    }

    fn raw_chunk(&self) -> *const Chunk {
        &self.chunk
    }

    fn set_chain_ver(&mut self, chain_ver: u32) {
        self.chunk.set_chain_ver(chain_ver);
    }
}

impl Engine {
    fn raw_used_size(&self) -> ffi::RawUsedSize {
        unsafe { std::mem::transmute(self.used_size()) }
    }

    fn get_raw_chunk(&self, chunk_id: &[u8], error: Pin<&mut CxxString>) -> *const Chunk {
        match self.get(chunk_id) {
            Ok(None) => {
                error.clear();
                std::ptr::null()
            }
            Ok(Some(c)) => {
                error.clear();
                Arc::into_raw(c)
            }
            Err(e) => {
                error.push_str(&e.to_string());
                std::ptr::null()
            }
        }
    }

    fn get_raw_chunks(&self, reqs: &mut [GetReq], error: Pin<&mut CxxString>) {
        let chunk_ids = reqs
            .iter()
            .map(|r| Bytes::from(r.chunk_id))
            .collect::<BTreeSet<_>>();
        match self.batch_get(&chunk_ids) {
            Ok(chunks) => {
                for req in reqs {
                    match chunks.get(req.chunk_id) {
                        Some(c) => req.chunk_ptr = Arc::into_raw(c.clone()),
                        None => req.chunk_ptr = std::ptr::null_mut(),
                    }
                }
                error.clear();
            }
            Err(e) => {
                error.push_str(&e.to_string());
            }
        }
    }

    unsafe fn release_raw_chunk(&self, chunk: *const Chunk) {
        if !chunk.is_null() {
            Arc::from_raw(chunk);
        }
    }

    unsafe fn release_writing_chunk(&self, chunk: *mut WritingChunk) {
        if !chunk.is_null() {
            let _ = Box::from_raw(chunk);
        }
    }

    fn update_raw_chunk(
        &self,
        chunk_id: &[u8],
        mut req: Pin<&mut ffi::UpdateReq>,
        error: Pin<&mut CxxString>,
    ) -> *mut WritingChunk {
        match self.update_chunk(chunk_id, &mut req) {
            Ok(chunk) => Box::into_raw(Box::new(chunk)),
            Err(e) => {
                error.push_str(&e.to_string());
                req.out_error_code = match e {
                    Error::IoError(_) => 4011,              // ChunkWriteFailed
                    Error::RocksDBError(_) => 4003,         // ChunkMetadataSetError
                    Error::MetaError(_) => 4002,            // ChunkMetadataGetError
                    Error::InvalidArg(_) => 3,              // InvalidArg
                    Error::SerializationError(_) => 4002,   // ChunkMetadataGetError
                    Error::ChecksumMismatch(_) => 4080,     // ChecksumMismatch
                    Error::ChainVersionMismatch(_) => 4081, // ChainVersionMismatch
                    Error::ChunkETagMismatch(_) => 4083,    // ChunkETagMismatch
                    Error::ChunkAlreadyExists => 4084,      // ChunkAlreadyExists
                    Error::ChunkCommittedUpdate(_) => 4008, // ChunkCommittedUpdate
                    Error::ChunkMissingUpdate(_) => 4007,   // ChunkMissingUpdate
                    Error::NoSpace => 7021,                 // NoSpace
                };
                std::ptr::null_mut()
            }
        }
    }

    unsafe fn commit_raw_chunk(
        &self,
        new_chunk: *mut WritingChunk,
        sync: bool,
        error: Pin<&mut CxxString>,
    ) {
        let new_chunk = Box::from_raw(new_chunk);
        match self.commit_chunk(*new_chunk, sync) {
            Ok(_) => (),
            Err(e) => error.push_str(&e.to_string()),
        }
    }

    unsafe fn commit_raw_chunks(
        &self,
        reqs: &[*mut WritingChunk],
        sync: bool,
        error: Pin<&mut CxxString>,
    ) {
        let chunks = reqs.iter().map(|c| *Box::from_raw(*c)).collect::<Vec<_>>();
        match self.commit_chunks(chunks, sync) {
            Ok(_) => (),
            Err(e) => error.push_str(&e.to_string()),
        }
    }

    fn query_raw_chunks(
        &self,
        begin: &[u8],
        end: &[u8],
        max_count: u64,
        error: Pin<&mut CxxString>,
    ) -> Box<RawChunks> {
        match self.query_chunks(begin, end, max_count) {
            Ok(vec) => Box::new(RawChunks { vec }),
            Err(e) => {
                error.push_str(&e.to_string());
                Default::default()
            }
        }
    }

    fn query_all_raw_chunks(&self, prefix: &[u8], error: Pin<&mut CxxString>) -> Box<RawChunks> {
        match self.query_all_chunks(prefix) {
            Ok(vec) => Box::new(RawChunks { vec }),
            Err(e) => {
                error.push_str(&e.to_string());
                Default::default()
            }
        }
    }

    fn query_raw_chunks_by_timestamp(
        &self,
        prefix: &[u8],
        begin: u64,
        end: u64,
        max_count: u64,
        error: Pin<&mut CxxString>,
    ) -> Box<RawChunks> {
        match self.query_chunks_by_timestamp(prefix, begin, end, max_count) {
            Ok(vec) => Box::new(RawChunks { vec }),
            Err(e) => {
                error.push_str(&e.to_string());
                Default::default()
            }
        }
    }

    fn raw_batch_remove(
        &self,
        begin: &[u8],
        end: &[u8],
        max_count: u64,
        error: Pin<&mut CxxString>,
    ) -> u64 {
        match self.batch_remove(begin, end, max_count) {
            Ok(cnt) => cnt,
            Err(e) => {
                error.push_str(&e.to_string());
                0
            }
        }
    }

    fn query_raw_used_size(&self, prefix: &[u8], error: Pin<&mut CxxString>) -> u64 {
        match self.meta_store.query_used_size(prefix) {
            Ok(size) => size,
            Err(e) => {
                error.push_str(&e.to_string());
                0
            }
        }
    }

    fn get_metrics(&self) -> ffi::Metrics {
        let metrics = self.metrics.as_ref();
        let copy_on_write_times = metrics.copy_on_write_times.swap(0, Ordering::AcqRel);
        let copy_on_write_latency = metrics.copy_on_write_latency.swap(0, Ordering::AcqRel);
        let copy_on_write_read_times = metrics.copy_on_write_read_times.swap(0, Ordering::AcqRel);
        let copy_on_write_read_latency =
            metrics.copy_on_write_read_latency.swap(0, Ordering::AcqRel);
        let allocate_total_latency = metrics.allocate_latency.swap(0, Ordering::AcqRel);
        let allocate_total_times = metrics.allocate_times.swap(0, Ordering::AcqRel);
        let pwrite_total_latency = metrics.pwrite_latency.swap(0, Ordering::AcqRel);
        let pwrite_total_times = metrics.pwrite_times.swap(0, Ordering::AcqRel);
        ffi::Metrics {
            copy_on_write_times,
            copy_on_write_latency: copy_on_write_latency / std::cmp::max(1, copy_on_write_times),
            copy_on_write_read_bytes: metrics.copy_on_write_read_bytes.swap(0, Ordering::AcqRel),
            copy_on_write_read_times,
            copy_on_write_read_latency: copy_on_write_read_latency
                / std::cmp::max(1, copy_on_write_read_times),
            checksum_reuse: metrics.checksum_reuse.swap(0, Ordering::AcqRel),
            checksum_combine: metrics.checksum_combine.swap(0, Ordering::AcqRel),
            checksum_recalculate: metrics.checksum_recalculate.swap(0, Ordering::AcqRel),
            safe_write_direct_append: metrics.safe_write_direct_append.swap(0, Ordering::AcqRel),
            safe_write_indirect_append: metrics
                .safe_write_indirect_append
                .swap(0, Ordering::AcqRel),
            safe_write_truncate_shorten: metrics
                .safe_write_truncate_shorten
                .swap(0, Ordering::AcqRel),
            safe_write_truncate_extend: metrics
                .safe_write_truncate_extend
                .swap(0, Ordering::AcqRel),
            safe_write_read_tail_times: metrics
                .safe_write_read_tail_times
                .swap(0, Ordering::AcqRel),
            safe_write_read_tail_bytes: metrics
                .safe_write_read_tail_bytes
                .swap(0, Ordering::AcqRel),
            allocate_latency: allocate_total_latency / std::cmp::max(1, allocate_total_times),
            allocate_times: allocate_total_times,
            pwrite_latency: pwrite_total_latency / std::cmp::max(1, pwrite_total_times),
            pwrite_times: pwrite_total_times,
        }
    }

    fn query_uncommitted_raw_chunks(
        &self,
        prefix: &[u8],
        error: Pin<&mut CxxString>,
    ) -> Box<RawChunks> {
        match self.query_uncommitted_chunks(prefix) {
            Ok(chunks) => Box::new(RawChunks { vec: chunks }),
            Err(e) => {
                error.push_str(&e.to_string());
                Default::default()
            }
        }
    }

    fn handle_uncommitted_raw_chunks(
        &self,
        prefix: &[u8],
        chain_ver: u32,
        error: Pin<&mut CxxString>,
    ) -> Box<RawChunks> {
        match self.handle_uncommitted_chunks(prefix, chain_ver) {
            Ok(chunks) => Box::new(RawChunks { vec: chunks }),
            Err(e) => {
                error.push_str(&e.to_string());
                Box::default()
            }
        }
    }
}

#[derive(Default)]
struct RawChunks {
    vec: Vec<(Bytes, ChunkMeta)>,
}

impl RawChunks {
    fn len(&self) -> usize {
        self.vec.len()
    }

    fn chunk_id(&self, pos: usize) -> &[u8] {
        self.vec[pos].0.as_ref()
    }

    fn chunk_meta(&self, pos: usize) -> &ffi::RawMeta {
        unsafe { std::mem::transmute(&self.vec[pos].1) }
    }

    fn chunk_etag(&self, pos: usize) -> &[u8] {
        &self.vec[pos].1.etag
    }

    fn chunk_uncommitted(&self, pos: usize) -> bool {
        self.vec[pos].1.uncommitted
    }
}

#[::cxx::bridge(namespace = "hf3fs::chunk_engine")]
pub mod ffi {
    #[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
    struct UpdateReq {
        without_checksum: bool,
        is_truncate: bool,
        is_remove: bool,
        is_syncing: bool,
        update_ver: u32,
        chain_ver: u32,
        checksum: u32,
        length: u32,
        offset: u32,
        data: u64,
        last_request_id: u64,
        last_client_low: u64,
        last_client_high: u64,
        expected_tag: &'static [u8],
        desired_tag: &'static [u8],
        create_new: bool,

        out_non_existent: bool,
        out_error_code: u16,
        out_commit_ver: u32,
        out_chain_ver: u32,
        out_checksum: u32,
    }

    #[derive(Clone, Copy, PartialEq, Eq, Debug)]
    struct GetReq<'a> {
        chunk_id: &'a [u8],
        chunk_ptr: *const Chunk,
    }

    #[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
    struct RawMeta {
        pos: u64,
        chain_ver: u32,
        chunk_ver: u32,
        len: u32,
        checksum: u32,
        timestamp: u64,
        last_request_id: u64,
        last_client_low: u64,
        last_client_high: u64,
    }

    #[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
    struct RawUsedSize {
        allocated_size: u64,
        reserved_size: u64,
        position_count: u64,
        position_rc: u64,
    }

    #[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
    struct FdAndOffset {
        fd: i32,
        offset: u64,
    }

    #[derive(Default, Clone, Copy, PartialEq, Eq, Debug)]
    pub struct Metrics {
        pub copy_on_write_times: u64,
        pub copy_on_write_latency: u64,
        pub copy_on_write_read_bytes: u64,
        pub copy_on_write_read_times: u64,
        pub copy_on_write_read_latency: u64,

        pub checksum_reuse: u64,
        pub checksum_combine: u64,
        pub checksum_recalculate: u64,

        pub safe_write_direct_append: u64,
        pub safe_write_indirect_append: u64,
        pub safe_write_truncate_shorten: u64,
        pub safe_write_truncate_extend: u64,
        pub safe_write_read_tail_times: u64,
        pub safe_write_read_tail_bytes: u64,

        pub allocate_times: u64,
        pub allocate_latency: u64,
        pub pwrite_times: u64,
        pub pwrite_latency: u64,
    }

    extern "Rust" {
        type Engine;
        fn create(
            path: &str,
            create: bool,
            prefix_len: usize,
            error: Pin<&mut CxxString>,
        ) -> *mut Engine;
        fn release(engine: Box<Engine>);

        fn raw_used_size(&self) -> RawUsedSize;
        fn allocate_groups(&self, min_remain: usize, max_remain: usize, batch_size: usize)
            -> usize;
        fn allocate_ultra_groups(
            &self,
            min_remain: usize,
            max_remain: usize,
            batch_size: usize,
        ) -> usize;
        fn compact_groups(&self, max_reserved: u64) -> usize;

        fn set_allow_to_allocate(&self, val: bool);
        fn speed_up_quit(&self);

        fn get_raw_chunk(&self, chunk_id: &[u8], error: Pin<&mut CxxString>) -> *const Chunk;
        fn get_raw_chunks(&self, reqs: &mut [GetReq], error: Pin<&mut CxxString>);
        unsafe fn release_raw_chunk(&self, chunk: *const Chunk);
        unsafe fn release_writing_chunk(&self, chunk: *mut WritingChunk);

        fn update_raw_chunk(
            &self,
            chunk_id: &[u8],
            req: Pin<&mut UpdateReq>,
            error: Pin<&mut CxxString>,
        ) -> *mut WritingChunk;

        unsafe fn commit_raw_chunk(
            &self,
            new_chunk: *mut WritingChunk,
            sync: bool,
            error: Pin<&mut CxxString>,
        );

        unsafe fn commit_raw_chunks(
            &self,
            reqs: &[*mut WritingChunk],
            sync: bool,
            error: Pin<&mut CxxString>,
        );

        fn query_raw_chunks(
            &self,
            begin: &[u8],
            end: &[u8],
            max_count: u64,
            error: Pin<&mut CxxString>,
        ) -> Box<RawChunks>;

        fn query_all_raw_chunks(&self, prefix: &[u8], error: Pin<&mut CxxString>)
            -> Box<RawChunks>;

        fn query_raw_chunks_by_timestamp(
            &self,
            prefix: &[u8],
            begin: u64,
            end: u64,
            max_count: u64,
            error: Pin<&mut CxxString>,
        ) -> Box<RawChunks>;

        fn raw_batch_remove(
            &self,
            begin: &[u8],
            end: &[u8],
            max_count: u64,
            error: Pin<&mut CxxString>,
        ) -> u64;

        fn query_raw_used_size(&self, prefix: &[u8], error: Pin<&mut CxxString>) -> u64;

        fn get_metrics(&self) -> Metrics;

        fn query_uncommitted_raw_chunks(
            &self,
            prefix: &[u8],
            error: Pin<&mut CxxString>,
        ) -> Box<RawChunks>;

        fn handle_uncommitted_raw_chunks(
            &self,
            prefix: &[u8],
            chain_ver: u32,
            error: Pin<&mut CxxString>,
        ) -> Box<RawChunks>;
    }

    extern "Rust" {
        type LogGuard;
        fn init_log(path: &str, error: Pin<&mut CxxString>) -> *mut LogGuard;
    }

    extern "Rust" {
        type Chunk;
        fn raw_meta(&self) -> &RawMeta;
        fn raw_etag(&self) -> &[u8];
        fn uncommitted(&self) -> bool;
        fn fd_and_offset(&self) -> FdAndOffset;
    }

    extern "Rust" {
        type WritingChunk;
        fn raw_meta(&self) -> &RawMeta;
        fn raw_etag(&self) -> &[u8];
        fn uncommitted(&self) -> bool;
        fn raw_chunk(&self) -> *const Chunk;
        fn set_chain_ver(&mut self, chain_ver: u32);
    }

    extern "Rust" {
        type RawChunks;
        fn len(&self) -> usize;
        fn chunk_id(&self, pos: usize) -> &[u8];
        fn chunk_meta(&self, pos: usize) -> &RawMeta;
        fn chunk_etag(&self, pos: usize) -> &[u8];
        fn chunk_uncommitted(&self, pos: usize) -> bool;
    }
}

static_assertions::const_assert_eq!(
    std::mem::align_of::<ChunkMeta>(),
    std::mem::align_of::<ffi::RawMeta>()
);
static_assertions::const_assert_eq!(
    std::mem::size_of::<UsedSize>(),
    std::mem::size_of::<ffi::RawUsedSize>()
);
static_assertions::const_assert_eq!(
    std::mem::align_of::<UsedSize>(),
    std::mem::align_of::<ffi::RawUsedSize>()
);
static_assertions::const_assert_eq!(
    std::mem::size_of::<Metrics>(),
    std::mem::size_of::<ffi::Metrics>()
);
static_assertions::const_assert_eq!(
    std::mem::align_of::<Metrics>(),
    std::mem::align_of::<ffi::Metrics>()
);
