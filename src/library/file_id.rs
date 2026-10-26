// SPDX-License-Identifier: GPL-3.0-or-later

//! What tells a file apart from another of the same size and modification
//! time, such as the other file of a swap.
//!
//! The library index and the waveform cache tell whether the file at a
//! path changed by its size and modification time, which a rename keeps.
//! When two files with the same size and time swap names, or one replaces
//! the other, neither changes, and each path's row would be taken for the
//! other file. The file's [`FileId`], its device and inode numbers, stored
//! with the row tells them apart.
//!
//! Some file systems do not keep a file's identity: FUSE file systems
//! without stable inode numbers, FAT, and the device numbers of network and
//! FUSE mounts, which can change when they are mounted again. So an
//! identity only breaks a tie (see [`identity_contradicts`]): it is
//! compared when another file has the same size and time, the only case
//! where size and time cannot tell the files apart. It can only turn a
//! match into a miss, never make a row match another file, and on such a
//! file system it costs a miss only for files that share their size and
//! time with another.

use crate::file_identity::FileId;

/// The `file_device` and `file_inode` column values of `id`: the bits of
/// each number, so that every value reads back the same; NULL for none.
pub(crate) fn file_id_columns(id: Option<FileId>) -> (Option<i64>, Option<i64>) {
    id.map_or((None, None), |id| {
        let (device, inode) = id.numbers();
        (Some(device.cast_signed()), Some(inode.cast_signed()))
    })
}

/// The identity that [`file_id_columns`] stored; `None`, unknown, for a row
/// from before identities were stored.
pub(crate) fn file_id_from_columns(device: Option<i64>, inode: Option<i64>) -> Option<FileId> {
    Some(FileId::from_numbers(
        device?.cast_unsigned(),
        inode?.cast_unsigned(),
    ))
}

/// Whether `stored`, the identity a row has for the file at its path, tells
/// that the file there now, of identity `current`, is another file, when
/// their size and modification time are the same.
///
/// That is so only when both are known, they differ, and `tied` tells that
/// another file has the same size and time. Without such a file, size and
/// time already tell any other file apart, and a different identity is
/// taken for one the file system did not keep (see the module comment): the
/// row matches, and takes the current identity. An unknown identity, such
/// as that of a row from before identities were stored, contradicts
/// nothing.
pub(crate) fn identity_contradicts(
    stored: Option<FileId>,
    current: Option<FileId>,
    tied: impl FnOnce() -> bool,
) -> bool {
    match (stored, current) {
        (Some(stored), Some(current)) if stored != current => tied(),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::Path;

    use super::*;

    #[test]
    fn every_identity_reads_back_from_its_columns() {
        for id in [
            FileId::from_numbers(0, 0),
            FileId::from_numbers(u64::MAX, 1 << 63),
        ] {
            let (device, inode) = file_id_columns(Some(id));
            assert_eq!(file_id_from_columns(device, inode), Some(id));
        }
        assert_eq!(file_id_columns(None), (None, None));
        assert_eq!(file_id_from_columns(None, Some(1)), None);
    }

    #[test]
    fn an_identity_breaks_only_a_tie() {
        let (a, b) = (
            Some(FileId::from_numbers(1, 10)),
            Some(FileId::from_numbers(1, 11)),
        );
        assert!(identity_contradicts(a, b, || true));
        assert!(
            !identity_contradicts(a, b, || false),
            "not kept by the file system"
        );
        assert!(!identity_contradicts(a, a, || panic!(
            "not asked when equal"
        )));
        assert!(!identity_contradicts(None, b, || panic!("unknown")));
        assert!(!identity_contradicts(a, None, || panic!("unknown")));
    }

    #[test]
    fn two_files_have_different_identities_and_a_rename_keeps_each() {
        let folder = crate::paths::test_root().join("file-id-rename");
        let _ = fs::remove_dir_all(&folder);
        fs::create_dir_all(&folder).expect("folder");
        let (a, b, c) = (folder.join("a"), folder.join("b"), folder.join("c"));
        fs::write(&a, b"same").expect("a");
        fs::write(&b, b"same").expect("b");
        let id = |path: &Path| FileId::of(&fs::metadata(path).expect("metadata"));
        let (id_a, id_b) = (id(&a), id(&b));
        assert_ne!(id_a, id_b);
        fs::rename(&a, &c).expect("rename");
        assert_eq!(id(&c), id_a);
        fs::remove_dir_all(folder).expect("cleanup");
    }
}
