// SPDX-License-Identifier: GPL-3.0-or-later

use crossbeam_channel::{unbounded, Receiver};

use super::inotify::{parse_events, RawEvent};
use super::mounts::{misses_changes, mount_type_of};
use super::*;

fn tuning() -> WatchTuning {
    WatchTuning {
        quiet: Duration::from_millis(500),
        max_delay: Duration::from_secs(5),
        periodic: Duration::from_secs(1800),
        max_paths: 4,
    }
}

#[test]
fn a_burst_of_changes_is_due_once_after_it_goes_quiet() {
    let tuning = tuning();
    let root = Path::new("/music");
    let start = Instant::now();
    let mut pending = Coalescer::default();
    for (step, name) in ["a.flac", "b.flac", "a.flac"].iter().enumerate() {
        let at = start + Duration::from_millis(100) * u32::try_from(step).expect("step");
        pending.add(root, root.join(name), at, tuning.max_paths);
        assert!(pending.take_due(at, &tuning).is_empty());
    }
    let quiet_at = start + Duration::from_millis(200 + 500);
    assert_eq!(pending.deadline(&tuning), Some(quiet_at));
    let just_before = start + Duration::from_millis(200 + 499);
    assert!(pending.take_due(just_before, &tuning).is_empty());

    let due = pending.take_due(quiet_at, &tuning);
    assert_eq!(due.len(), 1);
    assert_eq!(due[0].0, root);
    assert_eq!(
        due[0].1.paths.iter().cloned().collect::<Vec<_>>(),
        vec![root.join("a.flac"), root.join("b.flac")]
    );
    assert!(!due[0].1.whole);
    assert_eq!(pending.deadline(&tuning), None);
}

#[test]
fn changes_that_keep_coming_are_due_after_the_longest_delay() {
    let tuning = tuning();
    let root = Path::new("/music");
    let start = Instant::now();
    let mut pending = Coalescer::default();
    let mut at = start;
    while at < start + Duration::from_secs(6) {
        pending.add(root, root.join("a.flac"), at, tuning.max_paths);
        at += Duration::from_millis(400);
    }
    assert_eq!(
        pending.deadline(&tuning),
        Some(start + Duration::from_secs(5))
    );
}

#[test]
fn too_many_changed_paths_rescan_the_root_and_roots_are_separate() {
    let tuning = tuning();
    let now = Instant::now();
    let mut pending = Coalescer::default();
    for index in 0..5 {
        pending.add(
            Path::new("/a"),
            PathBuf::from(format!("/a/{index}.flac")),
            now,
            tuning.max_paths,
        );
    }
    pending.add(
        Path::new("/b"),
        PathBuf::from("/b/x.flac"),
        now,
        tuning.max_paths,
    );
    let due = pending.take_due(now + tuning.quiet, &tuning);
    assert_eq!(due.len(), 2);
    assert!(due[0].1.whole && due[0].1.paths.is_empty());
    assert!(!due[1].1.whole);
    assert_eq!(due[1].1.paths.len(), 1);
}

#[test]
fn a_root_held_by_a_test_has_no_deadline() {
    let tuning = tuning();
    let root = crate::paths::test_root().join("watch-held-root");
    let now = Instant::now();
    let mut pending = Coalescer::default();
    pending.add(&root, root.join("a.flac"), now, tuning.max_paths);
    let held = watch_test_hooks::hold_changes(&root);
    assert_eq!(pending.deadline(&tuning), None);
    assert!(pending.take_due(now + tuning.max_delay, &tuning).is_empty());
    drop(held);
    assert_eq!(pending.take_due(now + tuning.max_delay, &tuning).len(), 1);
}

#[test]
fn the_watch_table_forgets_folders_below_a_path_and_shares_descriptors() {
    let mut table = WatchTable::default();
    table.insert(PathBuf::from("/m"), 1);
    table.insert(PathBuf::from("/m/a"), 2);
    table.insert(PathBuf::from("/m/a/b"), 3);
    table.insert(PathBuf::from("/m/a b"), 4);
    // A link to /m/a/b: the same folder, the same descriptor.
    table.insert(PathBuf::from("/m/link"), 3);

    let mut unused = table.remove_below(Path::new("/m/a"));
    unused.sort_unstable();
    assert_eq!(unused, vec![2], "/m/link still uses 3");
    assert_eq!(table.count_below(Path::new("/m")), 3);
    assert_eq!(table.paths.get(&3), Some(&vec![PathBuf::from("/m/link")]));
    assert_eq!(table.forget(4), vec![PathBuf::from("/m/a b")]);
    assert_eq!(table.count_below(Path::new("/m")), 2);
}

#[test]
fn inotify_events_parse_with_and_without_names() {
    let mut bytes = Vec::new();
    for (wd, mask, name) in [
        (3_i32, libc::IN_CREATE, &b"song.flac\0\0\0"[..]),
        (4, libc::IN_IGNORED, &b""[..]),
    ] {
        bytes.extend_from_slice(&wd.to_ne_bytes());
        bytes.extend_from_slice(&mask.to_ne_bytes());
        bytes.extend_from_slice(&0_u32.to_ne_bytes());
        bytes.extend_from_slice(&u32::try_from(name.len()).expect("len").to_ne_bytes());
        bytes.extend_from_slice(name);
    }
    let mut events = Vec::new();
    parse_events(&bytes, &mut events);
    assert_eq!(
        events,
        vec![
            RawEvent {
                wd: 3,
                mask: libc::IN_CREATE,
                name: Some("song.flac".into()),
            },
            RawEvent {
                wd: 4,
                mask: libc::IN_IGNORED,
                name: None,
            },
        ]
    );
    // A truncated record is dropped.
    let mut truncated = Vec::new();
    parse_events(&bytes[..20], &mut truncated);
    assert!(truncated.is_empty());
}

