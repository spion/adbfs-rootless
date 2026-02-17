use std::ffi::CString;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

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

/// Converts a 10-character mode string (e.g. "drwxr-xr-x") to raw mode bits.
///
/// Type characters: s=SOCK, l=LNK, -=REG, d=DIR, b=BLK, c=CHR, p=FIFO.
/// Permission triplet handles s/S (setuid/setgid) and t/T (sticky).
pub fn parse_mode_string(s: &str) -> u32 {
  let b = s.as_bytes();
  if b.len() < 10 {
    return 0;
  }

  let mut mode: u32 = 0;

  // File type from first character
  mode |= match b[0] {
    b's' => libc::S_IFSOCK,
    b'l' => libc::S_IFLNK,
    b'-' => libc::S_IFREG,
    b'd' => libc::S_IFDIR,
    b'b' => libc::S_IFBLK,
    b'c' => libc::S_IFCHR,
    b'p' => libc::S_IFIFO,
    _ => 0,
  };

  // Owner permissions
  if b[1] == b'r' {
    mode |= libc::S_IRUSR;
  }
  if b[2] == b'w' {
    mode |= libc::S_IWUSR;
  }
  match b[3] {
    b'x' => mode |= libc::S_IXUSR,
    b's' => mode |= libc::S_ISUID | libc::S_IXUSR,
    b'S' => mode |= libc::S_ISUID,
    _ => {}
  }

  // Group permissions
  if b[4] == b'r' {
    mode |= libc::S_IRGRP;
  }
  if b[5] == b'w' {
    mode |= libc::S_IWGRP;
  }
  match b[6] {
    b'x' => mode |= libc::S_IXGRP,
    b's' => mode |= libc::S_ISGID | libc::S_IXGRP,
    b'S' => mode |= libc::S_ISGID,
    _ => {}
  }

  // Other permissions
  if b[7] == b'r' {
    mode |= libc::S_IROTH;
  }
  if b[8] == b'w' {
    mode |= libc::S_IWOTH;
  }
  match b[9] {
    b'x' => mode |= libc::S_IXOTH,
    b't' => mode |= libc::S_ISVTX | libc::S_IXOTH,
    b'T' => mode |= libc::S_ISVTX,
    _ => {}
  }

  mode
}

