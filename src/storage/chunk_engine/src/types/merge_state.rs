use std::collections::HashSet;

use super::super::*;
use derse::Deserialize;

#[derive(Clone, Debug, Default, derse::Deserialize, derse::Serialize, PartialEq)]
pub struct MergeState {
    pub acquire: HashSet<u8>,
    pub release: HashSet<u8>,
}

impl MergeState {
    pub fn empty() -> Self {
        Self::default()
    }

    pub fn from(value: &[u8]) -> Result<Self> {
        Self::deserialize(value).map_err(Error::SerializationError)
    }

    pub fn acquire(pos: u8) -> Self {
        let mut b = Self::empty();
        b.acquire.insert(pos);
        b
    }

    pub fn release(pos: u8) -> Self {
        let mut b = Self::empty();
        b.release.insert(pos);
        b
    }

    pub fn merge(&mut self, right: &Self) {
        for pos in &right.acquire {
            self.acquire.insert(*pos);
            self.release.remove(pos);
        }
        for pos in &right.release {
            self.acquire.remove(pos);
            self.release.insert(*pos);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_merge_bits() {
        fn group_bits_apply(mut bits: GroupState, merge_bits: &MergeState) -> GroupState {
            bits.update(merge_bits);
            bits
        }

        let state = GroupState::empty();

        assert_eq!(group_bits_apply(state, &MergeState::empty()), state);

        let acquire_first_bit = MergeState::acquire(0);
        let state_after_acquire = group_bits_apply(state, &acquire_first_bit);
        assert_eq!(state_after_acquire.as_bytes()[0], 1);
        assert_eq!(state_after_acquire.as_bytes()[1..], state.as_bytes()[1..]);

        let release_first_bit = MergeState::release(0);
        let state_after_release = group_bits_apply(state_after_acquire, &release_first_bit);
        assert_eq!(state_after_release, state);

        let mut merge_bits = acquire_first_bit;
        merge_bits.merge(&release_first_bit);
        assert_eq!(merge_bits, release_first_bit);
        assert_eq!(state, group_bits_apply(state, &merge_bits));

        let mut merge_bits = MergeState::empty();
        for i in 0..=255 {
            merge_bits.merge(&MergeState::acquire(i));
        }
        let full_state = group_bits_apply(state, &merge_bits);
        assert!(full_state.is_full());

        for i in 0..=255 {
            merge_bits.merge(&MergeState::release(i));
        }
        let empty_state = group_bits_apply(full_state, &merge_bits);
        assert_eq!(empty_state, state);

        assert!(MergeState::from(&[]).is_err());
    }
}
