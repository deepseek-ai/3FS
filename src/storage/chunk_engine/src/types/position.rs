use super::super::*;

use derse::{Deserialize, Deserializer, Serialize, Serializer};

#[derive(Copy, Clone, Eq, PartialEq, Hash, PartialOrd, Ord)]
#[repr(C)]
pub struct Position(pub u64);

impl Position {
    const SHIFT: u32 = 8;

    // 24bit chunk size + 8bit cluster + 24bit group + 8bit zero
    pub const fn new(group_id: GroupId, index: u8) -> Self {
        const CLEAN: u64 = !((GroupId::COUNT - 1) as u64);
        Self(group_id.inner() & CLEAN | index as u64 | (group_id.cluster() as u64) << 32)
    }

    pub fn group_id(&self) -> GroupId {
        const MARKS: u64 = (GroupId::COUNT - 1) as u64;
        const CLEAN: u64 = !(MARKS | MARKS << 32);
        GroupId::from(self.0 & CLEAN | self.cluster() as u64)
    }

    pub fn chunk_size(&self) -> Size {
        Size::new(self.0 >> 40 << 8)
    }

    pub fn cluster(&self) -> u8 {
        (self.0 >> 32) as u8
    }

    pub fn group(&self) -> u32 {
        (self.0 as u32) >> Self::SHIFT
    }

    pub fn index(&self) -> u8 {
        self.0 as u8
    }

    pub fn offset(&self) -> Size {
        self.chunk_size() * self.0 as u32 as u64
    }
}

impl Default for Position {
    fn default() -> Self {
        Position::new(GroupId::default(), 0)
    }
}

impl From<u64> for Position {
    fn from(value: u64) -> Self {
        Self(value)
    }
}

impl std::ops::Deref for Position {
    type Target = u64;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl std::fmt::Debug for Position {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Position {{ chunk_size: {}, cluster: {}, group: {}, index: {} }}",
            self.chunk_size(),
            self.cluster(),
            self.group(),
            self.index(),
        )
    }
}

impl Serialize for Position {
    fn serialize_to<T: Serializer>(&self, serializer: &mut T) -> derse::Result<()> {
        self.0.serialize_to(serializer)
    }
}

impl<'a> Deserialize<'a> for Position {
    fn deserialize_from<T: Deserializer<'a>>(buf: &mut T) -> derse::Result<Self> {
        Ok(Self(u64::deserialize_from(buf)?))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_group_id_and_position() {
        let group_id = GroupId::new(64 * Size::KB, 23, 233);
        assert_eq!(group_id.chunk_size(), 64 * Size::KB);
        assert_eq!(group_id.cluster(), 23);
        assert_eq!(group_id.group(), 233);
        assert_eq!(
            format!("{:?}", group_id),
            "GroupId { chunk_size: 64KiB, cluster: 23, group: 233 }"
        );

        let position = Position::new(group_id, 223);
        assert_eq!(position.chunk_size(), 64 * Size::KB);
        assert_eq!(position.cluster(), 23);
        assert_eq!(position.group(), 233);
        assert_eq!(position.index(), 223);
        assert_eq!(position.group_id(), group_id);
        assert_eq!(position.to_be_bytes().len(), 8);
        assert_eq!(
            format!("{:?}", position),
            "Position { chunk_size: 64KiB, cluster: 23, group: 233, index: 223 }"
        );
    }
}