#[test]
fn the_file_system_of_a_root_is_its_deepest_mount() {
    let mountinfo = "\
22 1 0:21 / / rw,relatime shared:1 - btrfs /dev/nvme0n1p3 rw,ssd
40 22 0:35 / /mnt/music rw,relatime shared:20 - nfs4 nas:/music rw,vers=4.2
41 22 0:36 / /mnt/my\\040drive rw,relatime - fuseblk /dev/sdb1 rw
42 22 0:37 / /mnt/cloud rw,nosuid shared:3 master:2 - fuse.rclone remote: rw
43 22 0:38 / /mnt/music rw,relatime - cifs //nas/music rw
";
    let of = |path: &str| mount_type_of(mountinfo, Path::new(path));
    assert_eq!(of("/home/me/Music").as_deref(), Some("btrfs"));
    // Stacked mounts: the last one is seen.
    assert_eq!(of("/mnt/music/Artist").as_deref(), Some("cifs"));
    assert_eq!(of("/mnt/my drive/Album").as_deref(), Some("fuseblk"));
    assert_eq!(of("/mnt/cloud").as_deref(), Some("fuse.rclone"));
    assert_eq!(of("/mnt/musical").as_deref(), Some("btrfs"));

    for watched in ["btrfs", "ext4", "xfs", "tmpfs", "fuseblk", "zfs"] {
        assert!(!misses_changes(watched), "{watched}");
    }
    for missed in [
        "nfs",
        "nfs4",
        "cifs",
        "smb3",
        "fuse.sshfs",
        "fuse.rclone",
        "fuse",
        "9p",
    ] {
        assert!(misses_changes(missed), "{missed}");
    }
}

#[test]
fn unwatched_roots_say_why_and_what_happens_instead() {
    let periodic = Duration::from_secs(30 * 60);
    let reason = Fallback::FileSystem("nfs4".to_string()).describe(periodic);
    assert!(reason.contains("nfs4"), "{reason}");
    assert!(reason.contains("every 30 minutes"), "{reason}");
    let reason = Fallback::WatchLimit.describe(periodic);
    assert!(reason.contains("fs.inotify.max_user_watches"), "{reason}");
    assert_eq!(describe_interval(Duration::from_secs(1)), "1 second");
}

/// A watcher run by the test itself, and the commands it sends.
fn watcher() -> (Watcher, Receiver<LibraryCommand>) {
    let (commands_tx, commands) = unbounded();
    let (_messages_tx, messages) = unbounded();
    let watcher = Watcher::new(
        WatchTuning::default(),
        messages,
        commands_tx,
        Arc::new(AtomicBool::new(false)),
    );
    (watcher, commands)
}

fn test_folder(name: &str) -> PathBuf {
    let folder = crate::paths::test_root().join(format!("watch-{name}"));
    let _ = std::fs::remove_dir_all(&folder);
    std::fs::create_dir_all(&folder).expect("test folder");
    folder.canonicalize().expect("canonical test folder")
}

#[test]
fn watching_starts_with_a_rescan_of_the_roots_that_are_there() {
    let folder = test_folder("catch-up");
    let present = folder.join("present");
    std::fs::create_dir(&present).expect("root");
    let gone = folder.join("gone");
    let (mut watcher, commands) = watcher();
    watcher.handle(WatchMessage::Enabled(true));
    assert!(commands.try_recv().is_err(), "the roots are not known yet");

    watcher.handle(WatchMessage::Roots(vec![present.clone(), gone.clone()]));

    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::RescanRoot(present.clone())]
    );
    assert_eq!(watcher.unwatched.get(&gone), Some(&Fallback::Missing));
    assert!(watcher.table.count_below(&present) > 0);
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn a_root_whose_folder_is_replaced_when_mounts_change_is_watched_again_and_rescanned() {
    let folder = test_folder("remount");
    let root = folder.join("drive");
    std::fs::create_dir(&root).expect("root");
    let (mut watcher, commands) = watcher();
    watcher.handle(WatchMessage::Roots(vec![root.clone()]));
    watcher.handle(WatchMessage::Enabled(true));
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    let first = watcher.table.wds.get(&root).copied();

    // Nothing changed for this root.
    watcher.recheck_root_folders();
    assert!(commands.try_recv().is_err());

    // Another folder at the root's path, as when a drive is mounted there.
    std::fs::rename(&root, folder.join("underneath")).expect("move aside");
    std::fs::create_dir(&root).expect("mounted");
    watcher.recheck_root_folders();
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert_ne!(watcher.table.wds.get(&root).copied(), first);
    assert!(watcher.unwatched.is_empty());

    // Gone, as when unmounted with no folder underneath: not rescanned.
    std::fs::remove_dir(&root).expect("unmounted");
    watcher.recheck_root_folders();
    assert!(commands.try_recv().is_err());
    assert_eq!(watcher.unwatched.get(&root), Some(&Fallback::Missing));
    // Back.
    std::fs::create_dir(&root).expect("mounted again");
    watcher.recheck_root_folders();
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert!(watcher.unwatched.is_empty());
    let _ = std::fs::remove_dir_all(folder);
}
