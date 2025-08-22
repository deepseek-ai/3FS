use std::borrow::Borrow;
use std::collections::{hash_map::DefaultHasher, HashSet};
use std::hash::{Hash, Hasher};

pub struct ShardsSet<T, const S: usize = 64> {
    shards: [HashSet<T>; S],
}

pub struct ShardsSetIter<'a, T> {
    array_it: std::slice::Iter<'a, HashSet<T>>,
    inner_it: std::collections::hash_set::Iter<'a, T>,
}

impl<T, const S: usize> ShardsSet<T, S>
where
    T: Eq + Hash,
{
    pub fn new() -> Self {
        Self {
            shards: [(); S].map(|_| Default::default()),
        }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        let cap = (capacity / S).next_power_of_two();
        Self {
            shards: [(); S].map(|_| HashSet::with_capacity(cap)),
        }
    }

    fn shard<Q>(key: &Q) -> usize
    where
        T: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        let mut s = DefaultHasher::new();
        key.hash(&mut s);
        s.finish() as usize % S
    }

    pub fn contains<Q>(&self, value: &Q) -> bool
    where
        T: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.shards[Self::shard(value)].contains(value)
    }

    pub fn is_empty(&self) -> bool {
        self.shards.iter().all(|m| m.is_empty())
    }

    pub fn len(&self) -> usize {
        self.shards.iter().map(|m| m.len()).sum()
    }

    pub fn iter(&self) -> ShardsSetIter<'_, T> {
        ShardsSetIter {
            array_it: self.shards[1..].iter(),
            inner_it: self.shards[0].iter(),
        }
    }

    pub fn insert(&mut self, value: T) -> bool {
        self.shards[Self::shard(&value)].insert(value)
    }

    pub fn remove<Q>(&mut self, value: &Q) -> bool
    where
        T: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.shards[Self::shard(value)].remove(value)
    }
}

impl<T, const S: usize> Default for ShardsSet<T, S>
where
    T: Eq + Hash,
{
    fn default() -> Self {
        Self::new()
    }
}

impl<'a, T> Iterator for ShardsSetIter<'a, T> {
    type Item = &'a T;

    fn next(&mut self) -> Option<Self::Item> {
        loop {
            if let Some(value) = self.inner_it.next() {
                return Some(value);
            } else if let Some(map) = self.array_it.next() {
                self.inner_it = map.iter();
            } else {
                return None;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_shards_map() {
        let mut set = ShardsSet::<usize, 4>::with_capacity(1024);
        assert!(set.is_empty());
        assert_eq!(set.len(), 0);

        const N: usize = 1024;
        for i in 0..N {
            assert!(!set.contains(&i));
            assert!(set.insert(i));
        }
        assert!(!set.is_empty());
        assert_eq!(set.len(), N);

        for i in 0..N {
            assert!(set.contains(&i));
            assert!(set.remove(&i));
            assert!(!set.remove(&i));
        }

        assert!(ShardsSet::<usize>::default().is_empty());
    }
}
