#![allow(non_upper_case_globals)]
#![allow(non_camel_case_types)]
#![allow(non_snake_case)]

include!(concat!(env!("OUT_DIR"), "/bindings.rs"));

use std::ffi::CString;
use std::mem::{size_of, transmute};
use std::ops::{Range, RangeInclusive};
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd, RawFd};
use std::os::unix::fs;
use std::ptr;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use shared_memory::Shmem;
use uuid::Uuid;

impl Default for hf3fs_iov {
    fn default() -> Self {
        Self {
            base: ptr::null_mut(),
            iovh: ptr::null_mut(),
            id: [0; 16],
            mount_point: [0; 256],
            size: 0,
            block_size: 0,
            numa: 0,
        }
    }
}
pub struct Iov(hf3fs_iov);

unsafe impl Send for Iov {}
unsafe impl Sync for Iov {}

impl Iov {
    pub fn wrap(mountpoint: &str, shm: &Shmem, numa: i32) -> Result<Self, i32> {
        let id = Uuid::new_v4();
        let targ = format!("/dev/shm/{}", shm.get_os_id());
        let link = format!(
            "{}/3fs-virt/iovs/{}",
            mountpoint,
            id.as_hyphenated().to_string()
        );
        fs::symlink(&targ, &link).expect(&format!("failed to create symlink for shm {}", targ));

        let mut iov = hf3fs_iov::default();
        let mp = CString::new(mountpoint)
            .expect("failed to alloc mountpoint string")
            .into_raw();
        let r = unsafe {
            hf3fs_iovwrap(
                &mut iov as *mut _,
                shm.as_ptr() as *mut _,
                id.as_bytes().as_ptr(),
                mp,
                shm.len(),
                0,
                numa,
            )
        };
        if r == 0 {
            Ok(Self(iov))
        } else {
            Err(r)
        }
    }
}

pub struct Ior {
    ior: hf3fs_ior,
}

unsafe impl Send for Ior {}

impl Ior {
    pub fn create(
        mountpoint: &str,
        for_read: bool,
        entries: i32,
        iodepth: i32,
        timeout: i32,
        numa: i32,
        flags: u64,
    ) -> Result<Self, i32> {
        let mut ior = hf3fs_ior {
            iov: hf3fs_iov::default(),
            iorh: ptr::null_mut(),
            mount_point: [0; 256],
            for_read: true,
            io_depth: 0,
            priority: 0,
            timeout: 0,
            flags: 0,
        };
        let mp = CString::new(mountpoint)
            .expect("failed to alloc mountpoint string")
            .into_raw();
        let r = unsafe {
            hf3fs_iorcreate4(
                &mut ior as *mut _,
                mp,
                entries,
                for_read,
                iodepth,
                timeout,
                numa,
                flags,
            )
        };
        if r == 0 {
            Ok(Self { ior })
        } else {
            Err(r)
        }
    }
    pub fn prepare<T>(
        &self,
        iov: &Iov,
        bufrng: Range<usize>,
        fd: &RegisteredFd,
        file_off: usize,
        extra: T,
    ) -> Result<(), i32> {
        let io = Box::new(PreparedIo {
            buf: iov.0.base,
            extra,
            result: 0,
        });
        let r = unsafe {
            hf3fs_prep_io(
                &self.ior as *const _,
                &iov.0 as *const _,
                self.ior.for_read,
                iov.0.base.add(bufrng.start) as *mut _,
                fd.0.as_raw_fd(),
                file_off,
                (bufrng.end - bufrng.start) as _,
                Box::into_raw(io) as *const _,
            )
        };
        if r >= 0 {
            Ok(())
        } else {
            Err(r)
        }
    }
    pub fn submit(&self) {
        unsafe {
            hf3fs_submit_ios(&self.ior as *const _);
        }
    }
    pub fn poll<T>(
        &self,
        wanted: RangeInclusive<usize>,
        timeout_in_ms: u64,
    ) -> Vec<Box<PreparedIo<T>>> {
        let dl = SystemTime::now() + Duration::from_millis(timeout_in_ms);
        let ts = dl.duration_since(UNIX_EPOCH).unwrap();
        let to = timespec {
            tv_sec: ts.as_secs() as _,
            tv_nsec: ts.subsec_nanos() as _,
        };
        let arr: __BindgenOpaqueArray<u8, { size_of::<timespec>() }> = unsafe { transmute(to) };
        let pto = &arr as *const __BindgenOpaqueArray<u8, { size_of::<timespec>() }>;

        let min = *wanted.start();
        let max = *wanted.end();
        let mut cqes = Vec::with_capacity(max);
        cqes.resize(max, hf3fs_cqe::default());
        let n = unsafe {
            hf3fs_wait_for_ios(
                &self.ior as *const _,
                cqes.as_mut_ptr(),
                max as _,
                min as _,
                pto as _,
            )
        };
        (0..n)
            .map(|i| {
                let cqe: &hf3fs_cqe = &cqes[i as usize];
                let mut io = unsafe { Box::from_raw(cqe.userdata as *mut PreparedIo<T>) };
                io.result = cqe.result;
                io
            })
            .collect()
    }
}

impl Drop for Ior {
    fn drop(&mut self) {
        unsafe {
            hf3fs_iordestroy(&mut self.ior as *mut _);
        }
    }
}

impl Default for hf3fs_cqe {
    fn default() -> Self {
        Self {
            index: 0,
            reserved: 0,
            result: 0,
            userdata: ptr::null_mut(),
        }
    }
}

pub struct RegisteredFd(OwnedFd);

impl RegisteredFd {
    fn register(fd: RawFd) -> Self {
        unsafe {
            hf3fs_reg_fd(fd as _, 0);
            Self(OwnedFd::from_raw_fd(fd))
        }
    }
    pub fn open_and_register<P: AsRef<std::path::Path>>(path: P) -> std::io::Result<Self> {
        std::fs::File::open(path).map(|f| Self::register(f.into_raw_fd()))
    }
}

impl Drop for RegisteredFd {
    fn drop(&mut self) {
        unsafe {
            hf3fs_dereg_fd(self.0.as_raw_fd());
        }
    }
}

pub struct PreparedIo<T> {
    pub buf: *mut u8,
    pub extra: T,
    pub result: i64,
}

#[repr(C)]
struct timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

#[cfg(test)]
mod tests {
    use super::*;
    use shared_memory::*;

    #[test]
    fn test_io() {
        let shm = ShmemConf::new()
            .os_id("/123")
            .size(10 << 10)
            .create()
            .unwrap();
        let iov = Iov::wrap("/3fs/test", &shm, 0).unwrap();

        let ior = Ior::create(
            "/3fs/test",
            true,
            1000,
            0,
            1000,
            -1,
            HF3FS_IOR_FORBID_READ_HOLES as _,
        )
        .unwrap();
        let fp = "/3fs/test";
        let fd = RegisteredFd::open_and_register(fp).unwrap();
        ior.prepare(&iov, 0..52, &fd, 0, (1u64, 2usize, 3usize))
            .unwrap();
        ior.submit();
        let rs = ior.poll::<(u64, usize, usize)>(1..=1, 1000);
        assert_eq!(rs.len(), 1);
        println!("{:?} {}", rs[0].extra, rs[0].result);
        assert_eq!(rs[0].extra, (1u64, 2usize, 3usize));
        assert_eq!(rs[0].result, 52);
    }
}
