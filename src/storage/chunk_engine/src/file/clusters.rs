use super::super::*;
use std::{fmt::Debug, os::fd::AsRawFd, path::PathBuf};

#[derive(Debug, Default)]
pub struct ClustersConfig {
    pub path: PathBuf,
    pub chunk_size: Size,
    pub create: bool,
}

pub struct Clusters {
    pub path: PathBuf,
    pub chunk_size: Size,
    files: Vec<Cluster>,
}

impl Clusters {
    const COUNT: u32 = 256;

    pub fn open(config: &ClustersConfig) -> Result<Self> {
        let mut files: Vec<Cluster> = vec![];

        if config.create {
            std::fs::create_dir_all(&config.path)
                .map_err(|e| Error::IoError(format!("create dir {:?} fail: {e:?}", config.path)))?;
        }

        let support_direct_io = FsType::check(&config.path).support_direct_io();
        for cluster_id in 0..Self::COUNT {
            let file_path = config.path.join(format!("{:02X}", cluster_id));
            files.push(Cluster::open(&file_path, config.create, support_direct_io)?);
        }

        Ok(Clusters {
            path: config.path.clone(),
            chunk_size: config.chunk_size,
            files,
        })
    }

    pub fn allocate(&self, group_id: GroupId) -> Result<()> {
        self.files[group_id.cluster() as usize].fallocate(group_id, false)
    }

    pub fn deallocate(&self, group_id: GroupId) -> Result<()> {
        self.files[group_id.cluster() as usize].fallocate(group_id, true)
    }

    pub fn pread(&self, pos: Position, buf: &mut [u8], offset: u32) -> Result<()> {
        self.files[pos.cluster() as usize].pread(pos, buf, offset)
    }

    pub fn pwrite(&self, pos: Position, buf: &[u8], offset: u32) -> Result<()> {
        self.files[pos.cluster() as usize].pwrite(pos, buf, offset)
    }

    pub fn fd_and_offset(&self, pos: Position) -> FdAndOffset {
        FdAndOffset {
            fd: self.files[pos.cluster() as usize].direct_fd.as_raw_fd(),
            offset: pos.offset().into(),
        }
    }
}

impl Debug for Clusters {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Clusters")
            .field("path", &self.path)
            .finish()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_clusters() {
        let dir = tempfile::tempdir().unwrap();

        let config = ClustersConfig {
            path: dir.path().into(),
            chunk_size: CHUNK_SIZE_NORMAL,
            create: true,
        };

        let clusters = Clusters::open(&config).unwrap();

        let group_id = GroupId::new(CHUNK_SIZE_NORMAL, 0, 0);
        let cluster = &clusters.files[0];
        let meta = cluster.normal_fd.metadata().unwrap();
        assert_eq!(meta.len(), 0);

        clusters.allocate(group_id).unwrap();
        let meta = cluster.normal_fd.metadata().unwrap();
        assert_eq!(meta.len(), group_id.size());

        let group_id_3 = GroupId::new(CHUNK_SIZE_NORMAL, 0, 3);
        clusters.allocate(group_id_3).unwrap();
        let meta = cluster.normal_fd.metadata().unwrap();
        assert_eq!(meta.len(), group_id.size() * 4);

        clusters.deallocate(group_id).unwrap();
        let meta = cluster.normal_fd.metadata().unwrap();
        assert_eq!(meta.len(), group_id.size() * 4);

        clusters.deallocate(group_id_3).unwrap();
        let meta = cluster.normal_fd.metadata().unwrap();
        assert_eq!(meta.len(), group_id.size() * 4);

        let config = ClustersConfig {
            path: std::path::Path::new("/proc/test").into(),
            chunk_size: CHUNK_SIZE_NORMAL,
            create: true,
        };
        assert!(Clusters::open(&config).is_err());
    }
}
