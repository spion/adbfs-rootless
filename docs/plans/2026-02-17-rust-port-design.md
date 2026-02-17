# adbfs-rootless Rust Port Design

## Motivation

Full modernization of the C++ codebase (~1,335 lines across `adbfs.cpp` and `utils.h`):
safety, correctness, maintainability, and performance.

## Key Decisions

- **FUSE crate:** fuser (Linux + macOS via macFUSE)
- **Async runtime:** tokio (async ADB command execution via `tokio::process::Command`)
- **ADB interaction:** CLI-based (`adb` binary) behind an async trait for future flexibility
  (e.g. protocol-level ADB client). Commands, parsing, and escaping preserved from C++ for
  Android compatibility.
- **Concurrency:** DashMap for caches (read-heavy, occasional writes)
- **Error reporting:** color_eyre for user-facing errors, thiserror for typed internal errors
- **Logging:** tracing + tracing-subscriber with env-filter (`RUST_LOG=adbfs=debug`)
- **Android compatibility:** battle-tested ADB interaction layer preserved. `ls` parsing,
  shell escaping, and touch format fallback kept as-is. Runtime `--compat` flag to opt into
  modern strategies (stat, readlink, simpler escaping) when targeting newer Android versions.

## Architecture

Three layers:

```
AdbFs (fs.rs)          — thin FUSE callbacks, metadata cache, open file tracking
  → DeviceOps          — device interaction methods, compat strategies via self
    → dyn AdbDevice    — async transport trait (adb CLI implementation)
```

**Why the AdbFs / DeviceOps split:** fuser's `Request` and `Reply` types have private fields
and no public constructors, making FUSE callbacks impossible to unit test directly. DeviceOps
extracts all testable logic below the FUSE boundary.

**Why the AdbDevice trait:** enables mocking for tests now, and swapping in a protocol-level
ADB implementation later without touching the layers above.

## Project Structure

```
src/
  main.rs          — CLI parsing (clap), fuser mount setup, tracing init, color_eyre
  fs.rs            — AdbFs struct, implements fuser::Filesystem, delegates to DeviceOps
  ops.rs           — DeviceOps struct, high-level device operations, compat-aware via self
  adb/
    mod.rs         — AdbDevice trait definition, AdbError type, ShellOutput
    cli.rs         — AdbCli: trait impl using tokio::process::Command
  cache.rs         — DashMap-based metadata cache with configurable TTL
  parse.rs         — ls output parsing, mode string conversion, timestamp parsing
  escape.rs        — shell escaping (preserved from C++ behavior)
```

## Data Model

### File metadata

```rust
struct FileMeta {
    mode: u32,
    size: u64,
    uid: u32,
    gid: u32,
    mtime: SystemTime,
    atime: SystemTime,
    ctime: SystemTime,
    raw_ls_line: String, // preserved for symlink parsing
}
```

### Metadata cache (cache.rs)

- `DashMap<String, CacheEntry>` keyed by device path
- Configurable TTL (default 30s)
- Lazy eviction on access, no background thread
- Methods: `get`, `insert`, `invalidate`, `invalidate_prefix`

### Open file tracking

```rust
struct OpenFile {
    local_path: PathBuf,
    device_path: String,
    fd: File,
    dirty: bool,
    truncated: bool,
}
```

Tracked in `DashMap<u64, OpenFile>` keyed by fuse file handle. Temp directory via
`tempfile::TempDir` (cleaned up on drop).

## ADB Layer

### Trait

```rust
#[async_trait]
trait AdbDevice: Send + Sync {
    async fn shell(&self, command: &str) -> Result<Vec<String>>;
    async fn shell_with_stderr(&self, command: &str) -> Result<ShellOutput>;
    async fn pull(&self, remote: &Path, local: &Path) -> Result<()>;
    async fn push(&self, local: &Path, remote: &Path) -> Result<()>;
    async fn sync_device(&self) -> Result<()>;
}
```

### Error types

```rust
#[derive(Debug, thiserror::Error)]
enum AdbError {
    #[error("device not found")]
    DeviceNotFound,
    #[error("permission denied: {path}")]
    PermissionDenied { path: String },
    #[error("adb command failed (exit {exit_code}): {stderr}")]
    CommandFailed { exit_code: i32, stderr: String },
    #[error(transparent)]
    Io(#[from] std::io::Error),
}
```