/// Heuristic to determine whether a line is valid `ls` output rather than an
/// error message. Returns false if the first char is '/' or the second char is
/// not 'r' or '-'. Also false for empty/short lines.
pub fn is_valid_ls_output(line: &str) -> bool {
  let b = line.as_bytes();
  if b.len() < 2 {
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

/// Split on whitespace the same way the C++ `make_array` does (split_whitespace).
fn split_fields(s: &str) -> Vec<&str> {
  s.split_whitespace().collect()
}

/// Resolve a username to a UID via `getpwnam`, falling back to 98.
fn resolve_uid(name: &str) -> u32 {
  let Ok(cname) = CString::new(name) else {
    return 98;
  };
  // SAFETY: getpwnam is called with a valid C string. The returned pointer
  // is only read immediately and not stored.
  let pw = unsafe { libc::getpwnam(cname.as_ptr()) };
  if pw.is_null() {
    98
  } else {
    unsafe { (*pw).pw_uid }
  }
}

/// Resolve a group name to a GID via `getgrnam`, falling back to 98.
fn resolve_gid(name: &str) -> u32 {
  let Ok(cname) = CString::new(name) else {
    return 98;
  };
  // SAFETY: getgrnam is called with a valid C string. The returned pointer
  // is only read immediately and not stored.
  let gr = unsafe { libc::getgrnam(cname.as_ptr()) };
  if gr.is_null() {
    98
  } else {
    unsafe { (*gr).gr_gid }
  }
}

/// Parse date and time fields ("YYYY-MM-DD" and "HH:MM") into a `SystemTime`
/// using `libc::mktime`, matching the original C++ behavior.
fn parse_datetime(date_str: &str, time_str: &str) -> Option<SystemTime> {
  let ymd: Vec<&str> = date_str.split('-').collect();
  let hm: Vec<&str> = time_str.split(':').collect();
  if ymd.len() < 3 || hm.len() < 2 {
    return None;
  }

  let year: i32 = ymd[0].parse().ok()?;
  let mon: i32 = ymd[1].parse().ok()?;
  let mday: i32 = ymd[2].parse().ok()?;
  let hour: i32 = hm[0].parse().ok()?;
  let min: i32 = hm[1].parse().ok()?;

  // SAFETY: zeroed tm is valid; mktime is safe to call with a mutable pointer.
  unsafe {
    let mut tm: libc::tm = std::mem::zeroed();
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = mday;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    let epoch_secs = libc::mktime(&mut tm);
    if epoch_secs == -1 {
      return None;
    }
    Some(UNIX_EPOCH + Duration::from_secs(epoch_secs as u64))
  }
}

/// Parse a single line of `ls -l -a [-d]` output into a `FileMeta`.
///
/// Handles two Android ls formats:
/// - Without nlink: `-rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html`
/// - With nlink:    `-rw-r--r--   1 root   root      5905 1970-01-01 01:00 ueventd.rc`
///
/// Detection: if field\[1\] parses as u32 > 0 it is nlink, otherwise uid starts
/// at field\[1\].
pub fn parse_ls_line(line: &str) -> Option<FileMeta> {
  let fields = split_fields(line);
  if fields.len() < 6 {
    return None;
  }

  if !is_valid_ls_output(fields[0]) {
    return None;
  }

  let mode = parse_mode_string(fields[0]);

  // Detect nlink field
  let (nlink, uid_offset) = match fields[1].parse::<u32>() {
    Ok(n) if n > 0 => (n, 1usize),
    _ => (1u32, 0usize),
  };

  // uid and gid fields
  let uid_field = *fields.get(uid_offset + 1)?;
  let gid_field = *fields.get(uid_offset + 2)?;
  let uid = resolve_uid(uid_field);
  let gid = resolve_gid(gid_field);

  let file_type = mode & libc::S_IFMT;

  let (size, rdev, date_idx) = match file_type {
    ft if ft == libc::S_IFBLK || ft == libc::S_IFCHR => {
      // major, minor device numbers
      let major_str = fields.get(uid_offset + 3)?;
      let minor_str = fields.get(uid_offset + 4)?;
      // major may have trailing comma
      let major: u64 = major_str.trim_end_matches(',').parse().unwrap_or(0);
      let minor: u64 = minor_str.parse().unwrap_or(0);
      (0u64, major * 256 + minor, uid_offset + 5)
    }
    ft if ft == libc::S_IFREG => {
      let size: u64 = fields.get(uid_offset + 3)?.parse().unwrap_or(0);
      (size, 0u64, uid_offset + 4)
    }
    _ => {
      // DIR, LNK, SOCK, FIFO: size is 0, but the size field may or
      // may not be present. If the candidate field doesn't contain '-'
      // (which dates always have), it's a size field and we skip it.
      let candidate_idx = uid_offset + 3;
      let candidate = *fields.get(candidate_idx)?;
      let date_idx = if candidate.contains('-') {
        candidate_idx
      } else {
        candidate_idx + 1
      };
      (0u64, 0u64, date_idx)
    }
  };

  let date_str = *fields.get(date_idx)?;
  let time_str = *fields.get(date_idx + 1)?;
  let mtime = parse_datetime(date_str, time_str)?;

  Some(FileMeta {
    mode,
    nlink,
    uid,
    gid,
    size,
    rdev,
    mtime,
    raw_line: line.to_owned(),
  })
}

/// Extract the filename from an `ls -la` output line.
///
/// Finds the first ':' (the time separator in HH:MM), skips 4 characters past
/// it (the minutes + space), and returns the remainder. For symlinks the
/// " -> target" suffix is stripped.
pub fn extract_filename(line: &str) -> Option<&str> {
  let colon_pos = line.find(':')?;
  let name_start = colon_pos + 4;
  if name_start > line.len() {
    return None;
  }
  let name = &line[name_start..];
  // Strip symlink target
  let name = match name.find(" -> ") {
    Some(pos) => &name[..pos],
    None => name,
  };
  if name.is_empty() { None } else { Some(name) }
}

/// Extract and relativize the symlink target from an `ls -l` line.
///
/// If the target is absolute (starts with '/'), it is converted to a relative
/// path by prepending `../` repeated `(num_slashes_in_path - 1)` times and
/// stripping the leading slashes from the target.
pub fn parse_symlink_target(ls_line: &str, num_slashes_in_path: usize) -> Option<String> {
  let arrow_pos = ls_line.find(" -> ")?;
  let target = &ls_line[arrow_pos + 4..];
  if target.is_empty() {
    return None;
  }

  if target.starts_with('/') {
    let depth = num_slashes_in_path.saturating_sub(1);
    let trimmed = target.trim_start_matches('/');
    let mut result = "../".repeat(depth);
    result.push_str(trimmed);
    Some(result)
  } else {
    Some(target.to_owned())
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  // ---- parse_mode_string tests ----

  #[test]
  fn mode_regular_file_rw() {
    let mode = parse_mode_string("-rw-r--r--");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFREG as u32);
    assert_ne!(mode & libc::S_IRUSR as u32, 0);
    assert_ne!(mode & libc::S_IWUSR as u32, 0);
    assert_eq!(mode & libc::S_IXUSR as u32, 0);
    assert_ne!(mode & libc::S_IRGRP as u32, 0);
    assert_eq!(mode & libc::S_IWGRP as u32, 0);
    assert_ne!(mode & libc::S_IROTH as u32, 0);
    assert_eq!(mode & libc::S_IWOTH as u32, 0);
  }

  #[test]
  fn mode_directory() {
    let mode = parse_mode_string("drwxr-xr-x");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFDIR as u32);
    assert_ne!(mode & libc::S_IXUSR as u32, 0);
    assert_ne!(mode & libc::S_IXGRP as u32, 0);
    assert_ne!(mode & libc::S_IXOTH as u32, 0);
  }

  #[test]
  fn mode_symlink() {
    let mode = parse_mode_string("lrwxrwxrwx");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFLNK as u32);
    assert_ne!(mode & libc::S_IRWXU as u32, 0);
    assert_ne!(mode & libc::S_IRWXG as u32, 0);
    assert_ne!(mode & libc::S_IRWXO as u32, 0);
  }

  #[test]
  fn mode_socket() {
    let mode = parse_mode_string("srwxrwxrwx");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFSOCK as u32);
  }

  #[test]
  fn mode_block_device() {
    let mode = parse_mode_string("brw-rw----");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFBLK as u32);
  }

  #[test]
  fn mode_char_device() {
    let mode = parse_mode_string("crw-rw-rw-");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFCHR as u32);
  }

  #[test]
  fn mode_fifo() {
    let mode = parse_mode_string("prw-r--r--");
    assert_eq!(mode & libc::S_IFMT as u32, libc::S_IFIFO as u32);
  }

  #[test]
  fn mode_setuid_with_exec() {
    let mode = parse_mode_string("-rwsr-xr-x");
    assert_ne!(mode & libc::S_ISUID as u32, 0);
    assert_ne!(mode & libc::S_IXUSR as u32, 0);
  }

  #[test]
  fn mode_setuid_without_exec() {
    let mode = parse_mode_string("-rwSr-xr-x");
    assert_ne!(mode & libc::S_ISUID as u32, 0);
    assert_eq!(mode & libc::S_IXUSR as u32, 0);
  }

  #[test]
  fn mode_setgid_with_exec() {
    let mode = parse_mode_string("-rwxr-sr-x");
    assert_ne!(mode & libc::S_ISGID as u32, 0);
    assert_ne!(mode & libc::S_IXGRP as u32, 0);
  }

  #[test]
  fn mode_setgid_without_exec() {
    let mode = parse_mode_string("-rwxr-Sr-x");
    assert_ne!(mode & libc::S_ISGID as u32, 0);
    assert_eq!(mode & libc::S_IXGRP as u32, 0);
  }

  #[test]
  fn mode_sticky_with_exec() {
    let mode = parse_mode_string("drwxr-xr-t");
    assert_ne!(mode & libc::S_ISVTX as u32, 0);
    assert_ne!(mode & libc::S_IXOTH as u32, 0);
  }

  #[test]
  fn mode_sticky_without_exec() {
    let mode = parse_mode_string("drwxr-xr-T");
    assert_ne!(mode & libc::S_ISVTX as u32, 0);
    assert_eq!(mode & libc::S_IXOTH as u32, 0);
  }

  // ---- is_valid_ls_output tests ----

  #[test]
  fn valid_ls_regular_file() {
    assert!(is_valid_ls_output(
      "-rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html"
    ));
  }

  #[test]
  fn valid_ls_directory() {
    assert!(is_valid_ls_output(
      "drwxr-xr-x root     root              2012-06-22 02:16 somedir"
    ));
  }

  #[test]
  fn invalid_ls_error_message() {
    assert!(!is_valid_ls_output("/data/misc: Permission denied"));
  }

  #[test]
  fn invalid_ls_opendir_error() {
    assert!(!is_valid_ls_output(
      "opendir failed, No such file or directory"
    ));
  }

  #[test]
  fn invalid_ls_empty() {
    assert!(!is_valid_ls_output(""));
  }

  #[test]
  fn invalid_ls_short() {
    assert!(!is_valid_ls_output("x"));
  }

  // ---- parse_ls_line tests ----

  #[test]
  fn parse_regular_file_without_nlink() {
    let line = "-rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html";
    let meta = parse_ls_line(line).expect("should parse");
    assert_eq!(meta.mode & libc::S_IFMT as u32, libc::S_IFREG as u32);
    assert_eq!(meta.nlink, 1);
    assert_eq!(meta.size, 763362);
    assert_eq!(meta.rdev, 0);
    assert_eq!(meta.raw_line, line);
  }

  #[test]
  fn parse_regular_file_with_nlink() {
    let line = "-rw-r--r--   1 root   root      5905 1970-01-01 01:00 ueventd.rc";
    let meta = parse_ls_line(line).expect("should parse");
    assert_eq!(meta.mode & libc::S_IFMT as u32, libc::S_IFREG as u32);
    assert_eq!(meta.nlink, 1);
    assert_eq!(meta.size, 5905);
  }

  #[test]
  fn parse_directory() {
    let line = "drwxr-xr-x root     root              2012-06-22 02:16 somedir";
    let meta = parse_ls_line(line).expect("should parse");
    assert_eq!(meta.mode & libc::S_IFMT as u32, libc::S_IFDIR as u32);
    assert_eq!(meta.size, 0);
  }

  #[test]
  fn parse_directory_with_nlink() {
    let line = "drwxr-xr-x   2 root   root      4096 1970-01-01 01:00 dev";
    let meta = parse_ls_line(line).expect("should parse");
    assert_eq!(meta.mode & libc::S_IFMT as u32, libc::S_IFDIR as u32);
    assert_eq!(meta.nlink, 2);
    // size field is present for DIR but we skip it since it's not a date
    assert_eq!(meta.size, 0);
  }

  #[test]
  fn parse_symlink() {
    let line = "lrwxrwxrwx root     root              2012-06-22 02:16 link -> /target";
    let meta = parse_ls_line(line).expect("should parse");
    assert_eq!(meta.mode & libc::S_IFMT as u32, libc::S_IFLNK as u32);
    assert_eq!(meta.size, 0);
  }

  #[test]
  fn parse_char_device() {
    let line = "crw-rw-rw- root     root     10, 123 1970-01-01 01:00 urandom";
    let meta = parse_ls_line(line).expect("should parse");
    assert_eq!(meta.mode & libc::S_IFMT as u32, libc::S_IFCHR as u32);
    assert_eq!(meta.rdev, 10 * 256 + 123);
    assert_eq!(meta.size, 0);
  }

  #[test]
  fn parse_invalid_line() {
    assert!(parse_ls_line("/data: Permission denied").is_none());
  }

  #[test]
  fn parse_empty_line() {
    assert!(parse_ls_line("").is_none());
  }

  // ---- extract_filename tests ----

  #[test]
  fn extract_filename_regular() {
    let line = "-rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 file.html";
    assert_eq!(extract_filename(line), Some("file.html"));
  }

  #[test]
  fn extract_filename_with_spaces() {
    let line = "-rw-rw-r-- root     sdcard_rw   763362 2012-06-22 02:16 my file name.txt";
    assert_eq!(extract_filename(line), Some("my file name.txt"));
  }

  #[test]
  fn extract_filename_symlink() {
    let line = "lrwxrwxrwx root     root              2012-06-22 02:16 link -> /target";
    assert_eq!(extract_filename(line), Some("link"));
  }

  #[test]
  fn extract_filename_no_colon() {
    assert_eq!(extract_filename("no time field here"), None);
  }

  // ---- parse_symlink_target tests ----

  #[test]
  fn symlink_absolute_to_relative() {
    let line = "lrwxrwxrwx root root 2012-06-22 02:16 link -> /system/bin/sh";
    // path = /system/link has 2 slashes, so depth = 2-1 = 1
    let result = parse_symlink_target(line, 2).expect("should parse");
    assert_eq!(result, "../system/bin/sh");
  }

  #[test]
  fn symlink_absolute_deep_path() {
    let line = "lrwxrwxrwx root root 2012-06-22 02:16 link -> /data/local/tmp";
    // path with 4 slashes => depth = 3
    let result = parse_symlink_target(line, 4).expect("should parse");
    assert_eq!(result, "../../../data/local/tmp");
  }

  #[test]
  fn symlink_relative_target() {
    let line = "lrwxrwxrwx root root 2012-06-22 02:16 link -> ../other/file";
    let result = parse_symlink_target(line, 2).expect("should parse");
    assert_eq!(result, "../other/file");
  }

  #[test]
  fn symlink_no_arrow() {
    let line = "-rw-r--r-- root root 5905 1970-01-01 01:00 regular_file";
    assert!(parse_symlink_target(line, 2).is_none());
  }

  #[test]
  fn symlink_absolute_root_path() {
    let line = "lrwxrwxrwx root root 2012-06-22 02:16 link -> /init";
    // path with 1 slash (root level): depth = 0
    let result = parse_symlink_target(line, 1).expect("should parse");
    assert_eq!(result, "init");
  }
}

