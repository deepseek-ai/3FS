use super::super::{Bytes, Error, GroupId, Position, Result};
use byteorder::{BigEndian, ByteOrder};

pub struct MetaKey(Bytes);

impl MetaKey {
    pub const CHUNK_META_KEY_PREFIX: u8 = 1;
    pub const GROUP_BITS_KEY_PREFIX: u8 = 2;
    pub const POS_TO_CHUNK_KEY_PREFIX: u8 = 3;
    pub const USED_SIZE_KEY_PREFIX: u8 = 4;
    pub const USED_SIZE_PREFIX_LEN_KEY: u8 = 5;
    pub const TIMESTAMP_KEY_PREFIX: u8 = 6;
    // pub const WRITING_CHUNK_KEY_PREFIX: u8 = 7;
    pub const VERSION_KEY: u8 = 8;
    pub const WRITING_CHUNK_KEY_PREFIX: u8 = 9;
    pub const TEST_KEY_PREFIX: u8 = b'm';

    fn prefix(mark: u8) -> Self {
        let mut vec = Bytes::new();
        vec.push(mark);
        Self(vec)
    }

    pub fn chunk_meta_key_prefix() -> Self {
        Self::prefix(Self::CHUNK_META_KEY_PREFIX)
    }

    pub fn chunk_meta_key(chunk_id: &[u8]) -> Self {
        let mut out = Self::chunk_meta_key_prefix();
        for num in chunk_id {
            out.0.push(!num)
        }
        out
    }

    pub fn parse_chunk_meta_key(key: &[u8]) -> Bytes {
        let mut out = Bytes::new();
        for num in &key[1..] {
            out.push(!num);
        }
        out
    }

    pub fn group_bits_key_prefix() -> Self {
        Self::prefix(Self::GROUP_BITS_KEY_PREFIX)
    }

    pub fn group_bits_chunk_size_prefix(group_id: GroupId) -> Self {
        let mut out = Self::group_bits_key_prefix();
        out.0.extend_from_slice(&group_id.to_be_bytes()[..4]);
        out
    }

    pub fn group_bits_key(group_id: GroupId) -> Self {
        let mut out = Self::group_bits_key_prefix();
        out.0.extend_from_slice(&group_id.to_be_bytes());
        out
    }

    pub fn parse_group_bits_key(key: &[u8]) -> Result<GroupId> {
        if key.len() == std::mem::size_of::<u8>() + std::mem::size_of::<u64>() {
            let group_id = BigEndian::read_u64(&key[1..]);
            Ok(GroupId::from(group_id))
        } else {
            Err(Error::MetaError(format!(
                "parse group bits key fail: {:?}",
                key
            )))
        }
    }

    pub fn pos_to_chunk_key_prefix() -> Self {
        Self::prefix(Self::POS_TO_CHUNK_KEY_PREFIX)
    }

    pub fn group_to_chunks_key_prefix(group_id: GroupId) -> Self {
        let mut out = Self::pos_to_chunk_key_prefix();
        out.0
            .extend_from_slice(&Position::new(group_id, 0).to_be_bytes());
        out.0.pop();
        out
    }

    pub fn pos_to_chunk_key(pos: Position) -> Self {
        let mut out = Self::pos_to_chunk_key_prefix();
        out.0.extend_from_slice(&pos.to_be_bytes());
        out
    }

    pub fn parse_pos_to_chunk_key(key: &[u8]) -> Result<Position> {
        if key.len() == std::mem::size_of::<u8>() + std::mem::size_of::<u64>() {
            Ok(Position::from(BigEndian::read_u64(&key[1..])))
        } else {
            Err(Error::MetaError(format!(
                "parse pos to chunk key fail: {:?}",
                key
            )))
        }
    }

    pub fn used_size_key_prefix() -> Self {
        Self::prefix(Self::USED_SIZE_KEY_PREFIX)
    }

    pub fn used_size_key(prefix: &[u8]) -> Self {
        let mut out = Self::used_size_key_prefix();
        out.0.extend_from_slice(prefix);
        out
    }

    pub fn used_size_prefix_len_key() -> Self {
        Self::prefix(Self::USED_SIZE_PREFIX_LEN_KEY)
    }

