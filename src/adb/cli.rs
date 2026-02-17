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
    let output = self
      .base_command()
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
    let output = self
      .base_command()
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
    let output = self
      .base_command()
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
    let output = self
      .base_command()
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
