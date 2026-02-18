use std::ffi::OsStr;
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use dashmap::DashMap;
use fuser::{
  AccessFlags, Config, Errno, FileAttr, FileHandle, FileType, Filesystem, FopenFlags, Generation,
  INodeNo, LockOwner, MountOption, OpenFlags, RenameFlags, ReplyAttr, ReplyCreate, ReplyData,
  ReplyDirectory, ReplyEmpty, ReplyEntry, ReplyOpen, ReplyWrite, Request, SessionACL, TimeOrNow,
  WriteFlags,
};
use tracing::{debug, trace, warn};

use crate::cache::MetadataCache;
use crate::ops::{DeviceError, DeviceOps};
use crate::parse::FileMeta;

const TTL: Duration = Duration::from_secs(1);
const FUSE_ROOT_INO: u64 = 1;

fn err(code: i32) -> Errno {
  Errno::from_i32(code)
}

fn device_err(e: DeviceError) -> Errno {
  err(e.to_errno())
}

fn io_err(e: std::io::Error) -> Errno {
  Errno::from(e)
}

struct OpenFile {
  local_path: PathBuf,
  device_path: String,
  file: std::fs::File,
  dirty: bool,
}

struct OpenDir {
  entries: Vec<(u64, FileType, String)>,
}

pub struct AdbFs {
  ops: Arc<DeviceOps>,
  cache: MetadataCache,
  open_files: DashMap<u64, OpenFile>,
  open_dirs: DashMap<u64, OpenDir>,
  tmp_dir: tempfile::TempDir,
  next_fh: AtomicU64,
  rt: tokio::runtime::Handle,
  // Inode mapping: ADB is path-based, FUSE is inode-based
  path_to_ino: DashMap<String, u64>,
  ino_to_path: DashMap<u64, String>,
  next_ino: AtomicU64,
}

impl AdbFs {
  pub fn new(
    ops: Arc<DeviceOps>,
    cache_ttl: Duration,
    rt: tokio::runtime::Handle,
  ) -> color_eyre::Result<Self> {
    let fs = Self {
      ops,
      cache: MetadataCache::new(cache_ttl),
      open_files: DashMap::new(),
      open_dirs: DashMap::new(),
      tmp_dir: tempfile::TempDir::new()?,
      next_fh: AtomicU64::new(1),
      rt,
      path_to_ino: DashMap::new(),
      ino_to_path: DashMap::new(),
      next_ino: AtomicU64::new(FUSE_ROOT_INO + 1),
    };
    fs.path_to_ino.insert("/".to_string(), FUSE_ROOT_INO);
    fs.ino_to_path.insert(FUSE_ROOT_INO, "/".to_string());
    Ok(fs)
  }

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

  fn child_path(parent: &str, name: &OsStr) -> String {
    let name_str = name.to_string_lossy();
    if parent == "/" {
      format!("/{name_str}")
    } else {
      format!("{parent}/{name_str}")
    }
  }

  fn meta_to_attr(&self, ino: u64, meta: &FileMeta) -> FileAttr {
    FileAttr {
      ino: INodeNo(ino),
      size: meta.size,
      blocks: meta.size.div_ceil(512),
      atime: meta.mtime,
      mtime: meta.mtime,
      ctime: meta.mtime,
      crtime: UNIX_EPOCH,
      kind: mode_to_filetype(meta.mode),
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
    let safe_name = device_path.replace('/', "_");
    self.tmp_dir.path().join(safe_name)
  }

  fn fetch_meta(&self, path: &str) -> Result<FileMeta, DeviceError> {
    if let Some(meta) = self.cache.get(path) {
      trace!(path = %path, "cache hit");
      return Ok(meta);
    }
    let meta = self.rt.block_on(self.ops.get_metadata(path))?;
    self.cache.insert(path.to_string(), meta.clone());
    Ok(meta)
  }
}

fn mode_to_filetype(mode: u32) -> FileType {
  let ft = mode & libc::S_IFMT;
  match ft {
    x if x == libc::S_IFDIR => FileType::Directory,
    x if x == libc::S_IFLNK => FileType::Symlink,
    x if x == libc::S_IFBLK => FileType::BlockDevice,
    x if x == libc::S_IFCHR => FileType::CharDevice,
    x if x == libc::S_IFIFO => FileType::NamedPipe,
    x if x == libc::S_IFSOCK => FileType::Socket,
    _ => FileType::RegularFile,
  }
}

impl Filesystem for AdbFs {
  fn getattr(&self, _req: &Request, ino: INodeNo, _fh: Option<FileHandle>, reply: ReplyAttr) {
    let ino_raw = ino.0;
    trace!(ino = ino_raw, "getattr");
    let path = match self.get_path(ino_raw) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };

