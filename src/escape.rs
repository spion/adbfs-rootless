//! Shell escaping utilities ported from the C++ adbfs codebase.
//!
//! These functions escape special characters for safe use in shell commands
//! passed to local shells and to `adb shell`.

/// Escapes a path for safe embedding inside single-quoted shell strings.
///
/// `'` is replaced with `'\''` (end quote, escaped quote, reopen quote) and
/// `"` is replaced with `\"`.
pub fn shell_escape_path(path: &str) -> String {
  // Order matters: the C++ code processes `'` first, then `"`.
  let s = path.replace('\'', "'\\''");
  s.replace('"', "\\\"")
}

/// Escapes a command string for safe use with a local shell.
///
/// Escapes `\`, `'`, and `` ` `` by prefixing each with a backslash.
/// Backslashes are escaped first to avoid double-escaping.
#[allow(dead_code)]
pub fn shell_escape_command(cmd: &str) -> String {
  let s = cmd.replace('\\', "\\\\");
  let s = s.replace('\'', "\\'");
  s.replace('`', "\\`")
}

/// Characters that need backslash-escaping for `adb shell`, in the order
/// the C++ implementation applies replacements.
const ADB_ESCAPE_CHARS: &[char] = &[
  '\\', '(', ')', '\'', '`', '|', '&', ';', '<', '>', '*', '#', '%', '=', '~',
];

/// ANSI-like color code fragments that the C++ codebase strips.
/// These are literal `/[...m` strings (not real ESC sequences) that appear
/// in some `adb shell` output where the ESC byte has been lost.
const ANSI_STRIP_PATTERNS: &[&str] = &["/[0;0m", "/[1;32m", "/[1;34m", "/[1;36m"];

/// Escapes a command string for safe use with `adb shell`.
///
/// Strips known ANSI-like color code fragments that appear in some device
/// output, then backslash-escapes a broad set of shell metacharacters.
///
/// Note: the C++ code applied stripping after escaping, which meant patterns
/// containing `;` (e.g. `/[0;0m`) could never actually match once `;` was
/// escaped to `\;`. We strip first so the removal is effective.
#[allow(dead_code)]
pub fn adb_shell_escape_command(cmd: &str) -> String {
  // Strip ANSI-like color code fragments first, before any escaping
  // turns their interior punctuation into something else.
  let mut s = cmd.to_owned();
  for pattern in ANSI_STRIP_PATTERNS {
    s = s.replace(pattern, "");
  }

  // Apply backslash escaping in the same order as the C++ code.
  // Backslash must be first to avoid double-escaping the backslashes
  // we introduce for later characters.
  s = s.replace('\\', "\\\\");
  for &ch in &ADB_ESCAPE_CHARS[1..] {
    let find = String::from(ch);
    let replace = format!("\\{ch}");
    s = s.replace(&find, &replace);
  }

  s
}

#[cfg(test)]
mod tests {
  use super::*;
  use proptest::prelude::*;

  // ---- shell_escape_path ----

  #[test]
  fn escape_path_empty() {
    assert_eq!(shell_escape_path(""), "");
  }

  #[test]
  fn escape_path_no_special_chars() {
    assert_eq!(
      shell_escape_path("/mnt/sdcard/photo.jpg"),
      "/mnt/sdcard/photo.jpg"
    );
  }

  #[test]
  fn escape_path_single_quote() {
    assert_eq!(shell_escape_path("it's"), "it'\\''s");
  }

