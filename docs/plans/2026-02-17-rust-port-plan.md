# adbfs-rootless Rust Port Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Port the C++ adbfs-rootless FUSE filesystem to Rust with improved safety, testability, and maintainability.

**Architecture:** Three layers — `AdbFs` (thin FUSE callbacks + cache) → `DeviceOps` (device interaction + compat strategies) → `dyn AdbDevice` (async CLI transport). See `docs/plans/2026-02-17-rust-port-design.md` for full design.

**Tech Stack:** fuser, tokio, dashmap, clap, thiserror, color_eyre, tracing, async-trait, tempfile, proptest

---

### Task 1: Project Scaffolding

**Files:**
- Create: `src/main.rs`
- Create: `Cargo.toml`

**Step 1: Initialize the Rust project**

Run: `cargo init --name adbfs` in the project root.

This will create `src/main.rs` and `Cargo.toml`.

**Step 2: Add dependencies**

Run:
```bash
cargo add fuser tokio --features rt-multi-thread,process,macros,fs \
  dashmap clap --features derive \
  thiserror color-eyre tracing tracing-subscriber --features env-filter \
  async-trait tempfile
cargo add --dev proptest
```

**Step 3: Write minimal main.rs**

```rust
use clap::Parser;
use color_eyre::eyre::Result;
use std::path::PathBuf;

#[derive(Parser)]
#[command(name = "adbfs", about = "Mount Android device filesystem via ADB")]
struct Cli {
    /// Mount point
    mountpoint: PathBuf,

    /// Trigger Android media rescan on file changes
    #[arg(long)]
    rescan: bool,

    /// Cache TTL in seconds
    #[arg(long, default_value = "30")]
    cache_ttl: u64,

    /// Additional FUSE mount options (passed through to fuser)
    #[arg(short = 'o', value_delimiter = ',')]
    options: Vec<String>,
}

fn main() -> Result<()> {
    color_eyre::install()?;
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "adbfs=warn".parse().unwrap()),
        )
        .init();

    let cli = Cli::parse();
    println!("Would mount at {:?}", cli.mountpoint);
    Ok(())
}
```

**Step 4: Verify it compiles and runs**

Run: `cargo build`
Run: `cargo run -- /tmp/test-mount --help`
Expected: help output with mountpoint, --rescan, --cache-ttl, -o options.

**Step 5: Commit**

```
feat: scaffold Rust project with CLI and dependencies
```

---

### Task 2: Shell Escaping (`escape.rs`)

**Files:**
- Create: `src/escape.rs`
- Modify: `src/main.rs` (add `mod escape;`)

**Step 1: Write tests for shell_escape_path**

Port the C++ `shell_escape_path` logic. It escapes `'` → `'\''` and `"` → `\"`.

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn escape_path_single_quote() {
        assert_eq!(shell_escape_path("it's"), "it'\\''s");
    }

    #[test]
    fn escape_path_double_quote() {
        assert_eq!(shell_escape_path("say \"hi\""), "say \\\"hi\\\"");
    }

    #[test]
    fn escape_path_no_special() {
        assert_eq!(shell_escape_path("/sdcard/DCIM"), "/sdcard/DCIM");
    }

    #[test]
    fn escape_path_both_quotes() {
        assert_eq!(shell_escape_path("it's \"fine\""), "it'\\''s \\\"fine\\\"");
    }
}
```

**Step 2: Run tests to verify they fail**

Run: `cargo test -p adbfs escape`
Expected: compilation error, module doesn't exist yet.

**Step 3: Implement escape.rs**

```rust
/// Escape a path for use inside single-quoted shell arguments.
/// Ported from C++ shell_escape_path: escapes ' and ".
pub fn shell_escape_path(path: &str) -> String {
    let mut result = path.replace('\'', "'\\''");
    result = result.replace('"', "\\\"");
    result
}

/// Escape a command string for local shell execution.
/// Ported from C++ shell_escape_command: escapes \, ', `.
pub fn shell_escape_command(cmd: &str) -> String {
    let mut result = cmd.replace('\\', "\\\\");
    result = result.replace('\'', "\\'");
    result = result.replace('`', "\\`");
    result
}

/// Escape a command for adb shell (double-shell layer).
/// Ported from C++ adb_shell_escape_command.
pub fn adb_shell_escape_command(cmd: &str) -> String {
    let mut result = cmd.replace('\\', "\\\\");
    for ch in ['(', ')', '\'', '`', '|', '&', ';', '<', '>', '*', '#', '%', '=', '~'] {
        result = result.replace(ch, &format!("\\{ch}"));
    }
    // Strip ANSI color codes (Android ls colorization)
    for code in ["/[0;0m", "/[1;32m", "/[1;34m", "/[1;36m"] {
        result = result.replace(code, "");
    }
    result
}
```

**Step 4: Add more tests including property-based**

```rust
    #[test]
    fn escape_command_backslash() {
        assert_eq!(shell_escape_command("a\\b"), "a\\\\b");
    }

    #[test]
    fn adb_escape_special_chars() {
        assert_eq!(adb_shell_escape_command("a|b&c;d"), "a\\|b\\&c\\;d");
    }

    #[test]
    fn adb_escape_strips_ansi() {
        assert_eq!(adb_shell_escape_command("file/[1;32m"), "file");
    }
```

Add proptest:

```rust
    use proptest::prelude::*;

    proptest! {
        #[test]
        fn escape_path_never_contains_unescaped_single_quote(s in "[a-zA-Z0-9 '/\"._-]{0,100}") {
            let escaped = shell_escape_path(&s);
            // After escaping, splitting on '\'' boundaries should not
            // produce empty unmatched quotes
            assert!(!escaped.contains("'''")); // would mean unescaped '
        }
    }
```

**Step 5: Run tests**

Run: `cargo test escape`
Expected: all pass.

**Step 6: Commit**

```
feat: add shell escaping module ported from C++
```

---

### Task 3: `ls` Output Parsing (`parse.rs`)

**Files:**
- Create: `src/parse.rs`
- Modify: `src/main.rs` (add `mod parse;`)

This is the most critical module — ported directly from the C++ `strmode_to_rawmode`, `is_valid_ls_output`, `adb_getattr` parsing logic, `find_nth`, and filename extraction from `readdir`.

**Step 1: Write tests for mode string parsing**

```rust
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_regular_file_mode() {
        let mode = parse_mode_string("-rw-r--r--");
        assert_eq!(mode & libc::S_IFREG as u32, libc::S_IFREG as u32);
        assert_eq!(mode & 0o777, 0o644);
    }

    #[test]
    fn parse_directory_mode() {
        let mode = parse_mode_string("drwxr-xr-x");
        assert_eq!(mode & libc::S_IFDIR as u32, libc::S_IFDIR as u32);
        assert_eq!(mode & 0o777, 0o755);
    }

    #[test]
    fn parse_symlink_mode() {
        let mode = parse_mode_string("lrwxrwxrwx");
        assert_eq!(mode & libc::S_IFLNK as u32, libc::S_IFLNK as u32);
    }

    #[test]
    fn parse_setuid() {
        let mode = parse_mode_string("-rwsr-xr-x");
        assert!(mode & libc::S_ISUID as u32 != 0);
        assert!(mode & libc::S_IXUSR as u32 != 0);
    }

    #[test]
    fn parse_setuid_no_exec() {
        let mode = parse_mode_string("-rwSr-xr-x");
        assert!(mode & libc::S_ISUID as u32 != 0);
        assert_eq!(mode & libc::S_IXUSR as u32, 0);
    }

    #[test]
    fn parse_sticky() {
        let mode = parse_mode_string("drwxrwxrwt");
        assert!(mode & libc::S_ISVTX as u32 != 0);
        assert!(mode & libc::S_IXOTH as u32 != 0);
    }

    #[test]
    fn parse_sticky_no_exec() {
        let mode = parse_mode_string("drwxrwxrwT");
        assert!(mode & libc::S_ISVTX as u32 != 0);
        assert_eq!(mode & libc::S_IXOTH as u32, 0);
    }
}
```

**Step 2: Run tests to verify they fail**

Run: `cargo test parse`
Expected: compilation error.

**Step 3: Implement parse_mode_string**

```rust
use std::time::SystemTime;

