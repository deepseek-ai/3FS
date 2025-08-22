use crate::{Error, Result, Size};
use std::path::PathBuf;

#[derive(Debug, Default, Clone)]
pub struct RocksDBConfig {
    pub path: PathBuf,
    pub create: bool,
    pub read_only: bool,
}

pub struct RocksDB {
    db: rocksdb::DB,
    write_options: [rocksdb::WriteOptions; 2], // 0 for non-sync, 1 for sync.
}

pub trait MergeOp {
    fn full_merge<'a>(
        key: &[u8],
        value: Option<&[u8]>,
        operands: impl Iterator<Item = &'a [u8]>,
    ) -> Option<Vec<u8>>;

    fn partial_merge<'a>(key: &[u8], operands: impl Iterator<Item = &'a [u8]>) -> Option<Vec<u8>>;
}

impl RocksDB {
    pub fn open<T: MergeOp + 'static>(config: &RocksDBConfig) -> Result<Self> {
        let mut db_options = rocksdb::Options::default();
        db_options.create_if_missing(config.create);
        db_options.set_merge_operator(
            "merge",
            |key, value, operands| T::full_merge(key, value, operands.iter()),
            |key, _value, operands| T::partial_merge(key, operands.iter()),
        );

        let mut table_options = rocksdb::BlockBasedOptions::default();
        table_options.set_bloom_filter(10.0, true);
        db_options.set_block_based_table_factory(&table_options);

        let db = if config.read_only {
            rocksdb::DB::open_for_read_only(&db_options, &config.path, false)
        } else {
            rocksdb::DB::open(&db_options, &config.path)
        }
        .map_err(|err| Error::RocksDBError(format!("open rocksdb fail: {:?}", err)))?;

        let mut sync_write_options = rocksdb::WriteOptions::new();
        sync_write_options.set_sync(true);

        Ok(Self {
            db,
            write_options: [rocksdb::WriteOptions::new(), sync_write_options],
        })
    }

    pub fn get(&self, key: impl AsRef<[u8]>) -> Result<Option<rocksdb::DBPinnableSlice>> {
        match self.db.get_pinned(key) {
            Ok(v) => Ok(v),
            Err(e) => Err(Error::RocksDBError(format!("RocksDB fail: {e:?}"))),
        }
    }

    pub fn put(&self, key: impl AsRef<[u8]>, value: impl AsRef<[u8]>, sync: bool) -> Result<()> {
        match self
            .db
            .put_opt(key, value, &self.write_options[sync as usize])
        {
            Ok(v) => Ok(v),
            Err(e) => Err(Error::RocksDBError(format!("RocksDB fail: {e:?}"))),
        }
    }

    pub fn delete(&self, key: impl AsRef<[u8]>, sync: bool) -> Result<()> {
        match self.db.delete_opt(key, &self.write_options[sync as usize]) {
            Ok(v) => Ok(v),
            Err(e) => Err(Error::RocksDBError(format!("RocksDB fail: {e:?}"))),
        }
    }

    pub fn new_write_batch() -> rocksdb::WriteBatch {
        rocksdb::WriteBatch::default()
    }

    pub fn write(&self, batch: rocksdb::WriteBatch, sync: bool) -> Result<()> {
        match self.db.write_opt(batch, &self.write_options[sync as usize]) {
            Ok(v) => Ok(v),
            Err(e) => Err(Error::RocksDBError(format!("RocksDB fail: {e:?}"))),
        }
    }

    pub fn new_iterator(&self) -> RocksDBIterator {
        let mut read_options = rocksdb::ReadOptions::default();
        read_options.set_readahead_size(Size::mebibyte(4).into());
        RocksDBIterator(self.db.raw_iterator_opt(read_options))
    }
}

impl Drop for RocksDB {
    fn drop(&mut self) {
        tracing::info!("RocksDB {:?} is closing...", self.db);
    }
}

pub struct RocksDBIterator<'a>(rocksdb::DBRawIterator<'a>);