  #[test]
  fn escape_path_double_quote() {
    assert_eq!(shell_escape_path(r#"say "hello""#), r#"say \"hello\""#);
  }

  #[test]
  fn escape_path_both_quotes() {
    assert_eq!(shell_escape_path(r#"it's "fine""#), r#"it'\''s \"fine\""#);
  }

  #[test]
  fn escape_path_multiple_single_quotes() {
    assert_eq!(shell_escape_path("a'b'c"), "a'\\''b'\\''c");
  }

  #[test]
  fn escape_path_only_quotes() {
    assert_eq!(shell_escape_path("'"), "'\\''");
    assert_eq!(shell_escape_path("''"), "'\\'''\\''");
  }

  // ---- shell_escape_command ----

  #[test]
  fn escape_command_empty() {
    assert_eq!(shell_escape_command(""), "");
  }

  #[test]
  fn escape_command_no_special_chars() {
    assert_eq!(shell_escape_command("ls -la"), "ls -la");
  }

  #[test]
  fn escape_command_backslash() {
    assert_eq!(shell_escape_command(r"a\b"), r"a\\b");
  }

  #[test]
  fn escape_command_single_quote() {
    assert_eq!(shell_escape_command("it's"), "it\\'s");
  }

  #[test]
  fn escape_command_backtick() {
    assert_eq!(shell_escape_command("echo `date`"), "echo \\`date\\`");
  }

  #[test]
  fn escape_command_all_three() {
    assert_eq!(shell_escape_command(r"a\b'c`d"), r"a\\b\'c\`d");
  }

  #[test]
  fn escape_command_double_backslash() {
    // `\\` -> `\\\\`
    assert_eq!(shell_escape_command(r"\\"), r"\\\\");
  }

  // ---- adb_shell_escape_command ----

  #[test]
  fn adb_escape_empty() {
    assert_eq!(adb_shell_escape_command(""), "");
  }

  #[test]
  fn adb_escape_no_special_chars() {
    assert_eq!(adb_shell_escape_command("ls -la /sdcard"), "ls -la /sdcard");
  }

  #[test]
  fn adb_escape_backslash() {
    assert_eq!(adb_shell_escape_command(r"a\b"), r"a\\b");
  }

  #[test]
  fn adb_escape_parentheses() {
    assert_eq!(adb_shell_escape_command("f(x)"), r"f\(x\)");
  }

  #[test]
  fn adb_escape_single_quote() {
    assert_eq!(adb_shell_escape_command("it's"), "it\\'s");
  }

  #[test]
  fn adb_escape_backtick() {
    assert_eq!(adb_shell_escape_command("`cmd`"), "\\`cmd\\`");
  }

  #[test]
  fn adb_escape_pipe() {
    assert_eq!(adb_shell_escape_command("a|b"), r"a\|b");
  }

  #[test]
  fn adb_escape_ampersand() {
    assert_eq!(adb_shell_escape_command("a&b"), r"a\&b");
  }

  #[test]
  fn adb_escape_semicolon() {
    assert_eq!(adb_shell_escape_command("a;b"), r"a\;b");
  }

  #[test]
  fn adb_escape_angle_brackets() {
    assert_eq!(adb_shell_escape_command("a<b>c"), r"a\<b\>c");
  }

  #[test]
  fn adb_escape_star() {
    assert_eq!(adb_shell_escape_command("*.txt"), r"\*.txt");
  }

  #[test]
  fn adb_escape_hash() {
    assert_eq!(adb_shell_escape_command("# comment"), r"\# comment");
  }

  #[test]
  fn adb_escape_percent() {
    assert_eq!(adb_shell_escape_command("100%"), r"100\%");
  }

  #[test]
  fn adb_escape_equals() {
    assert_eq!(adb_shell_escape_command("a=b"), r"a\=b");
  }

  #[test]
  fn adb_escape_tilde() {
    assert_eq!(adb_shell_escape_command("~user"), r"\~user");
  }

  #[test]
  fn adb_escape_all_special_chars() {
    let input = r"\()'`|&;<>*#%=~";
    let expected = r"\\\(\)\'\`\|\&\;\<\>\*\#\%\=\~";
    assert_eq!(adb_shell_escape_command(input), expected);
  }

  #[test]
  fn adb_escape_strips_ansi_codes() {
    assert_eq!(adb_shell_escape_command("hello/[0;0m world"), "hello world");
    assert_eq!(adb_shell_escape_command("/[1;32mgreen"), "green");
    assert_eq!(adb_shell_escape_command("blue/[1;34m"), "blue");
    assert_eq!(adb_shell_escape_command("/[1;36mcyan/[0;0m"), "cyan");
  }

  #[test]
  fn adb_escape_strips_ansi_and_escapes() {
    // ANSI stripping happens before escaping, so the pattern is removed
    // cleanly and then special chars in the remaining text are escaped.
    let input = "file/[0;0m|name";
    let result = adb_shell_escape_command(input);
    assert_eq!(result, r"file\|name");
  }

  #[test]
  fn adb_escape_multiple_specials_together() {
    assert_eq!(adb_shell_escape_command("a|b&c;d"), r"a\|b\&c\;d");
  }

  // ---- proptest ----

  proptest! {
      /// Every single quote in the escaped output must be part of the
      /// `'\''` escape sequence. After stripping all `'\''` occurrences,
      /// no bare single quotes should remain.
      #[test]
      fn escape_path_all_quotes_properly_escaped(s in ".*") {
          let escaped = shell_escape_path(&s);
          let stripped = escaped.replace("'\\''", "");
          prop_assert!(
              !stripped.contains('\''),
              "Escaped output contains bare single quote for input {:?}: escaped={:?}, after stripping escape sequences={:?}",
              s,
              escaped,
              stripped,
          );
      }
  }
}