    match self.fetch_meta(&path) {
      Ok(meta) => {
        let attr = self.meta_to_attr(ino_raw, &meta);
        reply.attr(&TTL, &attr);
      }
      Err(e) => reply.error(device_err(e)),
    }
  }

  fn lookup(&self, _req: &Request, parent: INodeNo, name: &OsStr, reply: ReplyEntry) {
    let parent_raw = parent.0;
    let parent_path = match self.get_path(parent_raw) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    let full_path = Self::child_path(&parent_path, name);
    trace!(path = %full_path, "lookup");

    match self.fetch_meta(&full_path) {
      Ok(meta) => {
        let ino = self.get_or_assign_ino(&full_path);
        let attr = self.meta_to_attr(ino, &meta);
        reply.entry(&TTL, &attr, Generation(0));
      }
      Err(e) => reply.error(device_err(e)),
    }
  }

  fn opendir(&self, _req: &Request, ino: INodeNo, _flags: OpenFlags, reply: ReplyOpen) {
    let ino_raw = ino.0;
    let path = match self.get_path(ino_raw) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    trace!(path = %path, "opendir");

    let entries = match self.rt.block_on(self.ops.list_dir(&path)) {
      Ok(e) => e,
      Err(e) => {
        reply.error(device_err(e));
        return;
      }
    };

    let mut full_entries: Vec<(u64, FileType, String)> = vec![
      (ino_raw, FileType::Directory, ".".to_string()),
      (ino_raw, FileType::Directory, "..".to_string()),
    ];

    for (name, meta) in entries {
      if name == "." || name == ".." {
        continue;
      }
      let child_path = Self::child_path(&path, OsStr::new(&name));
      let child_ino = self.get_or_assign_ino(&child_path);
      let ft = meta
        .as_ref()
        .map(|m| mode_to_filetype(m.mode))
        .unwrap_or(FileType::RegularFile);
      if let Some(m) = meta {
        self.cache.insert(child_path, m);
      }
      full_entries.push((child_ino, ft, name));
    }

    let fh = self.next_fh.fetch_add(1, Ordering::Relaxed);
    self.open_dirs.insert(fh, OpenDir { entries: full_entries });
    reply.opened(FileHandle(fh), FopenFlags::empty());
  }

  fn readdir(
    &self,
    _req: &Request,
    _ino: INodeNo,
    fh: FileHandle,
    offset: u64,
    mut reply: ReplyDirectory,
  ) {
    let dir = match self.open_dirs.get(&fh.0) {
      Some(d) => d,
      None => {
        reply.error(err(libc::EBADF));
        return;
      }
    };

    for (i, (child_ino, ft, name)) in dir.entries.iter().enumerate().skip(offset as usize) {
      if reply.add(INodeNo(*child_ino), (i + 1) as u64, *ft, name) {
        break;
      }
    }
    reply.ok();
  }

  fn releasedir(&self, _req: &Request, _ino: INodeNo, fh: FileHandle, _flags: OpenFlags, reply: ReplyEmpty) {
    self.open_dirs.remove(&fh.0);
    reply.ok();
  }

  fn open(&self, _req: &Request, ino: INodeNo, _flags: OpenFlags, reply: ReplyOpen) {
    let ino_raw = ino.0;
    let path = match self.get_path(ino_raw) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    trace!(path = %path, "open");

    let local = self.local_path_for(&path);

    if let Err(e) = self.rt.block_on(self.ops.pull(&path, &local)) {
      warn!(path = %path, err = %e, "pull failed");
      reply.error(device_err(e));
      return;
    }

    let file = match std::fs::OpenOptions::new()
      .read(true)
      .write(true)
      .open(&local)
    {
      Ok(f) => f,
      Err(e) => {
        reply.error(io_err(e));
        return;
      }
    };

    let fh = self.next_fh.fetch_add(1, Ordering::Relaxed);
    self.open_files.insert(
      fh,
      OpenFile {
        local_path: local,
        device_path: path,
        file,
        dirty: false,
      },
    );
    reply.opened(FileHandle(fh), FopenFlags::empty());
  }