/// Parsed file metadata from ls -l output.
#[derive(Debug, Clone)]
pub struct FileMeta {
    pub mode: u32,
    pub nlink: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: u64,
    pub rdev: u64,
    pub mtime: SystemTime,
    pub raw_line: String,
}

/// Parse a 10-character mode string (e.g. "drwxr-xr-x") into raw mode bits.
/// Ported from C++ strmode_to_rawmode.
pub fn parse_mode_string(s: &str) -> u32 {
    let b = s.as_bytes();
    let mut mode: u32 = 0;

    mode |= match b[0] {
        b's' => libc::S_IFSOCK,
        b'l' => libc::S_IFLNK,
        b'-' => libc::S_IFREG,
        b'd' => libc::S_IFDIR,
        b'b' => libc::S_IFBLK,
        b'c' => libc::S_IFCHR,
        b'p' => libc::S_IFIFO,
        _ => 0,
    } as u32;

    if b[1] == b'r' { mode |= libc::S_IRUSR as u32; }
    if b[2] == b'w' { mode |= libc::S_IWUSR as u32; }
    match b[3] {
        b'x' => mode |= libc::S_IXUSR as u32,
        b's' => mode |= (libc::S_ISUID | libc::S_IXUSR) as u32,
        b'S' => mode |= libc::S_ISUID as u32,
        _ => {}
    }

    if b[4] == b'r' { mode |= libc::S_IRGRP as u32; }
    if b[5] == b'w' { mode |= libc::S_IWGRP as u32; }
    match b[6] {
        b'x' => mode |= libc::S_IXGRP as u32,
        b's' => mode |= (libc::S_ISGID | libc::S_IXGRP) as u32,
        b'S' => mode |= libc::S_ISGID as u32,
        _ => {}
    }

    if b[7] == b'r' { mode |= libc::S_IROTH as u32; }
    if b[8] == b'w' { mode |= libc::S_IWOTH as u32; }
    match b[9] {
        b'x' => mode |= libc::S_IXOTH as u32,
        b't' => mode |= (libc::S_ISVTX | libc::S_IXOTH) as u32,
        b'T' => mode |= libc::S_ISVTX as u32,
        _ => {}
    }

    mode
}
```

Note: add `libc` as a dependency (`cargo add libc`).

**Step 4: Run tests**

Run: `cargo test parse`
Expected: all pass.

**Step 5: Write tests for is_valid_ls_output**

```rust
    #[test]
    fn valid_ls_regular_file() {
        assert!(is_valid_ls_output("-rw-r--r-- root root 1234 2024-01-01 12:00 file.txt"));
    }

    #[test]
    fn invalid_ls_error_message() {
        assert!(!is_valid_ls_output("/sdcard/nofile: No such file or directory"));
    }

    #[test]
    fn invalid_ls_stat_error() {
        assert!(!is_valid_ls_output("lstat '/efs' failed: Permission denied"));
    }
```

**Step 6: Implement is_valid_ls_output**

```rust
/// Heuristic to determine whether ls output represents an actual file.
/// Ported from C++ is_valid_ls_output.
pub fn is_valid_ls_output(line: &str) -> bool {
    let b = line.as_bytes();
    if b.is_empty() || b.len() < 2 {
        return false;
    }
    if b[0] == b'/' {
        return false;
    }
    if b[1] != b'r' && b[1] != b'-' {
        return false;
    }
    true
}
```

**Step 7: Write tests for parse_ls_line**

Test with real Android ls output formats:

```rust
    #[test]
    fn parse_ls_regular_file() {
        let line = "-rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html";
        let meta = parse_ls_line(line).unwrap();
        assert_eq!(meta.mode & libc::S_IFREG as u32, libc::S_IFREG as u32);
        assert_eq!(meta.size, 763362);
    }

    #[test]
    fn parse_ls_with_nlink() {
        let line = "-rw-r--r--   1 root   root      5905 1970-01-01 01:00 ueventd.rc";
        let meta = parse_ls_line(line).unwrap();
        assert_eq!(meta.nlink, 1);
        assert_eq!(meta.size, 5905);
    }

    #[test]
    fn parse_ls_directory() {
        let line = "drwxr-xr-x root     root              2024-01-15 10:30 sdcard";
        let meta = parse_ls_line(line).unwrap();
        assert_eq!(meta.mode & libc::S_IFDIR as u32, libc::S_IFDIR as u32);
        assert_eq!(meta.size, 0);
    }

    #[test]
    fn parse_ls_symlink() {
        let line = "lrwxrwxrwx root     root              2024-01-15 10:30 link -> /target";
        let meta = parse_ls_line(line).unwrap();
        assert_eq!(meta.mode & libc::S_IFLNK as u32, libc::S_IFLNK as u32);
    }

    #[test]
    fn parse_ls_char_device() {
        let line = "crw-rw---- root     root      10,  47 1970-01-01 01:00 hidraw0";
        let meta = parse_ls_line(line).unwrap();
        assert_eq!(meta.mode & libc::S_IFCHR as u32, libc::S_IFCHR as u32);
        assert!(meta.rdev > 0);
    }
```

**Step 8: Implement parse_ls_line**

This is the most complex parser — must handle the two ls formats (with and without nlink field), different file types having different field layouts (block/char devices have major,minor instead of size), and date parsing.

```rust
use std::time::{Duration, UNIX_EPOCH};

/// Parse a single line of `ls -l -a [-d]` output into FileMeta.
/// Handles two formats:
///   -rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html
///   -rw-r--r--   1 root   root      5905 1970-01-01 01:00 ueventd.rc
pub fn parse_ls_line(line: &str) -> Option<FileMeta> {
    if !is_valid_ls_output(line) {
        return None;
    }

    let fields: Vec<&str> = line.split_whitespace().collect();
    if fields.len() < 6 {
        return None;
    }

    let mode = parse_mode_string(fields[0]);

    // Detect whether field[1] is nlink (numeric) or owner (string)
    let (uid_offset, nlink) = match fields[1].parse::<u32>() {
        Ok(n) if n > 0 => (1, n),
        _ => (0, 1),
    };

    let owner = fields[uid_offset + 1];
    let group = fields[uid_offset + 2];
    let uid = uid_to_u32(owner);
    let gid = gid_to_u32(group);

    let file_type = mode & libc::S_IFMT as u32;

    let (size, rdev, date_idx) = match file_type {
        x if x == libc::S_IFBLK as u32 || x == libc::S_IFCHR as u32 => {
            // major, minor fields instead of size
            let major: u64 = fields[uid_offset + 3].trim_end_matches(',')
                .parse().unwrap_or(0);
            let minor: u64 = fields[uid_offset + 4].parse().unwrap_or(0);
            (0u64, major * 256 + minor, uid_offset + 5)
        }
        x if x == libc::S_IFREG as u32 => {
            let sz: u64 = fields[uid_offset + 3].parse().unwrap_or(0);
            (sz, 0u64, uid_offset + 4)
        }
        _ => {
            // dirs, symlinks, sockets, fifos: may or may not have a size field
            let mut idx = uid_offset + 3;
            // If the field doesn't contain a dash, it might be a size — skip it
            if !fields[idx].contains('-') {
                idx += 1;
            }
            (0u64, 0u64, idx)
        }
    };

    let mtime = parse_ls_date(fields.get(date_idx).copied()?,
                               fields.get(date_idx + 1).copied()?)
        .unwrap_or(UNIX_EPOCH);

    Some(FileMeta {
        mode,
        nlink,
        uid,
        gid,
        size,
        rdev,
        mtime,
        raw_line: line.to_string(),
    })
}

