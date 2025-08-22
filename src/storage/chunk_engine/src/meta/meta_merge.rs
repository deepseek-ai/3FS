use byteorder::{ByteOrder, LittleEndian};
use derse::{DownwardBytes, Serialize};

use super::super::{GroupState, MergeState, MetaKey};

pub struct MetaMergeOp;

impl super::MergeOp for MetaMergeOp {
    fn full_merge<'a>(
        key: &[u8],
        value: Option<&[u8]>,
        operands: impl Iterator<Item = &'a [u8]>,
    ) -> Option<Vec<u8>> {
        match key[0] {
            MetaKey::GROUP_BITS_KEY_PREFIX => {
                let mut merge_bits = MergeState::empty();
                for op in operands {
                    merge_bits.merge(&MergeState::from(op).ok()?);
                }

                let mut bits = if let Some(group_bits) = value {
                    GroupState::from(group_bits).ok()?
                } else {
                    GroupState::empty()
                };
                bits.update(&merge_bits);
                Some(Vec::from(bits.as_bytes()))
            }
            MetaKey::USED_SIZE_KEY_PREFIX => {
                let mut total = 0i64;
                for op in operands {
                    if op.len() != std::mem::size_of_val(&total) {
                        return None;
                    }
                    total += LittleEndian::read_i64(op);
                }
                if let Some(value) = value {
                    if value.len() != std::mem::size_of_val(&total) {
                        return None;
                    }
                    total += LittleEndian::read_i64(value);
                }
                let mut vec = Vec::with_capacity(std::mem::size_of_val(&total));
                vec.extend_from_slice(&total.to_le_bytes());
                Some(vec)
            }
            MetaKey::TEST_KEY_PREFIX => {
                let mut out = Vec::<u8>::new();
                if let Some(value) = value {
                    out.extend_from_slice(value);
                }
                for op in operands {
                    out.extend_from_slice(op);
                }
                Some(out)
            }
            _ => None,
        }
    }

    fn partial_merge<'a>(key: &[u8], operands: impl Iterator<Item = &'a [u8]>) -> Option<Vec<u8>> {
        match key[0] {
            MetaKey::GROUP_BITS_KEY_PREFIX => {
                let mut merge_bits = MergeState::empty();
                for op in operands {
                    merge_bits.merge(&MergeState::from(op).ok()?);
                }

                if let Ok(bytes) = merge_bits.serialize::<DownwardBytes>() {
                    Some(Vec::from(bytes.as_slice()))
                } else {
                    None
                }
            }
            MetaKey::USED_SIZE_KEY_PREFIX => {
                let mut total = 0i64;
                for op in operands {
                    if op.len() != std::mem::size_of_val(&total) {
                        return None;
                    }
                    total += LittleEndian::read_i64(op);
                }
                let mut vec = Vec::with_capacity(std::mem::size_of_val(&total));
                vec.extend_from_slice(&total.to_le_bytes());
                Some(vec)
            }
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::*;

    #[test]
    fn test_meta_merge_op() {
        let slice = [233u8].as_slice();
        assert_eq!(
            MetaMergeOp::partial_merge(&[233], vec![slice].into_iter()),
            None
        );
    }

    #[test]
    fn test_used_size_merge() {
        let mut ops = Vec::new();
        for i in 0..10 {
            let mut vec = Vec::with_capacity(std::mem::size_of::<i64>());
            vec.extend_from_slice(&(i as i64).to_le_bytes());
            ops.push(vec);
        }

        let merged = MetaMergeOp::partial_merge(
            &[MetaKey::USED_SIZE_KEY_PREFIX],
            ops.iter().map(|v| v.as_slice()),
        )
        .unwrap();
        assert_eq!(LittleEndian::read_i64(&merged), (0..10).sum::<i64>());

        // test full merge.
        let mut ops = Vec::new();
        for i in 0..10 {
            let mut vec = Vec::with_capacity(std::mem::size_of::<i64>());
            vec.extend_from_slice(&(i as i64).to_le_bytes());
            ops.push(vec);
        }

        let value = 10i64;
        let merged = MetaMergeOp::full_merge(
            &[MetaKey::USED_SIZE_KEY_PREFIX],
            Some(value.to_le_bytes().as_slice()),
            ops.iter().map(|v| v.as_slice()),
        )
        .unwrap();
        assert_eq!(
            LittleEndian::read_i64(&merged),
            (0..10).sum::<i64>() + value
        );

        // test invalid ops.
        let invalid_ops = [vec![1, 2, 3]];
        assert_eq!(
            MetaMergeOp::partial_merge(
                &[MetaKey::USED_SIZE_KEY_PREFIX],
                invalid_ops.iter().map(|v| v.as_slice()),
            ),
            None
        );
    }
}
