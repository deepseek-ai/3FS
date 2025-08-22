use super::super::*;

#[derive(Copy, Clone, Eq, PartialEq, Hash, PartialOrd, Ord)]
pub struct GroupId(pub u64);

impl Default for GroupId {
    fn default() -> Self {
        GroupId::new(CHUNK_SIZE_NORMAL, 0, 0)
    }
}

impl GroupId {
    // 32bit chunk size + 24bit group + 8bit cluster
    const SHIFT: u32 = 8;
    pub const COUNT: u32 = (1 << Self::SHIFT);

    pub const fn new(chunk_size: Size, cluster: u8, group: u32) -> Self {
        Self(chunk_size.0 << 32 | (group << Self::SHIFT | cluster as u32) as u64)
    }

    pub const fn chunk_size(&self) -> Size {
        Size::new(self.0 >> 32)
    }

    pub const fn cluster(&self) -> u8 {
        self.0 as u8
    }

    pub const fn group(&self) -> u32 {
        (self.0 as u32) >> Self::SHIFT
    }

    pub fn offset(&self) -> Size {
        const MARKS: u64 = !(GroupId::COUNT - 1) as u64;
        self.chunk_size() * (self.0 & MARKS)
    }

    pub fn size(&self) -> Size {
        self.chunk_size() * GroupId::COUNT as u64
    }

    pub fn plus_one(&self) -> Self {
        Self(self.0 + 1)
    }

    pub fn next(&mut self) {
        self.0 += 1
    }

    pub const fn inner(&self) -> u64 {
        self.0
    }
}

impl From<u64> for GroupId {
    fn from(value: u64) -> Self {
        Self(value)
    }
}

impl From<GroupId> for u64 {
    fn from(val: GroupId) -> Self {
        val.0
    }
}

impl std::ops::Deref for GroupId {
    type Target = u64;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl std::fmt::Debug for GroupId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "GroupId {{ chunk_size: {}, cluster: {}, group: {} }}",
            self.chunk_size(),
            self.cluster(),
            self.group(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_group_id_next() {
        let mut group_id = GroupId::default();

        for _ in 0..1000 {
            for i in 0..=255 {
                let next = group_id.plus_one();
                if i == 255 {
                    assert_eq!(group_id.chunk_size(), next.chunk_size());
                    assert_eq!(0, next.cluster());
                    assert_eq!(group_id.group() + 1, next.group());
                } else {
                    assert_eq!(group_id.chunk_size(), next.chunk_size());
                    assert_eq!(group_id.cluster() + 1, next.cluster());
                    assert_eq!(group_id.group(), next.group());
                }
                group_id = next;
            }
        }

        let value = u64::from(group_id);
        assert_eq!(value, group_id.0);
    }
}