fn parse_ls_date(date: &str, time: &str) -> Option<SystemTime> {
    // date = "2024-01-15", time = "10:30"
    let ymd: Vec<&str> = date.split('-').collect();
    let hm: Vec<&str> = time.split(':').collect();
    if ymd.len() < 3 || hm.len() < 2 {
        return None;
    }
    let year: i32 = ymd[0].parse().ok()?;
    let month: u32 = ymd[1].parse().ok()?;
    let day: u32 = ymd[2].parse().ok()?;
    let hour: u32 = hm[0].parse().ok()?;
    let min: u32 = hm[1].parse().ok()?;

    // Convert to timestamp using libc mktime
    let mut tm = libc::tm {
        tm_sec: 0,
        tm_min: min as i32,
        tm_hour: hour as i32,
        tm_mday: day as i32,
        tm_mon: month as i32 - 1,
        tm_year: year - 1900,
        tm_wday: 0,
        tm_yday: 0,
        tm_isdst: -1,
        tm_gmtoff: 0,
        tm_zone: std::ptr::null(),
    };
    let epoch = unsafe { libc::mktime(&mut tm) };
    if epoch == -1 {
        return None;
    }
    Some(UNIX_EPOCH + Duration::from_secs(epoch as u64))
}

fn uid_to_u32(name: &str) -> u32 {
    // Try to resolve username to uid, fallback to 98
    unsafe {
        let c_name = std::ffi::CString::new(name).ok();
        c_name
            .and_then(|n| {
                let pw = libc::getpwnam(n.as_ptr());
                if pw.is_null() { None } else { Some((*pw).pw_uid) }
            })
            .unwrap_or(98)
    }
}

fn gid_to_u32(name: &str) -> u32 {
    unsafe {
        let c_name = std::ffi::CString::new(name).ok();
        c_name
            .and_then(|n| {
                let gr = libc::getgrnam(n.as_ptr());
                if gr.is_null() { None } else { Some((*gr).gr_gid) }
            })
            .unwrap_or(98)
    }
}
```

**Step 9: Write tests and implement extract_filename and parse_symlink_target**

```rust
/// Extract the filename from an ls -l line.
/// Filename starts after the time field (HH:MM + space).
pub fn extract_filename(line: &str) -> Option<&str> {
    // Find the colon in the time field, skip to +4 chars past it
    let colon_pos = line.find(':')?;
    let name_start = colon_pos + 4;
    if name_start >= line.len() {
        return None;
    }
    let name = &line[name_start..];
    // Strip symlink target if present
    Some(name.split(" -> ").next().unwrap_or(name))
}

/// Parse the symlink target from an ls -l line and convert absolute
/// paths to relative using slash-counting. Ported from C++ adb_readlink.
pub fn parse_symlink_target(ls_line: &str, num_slashes_in_path: usize) -> Option<String> {
    let pos = ls_line.find(" -> ")?;
    let target = &ls_line[pos + 4..];

    if target.starts_with('/') {
        // Convert absolute symlink to relative
        let target_trimmed = target.trim_start_matches('/');
        let parent_count = if num_slashes_in_path >= 1 {
            num_slashes_in_path - 1
        } else {
            0
        };
        let mut result = "../".repeat(parent_count);
        result.push_str(target_trimmed);
        Some(result)
    } else {
        Some(target.to_string())
    }
}
```

Tests:
```rust
    #[test]
    fn extract_filename_regular() {
        let line = "-rw-r--r-- root root 1234 2024-01-01 12:00 file.txt";
        assert_eq!(extract_filename(line), Some("file.txt"));
    }

    #[test]
    fn extract_filename_symlink() {
        let line = "lrwxrwxrwx root root 2024-01-01 12:00 link -> /target";
        assert_eq!(extract_filename(line), Some("link"));
    }

    #[test]
    fn parse_symlink_absolute_to_relative() {
        let line = "lrwxrwxrwx root root 2024-01-01 12:00 link -> /system/lib/libc.so";
        // path = "/vendor/lib/link" has 3 slashes, so num_slashes - 1 = 2
        let target = parse_symlink_target(line, 3).unwrap();
        assert_eq!(target, "../../system/lib/libc.so");
    }

    #[test]
    fn parse_symlink_relative() {
        let line = "lrwxrwxrwx root root 2024-01-01 12:00 link -> ../other";
        let target = parse_symlink_target(line, 2).unwrap();
        assert_eq!(target, "../other");
    }
```

**Step 10: Add proptest for mode string round-trip**

```rust
    use proptest::prelude::*;

    fn arb_mode_char(set: &'static [u8]) -> impl Strategy<Value = char> {
        prop::sample::select(set).prop_map(|b| b as char)
    }

    prop_compose! {
        fn arb_mode_string()(
            ftype in arb_mode_char(b"-dlcbps"),
            ur in arb_mode_char(b"r-"),
            uw in arb_mode_char(b"w-"),
            ux in arb_mode_char(b"xsS-"),
            gr in arb_mode_char(b"r-"),
            gw in arb_mode_char(b"w-"),
            gx in arb_mode_char(b"xsS-"),
            or in arb_mode_char(b"r-"),
            ow in arb_mode_char(b"w-"),
            ox in arb_mode_char(b"xtT-"),
        ) -> String {
            format!("{ftype}{ur}{uw}{ux}{gr}{gw}{gx}{or}{ow}{ox}")
        }
    }

    proptest! {
        #[test]
        fn mode_string_always_parses(s in arb_mode_string()) {
            let mode = parse_mode_string(&s);
            // File type should always be set
            assert!(mode & libc::S_IFMT as u32 != 0);
        }
    }
```

**Step 11: Run all tests**

Run: `cargo test parse`
Expected: all pass.

**Step 12: Commit**

```
feat: add ls output parsing module ported from C++
```

---

### Task 4: Metadata Cache (`cache.rs`)

**Files:**
- Create: `src/cache.rs`
- Modify: `src/main.rs` (add `mod cache;`)

**Step 1: Write tests**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use std::thread::sleep;
    use std::time::Duration;

    fn dummy_meta() -> FileMeta {
        FileMeta {
            mode: 0o100644,
            nlink: 1,
            uid: 0,
            gid: 0,
            size: 1234,
            rdev: 0,
            mtime: SystemTime::now(),
            raw_line: "test".to_string(),
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
        cache.insert("/test".to_string(), dummy_meta());
        assert!(cache.get("/test").is_some());
    }

    #[test]
    fn expired_entry_returns_none() {
        let cache = MetadataCache::new(Duration::from_millis(50));
        cache.insert("/test".to_string(), dummy_meta());
        sleep(Duration::from_millis(100));
        assert!(cache.get("/test").is_none());
    }

    #[test]
    fn invalidate_removes_entry() {
        let cache = MetadataCache::new(Duration::from_secs(30));
        cache.insert("/test".to_string(), dummy_meta());
        cache.invalidate("/test");
        assert!(cache.get("/test").is_none());
    }

    #[test]
    fn invalidate_prefix_removes_children() {
        let cache = MetadataCache::new(Duration::from_secs(30));
        cache.insert("/sdcard/a".to_string(), dummy_meta());
        cache.insert("/sdcard/b".to_string(), dummy_meta());
        cache.insert("/other".to_string(), dummy_meta());
        cache.invalidate_prefix("/sdcard");
        assert!(cache.get("/sdcard/a").is_none());
        assert!(cache.get("/sdcard/b").is_none());
        assert!(cache.get("/other").is_some());
    }
}
```

**Step 2: Run tests to verify they fail**

Run: `cargo test cache`
Expected: compilation error.

**Step 3: Implement cache.rs**

```rust
use dashmap::DashMap;
use std::time::{Duration, Instant};

use crate::parse::FileMeta;

struct CacheEntry {
    meta: FileMeta,
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

    pub fn get(&self, path: &str) -> Option<FileMeta> {
        let entry = self.entries.get(path)?;
        if entry.inserted_at.elapsed() > self.ttl {
            drop(entry);
            self.entries.remove(path);
            return None;
        }
        Some(entry.meta.clone())
    }

    pub fn insert(&self, path: String, meta: FileMeta) {
        self.entries.insert(path, CacheEntry {
            meta,
            inserted_at: Instant::now(),
        });
    }

    pub fn invalidate(&self, path: &str) {
        self.entries.remove(path);
    }

    pub fn invalidate_prefix(&self, prefix: &str) {
        self.entries.retain(|k, _| !k.starts_with(prefix));
    }
}
```

**Step 4: Run tests**

Run: `cargo test cache`
Expected: all pass.

**Step 5: Commit**

