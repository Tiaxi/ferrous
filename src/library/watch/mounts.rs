// SPDX-License-Identifier: GPL-3.0-or-later

//! Which file system a root is on, and whether inotify reports its changes.

use std::path::{Path, PathBuf};

/// The mounts, as mount point and file system type, in the order they were
/// mounted, from `/proc/self/mountinfo`. Empty if it cannot be read.
pub(super) fn mount_table() -> Vec<(PathBuf, String)> {
    let table = std::fs::read_to_string("/proc/self/mountinfo")
        .map(|mountinfo| parse_mountinfo(&mountinfo))
        .unwrap_or_default();
    // Tests mount file systems of their own.
    #[cfg(test)]
    let table = [table, super::watch_test_hooks::fake_mounts()].concat();
    table
}

pub(super) fn parse_mountinfo(mountinfo: &str) -> Vec<(PathBuf, String)> {
    mountinfo.lines().filter_map(parse_mount).collect()
}

/// The type of the file system that `path` is on: the one mounted at the
/// longest mount point above it, the last one there if mounts are stacked.
pub(super) fn mount_type_of(table: &[(PathBuf, String)], path: &Path) -> Option<String> {
    let mut best: Option<(usize, &str)> = None;
    for (mount_point, fs_type) in table {
        if !path.starts_with(mount_point) {
            continue;
        }
        let depth = mount_point.components().count();
        if best.is_none_or(|(best, _)| depth >= best) {
            best = Some((depth, fs_type));
        }
    }
    best.map(|(_, fs_type)| fs_type.to_string())
}

/// The mounts strictly below `root`, sorted by mount point, with the type
/// of the last one mounted at each.
pub(super) fn mounts_below(table: &[(PathBuf, String)], root: &Path) -> Vec<(PathBuf, String)> {
    let mut below = std::collections::BTreeMap::new();
    for (mount_point, fs_type) in table {
        if mount_point != root && mount_point.starts_with(root) {
            below.insert(mount_point.clone(), fs_type.clone());
        }
    }
    below.into_iter().collect()
}

/// The mount point and file system type of a `mountinfo` line:
/// `id parent major:minor root mount-point options [optional...] - type
/// source super-options`.
fn parse_mount(line: &str) -> Option<(PathBuf, String)> {
    let mut fields = line.split(' ');
    let mount_point = fields.nth(4)?;
    let mut fields = fields.skip_while(|field| *field != "-");
    fields.next()?;
    let fs_type = fields.next()?;
    Some((PathBuf::from(unescape(mount_point)), unescape(fs_type)))
}

/// Undoes the octal escapes (`\040` for a space) of `mountinfo` fields.
fn unescape(field: &str) -> String {
    let bytes = field.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut at = 0;
    while at < bytes.len() {
        let escape = bytes.get(at + 1..at + 4).filter(|digits| {
            bytes[at] == b'\\' && digits.iter().all(|digit| (b'0'..=b'7').contains(digit))
        });
        if let Some(byte) =
            escape.and_then(|digits| u8::from_str_radix(std::str::from_utf8(digits).ok()?, 8).ok())
        {
            out.push(byte);
            at += 4;
        } else {
            out.push(bytes[at]);
            at += 1;
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// Whether inotify misses changes on file systems of type `fs_type`:
/// network file systems, where other computers change files, and FUSE file
/// systems whose daemon can change files without the kernel seeing it.
/// `fuseblk` (such as NTFS or exFAT drives) is a local disk that every
/// change goes through.
pub(super) fn misses_changes(fs_type: &str) -> bool {
    const NETWORK: &[&str] = &[
        "nfs",
        "nfs4",
        "cifs",
        "smb3",
        "smbfs",
        "ncpfs",
        "afs",
        "9p",
        "ceph",
        "glusterfs",
        "lustre",
        "gpfs",
        "ocfs2",
        "gfs2",
        "davfs",
        "coda",
        "fuse",
    ];
    NETWORK.contains(&fs_type) || fs_type.starts_with("fuse.")
}
