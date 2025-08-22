use super::super::Size;

pub const CHUNK_SIZE_SMALL: Size = Size::kibibyte(64);
pub const CHUNK_SIZE_NORMAL: Size = Size::kibibyte(512);
pub const CHUNK_SIZE_LARGE: Size = Size::mebibyte(4);
pub const CHUNK_SIZE_ULTRA: Size = Size::mebibyte(64);
pub const CHUNK_SIZE_SHIFT: usize = 16; // 64KiB is 2^16
pub const CHUNK_SIZE_NUMBER: usize = 11; // from 64KiB to 64MiB