```
feat: add DashMap-based metadata cache with TTL
```

---

### Task 5: ADB Trait & CLI Implementation (`adb/`)

**Files:**
- Create: `src/adb/mod.rs`
- Create: `src/adb/cli.rs`
- Modify: `src/main.rs` (add `mod adb;`)

**Step 1: Define the trait and error types in mod.rs**

```rust
use async_trait::async_trait;
use std::path::Path;
use thiserror::Error;

#[derive(Debug)]
pub struct ShellOutput {
    pub stdout: Vec<String>,
    pub stderr: Vec<String>,
    pub exit_code: i32,
}

#[derive(Debug, Error)]
pub enum AdbError {
    #[error("device not found")]
    DeviceNotFound,
    #[error("permission denied: {path}")]
    PermissionDenied { path: String },
    #[error("command failed (exit {exit_code}): {stderr}")]
    CommandFailed { exit_code: i32, stderr: String },
    #[error(transparent)]
    Io(#[from] std::io::Error),
}

#[async_trait]
pub trait AdbDevice: Send + Sync {
    async fn shell(&self, command: &str) -> Result<Vec<String>, AdbError>;
    async fn shell_with_stderr(&self, command: &str) -> Result<ShellOutput, AdbError>;
    async fn pull(&self, remote: &Path, local: &Path) -> Result<(), AdbError>;
    async fn push(&self, local: &Path, remote: &Path) -> Result<(), AdbError>;
    async fn sync_device(&self) -> Result<(), AdbError>;
}

pub mod cli;
```

**Step 2: Implement cli.rs**

```rust
use super::{AdbDevice, AdbError, ShellOutput};
use async_trait::async_trait;
use std::path::Path;
use tokio::process::Command;
use tracing::debug;

pub struct AdbCli {
    serial: Option<String>,
}

impl AdbCli {
    pub fn new(serial: Option<String>) -> Self {
        Self { serial }
    }

    fn base_command(&self) -> Command {
        let mut cmd = Command::new("adb");
        if let Some(ref serial) = self.serial {
            cmd.arg("-s").arg(serial);
        }
        cmd
    }
}

#[async_trait]
impl AdbDevice for AdbCli {
    async fn shell(&self, command: &str) -> Result<Vec<String>, AdbError> {
        debug!(cmd = command, "adb shell");
        let output = self.base_command()
            .arg("shell")
            .arg(command)
            .output()
            .await?;

        let stdout = String::from_utf8_lossy(&output.stdout);
        let lines: Vec<String> = stdout
            .lines()
            .map(|l| l.trim_end_matches('\r').to_string())
            .collect();

        if lines.is_empty() && !output.status.success() {
            return Err(AdbError::DeviceNotFound);
        }
        Ok(lines)
    }

    async fn shell_with_stderr(&self, command: &str) -> Result<ShellOutput, AdbError> {
        debug!(cmd = command, "adb shell (with stderr)");
        let output = self.base_command()
            .arg("shell")
            .arg(command)
            .output()
            .await?;

        let stdout: Vec<String> = String::from_utf8_lossy(&output.stdout)
            .lines()
            .map(|l| l.trim_end_matches('\r').to_string())
            .collect();
        let stderr: Vec<String> = String::from_utf8_lossy(&output.stderr)
            .lines()
            .map(|l| l.trim_end_matches('\r').to_string())
            .collect();

        Ok(ShellOutput {
            stdout,
            stderr,
            exit_code: output.status.code().unwrap_or(-1),
        })
    }

    async fn pull(&self, remote: &Path, local: &Path) -> Result<(), AdbError> {
        debug!(?remote, ?local, "adb pull");
        let output = self.base_command()
            .arg("pull")
            .arg(remote)
            .arg(local)
            .output()
            .await?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr).to_string();
            return Err(AdbError::CommandFailed {
                exit_code: output.status.code().unwrap_or(-1),
                stderr,
            });
        }
        Ok(())
    }

    async fn push(&self, local: &Path, remote: &Path) -> Result<(), AdbError> {
        debug!(?local, ?remote, "adb push");
        let output = self.base_command()
            .arg("push")
            .arg(local)
            .arg(remote)
            .output()
            .await?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr).to_string();
            return Err(AdbError::CommandFailed {
                exit_code: output.status.code().unwrap_or(-1),
                stderr,
            });
        }
        Ok(())
    }

    async fn sync_device(&self) -> Result<(), AdbError> {
        self.shell("sync").await?;
        Ok(())
    }
}
```

**Step 3: Verify it compiles**

Run: `cargo build`
Expected: compiles successfully.

**Step 4: Commit**

```
feat: add AdbDevice trait and CLI implementation
```

Note: `AdbCli` is tested via integration tests only (needs a real device). The trait enables mocking in `DeviceOps` tests.

---

### Task 6: DeviceOps (`ops.rs`)

**Files:**
- Create: `src/ops.rs`
- Modify: `src/main.rs` (add `mod ops;`)

**Step 1: Write tests with a mock AdbDevice**

```rust
#[cfg(test)]
mod tests {
    use super::*;
    use crate::adb::{AdbDevice, AdbError, ShellOutput};
    use async_trait::async_trait;
    use std::sync::Mutex;

    struct MockAdb {
        responses: Mutex<Vec<Result<Vec<String>, AdbError>>>,
    }

    impl MockAdb {
        fn new(responses: Vec<Result<Vec<String>, AdbError>>) -> Self {
            Self { responses: Mutex::new(responses) }
        }
    }

    #[async_trait]
    impl AdbDevice for MockAdb {
        async fn shell(&self, _cmd: &str) -> Result<Vec<String>, AdbError> {
            self.responses.lock().unwrap().remove(0)
        }
        async fn shell_with_stderr(&self, _cmd: &str) -> Result<ShellOutput, AdbError> {
            let result = self.responses.lock().unwrap().remove(0);
            match result {
                Ok(lines) => Ok(ShellOutput { stdout: lines, stderr: vec![], exit_code: 0 }),
                Err(e) => Err(e),
            }
        }
        async fn pull(&self, _r: &Path, _l: &Path) -> Result<(), AdbError> { Ok(()) }
        async fn push(&self, _l: &Path, _r: &Path) -> Result<(), AdbError> { Ok(()) }
        async fn sync_device(&self) -> Result<(), AdbError> { Ok(()) }
    }

    #[tokio::test]
    async fn get_metadata_parses_ls_output() {
        let mock = Arc::new(MockAdb::new(vec![
            Ok(vec!["-rw-r--r-- root root 1234 2024-01-15 10:30 test.txt".to_string()]),
        ]));
        let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
        let meta = ops.get_metadata("/sdcard/test.txt").await.unwrap();
        assert_eq!(meta.size, 1234);
        assert_eq!(meta.mode & libc::S_IFREG as u32, libc::S_IFREG as u32);
    }

    #[tokio::test]
    async fn get_metadata_permission_denied() {
        let mock = Arc::new(MockAdb::new(vec![
            Ok(vec!["/sbin/healthd: Permission denied".to_string()]),
        ]));
        let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
        let result = ops.get_metadata("/sbin/healthd").await;
        assert!(matches!(result, Err(DeviceError::PermissionDenied { .. })));
    }

    #[tokio::test]
    async fn get_metadata_no_device() {
        let mock = Arc::new(MockAdb::new(vec![
            Ok(vec![]),
        ]));
        let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
        let result = ops.get_metadata("/any").await;
        assert!(matches!(result, Err(DeviceError::NoDevice)));
    }
}
```

**Step 2: Run tests to verify they fail**

Run: `cargo test ops`
Expected: compilation error.

**Step 3: Implement ops.rs**

