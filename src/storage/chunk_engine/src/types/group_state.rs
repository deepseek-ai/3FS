use super::super::*;
use std::num::NonZeroU64;

type Item = u64;
type Bits = [Item; 4];

#[derive(Debug, PartialEq, Copy, Clone)]
pub struct GroupState {
    bits: Bits,
    count: u32,
}

impl GroupState {
    const TOTAL_BYTES: usize = std::mem::size_of::<Bits>();
    pub const TOTAL_BITS: usize = 8 * Self::TOTAL_BYTES;
    pub const ITEM_BITS: u8 = 8 * std::mem::size_of::<Item>() as u8;
    pub const LEN: usize = Self::TOTAL_BYTES / std::mem::size_of::<Item>();
    pub const LEVELS: usize = 4;

    pub fn from(value: &[u8]) -> Result<Self> {
        let mut out = Self::empty();
        if value.len() != Self::TOTAL_BYTES {
            return Err(Error::MetaError(format!(
                "group state load bytes {} != {}",
                value.len(),
                Self::TOTAL_BYTES
            )));
        }
        out.as_mut_bytes().copy_from_slice(value);
        out.count = out.bits.iter().map(|b| b.count_ones()).sum();
        Ok(out)
    }

    pub const fn empty() -> Self {
        Self {
            bits: [0; Self::LEN],
            count: 0,
        }
    }

    pub fn full() -> Self {
        Self {
            bits: [!0; Self::LEN],
            count: Self::TOTAL_BITS as u32,
        }
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    pub fn is_full(&self) -> bool {
        self.count == Self::TOTAL_BITS as u32
    }

    pub fn allocate(&mut self) -> Option<u8> {
        for (i, v) in self.bits.iter_mut().enumerate() {
            if let Some(mark) = NonZeroU64::new(!*v) {
                let idx = mark.trailing_zeros();
                *v |= 1 << idx;
                self.count += 1;
                return Some(i as u8 * Self::ITEM_BITS + idx as u8);
            } else {
                continue;
            }
        }
        None
    }

    pub fn count(&self) -> u32 {
        self.count
    }

    pub fn level(&self) -> u32 {
        self.count() / (Self::TOTAL_BITS / Self::LEVELS) as u32
    }

    pub fn check(&self, index: u8) -> bool {
        let x = index / Self::ITEM_BITS;
        let y = index % Self::ITEM_BITS;
        self.bits[x as usize] & (1 << y) != 0
    }

    pub fn deallocate(&mut self, index: u8) -> Result<()> {
        let x = index / Self::ITEM_BITS;
        let y = index % Self::ITEM_BITS;
        let mark = &mut self.bits[x as usize];
        if *mark & (1 << y) != 0 {
            *mark ^= 1 << y;
            self.count -= 1;
            Ok(())
        } else {
            Err(Error::MetaError(format!(
                "group state deallocate fail: index {}",
                index
            )))
        }
    }

    pub fn update(&mut self, merge_bits: &MergeState) {
        for pos in &merge_bits.acquire {
            let x = pos / Self::ITEM_BITS;
            let y = pos % Self::ITEM_BITS;
            self.bits[x as usize] |= 1 << y;
        }
        for pos in &merge_bits.release {
            let x = pos / Self::ITEM_BITS;
            let y = pos % Self::ITEM_BITS;
            self.bits[x as usize] &= !(1 << y);
        }
        self.count = self.bits.iter().map(|b| b.count_ones()).sum();
    }

    pub fn as_bytes(&self) -> &[u8; Self::TOTAL_BYTES] {
        unsafe { std::mem::transmute(&self.bits) }
    }

    pub fn as_mut_bytes(&mut self) -> &mut [u8; Self::TOTAL_BYTES] {
        unsafe { std::mem::transmute(&mut self.bits) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_group_bits_normal() {
        use rand::seq::SliceRandom;

        let mut group_state = GroupState::empty();
        assert_eq!(group_state.count(), 0);

        for i in 0..=255 {
            assert_eq!(i, group_state.allocate().unwrap());
        }
        assert!(group_state.allocate().is_none());
        assert_eq!(group_state.count(), 256);

        let mut vec = (0..=255).collect::<Vec<u8>>();
        vec.shuffle(&mut rand::thread_rng());
        for i in vec {
            group_state.deallocate(i).unwrap();
            group_state.deallocate(i).unwrap_err();

            let j = group_state.allocate().unwrap();
            group_state.deallocate(j).unwrap();
            group_state.deallocate(j).unwrap_err();
        }
        assert_eq!(group_state.count(), 0);

        group_state.allocate().unwrap();
        group_state.allocate().unwrap();
        group_state.deallocate(0).unwrap();
        assert!(group_state.check(1));

        let bytes = group_state.as_bytes();
        let another_state = GroupState::from(bytes).unwrap();
        assert_eq!(another_state, group_state);

        assert!(GroupState::from(&bytes[1..]).is_err());
    }
}
