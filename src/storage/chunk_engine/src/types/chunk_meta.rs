use super::super::*;

pub type ETag = tinyvec::TinyVec<[u8; 14]>;

#[derive(derse::Serialize, derse::Deserialize, Clone, PartialEq, Eq, Debug)]
#[repr(C)]
pub struct ChunkMeta {
    pub pos: Position,
    pub chain_ver: u32,
    pub chunk_ver: u32,
    pub len: u32,
    pub checksum: u32,
    pub timestamp: u64,
    pub last_request_id: u64,
    pub last_client_low: u64,
    pub last_client_high: u64,
    pub etag: ETag,
    pub uncommitted: bool,
}

impl ChunkMeta {
    pub fn now() -> u64 {
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_micros() as _
    }

    pub fn set_default_etag_if_need(&mut self) {
        if self.etag.is_empty() {
            self.etag = ETag::from(format!("{:X}", self.checksum).as_bytes());
        }
    }
}

impl Default for ChunkMeta {
    fn default() -> Self {
        Self {
            pos: Position::new(GroupId::new(Size::GB, 0, 0), 0),
            chain_ver: 0,
            chunk_ver: 0,
            len: 0,
            checksum: 0,
            timestamp: Self::now(),
            last_request_id: 0,
            last_client_low: 0,
            last_client_high: 0,
            etag: Default::default(),
            uncommitted: false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use derse::{Deserialize, Serialize};

    #[test]
    fn test_chunk_meta_seralization() {
        let ser = ChunkMeta {
            pos: Position::new(GroupId::default(), 88),
            chain_ver: 1,
            chunk_ver: 1,
            len: 2,
            timestamp: 0,
            etag: ETag::from(b"hello".as_slice()),
            ..Default::default()
        };

        let bytes: derse::DownwardBytes = ser.serialize().unwrap();
        assert_eq!(
            bytes.as_slice(),
            &[
                63, 88, 0, 0, 0, 0, 0, 8, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                0, 0, 5, b'h', b'e', b'l', b'l', b'o', 0,
            ]
        );

        let der = ChunkMeta::deserialize(&bytes[..]).unwrap();
        assert_eq!(ser, der);
    }
}