```rust
use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::SystemTime;
use thiserror::Error;
use tracing::debug;

use crate::adb::{AdbDevice, AdbError};
use crate::escape::shell_escape_path;
use crate::parse::{self, FileMeta};

static PERMISSION_ERR_SUFFIX: &str = ": Permission denied";
static TOUCH_TOYBOX_ERR_PREFIX: &str = "touch: bad '@";
static TOUCH_BUSYBOX_ERR_PREFIX: &str = "touch: invalid date '@";

#[derive(Debug, Error)]
pub enum DeviceError {
    #[error("no device connected")]
    NoDevice,
    #[error("permission denied: {path}")]
    PermissionDenied { path: String },
    #[error("file not found: {path}")]
    NotFound { path: String },
    #[error("not supported")]
    NotSupported,
    #[error(transparent)]
    Adb(#[from] AdbError),
}

impl DeviceError {
    pub fn to_errno(&self) -> i32 {
        match self {
            Self::NoDevice => libc::EAGAIN,
            Self::PermissionDenied { .. } => libc::EACCES,
            Self::NotFound { .. } => libc::ENOENT,
            Self::NotSupported => libc::ENOSYS,
            Self::Adb(_) => libc::EIO,
        }
    }
}

pub struct ResolvedCompat {
    // Future: metadata_strategy, symlink_strategy, etc.
}

impl ResolvedCompat {
    pub fn legacy() -> Self {
        Self {}
    }
}

pub struct DeviceOps {
    adb: Arc<dyn AdbDevice>,
    compat: ResolvedCompat,
    touch_gnu_mode: AtomicBool,
    pub rescan: bool,
}

impl DeviceOps {
    pub fn new(adb: Arc<dyn AdbDevice>, compat: ResolvedCompat) -> Self {
        Self {
            adb,
            compat,
            touch_gnu_mode: AtomicBool::new(true),
            rescan: false,
        }
    }

    pub fn with_rescan(mut self, rescan: bool) -> Self {
        self.rescan = rescan;
        self
    }

    pub async fn get_metadata(&self, path: &str) -> Result<FileMeta, DeviceError> {
        let escaped = shell_escape_path(path);
        let cmd = format!("ls -l -a -d '{escaped}'");
        let lines = self.adb.shell(&cmd).await?;

        if lines.is_empty() {
            return Err(DeviceError::NoDevice);
        }

        let first = &lines[0];
        if first.ends_with(PERMISSION_ERR_SUFFIX) {
            return Err(DeviceError::PermissionDenied {
                path: path.to_string(),
            });
        }

        parse::parse_ls_line(first).ok_or_else(|| DeviceError::NotFound {
            path: path.to_string(),
        })
    }

    pub async fn list_dir(&self, path: &str) -> Result<Vec<(String, Option<FileMeta>)>, DeviceError> {
        let escaped = shell_escape_path(path);
        let cmd = format!("ls -l -a '{escaped}'");
        let lines = self.adb.shell(&cmd).await?;

        let mut entries = Vec::new();
        for line in &lines {
            if line.len() < 3 {
                continue;
            }
            if !parse::is_valid_ls_output(line) {
                // Check for permission denied on individual files
                if line.ends_with(PERMISSION_ERR_SUFFIX) {
                    if let Some(name) = extract_name_from_perm_error(line) {
                        entries.push((name, None));
                    }
                }
                continue;
            }
            if let Some(name) = parse::extract_filename(line) {
                let meta = parse::parse_ls_line(line);
                entries.push((name.to_string(), meta));
            }
        }
        Ok(entries)
    }

    pub async fn resolve_symlink(
        &self,
        path: &str,
        raw_line: &str,
    ) -> Result<String, DeviceError> {
        let num_slashes = path.chars().filter(|&c| c == '/').count();
        parse::parse_symlink_target(raw_line, num_slashes)
            .ok_or(DeviceError::NotSupported)
    }

    pub async fn touch(
        &self,
        path: &str,
        atime: Option<std::time::SystemTime>,
        mtime: Option<std::time::SystemTime>,
    ) -> Result<(), DeviceError> {
        let escaped = shell_escape_path(path);
        let gnu = self.touch_gnu_mode.load(Ordering::Relaxed);
        let mut parts = Vec::new();

        if let Some(at) = atime {
            parts.push(format!(
                "touch -a -d {} '{escaped}'",
                format_touch_time(at, gnu)
            ));
        }
        if let Some(mt) = mtime {
            parts.push(format!(
                "touch -m -d {} '{escaped}'",
                format_touch_time(mt, gnu)
            ));
        }
        if parts.is_empty() {
            return Ok(());
        }

        let cmd = parts.join(" && ");
        let output = self.adb.shell_with_stderr(&cmd).await?;

        if let Some(first) = output.stdout.first() {
            let is_date_err = first.starts_with(TOUCH_TOYBOX_ERR_PREFIX)
                || first.starts_with(TOUCH_BUSYBOX_ERR_PREFIX);
            if is_date_err {
                if !gnu {
                    return Err(DeviceError::NotSupported);
                }
                debug!("Touch doesn't support GNU dates, switching to legacy mode");
                self.touch_gnu_mode.store(false, Ordering::Relaxed);
                // Retry with legacy format
                return Box::pin(self.touch(path, atime, mtime)).await;
            }
        }

        if self.rescan {
            self.rescan_file(path).await?;
        }
        Ok(())
    }

    pub async fn mkdir(&self, path: &str) -> Result<(), DeviceError> {
        let escaped = shell_escape_path(path);
        self.adb.shell(&format!("mkdir '{escaped}'")).await?;
        Ok(())
    }

    pub async fn rm(&self, path: &str) -> Result<(), DeviceError> {
        let escaped = shell_escape_path(path);
        self.adb.shell(&format!("rm '{escaped}'")).await?;
        if self.rescan {
            self.rescan_file(path).await?;
        }
        Ok(())
    }

    pub async fn rmdir(&self, path: &str) -> Result<(), DeviceError> {
        let escaped = shell_escape_path(path);
        self.adb.shell(&format!("rmdir '{escaped}'")).await?;
        if self.rescan {
            self.rescan_dir_removed(path).await?;
        }
        Ok(())
    }

    pub async fn mv(&self, from: &str, to: &str) -> Result<(), DeviceError> {
        let from_escaped = shell_escape_path(from);
        let to_escaped = shell_escape_path(to);
        self.adb
            .shell(&format!("mv '{from_escaped}' '{to_escaped}'"))
            .await?;
        if self.rescan {
            self.rescan_file(from).await?;
            self.rescan_file(to).await?;
        }
        Ok(())
    }

    pub async fn pull(&self, remote: &str, local: &Path) -> Result<(), DeviceError> {
        self.adb
            .pull(Path::new(remote), local)
            .await
            .map_err(Into::into)
    }

    pub async fn push(&self, local: &Path, remote: &str) -> Result<(), DeviceError> {
        self.adb
            .push(local, Path::new(remote))
            .await
            .map_err(Into::into)
    }

    pub async fn sync_device(&self) -> Result<(), DeviceError> {
        self.adb.sync_device().await.map_err(Into::into)
    }

    async fn rescan_file(&self, path: &str) -> Result<(), DeviceError> {
        let cmd = format!(
            "am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://{path}'"
        );
        self.adb.shell(&cmd).await?;
        Ok(())
    }

    async fn rescan_dir_removed(&self, path: &str) -> Result<(), DeviceError> {
        let cmd = format!(
            "am broadcast -a android.intent.action.MEDIA_UNMOUNTED -d 'file://{path}'"
        );
        self.adb.shell(&cmd).await?;
        Ok(())
    }
}

fn format_touch_time(time: SystemTime, gnu_format: bool) -> String {
    let dur = time.duration_since(std::time::UNIX_EPOCH).unwrap_or_default();
    if gnu_format {
        format!("@{}.{:09}", dur.as_secs(), dur.subsec_nanos())
    } else {
        format!(
            "`date -ud @{}.{:09} +%Y-%m-%dT%H:%M:%S`",
            dur.as_secs(),
            dur.subsec_nanos()
        )
    }
}

fn extract_name_from_perm_error(line: &str) -> Option<String> {
    // Format: "lstat '//efs' failed: Permission denied"
    let last_slash = line.rfind('/')?;
    let end = line.find("' ")?;
    if last_slash < end {
        Some(line[last_slash + 1..end].to_string())
    } else {
        None
    }
}
```

**Step 4: Run tests**

Run: `cargo test ops`
Expected: all pass.

**Step 5: Commit**

```
feat: add DeviceOps layer for device interaction with compat support
```

---

### Task 7: FUSE Filesystem (`fs.rs`)

