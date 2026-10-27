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
//! FUSE mounts, which can change when they are mounted again. Every file
//! there takes another identity at once, which no single file can tell from
//! its own replacement. So a changed identity means another file only with
//! more evidence:
//!
//! - **A tie.** Another file the library or the waveform cache knows has
//!   the same size and time, so only identities can tell the two apart, as
//!   in a swap. For a scan, so does a file new to the library that the
//!   same scan finds, as the other file of a swap whose first rename an
//!   earlier scan of changed paths saw alone. The file is read or decoded
//!   again.
//! - **A scan: the other files of the device kept theirs.** A scan counts,
//!   by stored device, the unchanged files that kept their identity and
//!   those that did not. Where most kept theirs, the identities are stable,
//!   and a changed one means another file, such as one swapped in from
//!   outside the library, which shares nothing with any known file. Where
//!   most did not, the file system did not keep them: a full scan stores
//!   the new ones without reading the files, and a scan of a few changed
//!   paths, which cannot tell, leaves the rows to the next full scan.
//! - **A saved waveform: the library's word.** A waveform whose identity
//!   changed without a tie is used only once the library row at its path
//!   has the file's current identity, which a scan stores without reading
//!   the file only where identities are not kept. A scan that reads a file
//!   again because it is another one deletes its saved waveform, and a
//!   decode that ends later is not saved if the file it read has another
//!   identity than the row.
//!
//! A row with no identity yet, as from before identities were stored,
//! takes the file's identity the next time it matches by size and time.
//!
//! What this cannot tell: on a device whose other unchanged files did not
//! keep their identities either, or with no other unchanged file, a file
//! replaced by another of the same size and time that no known file ties
//! with is taken for the same file, as size and time alone took it before.
//! And a saved waveform of a file outside the library whose identity
//! changed is decoded again, as nothing vouches for it: once each time a
//! file system that does not keep identities is mounted again.

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

/// Whether `stored`, the identity a row has for the file at its path,
/// differs from `current`, that of the file there now. An unknown identity
/// differs from none. What a change means is for the caller to tell; see
/// the module comment.
pub(crate) fn identity_changed(stored: Option<FileId>, current: Option<FileId>) -> bool {
    matches!((stored, current), (Some(stored), Some(current)) if stored != current)
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
    fn only_two_known_identities_can_differ() {
        let (a, b) = (
            Some(FileId::from_numbers(1, 10)),
            Some(FileId::from_numbers(1, 11)),
        );
        assert!(identity_changed(a, b));
        assert!(!identity_changed(a, a));
        assert!(!identity_changed(None, b));
        assert!(!identity_changed(a, None));
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
