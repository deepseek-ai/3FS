use std::sync::{
    atomic::{AtomicUsize, Ordering},
    Arc,
};

use anyhow::{Context, Result};
use chunk_engine::*;
use serde::Deserialize;

#[derive(Debug, Default, Deserialize)]
struct Config {
    engine: EngineConfig,
    threads: usize,
    count: usize,
    level: String,
}

fn main() -> Result<()> {
    let mut iter = std::env::args();
    iter.next();
    let config_path = iter
        .next()
        .ok_or(anyhow::anyhow!("get config path failed"))?;

    let content = std::fs::read_to_string(&config_path)
        .with_context(|| format!("failed to open config file {:?}", config_path))?;

    let config: Config = toml::from_str(&content)
        .with_context(|| format!("failed to parse config file {:?}", config_path))?;

    let level = match config.level.as_str() {
        "info" => tracing::Level::INFO,
        "debug" => tracing::Level::DEBUG,
        _ => tracing::Level::WARN,
    };
    tracing_subscriber::fmt().with_max_level(level).init();
    tracing::info!("config content: {:#?}", config);

    let engine = chunk_engine::Engine::open(&config.engine).unwrap();
    engine.start_allocate_workers(2);
    std::thread::sleep(std::time::Duration::from_millis(100));
    let bytes = Arc::new(AtomicUsize::default());
    let running = Arc::new(AtomicUsize::default());

    let threads = (0..config.threads)
        .map(|i| {
            let engine = engine.clone();
            let bytes = bytes.clone();
            let running = running.clone();

            let mut vec = create_aligned_buf(CHUNK_SIZE_NORMAL);
            vec.fill(i as u8);
            let checksum = crc32c::crc32c(&vec);
            running.fetch_add(1, Ordering::SeqCst);

            Ok(std::thread::spawn(move || {
                let mut chunk_id: usize = i << 32;
                for _ in 0..config.count {
                    engine
                        .write(&chunk_id.to_be_bytes(), &vec, 0, checksum)
                        .unwrap();
                    chunk_id += 1;
                    bytes.fetch_add(vec.len(), Ordering::SeqCst);
                }
                running.fetch_sub(1, Ordering::SeqCst);
            }))
        })
        .collect::<Result<Vec<_>>>()?;

    while running.load(Ordering::Acquire) > 0 {
        std::thread::sleep(std::time::Duration::from_secs(1));
        let bytes = bytes.swap(0, Ordering::Acquire);
        let used_size = engine.used_size();
        tracing::info!(
            "throughput: {:?}/s, allocated: {:?}, reserved: {:?}",
            Size::from(bytes),
            used_size.allocated_size,
            used_size.reserved_size,
        );
    }

    for thread in threads {
        thread.join().unwrap();
    }

    engine.stop_and_join();
    engine.speed_up_quit();
    Ok(())
}