**Files:**
- Create: `src/fs.rs`
- Modify: `src/main.rs` (add `mod fs;`, wire up mounting)

This is the largest task — implementing all 16 FUSE operations as thin wrappers around DeviceOps + cache.

**Step 1: Implement the AdbFs struct and core helpers**

```rust
use std::collections::HashMap;
use std::ffi::OsStr;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use dashmap::DashMap;
use fuser::{
    FileAttr, FileType, Filesystem, MountOption, ReplyAttr, ReplyData,
    ReplyDirectory, ReplyEmpty, ReplyEntry, ReplyOpen, ReplyWrite, Request,
    TimeOrNow,
};
use tracing::{debug, trace, warn};

use crate::cache::MetadataCache;
use crate::ops::{DeviceOps, DeviceError};
use crate::parse::FileMeta;

const TTL: Duration = Duration::from_secs(1);
const FUSE_ROOT_INO: u64 = 1;

pub struct OpenFile {
    local_path: PathBuf,
    device_path: String,
    file: std::fs::File,
    dirty: bool,
}

pub struct AdbFs {
    ops: Arc<DeviceOps>,
    cache: MetadataCache,
    open_files: DashMap<u64, OpenFile>,
    tmp_dir: tempfile::TempDir,
    next_fh: AtomicU64,
    rt: tokio::runtime::Handle,
}
```

Key design note: fuser's `Filesystem` trait methods are synchronous. We hold a `tokio::runtime::Handle` and use `rt.block_on()` to bridge into async DeviceOps calls. The tokio runtime is created in `main()` and its handle passed to `AdbFs`.

**Step 2: Implement helper to convert FileMeta → FileAttr**

```rust
impl AdbFs {
    pub fn new(
        ops: Arc<DeviceOps>,
        cache_ttl: Duration,
        rt: tokio::runtime::Handle,
    ) -> color_eyre::Result<Self> {
        Ok(Self {
            ops,
            cache: MetadataCache::new(cache_ttl),
            open_files: DashMap::new(),
            tmp_dir: tempfile::TempDir::new()?,
            next_fh: AtomicU64::new(1),
            rt,
        })
    }

    fn meta_to_attr(&self, meta: &FileMeta) -> FileAttr {
        let kind = mode_to_filetype(meta.mode);
        FileAttr {
            ino: FUSE_ROOT_INO,
            size: meta.size,
            blocks: (meta.size + 256) / 512,
            atime: meta.mtime,
            mtime: meta.mtime,
            ctime: meta.mtime,
            crtime: UNIX_EPOCH,
            kind,
            perm: (meta.mode & 0o7777) as u16,
            nlink: meta.nlink,
            uid: meta.uid,
            gid: meta.gid,
            rdev: meta.rdev as u32,
            blksize: 512,
            flags: 0,
        }
    }

    fn local_path_for(&self, device_path: &str) -> PathBuf {
        let safe_name = device_path.replace('/', "-");
        self.tmp_dir.path().join(safe_name)
    }
}

fn mode_to_filetype(mode: u32) -> FileType {
    let ft = mode & libc::S_IFMT as u32;
    match ft {
        x if x == libc::S_IFDIR as u32 => FileType::Directory,
        x if x == libc::S_IFLNK as u32 => FileType::Symlink,
        x if x == libc::S_IFBLK as u32 => FileType::BlockDevice,
        x if x == libc::S_IFCHR as u32 => FileType::CharDevice,
        x if x == libc::S_IFIFO as u32 => FileType::NamedPipe,
        x if x == libc::S_IFSOCK as u32 => FileType::Socket,
        _ => FileType::RegularFile,
    }
}
```

**Step 3: Implement Filesystem trait — getattr, readdir, access**

```rust
impl Filesystem for AdbFs {
    fn getattr(&mut self, _req: &Request, ino: u64, reply: ReplyAttr) {
        // Root inode is special — always return directory
        // For non-root, we don't have inode→path mapping in this simple impl,
        // so we rely on the kernel calling lookup first.
        // fuser calls getattr with ino=1 for root.
        trace!(ino, "getattr");
        if ino == FUSE_ROOT_INO {
            let attr = FileAttr {
                ino: FUSE_ROOT_INO,
                size: 0,
                blocks: 0,
                atime: UNIX_EPOCH,
                mtime: UNIX_EPOCH,
                ctime: UNIX_EPOCH,
                crtime: UNIX_EPOCH,
                kind: FileType::Directory,
                perm: 0o755,
                nlink: 2,
                uid: 0,
                gid: 0,
                rdev: 0,
                blksize: 512,
                flags: 0,
            };
            reply.attr(&TTL, &attr);
            return;
        }
        reply.error(libc::ENOENT);
    }

    fn access(&mut self, _req: &Request, _ino: u64, _mask: i32, reply: ReplyEmpty) {
        reply.ok();
    }
    // ... remaining operations follow the same pattern
}
```

**Important note on inode mapping:** fuser is inode-based while our operations are path-based. We need a path↔inode mapping. The simplest approach: maintain a `DashMap<u64, String>` (inode → path) and `DashMap<String, u64>` (path → inode), assigning incrementing inodes. Root inode 1 maps to "/". This is populated during `lookup` and `readdir` calls.

Add these fields to `AdbFs`:

```rust
    path_to_ino: DashMap<String, u64>,
    ino_to_path: DashMap<u64, String>,
    next_ino: AtomicU64,
```

And a helper:

```rust
    fn get_or_assign_ino(&self, path: &str) -> u64 {
        if let Some(ino) = self.path_to_ino.get(path) {
            return *ino;
        }
        let ino = self.next_ino.fetch_add(1, Ordering::Relaxed);
        self.path_to_ino.insert(path.to_string(), ino);
        self.ino_to_path.insert(ino, path.to_string());
        ino
    }

    fn get_path(&self, ino: u64) -> Option<String> {
        self.ino_to_path.get(&ino).map(|v| v.clone())
    }
```

**Step 4: Implement lookup**

```rust
    fn lookup(&mut self, _req: &Request, parent: u64, name: &OsStr, reply: ReplyEntry) {
        let parent_path = match self.get_path(parent) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        let name_str = name.to_string_lossy();
        let full_path = if parent_path == "/" {
            format!("/{name_str}")
        } else {
            format!("{parent_path}/{name_str}")
        };

        trace!(path = %full_path, "lookup");

        // Check cache first
        if let Some(meta) = self.cache.get(&full_path) {
            let ino = self.get_or_assign_ino(&full_path);
            let mut attr = self.meta_to_attr(&meta);
            attr.ino = ino;
            reply.entry(&TTL, &attr, 0);
            return;
        }

        match self.rt.block_on(self.ops.get_metadata(&full_path)) {
            Ok(meta) => {
                let ino = self.get_or_assign_ino(&full_path);
                let mut attr = self.meta_to_attr(&meta);
                attr.ino = ino;
                self.cache.insert(full_path, meta);
                reply.entry(&TTL, &attr, 0);
            }
            Err(e) => reply.error(e.to_errno()),
        }
    }
```

**Step 5: Update getattr to use inode mapping**

```rust
    fn getattr(&mut self, _req: &Request, ino: u64, reply: ReplyAttr) {
        trace!(ino, "getattr");
        let path = match self.get_path(ino) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };

        if let Some(meta) = self.cache.get(&path) {
            let mut attr = self.meta_to_attr(&meta);
            attr.ino = ino;
            reply.attr(&TTL, &attr);
            return;
        }

        match self.rt.block_on(self.ops.get_metadata(&path)) {
            Ok(meta) => {
                let mut attr = self.meta_to_attr(&meta);
                attr.ino = ino;
                self.cache.insert(path, meta);
                reply.attr(&TTL, &attr);
            }
            Err(e) => reply.error(e.to_errno()),
        }
    }
```

**Step 6: Implement readdir**

