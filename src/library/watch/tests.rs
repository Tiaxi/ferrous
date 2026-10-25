// SPDX-License-Identifier: GPL-3.0-or-later

use crossbeam_channel::{unbounded, Receiver};

use super::inotify::{parse_events, RawEvent};
use super::mounts::{
    misses_changes, mount_type_of, mounts_at_or_below, mounts_below, parse_mountinfo,
};
use super::*;

fn tuning() -> WatchTuning {
    WatchTuning {
        quiet: Duration::from_millis(500),
        max_delay: Duration::from_secs(5),
        periodic: Duration::from_secs(1800),
        max_paths: 4,
        mount_rescan: Duration::from_secs(300),
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
fn mount_rescans_are_due_after_a_burst_and_at_most_once_per_interval() {
    let tuning = tuning();
    let root = Path::new("/music");
    let mut rescans = MountRescans::default();
    let start = Instant::now();
    rescans.request(root, start, &tuning);
    rescans.request(root, start + Duration::from_millis(100), &tuning);
    assert_eq!(rescans.deadline(), Some(start + tuning.quiet));

    // A root gone when its rescan is due is not rescanned, and is rescanned
    // as soon as it is back.
    assert!(rescans.take_due(start + tuning.quiet, |_| false).is_empty());
    assert_eq!(rescans.deadline(), None);
    let back = start + Duration::from_secs(1);
    rescans.request(root, back, &tuning);
    assert_eq!(rescans.deadline(), Some(back + tuning.quiet));
    let sent = back + tuning.quiet;
    assert_eq!(rescans.take_due(sent, |_| true), vec![root.to_path_buf()]);

    // Changes after it wait for the interval.
    rescans.request(root, sent + Duration::from_secs(1), &tuning);
    assert!(rescans
        .take_due(sent + Duration::from_secs(2), |_| true)
        .is_empty());
    assert_eq!(rescans.deadline(), Some(sent + tuning.mount_rescan));
    rescans.forget(root);
    assert_eq!(rescans.deadline(), None);
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
    let table = parse_mountinfo(mountinfo);
    let of = |path: &str| mount_type_of(&table, Path::new(path));
    assert_eq!(of("/home/me/Music").as_deref(), Some("btrfs"));
    // Stacked mounts: the last one is seen.
    assert_eq!(of("/mnt/music/Artist").as_deref(), Some("cifs"));
    assert_eq!(of("/mnt/my drive/Album").as_deref(), Some("fuseblk"));
    assert_eq!(of("/mnt/cloud").as_deref(), Some("fuse.rclone"));
    assert_eq!(of("/mnt/musical").as_deref(), Some("btrfs"));
    assert_eq!(
        mounts_below(&table, Path::new("/mnt")),
        vec![
            (PathBuf::from("/mnt/cloud"), "fuse.rclone".to_string()),
            (PathBuf::from("/mnt/music"), "cifs".to_string()),
            (PathBuf::from("/mnt/my drive"), "fuseblk".to_string()),
        ]
    );
    assert!(mounts_below(&table, Path::new("/mnt/music")).is_empty());
    assert_eq!(
        mounts_at_or_below(&table, Path::new("/mnt/music")),
        vec![(PathBuf::from("/mnt/music"), "cifs".to_string())]
    );

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
    watcher.recheck_mounts();
    assert!(commands.try_recv().is_err());

    // Another folder at the root's path, as when a drive is mounted there.
    std::fs::rename(&root, folder.join("underneath")).expect("move aside");
    std::fs::create_dir(&root).expect("mounted");
    watcher.recheck_mounts();
    assert_eq!(
        rescans_for_mounts(&mut watcher, &commands),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert_ne!(watcher.table.wds.get(&root).copied(), first);
    assert!(watcher.unwatched.is_empty());

    // Gone, as when unmounted with no folder underneath: not rescanned.
    std::fs::remove_dir(&root).expect("unmounted");
    watcher.recheck_mounts();
    assert!(rescans_for_mounts(&mut watcher, &commands).is_empty());
    assert_eq!(watcher.unwatched.get(&root), Some(&Fallback::Missing));
    // Back.
    std::fs::create_dir(&root).expect("mounted again");
    watcher.recheck_mounts();
    assert_eq!(
        rescans_for_mounts(&mut watcher, &commands),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert!(watcher.unwatched.is_empty());
    let _ = std::fs::remove_dir_all(folder);
}

/// The commands sent once the rescans asked for by mount changes are due.
fn rescans_for_mounts(
    watcher: &mut Watcher,
    commands: &Receiver<LibraryCommand>,
) -> Vec<LibraryCommand> {
    if let Some(due) = watcher.mount_rescans.deadline() {
        watcher.rescan_for_mounts_if_due(due);
    }
    commands.try_iter().collect()
}

/// Applies the events waiting, and returns the commands for the changes
/// they make, as if their time had come.
fn changes_seen(watcher: &mut Watcher, commands: &Receiver<LibraryCommand>) -> Vec<LibraryCommand> {
    let mut events = Vec::new();
    watcher.read_events(&mut events);
    watcher.flush_due(Instant::now() + Duration::from_secs(3600));
    commands.try_iter().collect()
}

fn scan_paths(root: &Path, paths: &[PathBuf]) -> LibraryCommand {
    LibraryCommand::ScanPaths {
        root: root.to_path_buf(),
        paths: paths.to_vec(),
    }
}

/// A watcher watching `roots`, with its first rescans taken.
fn watching(roots: &[PathBuf]) -> (Watcher, Receiver<LibraryCommand>) {
    let (mut watcher, commands) = watcher();
    watcher.handle(WatchMessage::Roots(roots.to_vec()));
    watcher.handle(WatchMessage::Enabled(true));
    let _ = commands.try_iter().count();
    (watcher, commands)
}

#[test]
fn after_lost_events_folders_made_meanwhile_are_watched_and_gone_ones_forgotten() {
    let folder = test_folder("overflow");
    let root = folder.join("music");
    std::fs::create_dir_all(root.join("old")).expect("folders");
    let (mut watcher, commands) = watching(std::slice::from_ref(&root));
    assert!(watcher.table.has_below(&root.join("old")));

    std::fs::create_dir(root.join("new")).expect("new folder");
    std::fs::remove_dir(root.join("old")).expect("remove folder");
    // Their events are lost.
    let mut lost = Vec::new();
    watcher
        .inotify
        .as_mut()
        .expect("inotify")
        .read_events(&mut lost)
        .expect("read");
    assert!(!lost.is_empty());
    watcher.apply_event(
        &RawEvent {
            wd: -1,
            mask: libc::IN_Q_OVERFLOW,
            name: None,
        },
        Instant::now(),
    );
    assert_eq!(
        changes_seen(&mut watcher, &commands),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert!(!watcher.table.has_below(&root.join("old")));

    std::fs::write(root.join("new/1.mp3"), b"1").expect("track");
    assert_eq!(
        changes_seen(&mut watcher, &commands),
        vec![scan_paths(&root, &[root.join("new/1.mp3")])]
    );
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn mounts_below_a_root_that_miss_changes_are_rescanned_instead_and_reported() {
    let folder = test_folder("submounts");
    let root = folder.join("music");
    for name in ["nas/a", "usb", "local"] {
        std::fs::create_dir_all(root.join(name)).expect("folders");
    }
    let (nas, usb) = (root.join("nas"), root.join("usb"));
    watch_test_hooks::set_fs_type(&nas, "nfs4");
    let (mut watcher, commands) = watching(std::slice::from_ref(&root));
    assert!(
        !watcher.table.has_below(&nas),
        "the network mount is not watched"
    );
    assert!(watcher.table.has_below(&root.join("local")));
    assert!(watcher.unwatched.is_empty(), "the root is watched");

    watcher.report();
    let reported = commands.try_iter().collect::<Vec<_>>();
    let [LibraryCommand::SetUnwatchedRoots(unwatched)] = reported.as_slice() else {
        panic!("one report: {reported:?}");
    };
    assert_eq!(unwatched.len(), 1);
    assert_eq!(unwatched[0].path, root);
    assert!(
        unwatched[0]
            .reason
            .contains(&format!("{} (nfs4)", nas.display())),
        "{}",
        unwatched[0].reason
    );

    // Rescanned periodically, alone.
    let due = watcher.next_periodic.expect("a periodic rescan");
    watcher.rescan_unwatched_if_due(due);
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![scan_paths(&root, std::slice::from_ref(&nas))]
    );

    // A share mounted later.
    watch_test_hooks::set_fs_type(&usb, "cifs");
    watcher.recheck_mounts();
    assert_eq!(
        rescans_for_mounts(&mut watcher, &commands),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert!(!watcher.table.has_below(&usb));

    // Both unmounted: the folders under them are watched, and the root
    // rescanned.
    watch_test_hooks::unmount_fake(&nas);
    watch_test_hooks::unmount_fake(&usb);
    watcher.recheck_mounts();
    assert_eq!(
        rescans_for_mounts(&mut watcher, &commands),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert!(watcher.table.has_below(&nas.join("a")));
    assert!(watcher.table.has_below(&usb));
    watcher.report();
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::SetUnwatchedRoots(Vec::new())]
    );
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn removing_an_outer_or_inner_root_keeps_the_other_watched() {
    let folder = test_folder("nested-roots");
    let outer = folder.join("music");
    let inner = outer.join("inner");
    std::fs::create_dir_all(inner.join("album")).expect("folders");
    let (mut watcher, commands) = watching(&[outer.clone(), inner.clone()]);

    watcher.handle(WatchMessage::Roots(vec![inner.clone()]));
    assert!(watcher.table.has_below(&inner.join("album")));
    std::fs::write(inner.join("album/1.mp3"), b"1").expect("track");
    assert_eq!(
        changes_seen(&mut watcher, &commands),
        vec![scan_paths(&inner, &[inner.join("album/1.mp3")])]
    );

    // The other way: the outer root takes the inner one's folders.
    let (mut watcher, commands) = watching(&[outer.clone(), inner.clone()]);
    watcher.handle(WatchMessage::Roots(vec![outer.clone()]));
    std::fs::write(inner.join("album/2.mp3"), b"2").expect("track");
    assert_eq!(
        changes_seen(&mut watcher, &commands),
        vec![scan_paths(&outer, &[inner.join("album/2.mp3")])]
    );
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn a_change_waiting_when_mounts_below_its_root_change_is_still_scanned() {
    let folder = test_folder("submount-pending");
    let root = folder.join("music");
    for name in ["local", "nas"] {
        std::fs::create_dir_all(root.join(name)).expect("folders");
    }
    let (mut watcher, commands) = watching(std::slice::from_ref(&root));
    // An album being copied: seen, and waiting for more.
    std::fs::write(root.join("local/1.mp3"), b"1").expect("track");
    let mut events = Vec::new();
    watcher.read_events(&mut events);
    assert!(commands.try_recv().is_err());

    watch_test_hooks::set_fs_type(&root.join("nas"), "nfs4");
    watcher.recheck_mounts();
    watcher.flush_due(Instant::now() + Duration::from_secs(3600));

    let sent = rescans_for_mounts(&mut watcher, &commands);
    assert!(
        sent.iter().any(|command| match command {
            LibraryCommand::RescanRoot(rescanned) => *rescanned == root,
            LibraryCommand::ScanPaths { paths, .. } => paths.contains(&root.join("local/1.mp3")),
            _ => false,
        }),
        "the copied file is scanned: {sent:?}"
    );
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn an_outer_root_taking_an_inner_ones_folders_leaves_its_network_mounts_unwatched() {
    let folder = test_folder("nested-submount");
    let outer = folder.join("music");
    let inner = outer.join("inner");
    let nas = inner.join("nas");
    std::fs::create_dir_all(nas.join("album")).expect("folders");
    watch_test_hooks::set_fs_type(&nas, "nfs4");
    let (mut watcher, commands) = watching(&[outer.clone(), inner.clone()]);
    assert!(!watcher.table.has_below(&nas));

    watcher.handle(WatchMessage::Roots(vec![outer.clone()]));

    assert!(
        !watcher.table.has_below(&nas),
        "the network mount is not watched"
    );
    assert!(watcher.table.has_below(&inner));
    assert_eq!(
        watcher.unwatched_mounts.get(&nas),
        Some(&(outer.clone(), "nfs4".to_string()))
    );
    let due = watcher.next_periodic.expect("a periodic rescan");
    let _ = commands.try_iter().count();
    watcher.rescan_unwatched_if_due(due);
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![scan_paths(&outer, std::slice::from_ref(&nas))]
    );
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn a_network_mount_at_a_removed_inner_roots_folder_stays_unwatched() {
    let folder = test_folder("nested-mount-at-inner");
    let outer = folder.join("music");
    let inner = outer.join("nas");
    std::fs::create_dir_all(inner.join("album")).expect("folders");
    watch_test_hooks::set_fs_type(&inner, "nfs4");
    let (mut watcher, commands) = watching(&[outer.clone(), inner.clone()]);
    assert_eq!(
        watcher.unwatched.get(&inner),
        Some(&Fallback::FileSystem("nfs4".to_string()))
    );

    watcher.handle(WatchMessage::Roots(vec![outer.clone()]));

    assert!(
        !watcher.table.has_below(&inner),
        "the network mount is not watched"
    );
    assert!(watcher.table.has_below(&outer));
    assert_eq!(
        watcher.unwatched_mounts.get(&inner),
        Some(&(outer.clone(), "nfs4".to_string()))
    );
    let _ = commands.try_iter().count();
    watcher.report();
    let reported = commands.try_iter().collect::<Vec<_>>();
    let [LibraryCommand::SetUnwatchedRoots(unwatched)] = reported.as_slice() else {
        panic!("one report: {reported:?}");
    };
    assert_eq!(unwatched.len(), 1);
    assert_eq!(unwatched[0].path, outer);
    assert!(
        unwatched[0]
            .reason
            .contains(&format!("{} (nfs4)", inner.display())),
        "{}",
        unwatched[0].reason
    );
    let due = watcher.next_periodic.expect("a periodic rescan");
    watcher.rescan_unwatched_if_due(due);
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![scan_paths(&outer, std::slice::from_ref(&inner))]
    );
    watch_test_hooks::unmount_fake(&inner);
    let _ = std::fs::remove_dir_all(folder);
}

#[test]
fn a_flapping_mount_rescans_its_root_at_most_once_per_interval() {
    let folder = test_folder("flapping-mount");
    let root = folder.join("music");
    for name in ["local", "auto"] {
        std::fs::create_dir_all(root.join(name)).expect("folders");
    }
    let auto = root.join("auto");
    let (mut watcher, commands) = watching(std::slice::from_ref(&root));
    let interval = watcher.tuning.mount_rescan;
    let flap = |watcher: &mut Watcher, times: usize| {
        for _ in 0..times {
            watch_test_hooks::set_fs_type(&auto, "autofs");
            watcher.recheck_mounts();
            watch_test_hooks::unmount_fake(&auto);
            watcher.recheck_mounts();
        }
    };

    // A burst of mount changes is one rescan.
    flap(&mut watcher, 5);
    let first = watcher.mount_rescans.deadline().expect("a rescan");
    watcher.rescan_for_mounts_if_due(first);
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );

    // An album being copied: seen, and waiting for more, when the mount
    // keeps changing.
    std::fs::write(root.join("local/1.mp3"), b"1").expect("track");
    let mut events = Vec::new();
    watcher.read_events(&mut events);
    flap(&mut watcher, 5);
    // Its changes are scanned when due, while the next rescan waits.
    let local = first + watcher.tuning.max_delay;
    watcher.flush_due(local);
    watcher.rescan_for_mounts_if_due(local);
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![scan_paths(&root, &[root.join("local/1.mp3")])]
    );
    // Changes until the interval is over are the same rescan.
    flap(&mut watcher, 5);
    watcher.rescan_for_mounts_if_due(first + interval - Duration::from_millis(1));
    assert!(commands.try_recv().is_err());
    watcher.rescan_for_mounts_if_due(first + interval);
    assert_eq!(
        commands.try_iter().collect::<Vec<_>>(),
        vec![LibraryCommand::RescanRoot(root.clone())]
    );
    assert!(watcher.mount_rescans.deadline().is_none());
    let _ = std::fs::remove_dir_all(folder);
}
