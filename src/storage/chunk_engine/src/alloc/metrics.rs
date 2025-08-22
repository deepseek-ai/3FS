use std::sync::atomic::AtomicU64;

#[derive(Debug, Default)]
#[repr(C)]
pub struct Metrics {
    pub copy_on_write_times: AtomicU64,
    pub copy_on_write_latency: AtomicU64,
    pub copy_on_write_read_bytes: AtomicU64,
    pub copy_on_write_read_times: AtomicU64,
    pub copy_on_write_read_latency: AtomicU64,

    pub checksum_reuse: AtomicU64,
    pub checksum_combine: AtomicU64,
    pub checksum_recalculate: AtomicU64,

    pub safe_write_direct_append: AtomicU64,
    pub safe_write_indirect_append: AtomicU64,
    pub safe_write_truncate_shorten: AtomicU64,
    pub safe_write_truncate_extend: AtomicU64,
    pub safe_write_read_tail_times: AtomicU64,
    pub safe_write_read_tail_bytes: AtomicU64,

    pub allocate_times: AtomicU64,
    pub allocate_latency: AtomicU64,
    pub pwrite_times: AtomicU64,
    pub pwrite_latency: AtomicU64,
}