#[cfg(test)]
mod proptests {
  use super::*;
  use proptest::prelude::*;

  fn arb_mode_string() -> impl Strategy<Value = String> {
    let type_char = prop_oneof![
      Just('s'),
      Just('l'),
      Just('-'),
      Just('d'),
      Just('b'),
      Just('c'),
      Just('p'),
    ];
    let rw = prop::sample::select(vec!['-', 'r']);
    let ww = prop::sample::select(vec!['-', 'w']);
    let owner_x = prop::sample::select(vec!['-', 'x', 's', 'S']);
    let group_x = prop::sample::select(vec!['-', 'x', 's', 'S']);
    let other_x = prop::sample::select(vec!['-', 'x', 't', 'T']);

    (
      type_char,
      rw.clone(),
      ww.clone(),
      owner_x,
      rw.clone(),
      ww.clone(),
      group_x,
      rw,
      ww,
      other_x,
    )
      .prop_map(|(t, r1, w1, x1, r2, w2, x2, r3, w3, x3)| {
        format!(
          "{}{}{}{}{}{}{}{}{}{}",
          t, r1, w1, x1, r2, w2, x2, r3, w3, x3
        )
      })
  }

  proptest! {
      #[test]
      fn mode_string_always_has_file_type(s in arb_mode_string()) {
          let mode = parse_mode_string(&s);
          let file_type = mode & libc::S_IFMT as u32;
          // Must have one of the known file type bits set
          let valid_types = [
              libc::S_IFSOCK as u32,
              libc::S_IFLNK as u32,
              libc::S_IFREG as u32,
              libc::S_IFDIR as u32,
              libc::S_IFBLK as u32,
              libc::S_IFCHR as u32,
              libc::S_IFIFO as u32,
          ];
          prop_assert!(
              valid_types.contains(&file_type),
              "File type bits 0o{:o} not in valid set for mode string '{}'",
              file_type,
              s
          );
      }
  }
}