impl RocksDBIterator<'_> {
    pub fn iterate<P, Fn>(&mut self, prefix: P, mut func: Fn) -> Result<u32>
    where
        P: AsRef<[u8]>,
        Fn: FnMut(&[u8], &[u8]) -> Result<()>,
    {
        let it = &mut self.0;
        it.seek(prefix.as_ref());
        let mut count = 0;
        while it.valid() && it.key().unwrap().starts_with(prefix.as_ref()) {
            func(it.key().unwrap(), it.value().unwrap_or(&[]))?;
            it.next();
            count += 1;
        }
        self.status()?;
        Ok(count)
    }

    pub fn seek<P>(&mut self, prefix: P) -> Result<()>
    where
        P: AsRef<[u8]>,
    {
        self.0.seek(prefix.as_ref());
        self.status()
    }

    pub fn valid(&self) -> bool {
        self.0.valid()
    }

    pub fn status(&self) -> Result<()> {
        self.0
            .status()
            .map_err(|e| Error::RocksDBError(e.to_string()))
    }

    pub fn next(&mut self) {
        self.0.next();
    }

    pub fn key(&self) -> Option<&[u8]> {
        self.0.key()
    }

    pub fn value(&self) -> Option<&[u8]> {
        self.0.value()
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn test_rocksdb_create_get_set() {
        use super::super::*;
        let dir = tempfile::tempdir().unwrap();

        let config = RocksDBConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        let rocksdb = RocksDB::open::<MetaMergeOp>(&config).unwrap();

        let value = rocksdb.get("merry".as_bytes()).unwrap();
        assert!(value.is_none());

        rocksdb
            .put("merry".as_bytes(), "world".as_bytes(), false)
            .unwrap();

        let value = rocksdb.get("merry".as_bytes()).unwrap();
        assert_eq!(value.as_deref(), Some("world".as_bytes()));

        let mut batch = RocksDB::new_write_batch();
        batch.put("merry", "RocksDB");
        batch.put("peace", "love");
        rocksdb.write(batch, false).unwrap();

        let value = rocksdb.get("merry".as_bytes()).unwrap();
        assert_eq!(value.as_deref(), Some("RocksDB".as_bytes()));
        let value = rocksdb.get("peace".as_bytes()).unwrap();
        assert_eq!(value.as_deref(), Some("love".as_bytes()));

        let mut batch = RocksDB::new_write_batch();
        batch.merge("merry", "1");
        batch.merge("merry", "2");
        for i in 0..16 {
            batch.merge("merge", format!("{i}"));
        }
        rocksdb.write(batch, false).unwrap();

        let value = rocksdb.get("merry".as_bytes()).unwrap();
        assert_eq!(value.as_deref(), Some("RocksDB12".as_bytes()));
        let value = rocksdb.get("merge".as_bytes()).unwrap();
        assert_eq!(value.as_deref(), Some("0123456789101112131415".as_bytes()));

        let mut it = rocksdb.new_iterator();
        let mut count = 0;
        let mut runner = |_: &[u8], _: &[u8]| {
            count += 1;
            crate::Result::Ok(())
        };

        assert_eq!(it.iterate([], &mut runner).unwrap(), 3);
        assert_eq!(it.iterate("m", &mut runner).unwrap(), 2);
        assert_eq!(it.iterate("a", &mut runner).unwrap(), 0);
        assert_eq!(it.iterate("z", &mut runner).unwrap(), 0);

        let config = RocksDBConfig {
            path: std::path::Path::new("/proc/test").into(),
            create: true,
            ..Default::default()
        };
        assert!(RocksDB::open::<MetaMergeOp>(&config).is_err());
    }

    #[test]
    fn test_rocksdb_parallel_write() {
        use super::super::*;
        use std::sync::Arc;
        let dir = tempfile::tempdir().unwrap();

        let config = RocksDBConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        let rocksdb = Arc::new(RocksDB::open::<MetaMergeOp>(&config).unwrap());

        const T: usize = 16;
        const N: usize = 1000;
        let mut threads = vec![];
        for i in 0..T {
            let rocksdb = rocksdb.clone();
            threads.push(
                std::thread::Builder::new()
                    .name(format!("test-{i}"))
                    .spawn(move || {
                        for j in 0..N {
                            let value = [j as u8; 32];
                            let mut batch = RocksDB::new_write_batch();
                            batch.put(format!("a{}atesta", i * N + j), value);
                            batch.put(format!("b{}btestb", i * N + j), value);
                            batch.merge(format!("m{}mtestm", i * N + j), value);
                            rocksdb.write(batch, false).unwrap();
                        }
                    })
                    .unwrap(),
            )
        }

        for thread in threads {
            thread.join().unwrap();
        }
    }

    #[test]
    fn test_rocksdb_invalid_merge() {
        use super::super::*;
        let dir = tempfile::tempdir().unwrap();

        let config = RocksDBConfig {
            path: dir.path().into(),
            create: true,
            ..Default::default()
        };

        let rocksdb = RocksDB::open::<MetaMergeOp>(&config).unwrap();

        let mut batch = RocksDB::new_write_batch();
        batch.merge("invalid_merge", "");
        rocksdb.write(batch, false).unwrap();

        assert!(rocksdb.get("invalid_merge").is_err());

        let mut runner = |_: &[u8], _: &[u8]| crate::Result::Ok(());
        let mut it = rocksdb.new_iterator();
        assert!(it.iterate("invalid_merge", &mut runner).is_err());

        assert!(it.seek("invalid_merge").is_err());

        assert!(rocksdb.put("invalid_merge", "ok", false).is_ok());
        assert!(rocksdb.get("invalid_merge").is_ok());
        drop(it);

        let mut it = rocksdb.new_iterator();
        assert_eq!(it.iterate("invalid_merge", &mut runner), Ok(1));

        it.seek("invalid_merge").unwrap();
        assert!(it.valid());
        assert_eq!(it.key().unwrap(), "invalid_merge".as_bytes());
        assert_eq!(it.value().unwrap(), "ok".as_bytes());

        it.next();
        assert!(!it.valid());
        assert!(it.status().is_ok());

        drop(it);
        drop(rocksdb);
        let config = RocksDBConfig {
            path: dir.path().into(),
            create: false,
            read_only: true,
        };
        RocksDB::open::<MetaMergeOp>(&config).unwrap();
    }
}
