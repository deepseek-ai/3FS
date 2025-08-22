#[derive(Debug, PartialEq)]
pub enum Error {
    IoError(String),
    RocksDBError(String),
    MetaError(String),
    InvalidArg(String),
    SerializationError(derse::Error),
    ChecksumMismatch(String),
    ChainVersionMismatch(String),
    ChunkETagMismatch(String),
    ChunkAlreadyExists,
    ChunkCommittedUpdate(String),
    ChunkMissingUpdate(String),
    NoSpace,
}

pub type Result<T> = std::result::Result<T, Error>;

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        std::fmt::Debug::fmt(self, f)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_error_display() {
        let error = Error::InvalidArg("invalid pos".into());
        assert_eq!(error.to_string(), r#"InvalidArg("invalid pos")"#);
    }
}