```rust
    fn readdir(
        &mut self,
        _req: &Request,
        ino: u64,
        _fh: u64,
        offset: i64,
        mut reply: ReplyDirectory,
    ) {
        let path = match self.get_path(ino) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        trace!(path = %path, "readdir");

        let entries = match self.rt.block_on(self.ops.list_dir(&path)) {
            Ok(e) => e,
            Err(e) => { reply.error(e.to_errno()); return; }
        };

        let mut full_entries: Vec<(u64, FileType, String)> = vec![
            (ino, FileType::Directory, ".".to_string()),
            (ino, FileType::Directory, "..".to_string()),
        ];

        for (name, meta) in entries {
            let child_path = if path == "/" {
                format!("/{name}")
            } else {
                format!("{path}/{name}")
            };
            let child_ino = self.get_or_assign_ino(&child_path);
            let ft = meta.as_ref()
                .map(|m| mode_to_filetype(m.mode))
                .unwrap_or(FileType::RegularFile);
            if let Some(m) = meta {
                self.cache.insert(child_path, m);
            }
            full_entries.push((child_ino, ft, name));
        }

        for (i, (ino, ft, name)) in full_entries.iter().enumerate().skip(offset as usize) {
            if reply.add(*ino, (i + 1) as i64, *ft, name) {
                break;
            }
        }
        reply.ok();
    }
```

**Step 7: Implement open, read, write, flush, release**

```rust
    fn open(&mut self, _req: &Request, ino: u64, flags: i32, reply: ReplyOpen) {
        let path = match self.get_path(ino) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        trace!(path = %path, "open");

        let local = self.local_path_for(&path);

        // Pull file from device
        if let Err(e) = self.rt.block_on(self.ops.pull(&path, &local)) {
            warn!(path = %path, err = %e, "pull failed");
            reply.error(e.to_errno());
            return;
        }

        let file = match std::fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(&local)
        {
            Ok(f) => f,
            Err(e) => {
                reply.error(e.raw_os_error().unwrap_or(libc::EIO));
                return;
            }
        };

        let fh = self.next_fh.fetch_add(1, Ordering::Relaxed);
        self.open_files.insert(fh, OpenFile {
            local_path: local,
            device_path: path,
            file,
            dirty: false,
        });
        reply.opened(fh, 0);
    }

    fn read(
        &mut self,
        _req: &Request,
        _ino: u64,
        fh: u64,
        offset: i64,
        size: u32,
        _flags: i32,
        _lock_owner: Option<u64>,
        reply: ReplyData,
    ) {
        let mut entry = match self.open_files.get_mut(&fh) {
            Some(e) => e,
            None => { reply.error(libc::EBADF); return; }
        };
        let mut buf = vec![0u8; size as usize];
        match entry.file.seek(SeekFrom::Start(offset as u64))
            .and_then(|_| entry.file.read(&mut buf))
        {
            Ok(n) => reply.data(&buf[..n]),
            Err(e) => reply.error(e.raw_os_error().unwrap_or(libc::EIO)),
        }
    }

    fn write(
        &mut self,
        _req: &Request,
        _ino: u64,
        fh: u64,
        offset: i64,
        data: &[u8],
        _write_flags: u32,
        _flags: i32,
        _lock_owner: Option<u64>,
        reply: ReplyWrite,
    ) {
        let mut entry = match self.open_files.get_mut(&fh) {
            Some(e) => e,
            None => { reply.error(libc::EBADF); return; }
        };
        match entry.file.seek(SeekFrom::Start(offset as u64))
            .and_then(|_| entry.file.write(data))
        {
            Ok(n) => {
                entry.dirty = true;
                reply.written(n as u32);
            }
            Err(e) => reply.error(e.raw_os_error().unwrap_or(libc::EIO)),
        }
    }

    fn flush(&mut self, _req: &Request, _ino: u64, fh: u64, _lock_owner: u64, reply: ReplyEmpty) {
        let entry = match self.open_files.get(&fh) {
            Some(e) => e,
            None => { reply.error(libc::EBADF); return; }
        };
        if entry.dirty {
            let device_path = entry.device_path.clone();
            let local_path = entry.local_path.clone();
            drop(entry);

            if let Err(e) = self.rt.block_on(async {
                self.ops.push(&local_path, &device_path).await?;
                self.ops.sync_device().await?;
                Ok::<_, DeviceError>(())
            }) {
                reply.error(e.to_errno());
                return;
            }

            self.cache.invalidate(&device_path);
            if let Some(mut e) = self.open_files.get_mut(&fh) {
                e.dirty = false;
            }
        }
        reply.ok();
    }

    fn release(
        &mut self,
        _req: &Request,
        _ino: u64,
        fh: u64,
        _flags: i32,
        _lock_owner: Option<u64>,
        _flush: bool,
        reply: ReplyEmpty,
    ) {
        if let Some((_, open_file)) = self.open_files.remove(&fh) {
            let _ = std::fs::remove_file(&open_file.local_path);
        }
        reply.ok();
    }
```

**Step 8: Implement remaining mutating operations**

