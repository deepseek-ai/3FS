use chunk_engine::*;
use clap::Parser;
use derse::Deserialize;
use std::{
    collections::{BTreeMap, HashMap},
    path::PathBuf,
    sync::Arc,
};

/// A distributed copy/move tool.
#[derive(Parser, Debug, Clone)]
#[command(version, about, long_about = None)]
pub struct Args {
    /// Path to rocksdb.
    pub path: PathBuf,
}

fn main() -> Result<()> {
    let args = Args::parse();

    let meta_config = MetaStoreConfig {
        rocksdb: RocksDBConfig {
            path: args.path,
            create: false,
            read_only: true,
        },
        prefix_len: 4,
    };
    let meta_store = MetaStore::open(&meta_config)?;

    let mut chunk_allocators = HashMap::new();
    let mut used_map = BTreeMap::new();
    let mut reversed_map = BTreeMap::new();
    let mut group_count = BTreeMap::new();
    let mut chunk_size = CHUNK_SIZE_SMALL;
    let mut real_map = BTreeMap::new();
    loop {
        let counter = Arc::new(AllocatorCounter::new(chunk_size));
        let it = meta_store.iterator();
        let chunk_allocator = ChunkAllocator::load(it, counter.clone(), chunk_size)?;
        let allocated_chunks = counter.allocated_chunks();
        let reserved_chunks = counter.reserved_chunks();
        used_map.insert(chunk_size, allocated_chunks - reserved_chunks);
        reversed_map.insert(chunk_size, reserved_chunks);
        group_count.insert(
            chunk_size,
            (
                chunk_allocator.full_groups.len(),
                chunk_allocator.active_groups.len(),
            ),
        );
        real_map.insert(chunk_size, 0u64);
        chunk_allocators.insert(chunk_size, chunk_allocator);

        if chunk_size >= CHUNK_SIZE_ULTRA {
            break;
        }
        chunk_size *= 2;
    }

    let mut it = meta_store.iterator();
    let end_key = MetaKey::chunk_meta_key_prefix();
    it.seek(&end_key)?;

    if it.key() == Some(end_key.as_ref()) {
        it.next(); // [begin, end)
    }

    loop {
        if !it.valid() {
            break;
        }

        if it.key().unwrap()[0] != MetaKey::CHUNK_META_KEY_PREFIX {
            break;
        }

        let chunk_meta =
            ChunkMeta::deserialize(it.value().unwrap()).map_err(Error::SerializationError)?;

        let chunk_size = chunk_meta.pos.chunk_size();
        let allocator = chunk_allocators.get_mut(&chunk_size).unwrap();
        allocator.reference(chunk_meta.pos, true);
        real_map.entry(chunk_size).and_modify(|v| *v += 1);

        it.next();
    }
    println!("{:#?}", used_map);
    println!("{:#?}", reversed_map);
    println!("{:#?}", group_count);
    assert_eq!(used_map, real_map);

    Ok(())
}
