use std::{ffi::CString, os::unix::ffi::OsStrExt, path::Path};

#[derive(Debug, PartialEq, Clone, Copy)]
pub enum FsType {
    EXT4,
    NFS,
    XFS,
    ZFS,
    OTHER,
}

impl FsType {
    pub fn check(path: impl AsRef<Path>) -> Self {
        let path_cstr = CString::new(path.as_ref().as_os_str().as_bytes()).unwrap();
        let mut stat: libc::statfs = unsafe { std::mem::zeroed() };
        let result = unsafe { libc::statfs(path_cstr.as_ptr(), &mut stat) };
        if result != 0 {
            Self::OTHER
        } else {
            match stat.f_type {
                libc::EXT4_SUPER_MAGIC => Self::EXT4,
                libc::NFS_SUPER_MAGIC => Self::NFS,
                libc::XFS_SUPER_MAGIC => Self::XFS,
                0x2FC12FC1 => Self::ZFS, // https://github.com/openzfs/zfs/blob/33174af15112ed5c53299da2d28e763b0163f428/include/sys/fs/zfs.h#L1339
                _ => Self::OTHER,
            }
        }
    }

    pub fn support_direct_io(&self) -> bool {
        !matches!(self, FsType::ZFS)
    }
}