```rust
    fn mkdir(
        &mut self,
        _req: &Request,
        parent: u64,
        name: &OsStr,
        _mode: u32,
        _umask: u32,
        reply: ReplyEntry,
    ) {
        let parent_path = match self.get_path(parent) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        let name_str = name.to_string_lossy();
        let full_path = if parent_path == "/" {
            format!("/{name_str}")
        } else {
            format!("{parent_path}/{name_str}")
        };

        if let Err(e) = self.rt.block_on(self.ops.mkdir(&full_path)) {
            reply.error(e.to_errno());
            return;
        }
        self.cache.invalidate(&full_path);

        // Fetch metadata for the new directory
        match self.rt.block_on(self.ops.get_metadata(&full_path)) {
            Ok(meta) => {
                let ino = self.get_or_assign_ino(&full_path);
                let mut attr = self.meta_to_attr(&meta);
                attr.ino = ino;
                self.cache.insert(full_path, meta);
                reply.entry(&TTL, &attr, 0);
            }
            Err(e) => reply.error(e.to_errno()),
        }
    }

    fn unlink(&mut self, _req: &Request, parent: u64, name: &OsStr, reply: ReplyEmpty) {
        let parent_path = match self.get_path(parent) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        let name_str = name.to_string_lossy();
        let full_path = if parent_path == "/" {
            format!("/{name_str}")
        } else {
            format!("{parent_path}/{name_str}")
        };

        if let Err(e) = self.rt.block_on(self.ops.rm(&full_path)) {
            reply.error(e.to_errno());
            return;
        }
        self.cache.invalidate(&full_path);
        reply.ok();
    }

    fn rmdir(&mut self, _req: &Request, parent: u64, name: &OsStr, reply: ReplyEmpty) {
        let parent_path = match self.get_path(parent) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        let name_str = name.to_string_lossy();
        let full_path = if parent_path == "/" {
            format!("/{name_str}")
        } else {
            format!("{parent_path}/{name_str}")
        };

        if let Err(e) = self.rt.block_on(self.ops.rmdir(&full_path)) {
            reply.error(e.to_errno());
            return;
        }
        self.cache.invalidate(&full_path);
        reply.ok();
    }

    fn rename(
        &mut self,
        _req: &Request,
        parent: u64,
        name: &OsStr,
        newparent: u64,
        newname: &OsStr,
        _flags: u32,
        reply: ReplyEmpty,
    ) {
        let from = self.resolve_child_path(parent, name);
        let to = self.resolve_child_path(newparent, newname);
        let (from, to) = match (from, to) {
            (Some(f), Some(t)) => (f, t),
            _ => { reply.error(libc::ENOENT); return; }
        };

        if let Err(e) = self.rt.block_on(self.ops.mv(&from, &to)) {
            reply.error(e.to_errno());
            return;
        }
        self.cache.invalidate(&from);
        self.cache.invalidate(&to);
        reply.ok();
    }

    fn setattr(
        &mut self,
        _req: &Request,
        ino: u64,
        _mode: Option<u32>,
        _uid: Option<u32>,
        _gid: Option<u32>,
        size: Option<u64>,
        atime: Option<TimeOrNow>,
        mtime: Option<TimeOrNow>,
        _ctime: Option<SystemTime>,
        _fh: Option<u64>,
        _crtime: Option<SystemTime>,
        _chgtime: Option<SystemTime>,
        _bkuptime: Option<SystemTime>,
        _flags: Option<u32>,
        reply: ReplyAttr,
    ) {
        let path = match self.get_path(ino) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };

        // Handle truncate
        if let Some(new_size) = size {
            let local = self.local_path_for(&path);
            // Pull if not already local
            if !local.exists() {
                if let Err(e) = self.rt.block_on(self.ops.pull(&path, &local)) {
                    reply.error(e.to_errno());
                    return;
                }
            }
            if let Err(e) = std::fs::OpenOptions::new().write(true).open(&local)
                .and_then(|f| f.set_len(new_size))
            {
                reply.error(e.raw_os_error().unwrap_or(libc::EIO));
                return;
            }
            self.cache.invalidate(&path);
        }

        // Handle utimens
        let resolve_time = |t: TimeOrNow| -> SystemTime {
            match t {
                TimeOrNow::SpecificTime(st) => st,
                TimeOrNow::Now => SystemTime::now(),
            }
        };
        let at = atime.map(resolve_time);
        let mt = mtime.map(resolve_time);
        if at.is_some() || mt.is_some() {
            if let Err(e) = self.rt.block_on(self.ops.touch(&path, at, mt)) {
                reply.error(e.to_errno());
                return;
            }
            self.cache.invalidate(&path);
        }

        // Return updated attr
        match self.rt.block_on(self.ops.get_metadata(&path)) {
            Ok(meta) => {
                let mut attr = self.meta_to_attr(&meta);
                attr.ino = ino;
                self.cache.insert(path, meta);
                reply.attr(&TTL, &attr);
            }
            Err(e) => reply.error(e.to_errno()),
        }
    }

    fn readlink(&mut self, _req: &Request, ino: u64, reply: ReplyData) {
        let path = match self.get_path(ino) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };

        // Get ls output (from cache or fresh)
        let meta = match self.cache.get(&path) {
            Some(m) => m,
            None => match self.rt.block_on(self.ops.get_metadata(&path)) {
                Ok(m) => { self.cache.insert(path.clone(), m.clone()); m }
                Err(e) => { reply.error(e.to_errno()); return; }
            }
        };

        match self.rt.block_on(self.ops.resolve_symlink(&path, &meta.raw_line)) {
            Ok(target) => reply.data(target.as_bytes()),
            Err(e) => reply.error(e.to_errno()),
        }
    }

    fn mknod(
        &mut self,
        _req: &Request,
        parent: u64,
        name: &OsStr,
        mode: u32,
        _umask: u32,
        rdev: u32,
        reply: ReplyEntry,
    ) {
        let parent_path = match self.get_path(parent) {
            Some(p) => p,
            None => { reply.error(libc::ENOENT); return; }
        };
        let name_str = name.to_string_lossy();
        let full_path = if parent_path == "/" {
            format!("/{name_str}")
        } else {
            format!("{parent_path}/{name_str}")
        };

        let local = self.local_path_for(&full_path);
        // Create local node
        unsafe {
            let c_path = std::ffi::CString::new(
                local.to_string_lossy().as_ref()
            ).unwrap();
            libc::mknod(c_path.as_ptr(), mode, rdev as libc::dev_t);
        }

        // Push to device
        if let Err(e) = self.rt.block_on(async {
            self.ops.push(&local, &full_path).await?;
            self.ops.sync_device().await
        }) {
            reply.error(e.to_errno());
            return;
        }

        self.cache.invalidate(&full_path);
        let _ = std::fs::remove_file(&local);

        match self.rt.block_on(self.ops.get_metadata(&full_path)) {
            Ok(meta) => {
                let ino = self.get_or_assign_ino(&full_path);
                let mut attr = self.meta_to_attr(&meta);
                attr.ino = ino;
                self.cache.insert(full_path, meta);
                reply.entry(&TTL, &attr, 0);
            }
            Err(e) => reply.error(e.to_errno()),
        }
    }
}

impl AdbFs {
    fn resolve_child_path(&self, parent: u64, name: &OsStr) -> Option<String> {
        let parent_path = self.get_path(parent)?;
        let name_str = name.to_string_lossy();
        Some(if parent_path == "/" {
            format!("/{name_str}")
        } else {
            format!("{parent_path}/{name_str}")
        })
    }
}
```

**Step 9: Wire up main.rs**

```rust
use std::sync::Arc;
use std::time::Duration;

use clap::Parser;
use color_eyre::eyre::Result;
use fuser::MountOption;

mod adb;
mod cache;
mod escape;
mod fs;
mod ops;
mod parse;

#[derive(Parser)]
#[command(name = "adbfs", about = "Mount Android device filesystem via ADB")]
struct Cli {
    mountpoint: std::path::PathBuf,
    #[arg(long)]
    rescan: bool,
    #[arg(long, default_value = "30")]
    cache_ttl: u64,
    #[arg(short = 'o', value_delimiter = ',')]
    options: Vec<String>,
}

fn main() -> Result<()> {
    color_eyre::install()?;
    tracing_subscriber::fmt()
        .with_env_filter(
            tracing_subscriber::EnvFilter::try_from_default_env()
                .unwrap_or_else(|_| "adbfs=warn".parse().unwrap()),
        )
        .init();

    let cli = Cli::parse();

    let rt = tokio::runtime::Runtime::new()?;
    let adb = Arc::new(adb::cli::AdbCli::new(None));
    let device_ops = Arc::new(
        ops::DeviceOps::new(adb, ops::ResolvedCompat::legacy())
            .with_rescan(cli.rescan),
    );

    let adbfs = fs::AdbFs::new(
        device_ops,
        Duration::from_secs(cli.cache_ttl),
        rt.handle().clone(),
    )?;

    let mut mount_options = vec![
        MountOption::AutoUnmount,
        MountOption::AllowOther,
        MountOption::FSName("adbfs".to_string()),
    ];
    for opt in &cli.options {
        mount_options.push(MountOption::CUSTOM(opt.clone()));
    }

    fuser::mount2(adbfs, &cli.mountpoint, &mount_options)?;
    Ok(())
}
```

**Step 10: Verify it compiles**

Run: `cargo build`
Expected: compiles. May need to adjust fuser API signatures to match exact trait.

**Step 11: Commit**

```
feat: implement FUSE filesystem with all 16 operations
```

---

### Task 8: Compilation Fixes & Smoke Test

**Step 1: Fix any compilation errors**

After implementing all modules, run `cargo build` and fix any type mismatches, missing imports, or API differences with the actual fuser trait signatures. The code in this plan is written against the documented API but may need minor adjustments.

**Step 2: Run cargo clippy**

Run: `cargo clippy -- -D warnings`
Fix any warnings.

**Step 3: Run unit tests**

Run: `cargo test`
Expected: all unit tests pass (escape, parse, cache, ops).

**Step 4: Smoke test with a real device (if available)**

Run:
```bash
mkdir -p /tmp/adbfs-test
RUST_LOG=adbfs=debug cargo run -- /tmp/adbfs-test
# In another terminal:
ls /tmp/adbfs-test/
# Then unmount:
fusermount -u /tmp/adbfs-test
```

**Step 5: Commit**

```
fix: resolve compilation issues and verify smoke test
```

---

### Task 9: CI Pipeline Update

**Files:**
- Modify: `.github/workflows/ccpp.yml` (or create new `rust.yml`)

**Step 1: Create Rust CI workflow**

Add a workflow that:
1. Runs `cargo build`
2. Runs `cargo test`
3. Runs `cargo clippy -- -D warnings`
4. Runs `cargo fmt --check`
5. Runs the existing `tests/run.sh` integration tests against the Rust binary on an Android emulator

**Step 2: Verify the existing integration tests pass**

The `tests/run.sh` script creates files, checks timestamps, and verifies content. It should work against the Rust binary since the FUSE mount behaves identically.

**Step 3: Commit**

```
ci: add Rust build and test workflow
```

---

### Task 10: Cleanup & Documentation

**Step 1: Update README.md**

Add build instructions for the Rust version alongside the existing C++ instructions.

**Step 2: Update Makefile (optional)**

Either remove the Makefile or add a `cargo build --release` target to it for backwards compatibility.

**Step 3: Final commit**

```
docs: update README with Rust build instructions
```