  fn read(
    &self,
    _req: &Request,
    _ino: INodeNo,
    fh: FileHandle,
    offset: u64,
    size: u32,
    _flags: OpenFlags,
    _lock_owner: Option<LockOwner>,
    reply: ReplyData,
  ) {
    let fh_raw = fh.0;
    let mut entry = match self.open_files.get_mut(&fh_raw) {
      Some(e) => e,
      None => {
        reply.error(err(libc::EBADF));
        return;
      }
    };
    let mut buf = vec![0u8; size as usize];
    match entry
      .file
      .seek(SeekFrom::Start(offset))
      .and_then(|_| entry.file.read(&mut buf))
    {
      Ok(n) => reply.data(&buf[..n]),
      Err(e) => reply.error(io_err(e)),
    }
  }

  fn write(
    &self,
    _req: &Request,
    _ino: INodeNo,
    fh: FileHandle,
    offset: u64,
    data: &[u8],
    _write_flags: WriteFlags,
    _flags: OpenFlags,
    _lock_owner: Option<LockOwner>,
    reply: ReplyWrite,
  ) {
    let fh_raw = fh.0;
    let mut entry = match self.open_files.get_mut(&fh_raw) {
      Some(e) => e,
      None => {
        reply.error(err(libc::EBADF));
        return;
      }
    };
    match entry
      .file
      .seek(SeekFrom::Start(offset))
      .and_then(|_| entry.file.write(data))
    {
      Ok(n) => {
        entry.dirty = true;
        reply.written(n as u32);
      }
      Err(e) => reply.error(io_err(e)),
    }
  }

  fn flush(
    &self,
    _req: &Request,
    _ino: INodeNo,
    fh: FileHandle,
    _lock_owner: LockOwner,
    reply: ReplyEmpty,
  ) {
    let fh_raw = fh.0;
    let entry = match self.open_files.get(&fh_raw) {
      Some(e) => e,
      None => {
        reply.error(err(libc::EBADF));
        return;
      }
    };
    if entry.dirty {
      let device_path = entry.device_path.clone();
      let local_path = entry.local_path.clone();
      drop(entry);

      let ops = self.ops.clone();
      if let Err(e) = self.rt.block_on(async {
        ops.push(&local_path, &device_path).await?;
        ops.sync_device().await?;
        Ok::<_, DeviceError>(())
      }) {
        reply.error(device_err(e));
        return;
      }

      self.cache.invalidate(&device_path);
      if let Some(mut e) = self.open_files.get_mut(&fh_raw) {
        e.dirty = false;
      }
    }
    reply.ok();
  }

  fn release(
    &self,
    _req: &Request,
    _ino: INodeNo,
    fh: FileHandle,
    _flags: OpenFlags,
    _lock_owner: Option<LockOwner>,
    _flush: bool,
    reply: ReplyEmpty,
  ) {
    let fh_raw = fh.0;
    if let Some((_, open_file)) = self.open_files.remove(&fh_raw) {
      let _ = std::fs::remove_file(&open_file.local_path);
    }
    reply.ok();
  }

  fn access(&self, _req: &Request, _ino: INodeNo, _mask: AccessFlags, reply: ReplyEmpty) {
    reply.ok();
  }

  fn mkdir(
    &self,
    _req: &Request,
    parent: INodeNo,
    name: &OsStr,
    _mode: u32,
    _umask: u32,
    reply: ReplyEntry,
  ) {
    let parent_path = match self.get_path(parent.0) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    let full_path = Self::child_path(&parent_path, name);
    debug!(path = %full_path, "mkdir");

    if let Err(e) = self.rt.block_on(self.ops.mkdir(&full_path)) {
      reply.error(device_err(e));
      return;
    }
    self.cache.invalidate(&full_path);

    match self.rt.block_on(self.ops.get_metadata(&full_path)) {
      Ok(meta) => {
        let ino = self.get_or_assign_ino(&full_path);
        let attr = self.meta_to_attr(ino, &meta);
        self.cache.insert(full_path, meta);
        reply.entry(&TTL, &attr, Generation(0));
      }
      Err(e) => reply.error(device_err(e)),
    }
  }

