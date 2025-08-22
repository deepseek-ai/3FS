use std::borrow::Borrow;
use std::collections::{
    hash_map::{DefaultHasher, Entry},
    HashMap,
};
use std::hash::{Hash, Hasher};

pub struct ShardsMap<K, V, const S: usize = 64> {
    shards: [HashMap<K, V>; S],
}

pub struct ShardsMapIter<'a, K, V> {
    array_it: std::slice::Iter<'a, HashMap<K, V>>,
    inner_it: std::collections::hash_map::Iter<'a, K, V>,
}

impl<K, V, const S: usize> ShardsMap<K, V, S>
where
    K: Eq + Hash,
{
    pub fn new() -> Self {
        Self {
            shards: [(); S].map(|_| Default::default()),
        }
    }

    pub fn with_capacity(capacity: usize) -> Self {
        let cap = (capacity / S).next_power_of_two();
        Self {
            shards: [(); S].map(|_| HashMap::with_capacity(cap)),
        }
    }

    fn shard<Q>(key: &Q) -> usize
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        let mut s = DefaultHasher::new();
        key.hash(&mut s);
        s.finish() as usize % S
    }

    pub fn get<Q>(&self, k: &Q) -> Option<&V>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.shards[Self::shard(k)].get(k)
    }

    pub fn get_mut<Q>(&mut self, k: &Q) -> Option<&mut V>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.shards[Self::shard(k)].get_mut(k)
    }

    pub fn is_empty(&self) -> bool {
        self.shards.iter().all(|m| m.is_empty())
    }

    pub fn len(&self) -> usize {
        self.shards.iter().map(|m| m.len()).sum()
    }

    pub fn iter(&self) -> ShardsMapIter<'_, K, V> {
        ShardsMapIter {
            array_it: self.shards[1..].iter(),
            inner_it: self.shards[0].iter(),
        }
    }

    pub fn insert(&mut self, k: K, v: V) -> Option<V> {
        self.shards[Self::shard(&k)].insert(k, v)
    }

    pub fn entry(&mut self, k: K) -> Entry<'_, K, V> {
        self.shards[Self::shard(&k)].entry(k)
    }

    pub fn remove<Q>(&mut self, k: &Q) -> Option<V>
    where
        K: Borrow<Q>,
        Q: Eq + Hash + ?Sized,
    {
        self.shards[Self::shard(k)].remove(k)
    }
}

impl<K, V, const S: usize> Default for ShardsMap<K, V, S>
where
    K: Eq + Hash,
{
    fn default() -> Self {
        Self::new()
    }
}

impl<'a, K, V> Iterator for ShardsMapIter<'a, K, V> {
    type Item = (&'a K, &'a V);

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
        let mut map = ShardsMap::<usize, usize, 4>::with_capacity(1024);
        assert!(map.is_empty());
        assert_eq!(map.len(), 0);

        const N: usize = 1024;
        for i in 0..N {
            assert!(map.get(&i).is_none());
            map.insert(i, i * i);
        }
        assert!(!map.is_empty());
        assert_eq!(map.len(), N);

        assert_eq!(
            map.iter()
                .map(|(k, v)| {
                    assert_eq!(k * k, *v);
                })
                .count(),
            N
        );

        for i in 0..N {
            let value = map.get_mut(&i).unwrap();
            assert_eq!(i * i, *value);
            map.entry(i).and_modify(|v| *v += 1);
            assert_eq!(map.remove(&i).unwrap(), i * i + 1);
        }

        assert!(ShardsMap::<usize, usize, 4>::default().is_empty());
    }
}