Permission denied detected by string matching (preserving C++ behavior for Android compat),
converted to typed error at the ADB layer boundary.

## DeviceOps

Owns an `Arc<dyn AdbDevice>` and compat config. All device interaction methods live here,
accessing strategy flags via `self`:

- `get_metadata(path)` — ls or stat depending on compat
- `resolve_symlink(path)` — ls parsing or readlink depending on compat
- `touch(path, time)` — GNU format with auto-fallback to legacy
- `mkdir(path)`, `rm(path)`, `rmdir(path)`, `mv(from, to)`
- `pull(remote, local)`, `push(local, remote)`
- `rescan_file(path)`, `rescan_dir_removed(path)` — Android media scanner broadcasts

## Compatibility Modes

```rust
enum CompatMode {
    Legacy,  // Android 4+, ls parsing, aggressive escaping
    Modern,  // Android 6+, stat command, readlink, simpler escaping
    Auto,    // probe device capabilities at mount time
}
```

CLI: `--compat legacy|modern|auto` (default: legacy)

Per-strategy overrides available for power users:

```rust
struct AdbFsConfig {
    compat_mode: CompatMode,
    metadata_strategy: Option<MetadataStrategy>,
    symlink_strategy: Option<SymlinkStrategy>,
    rescan: bool,
    cache_ttl: Duration,
}
```

## FUSE Operations

All 16 operations from the C++ version:

| FUSE op    | ADB interaction                          |
|------------|------------------------------------------|
| getattr    | `shell("ls -l -a -d '<path>'")`          |
| readdir    | `shell("ls -l -a '<path>'")`             |
| open       | validate + `pull` to tmp                 |
| read       | local pread                              |
| write      | local pwrite, mark dirty                 |
| flush      | if dirty: `push` + `sync`               |
| release    | close fd, remove tmp                     |
| truncate   | pull if needed, truncate local           |
| utimens    | `shell("touch -d ...")`                  |
| mkdir      | `shell("mkdir '<path>'")`                |
| rmdir      | `shell("rmdir '<path>'")`                |
| unlink     | `shell("rm '<path>'")`                   |
| mknod      | local mknod + `push`                     |
| rename     | `shell("mv '<from>' '<to>'")`            |
| readlink   | parse `->` from ls output                |
| access     | no-op (returns Ok)                       |

## CLI

```rust
#[derive(Parser)]
struct Cli {
    mountpoint: PathBuf,
    #[arg(long)]
    rescan: bool,
    #[arg(long, default_value = "30")]
    cache_ttl: u64,
    #[arg(long, default_value = "legacy")]
    compat: CompatMode,
    #[arg(short = 'o', value_delimiter = ',')]
    options: Vec<String>,
}
```

## Dependencies

```toml
[dependencies]
fuser = "*"
tokio = { version = "*", features = ["rt-multi-thread", "process", "macros"] }
dashmap = "*"
clap = { version = "*", features = ["derive"] }
thiserror = "*"
color-eyre = "*"
tracing = "*"
tracing-subscriber = { version = "*", features = ["env-filter"] }
async-trait = "*"
tempfile = "*"

[dev-dependencies]
proptest = "*"
```

Versions resolved by cargo at build time.

## Testing

**Unit tests (fast, no device):**
- `parse.rs` — property-based (proptest) for mode string parser, hand-crafted cases from
  real Android ls output across versions 4.x–15
- `escape.rs` — property tests for round-trip correctness, known-dangerous inputs
- `DeviceOps` — mock AdbDevice, verify command construction per compat mode, output parsing,
  touch format fallback
- `cache.rs` — TTL expiry, invalidation, prefix invalidation

**Integration tests (slow, needs emulator):**
- Existing `tests/run.sh` — works against the Rust binary unchanged
- Docker + Android emulator CI setup already exists, reused

**Not unit tested:**
- `AdbFs` FUSE callbacks (thin delegation, covered by integration tests)
- `AdbCli` (real adb calls, covered by integration tests)

## CI

Reuse existing GitHub Actions structure:
1. `cargo build` + `cargo test` (unit tests)
2. Android emulator job: mount, run `tests/run.sh`
3. `cargo clippy` + `cargo fmt --check`
