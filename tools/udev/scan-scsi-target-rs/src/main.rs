// SPDX-License-Identifier: GPL-2.0-or-later
//
// Scan a SCSI target given a uevent path to one of its devices.
// Rust rewrite of scan-scsi-target.c
//
// Original author: Ewan D. Milne <emilne@redhat.com>
// Copyright (C) 2013, Red Hat Inc.

use std::fs::{self, File, OpenOptions};
use std::io::Write;
use std::process::ExitCode;

fn usage(prog: &str, err: bool) -> ExitCode {
    eprintln!("\nUsage:");
    eprintln!("{prog} <uevent DEVPATH of SCSI device>");
    eprintln!("\nOptions:");
    eprintln!("  -h, --help     display this help and exit");
    if err {
        ExitCode::FAILURE
    } else {
        ExitCode::SUCCESS
    }
}

fn invalid(prog: &str, devpath: &str) -> ExitCode {
    eprintln!("Invalid DEVPATH '{devpath}'.");
    usage(prog, true)
}

/// Format an io::Error the same way C's strerror() does — without the
/// "(os error N)" suffix that Rust's Display impl appends.
fn strerror(e: &std::io::Error) -> String {
    let full = e.to_string();
    match e.raw_os_error() {
        Some(code) => {
            let suffix = format!(" (os error {code})");
            full.strip_suffix(&suffix).unwrap_or(&full).to_string()
        }
        None => full,
    }
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let prog = args.first().map(|s| s.as_str()).unwrap_or("scan-scsi-target");

    let mut devpath = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "-h" | "--help" => {
                return usage(prog, false);
            }
            "--" => {
                i += 1;
                if devpath.is_none() && i < args.len() {
                    devpath = Some(args[i].as_str());
                }
                break;
            }
            s if s.starts_with('-') => {
                return usage(prog, true);
            }
            s if devpath.is_none() => {
                devpath = Some(s);
            }
            _ => {}
        }
        i += 1;
    }

    let devpath = match devpath {
        Some(d) => d,
        None => return usage(prog, true),
    };

    match run(devpath) {
        Ok(()) => ExitCode::SUCCESS,
        Err(InvalidDevpath) => invalid(prog, devpath),
        Err(IoError(msg)) => {
            eprintln!("{msg}");
            usage(prog, true)
        }
    }
}

enum RunError {
    InvalidDevpath,
    IoError(String),
}
use RunError::*;

fn run(devpath: &str) -> Result<(), RunError> {
    let full_path = format!("/sys{devpath}");
    let meta = fs::metadata(&full_path)
        .map_err(|e| IoError(format!("Cannot stat '{full_path}': {}", strerror(&e))))?;
    if !meta.is_dir() {
        return Err(InvalidDevpath);
    }

    let host_pos = find_host_segment(devpath).ok_or(InvalidDevpath)?;
    let host_seg = &devpath[host_pos..];

    let host_end = host_pos + 1 + host_seg[1..].find('/').ok_or(InvalidDevpath)?;

    let host_name = &devpath[host_pos..host_end];
    if host_name.len() <= "/host".len() {
        return Err(InvalidDevpath);
    }

    let target_pos = devpath.find("/target").ok_or(InvalidDevpath)?;
    let target_seg = &devpath[target_pos..];
    if target_seg.len() <= "/target".len() {
        return Err(InvalidDevpath);
    }

    let prefix = &devpath[..host_end];
    let sysfs_path = format!("/sys{prefix}/scsi_host{host_name}/scan");

    let (channel, id) = parse_target(target_seg).ok_or(InvalidDevpath)?;

    let scan_data = format!("{channel} {id} -");

    let mut fd: File = OpenOptions::new()
        .write(true)
        .open(&sysfs_path)
        .map_err(|e| IoError(format!("Cannot open '{sysfs_path}': {}", strerror(&e))))?;

    fd.write_all(scan_data.as_bytes())
        .map_err(|e| IoError(format!("Cannot write '{sysfs_path}': {}", strerror(&e))))?;

    Ok(())
}

/// Find the byte offset of the relevant `/host<N>` segment.
/// If `/vport` appears anywhere in the path, skip to the second `/host`.
fn find_host_segment(devpath: &str) -> Option<usize> {
    let first = devpath.find("/host")?;

    if devpath.contains("/vport") {
        let after_first = first + 1;
        devpath[after_first..].find("/host").map(|p| p + after_first)
    } else {
        Some(first)
    }
}

fn parse_target(target_seg: &str) -> Option<(&str, &str)> {
    let rest = &target_seg["/target".len()..];
    let target_part = rest.split('/').next()?;

    let mut parts = target_part.splitn(3, ':');
    let _host = parts.next()?;
    let channel = parts.next().filter(|s| !s.is_empty())?;
    let id = parts.next().filter(|s| !s.is_empty())?;

    Some((channel, id))
}