  fn unlink(&self, _req: &Request, parent: INodeNo, name: &OsStr, reply: ReplyEmpty) {
    let parent_path = match self.get_path(parent.0) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    let full_path = Self::child_path(&parent_path, name);
    debug!(path = %full_path, "unlink");

    if let Err(e) = self.rt.block_on(self.ops.rm(&full_path)) {
      reply.error(device_err(e));
      return;
    }
    self.cache.invalidate(&full_path);
    reply.ok();
  }

  fn rmdir(&self, _req: &Request, parent: INodeNo, name: &OsStr, reply: ReplyEmpty) {
    let parent_path = match self.get_path(parent.0) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    let full_path = Self::child_path(&parent_path, name);
    debug!(path = %full_path, "rmdir");

    if let Err(e) = self.rt.block_on(self.ops.rmdir(&full_path)) {
      reply.error(device_err(e));
      return;
    }
    self.cache.invalidate_prefix(&full_path);
    reply.ok();
  }

  fn rename(
    &self,
    _req: &Request,
    parent: INodeNo,
    name: &OsStr,
    newparent: INodeNo,
    newname: &OsStr,
    _flags: RenameFlags,
    reply: ReplyEmpty,
  ) {
    let from = self.get_path(parent.0).map(|p| Self::child_path(&p, name));
    let to = self
      .get_path(newparent.0)
      .map(|p| Self::child_path(&p, newname));
    let (from, to) = match (from, to) {
      (Some(f), Some(t)) => (f, t),
      _ => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    debug!(from = %from, to = %to, "rename");

    if let Err(e) = self.rt.block_on(self.ops.mv(&from, &to)) {
      reply.error(device_err(e));
      return;
    }
    self.cache.invalidate(&from);
    self.cache.invalidate(&to);
    reply.ok();
  }

  fn setattr(
    &self,
    _req: &Request,
    ino: INodeNo,
    _mode: Option<u32>,
    _uid: Option<u32>,
    _gid: Option<u32>,
    size: Option<u64>,
    atime: Option<TimeOrNow>,
    mtime: Option<TimeOrNow>,
    _ctime: Option<SystemTime>,
    _fh: Option<FileHandle>,
    _crtime: Option<SystemTime>,
    _chgtime: Option<SystemTime>,
    _bkuptime: Option<SystemTime>,
    _flags: Option<fuser::BsdFileFlags>,
    reply: ReplyAttr,
  ) {
    let ino_raw = ino.0;
    let path = match self.get_path(ino_raw) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    trace!(path = %path, "setattr");

    // Handle truncate
    if let Some(new_size) = size {
      let local = self.local_path_for(&path);
      if !local.exists()
        && let Err(e) = self.rt.block_on(self.ops.pull(&path, &local))
      {
        reply.error(device_err(e));
        return;
      }
      if let Err(e) = std::fs::OpenOptions::new()
        .write(true)
        .open(&local)
        .and_then(|f| f.set_len(new_size))
      {
        reply.error(io_err(e));
        return;
      }
      let ops = self.ops.clone();
      if let Err(e) = self.rt.block_on(async {
        ops.push(&local, &path).await?;
        ops.sync_device().await
      }) {
        reply.error(device_err(e));
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
        reply.error(device_err(e));
        return;
      }
      self.cache.invalidate(&path);
    }

    match self.rt.block_on(self.ops.get_metadata(&path)) {
      Ok(meta) => {
        let attr = self.meta_to_attr(ino_raw, &meta);
        self.cache.insert(path, meta);
        reply.attr(&TTL, &attr);
      }
      Err(e) => reply.error(device_err(e)),
    }
  }

  fn readlink(&self, _req: &Request, ino: INodeNo, reply: ReplyData) {
    let ino_raw = ino.0;
    let path = match self.get_path(ino_raw) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    trace!(path = %path, "readlink");

    let meta = match self.fetch_meta(&path) {
      Ok(m) => m,
      Err(e) => {
        reply.error(device_err(e));
        return;
      }
    };

    match self.ops.resolve_symlink(&path, &meta.raw_line) {
      Ok(target) => reply.data(target.as_bytes()),
      Err(e) => reply.error(device_err(e)),
    }
  }

  fn create(
    &self,
    _req: &Request,
    parent: INodeNo,
    name: &OsStr,
    _mode: u32,
    _umask: u32,
    _flags: i32,
    reply: ReplyCreate,
  ) {
    let parent_path = match self.get_path(parent.0) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    let full_path = Self::child_path(&parent_path, name);
    debug!(path = %full_path, "create");

    let local = self.local_path_for(&full_path);

    let file = match std::fs::OpenOptions::new()
      .read(true)
      .write(true)
      .create(true)
      .truncate(true)
      .open(&local)
    {
      Ok(f) => f,
      Err(e) => {
        reply.error(io_err(e));
        return;
      }
    };

    let ops = self.ops.clone();
    if let Err(e) = self.rt.block_on(async {
      ops.push(&local, &full_path).await?;
      ops.sync_device().await
    }) {
      reply.error(device_err(e));
      return;
    }

    match self.rt.block_on(self.ops.get_metadata(&full_path)) {
      Ok(meta) => {
        let ino = self.get_or_assign_ino(&full_path);
        let attr = self.meta_to_attr(ino, &meta);
        self.cache.insert(full_path.clone(), meta);

        let fh = self.next_fh.fetch_add(1, Ordering::Relaxed);
        self.open_files.insert(
          fh,
          OpenFile {
            local_path: local,
            device_path: full_path,
            file,
            dirty: false,
          },
        );
        reply.created(
          &TTL,
          &attr,
          Generation(0),
          FileHandle(fh),
          FopenFlags::empty(),
        );
      }
      Err(e) => reply.error(device_err(e)),
    }
  }

  fn mknod(
    &self,
    _req: &Request,
    parent: INodeNo,
    name: &OsStr,
    mode: u32,
    _umask: u32,
    rdev: u32,
    reply: ReplyEntry,
  ) {
    let parent_path = match self.get_path(parent.0) {
      Some(p) => p,
      None => {
        reply.error(err(libc::ENOENT));
        return;
      }
    };
    let full_path = Self::child_path(&parent_path, name);
    debug!(path = %full_path, "mknod");

    let local = self.local_path_for(&full_path);

    let c_path = match std::ffi::CString::new(local.to_string_lossy().as_ref()) {
      Ok(p) => p,
      Err(_) => {
        reply.error(err(libc::EINVAL));
        return;
      }
    };
    let ret = unsafe { libc::mknod(c_path.as_ptr(), mode, rdev as libc::dev_t) };
    if ret != 0 {
      reply.error(io_err(std::io::Error::last_os_error()));
      return;
    }

    let ops = self.ops.clone();
    if let Err(e) = self.rt.block_on(async {
      ops.push(&local, &full_path).await?;
      ops.sync_device().await
    }) {
      reply.error(device_err(e));
      return;
    }

    self.cache.invalidate(&full_path);
    let _ = std::fs::remove_file(&local);

    match self.rt.block_on(self.ops.get_metadata(&full_path)) {
      Ok(meta) => {
        let ino = self.get_or_assign_ino(&full_path);
        let attr = self.meta_to_attr(ino, &meta);
        self.cache.insert(full_path, meta);
        reply.entry(&TTL, &attr, Generation(0));
      }
      Err(e) => reply.error(device_err(e)),
    }
  }
}

pub fn mount(
  adbfs: AdbFs,
  mountpoint: &std::path::Path,
  options: Vec<String>,
) -> color_eyre::Result<()> {
  let mut mount_options = vec![
    MountOption::AutoUnmount,
    MountOption::FSName("adbfs".to_string()),
  ];
  for opt in options {
    mount_options.push(MountOption::CUSTOM(opt));
  }

  let n_threads = std::thread::available_parallelism()
    .map(|n| n.get())
    .unwrap_or(4);

  let mut config = Config::default();
  config.mount_options = mount_options;
  config.acl = SessionACL::RootAndOwner;
  config.n_threads = Some(n_threads);
  config.clone_fd = true;
  fuser::mount2(adbfs, mountpoint, &config)?;
  Ok(())
}
