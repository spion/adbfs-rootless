use dashmap::DashMap;
use std::time::{Duration, Instant};

use crate::parse::FileMeta;

struct CacheEntry {
  meta: Option<FileMeta>,
  inserted_at: Instant,
}

pub struct MetadataCache {
  entries: DashMap<String, CacheEntry>,
  ttl: Duration,
}

impl MetadataCache {
  pub fn new(ttl: Duration) -> Self {
    Self {
      entries: DashMap::new(),
      ttl,
    }
  }

  pub fn get(&self, path: &str) -> Option<Option<FileMeta>> {
    let entry = self.entries.get(path)?;
    if entry.inserted_at.elapsed() > self.ttl {
      drop(entry);
      self.entries.remove(path);
      return None;
    }
    Some(entry.meta.clone())
  }

  pub fn insert(&self, path: String, meta: Option<FileMeta>) {
    self.entries.insert(
      path,
      CacheEntry {
        meta,
        inserted_at: Instant::now(),
      },
    );
  }

  pub fn invalidate(&self, path: &str) {
    self.entries.remove(path);
  }

  pub fn invalidate_prefix(&self, prefix: &str) {
    self.entries.retain(|key, _| !key.starts_with(prefix));
  }
}

#[cfg(test)]
mod tests {
  use super::*;
  use std::time::SystemTime;

  fn dummy_meta() -> FileMeta {
    FileMeta {
      mode: 0o100644,
      nlink: 1,
      uid: 0,
      gid: 0,
      size: 42,
      rdev: 0,
      mtime: SystemTime::UNIX_EPOCH,
      raw_line: String::new(),
    }
  }

  #[test]
  fn get_returns_none_for_missing() {
    let cache = MetadataCache::new(Duration::from_secs(30));
    assert!(cache.get("/nonexistent").is_none());
  }

  #[test]
  fn insert_then_get() {
    let cache = MetadataCache::new(Duration::from_secs(30));
    let meta = dummy_meta();
    cache.insert("/sdcard/file.txt".to_owned(), Some(meta.clone()));

    let got = cache
      .get("/sdcard/file.txt")
      .expect("should be present")
      .expect("should be positive");
    assert_eq!(got.size, meta.size);
    assert_eq!(got.mode, meta.mode);
  }

  #[test]
  fn negative_cache_hit() {
    let cache = MetadataCache::new(Duration::from_secs(30));
    cache.insert("/sdcard/missing".to_owned(), None);

    let result = cache.get("/sdcard/missing");
    assert!(matches!(result, Some(None)));
  }

  #[test]
  fn expired_entry_returns_none() {
    let cache = MetadataCache::new(Duration::from_millis(50));
    cache.insert("/tmp/ephemeral".to_owned(), Some(dummy_meta()));

    std::thread::sleep(Duration::from_millis(100));

    assert!(cache.get("/tmp/ephemeral").is_none());
  }

  #[test]
  fn invalidate_removes_entry() {
    let cache = MetadataCache::new(Duration::from_secs(30));
    cache.insert("/sdcard/remove_me".to_owned(), Some(dummy_meta()));

    cache.invalidate("/sdcard/remove_me");
    assert!(cache.get("/sdcard/remove_me").is_none());
  }

  #[test]
  fn invalidate_prefix_removes_children() {
    let cache = MetadataCache::new(Duration::from_secs(30));
    cache.insert("/sdcard/a".to_owned(), Some(dummy_meta()));
    cache.insert("/sdcard/b".to_owned(), Some(dummy_meta()));
    cache.insert("/other".to_owned(), Some(dummy_meta()));

    cache.invalidate_prefix("/sdcard");

    assert!(cache.get("/sdcard/a").is_none());
    assert!(cache.get("/sdcard/b").is_none());
    assert!(cache.get("/other").is_some());
  }
}
