use std::{
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Condvar, Mutex,
    },
    thread::JoinHandle,
};

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum WorkerState {
    Continue,
    Pause,
    Wait(std::time::Duration),
    Stop,
}

#[derive(Default)]
pub struct WorkerBuilder {
    name: Option<String>,
    condvar: Option<Arc<Condvar>>,
}

impl WorkerBuilder {
    pub fn name(mut self, str: String) -> Self {
        self.name = Some(str);
        self
    }

    pub fn cond(mut self, condvar: Arc<Condvar>) -> Self {
        self.condvar = Some(condvar);
        self
    }

    pub fn spawn<F>(self, f: F) -> Worker
    where
        F: FnMut() -> WorkerState + Send + 'static,
    {
        Worker::new(f, self.name, self.condvar)
    }
}

pub struct Worker {
    stopping: Arc<AtomicBool>,
    condvar: Arc<Condvar>,
    handle: Option<JoinHandle<()>>,
}

impl Worker {
    pub fn new<F>(mut f: F, name: Option<String>, condvar: Option<Arc<Condvar>>) -> Worker
    where
        F: FnMut() -> WorkerState + Send + 'static,
    {
        let stopping = Arc::new(AtomicBool::default());
        let stopping_clone = stopping.clone();
        let condvar = condvar.unwrap_or_default();
        let condvar_clone = condvar.clone();

        let builder = if let Some(name) = name {
            std::thread::Builder::new().name(name)
        } else {
            std::thread::Builder::new()
        };
        let handle = Some(
            builder
                .spawn(move || {
                    let mutex = Mutex::new(());
                    while !stopping_clone.load(Ordering::Acquire) {
                        match f() {
                            WorkerState::Continue => continue,
                            WorkerState::Pause => {
                                drop(condvar_clone.wait(mutex.lock().unwrap()).unwrap());
                            }
                            WorkerState::Wait(duration) => {
                                drop(
                                    condvar_clone
                                        .wait_timeout(mutex.lock().unwrap(), duration)
                                        .unwrap(),
                                );
                            }
                            WorkerState::Stop => break,
                        }
                    }
                })
                .unwrap(),
        );

        Worker {
            stopping,
            condvar,
            handle,
        }
    }

    pub fn stop_and_join(&mut self) {
        self.stopping.store(true, Ordering::Release);
        self.condvar.notify_all();
        if let Some(handle) = self.handle.take() {
            handle.join().unwrap();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_worker() {
        let count = Arc::new(std::sync::atomic::AtomicUsize::default());
        let condvar = Default::default();
        let count_clone = count.clone();
        let mut worker = WorkerBuilder::default()
            .name("Worker".into())
            .cond(condvar)
            .spawn(move || {
                if count_clone.fetch_add(1, Ordering::SeqCst) + 1 < 10 {
                    WorkerState::Continue
                } else {
                    WorkerState::Pause
                }
            });

        while count.load(Ordering::Acquire) < 10 {
            std::thread::sleep(std::time::Duration::from_millis(10));
        }

        worker.stop_and_join();
        assert_eq!(count.load(Ordering::Acquire), 10);
    }

    #[test]
    fn test_worker_2() {
        let worker = WorkerBuilder::default().spawn(move || WorkerState::Stop);
        let _ = worker.handle.unwrap().join();
    }
}
