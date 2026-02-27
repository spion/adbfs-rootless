# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Is

adbfs-rootless is a FUSE filesystem that mounts Android devices over ADB without requiring root. The Rust port (branch `rust`) uses fuser (FUSE3); the legacy C++ version uses libfuse (FUSE2).

## Build & Test Commands

```bash
cargo build                    # debug build
cargo build --release          # release binary → target/release/adbfs
cargo test                     # all unit + property tests
cargo test parse::tests        # single module
cargo fmt --check              # formatting check
cargo clippy -- -D warnings    # lint (CI enforces this)
```

System dependency: `libfuse3-dev` (or equivalent for your distro).

## Running

```bash
./target/release/adbfs ~/droid                     # basic mount
./target/release/adbfs --cache-ttl 60 --rescan ~/droid  # custom TTL + media rescan
RUST_LOG=adbfs=debug ./target/release/adbfs ~/droid     # debug logging
fusermount -u ~/droid                              # unmount
```

## Architecture

Three-layer design:

```
FUSE interface (fs.rs)
  ↓ inode ↔ path mapping, file handle management, metadata cache
Device operations (ops.rs)
  ↓ high-level ops: list_dir, get_metadata, pull, push, mkdir, rm, mv, touch
ADB transport (adb/)
  ↓ shell commands via tokio::process::Command
Parsing & escaping (parse.rs, escape.rs)
```

### Key design decisions

- **Inode mapping**: ADB is path-based, FUSE is inode-based. `fs.rs` maintains bidirectional `DashMap<String, u64>` / `DashMap<u64, String>` with atomic inode counter.

- **Readdir caching**: FUSE3's `reply.add()` returns true when buffer is full, then the kernel calls readdir again with a new offset. Directory listings are fetched once in `opendir()` and cached per file-handle, then served from cache in `readdir()`. Without this, every offset continuation re-executes the adb `ls` command (this was the root cause of a 10x slowdown vs the C++ version).

- **File I/O via pull/push**: `open()` pulls the device file to a local tempdir. Writes go to the local copy. `flush()` pushes it back and syncs.

- **Async bridge**: All ADB operations are async (tokio). FUSE callbacks are sync → bridged via `rt.block_on()`.

- **Concurrency**: ADB daemon on-device is single-threaded. A semaphore in `adb/cli.rs` limits concurrent adb commands to `min(16, available_parallelism)`.

### Module roles

| Module | Role |
|--------|------|
| `fs.rs` | fuser::Filesystem impl — all FUSE callbacks, inode/handle maps, metadata cache |
| `ops.rs` | DeviceOps — high-level device operations, symlink resolution, media rescan |
| `cache.rs` | MetadataCache — DashMap with TTL expiration, prefix invalidation |
| `adb/mod.rs` | AdbDevice trait definition |
| `adb/cli.rs` | AdbCli — concrete impl executing `adb` binary |
| `parse.rs` | Parses Android `ls -l` output (multiple formats), mode strings, symlinks |
| `escape.rs` | Shell escaping for adb shell commands and paths |

## Testing

- **Property tests** in `parse.rs` and `escape.rs` (proptest) — verify parser robustness and escaping invariants
- **Mock ADB** in `ops.rs` — `MockAdb` implements `AdbDevice` for testing without a device
- **Integration tests** in CI — Android emulator (API 29) via `tests/run.sh`

## CI

GitHub Actions (`.github/workflows/rust.yml`): fmt → clippy → build → test → release build → integration test on Android emulator.
