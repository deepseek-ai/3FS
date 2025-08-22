use chunk_engine::*;
use criterion::{criterion_group, criterion_main, BenchmarkId, Criterion};
use std::sync::Arc;

fn allocate(allocator: &Arc<Allocator>, n: usize) {
    for _ in 0..n {
        drop(allocator.allocate(true).unwrap());
    }
}

fn criterion_benchmark(c: &mut Criterion) {
    let dir = tempfile::tempdir().unwrap();

    let cluster_config = ClustersConfig {
        path: dir.path().into(),
        chunk_size: CHUNK_SIZE_NORMAL,
        create: true,
    };
    let clusters = Clusters::open(&cluster_config).unwrap();

    let meta_store_config = MetaStoreConfig {
        rocksdb: RocksDBConfig {
            path: dir.path().join("meta"),
            create: true,
            ..Default::default()
        },
        ..Default::default()
    };
    let meta_store = std::sync::Arc::new(MetaStore::open(&meta_store_config).unwrap());

    let allocator = chunk_engine::Allocator::load(clusters, meta_store.iterator()).unwrap();
    allocator.do_allocate_task(1, 1, &meta_store).unwrap();

    let count: usize = 1 << 16;

    c.bench_with_input(BenchmarkId::new("allocate", count), &count, |b, &c| {
        b.iter(|| allocate(&allocator, c))
    });
}

criterion_group!(benches, criterion_benchmark);
criterion_main!(benches);
