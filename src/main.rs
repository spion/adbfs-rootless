mod adb;
mod cache;
mod escape;
mod fs;
mod ops;
mod parse;

use std::sync::Arc;
use std::time::Duration;

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

  let rt = tokio::runtime::Runtime::new()?;
  let adb = Arc::new(adb::cli::AdbCli::new(None));
  let device_ops =
    Arc::new(ops::DeviceOps::new(adb, ops::ResolvedCompat::legacy()).with_rescan(cli.rescan));

  let adbfs = fs::AdbFs::new(
    device_ops,
    Duration::from_secs(cli.cache_ttl),
    rt.handle().clone(),
  )?;

  fs::mount(adbfs, &cli.mountpoint, cli.options)?;
  Ok(())
}
