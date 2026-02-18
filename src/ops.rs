use std::path::Path;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::SystemTime;
use thiserror::Error;
use tracing::debug;

use crate::adb::{AdbDevice, AdbError};
use crate::escape::shell_escape_path;
use crate::parse::{self, FileMeta};

const PERMISSION_ERR_SUFFIX: &str = ": Permission denied";
const TOUCH_TOYBOX_ERR_PREFIX: &str = "touch: bad '@";
const TOUCH_BUSYBOX_ERR_PREFIX: &str = "touch: invalid date '@";

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
  #[allow(dead_code)]
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
    let output = self.adb.shell_with_stderr(&cmd).await?;

    // On modern Android, adb shell separates stdout/stderr.
    // When ls fails, the error message is in stderr, not stdout.
    let lines = if output.stdout.is_empty() && !output.stderr.is_empty() {
      &output.stderr
    } else {
      &output.stdout
    };

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
        if line.ends_with(PERMISSION_ERR_SUFFIX)
          && let Some(name) = extract_name_from_perm_error(line)
        {
          entries.push((name, None));
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

  pub fn resolve_symlink(&self, path: &str, raw_line: &str) -> Result<String, DeviceError> {
    let num_slashes = path.chars().filter(|&c| c == '/').count();
    parse::parse_symlink_target(raw_line, num_slashes).ok_or(DeviceError::NotSupported)
  }

  pub async fn touch(
    &self,
    path: &str,
    atime: Option<SystemTime>,
    mtime: Option<SystemTime>,
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
      let is_date_err =
        first.starts_with(TOUCH_TOYBOX_ERR_PREFIX) || first.starts_with(TOUCH_BUSYBOX_ERR_PREFIX);
      if is_date_err {
        if !gnu {
          return Err(DeviceError::NotSupported);
        }
        debug!("Touch doesn't support GNU dates, switching to legacy mode");
        self.touch_gnu_mode.store(false, Ordering::Relaxed);
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
    self
      .adb
      .shell(&format!("mv '{from_escaped}' '{to_escaped}'"))
      .await?;
    if self.rescan {
      self.rescan_file(from).await?;
      self.rescan_file(to).await?;
    }
    Ok(())
  }

  pub async fn pull(&self, remote: &str, local: &Path) -> Result<(), DeviceError> {
    self
      .adb
      .pull(Path::new(remote), local)
      .await
      .map_err(Into::into)
  }

  pub async fn push(&self, local: &Path, remote: &str) -> Result<(), DeviceError> {
    self
      .adb
      .push(local, Path::new(remote))
      .await
      .map_err(Into::into)
  }

  pub async fn sync_device(&self) -> Result<(), DeviceError> {
    self.adb.sync_device().await.map_err(Into::into)
  }

  async fn rescan_file(&self, path: &str) -> Result<(), DeviceError> {
    let cmd =
      format!("am broadcast -a android.intent.action.MEDIA_SCANNER_SCAN_FILE -d 'file://{path}'");
    self.adb.shell(&cmd).await?;
    Ok(())
  }

  async fn rescan_dir_removed(&self, path: &str) -> Result<(), DeviceError> {
    let cmd = format!("am broadcast -a android.intent.action.MEDIA_UNMOUNTED -d 'file://{path}'");
    self.adb.shell(&cmd).await?;
    Ok(())
  }
}

fn format_touch_time(time: SystemTime, gnu_format: bool) -> String {
  let dur = time
    .duration_since(std::time::UNIX_EPOCH)
    .unwrap_or_default();
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
      Self {
        responses: Mutex::new(responses),
      }
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
        Ok(lines) => Ok(ShellOutput {
          stdout: lines,
          stderr: vec![],
          exit_code: 0,
        }),
        Err(e) => Err(e),
      }
    }
    async fn pull(&self, _r: &Path, _l: &Path) -> Result<(), AdbError> {
      Ok(())
    }
    async fn push(&self, _l: &Path, _r: &Path) -> Result<(), AdbError> {
      Ok(())
    }
    async fn sync_device(&self) -> Result<(), AdbError> {
      Ok(())
    }
  }

  #[tokio::test]
  async fn get_metadata_parses_ls_output() {
    let mock = Arc::new(MockAdb::new(vec![Ok(vec![
      "-rw-r--r-- root root 1234 2024-01-15 10:30 test.txt".to_string(),
    ])]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let meta = ops.get_metadata("/sdcard/test.txt").await.unwrap();
    assert_eq!(meta.size, 1234);
    assert_eq!(meta.mode & libc::S_IFREG as u32, libc::S_IFREG as u32);
  }

  #[tokio::test]
  async fn get_metadata_permission_denied() {
    let mock = Arc::new(MockAdb::new(vec![Ok(vec![
      "/sbin/healthd: Permission denied".to_string(),
    ])]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let result = ops.get_metadata("/sbin/healthd").await;
    assert!(matches!(result, Err(DeviceError::PermissionDenied { .. })));
  }

  #[tokio::test]
  async fn get_metadata_no_device() {
    let mock = Arc::new(MockAdb::new(vec![Ok(vec![])]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let result = ops.get_metadata("/any").await;
    assert!(matches!(result, Err(DeviceError::NoDevice)));
  }

  #[tokio::test]
  async fn get_metadata_not_found() {
    let mock = Arc::new(MockAdb::new(vec![Ok(vec![
      "/sdcard/nofile: No such file or directory".to_string(),
    ])]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let result = ops.get_metadata("/sdcard/nofile").await;
    assert!(matches!(result, Err(DeviceError::NotFound { .. })));
  }

  #[tokio::test]
  async fn list_dir_parses_entries() {
    let mock = Arc::new(MockAdb::new(vec![Ok(vec![
      "-rw-r--r-- root root 100 2024-01-15 10:30 a.txt".to_string(),
      "drwxr-xr-x root root      2024-01-15 10:30 subdir".to_string(),
    ])]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let entries = ops.list_dir("/sdcard").await.unwrap();
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[0].0, "a.txt");
    assert_eq!(entries[1].0, "subdir");
    assert!(entries[0].1.is_some());
    assert!(entries[1].1.is_some());
  }

  #[tokio::test]
  async fn list_dir_handles_permission_errors() {
    let mock = Arc::new(MockAdb::new(vec![Ok(vec![
      "-rw-r--r-- root root 100 2024-01-15 10:30 ok.txt".to_string(),
      "lstat '//efs' failed: Permission denied".to_string(),
    ])]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let entries = ops.list_dir("/").await.unwrap();
    assert_eq!(entries.len(), 2);
    assert_eq!(entries[0].0, "ok.txt");
    assert_eq!(entries[1].0, "efs");
    assert!(entries[1].1.is_none());
  }

  #[tokio::test]
  async fn touch_gnu_fallback() {
    let mock = Arc::new(MockAdb::new(vec![
      // First attempt: GNU format fails
      Ok(vec!["touch: bad '@1700000000.000000000'".to_string()]),
      // Second attempt: legacy format succeeds
      Ok(vec![]),
    ]));
    let ops = DeviceOps::new(mock, ResolvedCompat::legacy());
    let time = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1700000000);
    ops.touch("/sdcard/test", None, Some(time)).await.unwrap();
    assert!(!ops.touch_gnu_mode.load(Ordering::Relaxed));
  }

  #[tokio::test]
  async fn resolve_symlink_works() {
    let ops = DeviceOps::new(Arc::new(MockAdb::new(vec![])), ResolvedCompat::legacy());
    let raw = "lrwxrwxrwx root root 2024-01-01 12:00 link -> /system/lib/libc.so";
    let target = ops.resolve_symlink("/vendor/lib/link", raw).unwrap();
    assert_eq!(target, "../../system/lib/libc.so");
  }
}