    pub fn timestamp_key_prefix() -> Self {
        Self::prefix(Self::TIMESTAMP_KEY_PREFIX)
    }

    pub fn timestamp_key_filter(prefix: &[u8], timestamp: u64) -> Self {
        let mut out = Self::timestamp_key_prefix();
        out.0.extend_from_slice(prefix);
        out.0.extend_from_slice(&timestamp.to_be_bytes());
        out
    }

    pub fn timestamp_key(timestamp: u64, chunk_id: &[u8], prefix_len: usize) -> Self {
        let mut out = Self::timestamp_key_filter(&chunk_id[..prefix_len], timestamp);
        out.0.extend_from_slice(&chunk_id[prefix_len..]);
        out
    }

    pub fn parse_timestamp_key(key: &[u8], prefix_len: usize) -> Result<(u64, Bytes)> {
        const L: usize = std::mem::size_of::<u8>() + std::mem::size_of::<u64>();
        if key.len() > L + prefix_len {
            let mut chunk_id = Bytes::from(&key[1..1 + prefix_len]);
            let timestamp = BigEndian::read_u64(&key[1 + prefix_len..]);
            chunk_id.extend_from_slice(&key[L + prefix_len..]);
            Ok((timestamp, chunk_id))
        } else {
            Err(Error::MetaError(format!(
                "parse timestamp key fail: {:?}",
                key
            )))
        }
    }

    pub fn version_key() -> Self {
        Self::prefix(Self::VERSION_KEY)
    }

    pub fn writing_chunk_key_prefix() -> Self {
        Self::prefix(Self::WRITING_CHUNK_KEY_PREFIX)
    }

    pub fn writing_chunk_key(chunk_id: &[u8]) -> Self {
        let mut out = Self::writing_chunk_key_prefix();
        out.0.extend_from_slice(chunk_id);
        out
    }

    pub fn parse_writing_chunk_key(key: &[u8]) -> Result<Bytes> {
        if key.len() > 1 {
            Ok(Bytes::from(&key[1..]))
        } else {
            Err(Error::MetaError(format!(
                "parse writing chunk key fail: {:?}",
                key
            )))
        }
    }
}

impl AsRef<[u8]> for MetaKey {
    fn as_ref(&self) -> &[u8] {
        &self.0
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn test_meta_key_create() {
        use super::super::super::*;

        let prefix = MetaKey::chunk_meta_key_prefix();
        assert_eq!(prefix.as_ref(), [MetaKey::CHUNK_META_KEY_PREFIX]);

        let meta_key = MetaKey::chunk_meta_key(&[1, 2, 3, 4]);
        assert_eq!(
            meta_key.as_ref(),
            [MetaKey::CHUNK_META_KEY_PREFIX, !1, !2, !3, !4]
        );

        let group_id = GroupId::new(CHUNK_SIZE_NORMAL, 1, 2);
        let pos = Position::new(group_id, 3);
        let pos_to_chunk_key = MetaKey::pos_to_chunk_key(pos);
        assert_eq!(pos_to_chunk_key.as_ref().len(), 1 + 8);
        let parsed_pos = MetaKey::parse_pos_to_chunk_key(pos_to_chunk_key.as_ref()).unwrap();
        assert_eq!(pos, parsed_pos);

        let group_to_chunks_key_prefix = MetaKey::group_to_chunks_key_prefix(group_id);
        assert_eq!(group_to_chunks_key_prefix.as_ref().len(), 8);

        assert!(MetaKey::parse_group_bits_key(&[]).is_err());
        assert!(MetaKey::parse_pos_to_chunk_key(&[]).is_err());

        let timestamp_key = MetaKey::timestamp_key(1024, &[1, 2, 3, 4], 2);
        let (timestamp, chunk) = MetaKey::parse_timestamp_key(&timestamp_key.0, 2).unwrap();
        assert_eq!(timestamp, 1024);
        assert_eq!(chunk, [1, 2, 3, 4].as_slice());

        MetaKey::parse_timestamp_key(&[MetaKey::TIMESTAMP_KEY_PREFIX, 0, 1, 2, 3, 4, 5, 6, 7], 0)
            .unwrap_err();

        MetaKey::parse_writing_chunk_key(MetaKey::writing_chunk_key_prefix().as_ref()).unwrap_err();
    }
}
