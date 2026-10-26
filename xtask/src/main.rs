//! Tác vụ repo, chạy bằng `cargo xtask <lệnh>`:
//!
//! - `bless`: ghi lại mọi golden replay sau khi đổi hành vi core có chủ đích.

use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

fn root() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .expect("xtask nằm trong workspace")
        .to_path_buf()
}

fn run(desc: &str, cmd: &mut Command) -> bool {
    eprintln!("==> {desc}");
    match cmd.current_dir(root()).status() {
        Ok(s) if s.success() => true,
        Ok(s) => {
            eprintln!("thất bại: {desc} ({s})");
            false
        }
        Err(e) => {
            eprintln!("không chạy được {desc}: {e}");
            false
        }
    }
}

fn cargo() -> Command {
    Command::new(std::env::var("CARGO").unwrap_or_else(|_| "cargo".into()))
}

fn replays() -> Vec<PathBuf> {
    let dir = root().join("fixtures/replays");
    let mut files: Vec<PathBuf> = std::fs::read_dir(&dir)
        .map(|it| {
            it.filter_map(|e| e.ok().map(|e| e.path()))
                .filter(|p| p.extension().is_some_and(|x| x == "toml"))
                .collect()
        })
        .unwrap_or_default();
    files.sort();
    files
}

fn replay(cmd: &str) -> bool {
    replays().iter().all(|file| {
        run(
            &format!("replay {cmd} {}", file.display()),
            cargo()
                .args([
                    "run",
                    "--quiet",
                    "--release",
                    "-p",
                    "boom-replay",
                    "--",
                    cmd,
                ])
                .arg(file),
        )
    })
}

fn main() -> ExitCode {
    let ok = match std::env::args().nth(1).as_deref() {
        Some("bless") => replay("bless"),
        _ => {
            eprintln!("usage: cargo xtask bless");
            return ExitCode::from(2);
        }
    };
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
