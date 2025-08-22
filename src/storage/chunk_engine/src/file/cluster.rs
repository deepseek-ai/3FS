use std::fs::File;
use std::os::fd::AsRawFd;
use std::os::unix::fs::FileExt;
use std::path::Path;
use std::{fs::OpenOptions, os::unix::fs::OpenOptionsExt};

use super::super::*;

const PUNCH_HOLE_FLAGS: i32 = libc::FALLOC_FL_PUNCH_HOLE | libc::FALLOC_FL_KEEP_SIZE;

pub struct Cluster {
    pub normal_fd: File,
    pub direct_fd: File,
}

impl Cluster {
    pub fn open(path: &Path, create: bool, support_direct_io: bool) -> Result<Self> {
        let normal_fd = OpenOptions::new()
            .read(true)
            .write(true)
            .create(create)
            .custom_flags(libc::O_SYNC)
            .open(path)
            .map_err(|err| Error::IoError(format!("open {:?} failed: {:?}", path, err)))?;

        let direct_fd = OpenOptions::new()
            .read(true)
            .write(true)
            .custom_flags(if support_direct_io {
                libc::O_DIRECT
            } else {
                libc::O_SYNC
            })
            .open(path)
            .map_err(|err| Error::IoError(format!("open {:?} failed: {:?}", path, err)))?;

        Ok(Self {
            normal_fd,
            direct_fd,
        })
    }

    pub fn fallocate(&self, group_id: GroupId, punch_hole: bool) -> Result<()> {
        let res = unsafe {
            libc::fallocate(
                self.direct_fd.as_raw_fd(),
                if punch_hole { PUNCH_HOLE_FLAGS } else { 0 },
                group_id.offset().into(),
                group_id.size().into(),
            )
        };
        if res == -1 {
            Err(Error::IoError(format!(
                "fallocate {} error: {:?}",
                self.direct_fd.as_raw_fd(),
                std::io::Error::last_os_error()
            )))
        } else {
            Ok(())
        }
    }

    pub fn pread(&self, pos: Position, mut buf: &mut [u8], offset: u32) -> Result<()> {
        let aligned = is_aligned_io(buf, offset);
        let mut offset = pos.offset() + offset;
        while !buf.is_empty() {
            let fd = if aligned && is_aligned_len(buf.len() as u32) {
                &self.direct_fd
            } else {
                &self.normal_fd
            };

            match fd.read_at(buf, offset.into()) {
                Ok(0) => return Err(Error::IoError(format!("read {:?} return 0", fd))),
                Ok(n) => {
                    buf = &mut buf[n..];
                    offset += n;
                }
                Err(e) => Self::handle_error(e)?,
            }
        }
        Ok(())
    }

    pub fn pwrite(&self, pos: Position, mut buf: &[u8], offset: u32) -> Result<()> {
        let aligned = is_aligned_io(buf, offset);
        let mut offset = pos.offset() + offset;
        while !buf.is_empty() {
            let fd = if aligned && is_aligned_len(buf.len() as u32) {
                &self.direct_fd
            } else {
                &self.normal_fd
            };

            match fd.write_at(buf, offset.into()) {
                Ok(0) => return Err(Error::IoError(format!("write {:?} return 0", fd))),
                Ok(n) => {
                    buf = &buf[n..];
                    offset += n as u64;
                }
                Err(e) => Self::handle_error(e)?,
            }
        }
        Ok(())
    }

    fn handle_error(e: std::io::Error) -> Result<()> {
        if e.kind() == std::io::ErrorKind::Interrupted {
            Ok(())
        } else {
            Err(Error::IoError(format!("io error: {:?}", e)))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::fd::FromRawFd;

    #[test]
    fn test_cluster_open() {
        let dir = tempfile::tempdir().unwrap();
        let support_direct_io = FsType::check(&dir).support_direct_io();

        for chunk_size in [CHUNK_SIZE_NORMAL, CHUNK_SIZE_SMALL, CHUNK_SIZE_LARGE] {
            let file_path = dir.path().join(format!("test.cluster.{}", chunk_size));
            assert!(Cluster::open(&file_path, false, support_direct_io).is_err());

            let cluster = Cluster::open(&file_path, true, support_direct_io).unwrap();
            let meta = cluster.normal_fd.metadata().unwrap();
            assert_eq!(meta.len(), 0);

            let cluster = Cluster::open(&file_path, false, support_direct_io).unwrap();
            let group_id = GroupId::new(chunk_size, 0, 0);

            let mut buf = [0u8; 5];
            let pos = Position::new(group_id, 0);
            assert!(cluster.pread(pos, &mut buf, 0).is_err());

            cluster.fallocate(group_id, false).unwrap();
            let meta = cluster.normal_fd.metadata().unwrap();
            assert_eq!(meta.len(), group_id.size());

            let bytes = "hello world!".as_bytes();
            assert!(cluster.pwrite(pos, bytes, 0).is_ok());

            assert!(cluster.pread(pos, &mut buf, 0).is_ok());
            assert_eq!(&buf, &bytes[0..buf.len()]);

            cluster.fallocate(group_id, true).unwrap();
            let meta = cluster.normal_fd.metadata().unwrap();
            assert_eq!(meta.len(), group_id.size());
        }

        assert!(Cluster::open(Path::new("/dev/null"), false, support_direct_io).is_err());

        let cluster = Cluster {
            normal_fd: File::open("/dev/null").unwrap(),
            direct_fd: File::open("/dev/null").unwrap(),
        };
        assert!(cluster.fallocate(GroupId::default(), false).is_err());
        assert!(cluster.fallocate(GroupId::default(), true).is_err());
        assert!(cluster.pwrite(Position::from(0), &[1], 0).is_err());

        let cluster = Cluster {
            normal_fd: unsafe { File::from_raw_fd(23333) },
            direct_fd: unsafe { File::from_raw_fd(23333) },
        };
        let mut buf = [0u8; 32];
        assert!(cluster.pread(Position::from(0), &mut buf, 0).is_err());
        std::mem::forget(cluster);

        assert!(Cluster::handle_error(std::io::Error::from_raw_os_error(libc::EINTR)).is_ok());
    }
}
