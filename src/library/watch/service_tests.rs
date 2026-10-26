// SPDX-License-Identifier: GPL-3.0-or-later

//! The watcher with the library service, on real folders: changes reach the
//! library without a rescan. The tests wait for the service's events and
//! the watcher's test hooks, never for a time.

use std::fs;
use std::sync::atomic::AtomicUsize;

use super::watch_test_hooks::{
    fail_watches, hold_changes, hold_watching, observe_deferred_walks, observe_pending,
    observe_watched, set_fs_type,
};
use super::*;
use crate::library::scan::scan_test_hooks::{hold_walk, scans_of};
use crate::library::scan::write_test_track;
use crate::library::scan::ScanTuning;
use crate::library::{LibraryEvent, LibraryMirror, LibraryService};
use crate::raw_audio::write_test_apev2_file;

const WAIT: Duration = Duration::from_secs(20);

/// Changes are scanned 50 ms after they stop; roots that are not watched
/// are rescanned every `periodic`.
fn tuning(periodic: Duration) -> WatchTuning {
    WatchTuning {
        quiet: Duration::from_millis(50),
        max_delay: Duration::from_secs(2),
        periodic,
        max_paths: 2048,
        mount_rescan: Duration::from_secs(300),
    }
}

/// A library folder and a database beside it.
struct Fixture {
    folder: PathBuf,
    db: PathBuf,
    root: PathBuf,
}

impl Fixture {
    fn new(name: &str) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let folder = crate::paths::test_root().join(format!(
            "library-watch-{name}-{}",
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&folder);
        let root = folder.join("music");
        fs::create_dir_all(&root).expect("library folder");
        Self {
            db: folder.join("library.sqlite3"),
            root: root.canonicalize().expect("canonical root"),
            folder,
        }
    }

    fn path(&self, name: &str) -> PathBuf {
        self.root.join(name)
    }

    fn write(&self, name: &str) {
        let path = self.path(name);
        fs::create_dir_all(path.parent().expect("parent")).expect("folder");
        write_test_track(&path, name.as_bytes());
    }

    /// The service, with its first event read.
    fn start(&self, watching: WatchTuning) -> Running {
        let (service, events) = LibraryService::with_watch_tuning(
            self.db.clone(),
            Duration::from_secs(5),
            ScanTuning {
                batch_rows: 256,
                batch_interval: Duration::from_millis(100),
                workers: 2,
            },
            watching,
        );
        let mut running = Running {
            service,
            events,
            mirror: LibraryMirror::default(),
            unwatched_changes: 0,
        };
        running.wait_for("the first event", |_| true);
        running
    }

    /// The service with the root added, watching it, after its first rescan.
    fn start_watching(&self, watching: WatchTuning) -> Running {
        self.start_with_watching_on(watching, true)
    }

    /// The service with the root added and watching on, after its first
    /// rescan; `watchable` tells whether the root can be watched.
    fn start_with_watching_on(&self, watching: WatchTuning, watchable: bool) -> Running {
        let watched = observe_watched(&self.root);
        let mut running = self.start(watching);
        running.service.command(LibraryCommand::AddRoot {
            path: self.root.clone(),
            name: String::new(),
        });
        running.wait_for("the root added", |mirror| {
            mirror.snapshot.roots.len() == 1 && !mirror.status.scan_in_progress
        });
        let scans = scans_of(&self.root);
        running.service.set_watching(true);
        if watchable {
            wait_for_count(&watched, "the root watched", |count| count > 0);
        }
        running.wait_for("the rescan when watching starts", |mirror| {
            scans_of(&self.root) > scans && !mirror.status.scan_in_progress
        });
        running
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.folder);
    }
}

struct Running {
    service: LibraryService,
    events: crossbeam_channel::Receiver<LibraryEvent>,
    mirror: LibraryMirror,
    /// How many events changed the roots that are not watched.
    unwatched_changes: usize,
}

impl Running {
    /// Applies events until the library satisfies `wanted`.
    fn wait_for(&mut self, what: &str, mut wanted: impl FnMut(&LibraryMirror) -> bool) {
        let deadline = Instant::now() + WAIT;
        loop {
            let event = self.events.recv_deadline(deadline).unwrap_or_else(|error| {
                panic!("{what}: {error}; library {:?}", self.mirror.paths())
            });
            self.apply(&event);
            if wanted(&self.mirror) {
                return;
            }
        }
    }

    /// Applies events until the library satisfies `wanted`, or until
    /// `waits` gets a message.
    fn wait_for_or(
        &mut self,
        waits: &crossbeam_channel::Receiver<()>,
        what: &str,
        mut wanted: impl FnMut(&LibraryMirror) -> bool,
    ) {
        let deadline = Instant::now() + WAIT;
        loop {
            crossbeam_channel::select! {
                recv(waits) -> _ => return,
                recv(self.events) -> event => {
                    let event = event.unwrap_or_else(|error| panic!("{what}: {error}"));
                    self.apply(&event);
                    if wanted(&self.mirror) {
                        return;
                    }
                },
                default(deadline.saturating_duration_since(Instant::now())) => {
                    panic!("{what}: timed out; library {:?}", self.mirror.paths());
                },
            }
        }
    }

    fn apply(&mut self, event: &LibraryEvent) {
        if event.status.unwatched_roots != self.mirror.status.unwatched_roots {
            self.unwatched_changes += 1;
        }
        self.mirror.apply(event);
    }

    fn has(&self, path: &Path) -> bool {
        self.mirror
            .snapshot
            .tracks
            .iter()
            .any(|track| track.path == path)
    }

    fn title_of(&self, path: &Path) -> Option<String> {
        self.mirror
            .snapshot
            .tracks
            .iter()
            .find(|track| track.path == path)
            .map(|track| track.title.clone())
    }
}

fn wait_for_count(
    counts: &crossbeam_channel::Receiver<usize>,
    what: &str,
    mut wanted: impl FnMut(usize) -> bool,
) -> usize {
    let deadline = Instant::now() + WAIT;
    loop {
        let count = counts
            .recv_deadline(deadline)
            .unwrap_or_else(|error| panic!("{what}: {error}"));
        if wanted(count) {
            return count;
        }
    }
}

fn tagged(path: &Path, title: &str) {
    write_test_apev2_file(path, &[("Title", title), ("Artist", "Artist")], true);
}

#[test]
fn files_added_changed_removed_and_renamed_reach_the_library_without_a_rescan() {
    let fixture = Fixture::new("changes");
    tagged(&fixture.path("a.dts"), "First");
    fixture.write("b.mp3");
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    assert_eq!(
        running.title_of(&fixture.path("a.dts")).as_deref(),
        Some("First")
    );

    fixture.write("c.mp3");
    running.wait_for("an added file", |mirror| {
        mirror
            .snapshot
            .tracks
            .iter()
            .any(|t| t.path == fixture.path("c.mp3"))
    });

    tagged(&fixture.path("a.dts"), "Second");
    running.wait_for("a changed file", |mirror| {
        mirror
            .snapshot
            .tracks
            .iter()
            .any(|t| t.path == fixture.path("a.dts") && t.title == "Second")
    });

    fs::remove_file(fixture.path("b.mp3")).expect("remove");
    running.wait_for("a removed file", |mirror| {
        !mirror
            .snapshot
            .tracks
            .iter()
            .any(|t| t.path == fixture.path("b.mp3"))
    });

    fs::rename(fixture.path("c.mp3"), fixture.path("d.mp3")).expect("rename");
    running.wait_for("a renamed file", |mirror| {
        let paths = mirror.paths();
        paths.contains(&fixture.path("d.mp3")) && !paths.contains(&fixture.path("c.mp3"))
    });

    // A folder made after watching started is watched too.
    fixture.write("album/1.mp3");
    running.wait_for("a new folder's file", |mirror| {
        mirror.paths().contains(&fixture.path("album/1.mp3"))
    });
    fixture.write("album/2.mp3");
    running.wait_for("a file added to the new folder", |mirror| {
        mirror.paths().contains(&fixture.path("album/2.mp3"))
    });

    fs::rename(fixture.path("album"), fixture.path("renamed")).expect("rename folder");
    running.wait_for("a renamed folder", |mirror| {
        mirror.paths()
            == vec![
                fixture.path("a.dts"),
                fixture.path("d.mp3"),
                fixture.path("renamed/1.mp3"),
                fixture.path("renamed/2.mp3"),
            ]
    });
    fixture.write("renamed/3.mp3");
    running.wait_for("a file added to the renamed folder", |mirror| {
        mirror.paths().contains(&fixture.path("renamed/3.mp3"))
    });
    assert!(running.has(&fixture.path("renamed/3.mp3")));
    assert_eq!(running.mirror.status.last_error, None);
    assert!(running.mirror.status.unwatched_roots.is_empty());
}

#[test]
fn files_of_the_same_size_and_time_swapped_outside_the_app_get_their_own_tags() {
    let fixture = Fixture::new("outside-swap");
    // One size, as the titles have one length, and one modification time.
    // FLAC files, which, unlike raw surround files without a duration,
    // a scan reads only when they changed.
    let (alpha, bravo) = (fixture.path("alpha.flac"), fixture.path("bravo.flac"));
    let time = std::time::UNIX_EPOCH + Duration::from_secs(1_700_000_000);
    for (path, title) in [(&alpha, "Alpha"), (&bravo, "Bravo")] {
        crate::tag_write::write_test_flac(path, &[("TITLE", title)]);
        fs::File::options()
            .write(true)
            .open(path)
            .and_then(|file| file.set_modified(time))
            .expect("modification time");
    }
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    assert_eq!(running.title_of(&alpha).as_deref(), Some("Alpha"));

    let aside = fixture.path("aside.tmp");
    fs::rename(&alpha, &aside).expect("aside");
    fs::rename(&bravo, &alpha).expect("bravo to alpha");
    fs::rename(&aside, &bravo).expect("alpha to bravo");

    running.wait_for("each file with its own tags", |mirror| {
        let title = |path: &Path| {
            mirror
                .snapshot
                .tracks
                .iter()
                .find(|track| track.path == path)
                .map(|track| track.title.clone())
        };
        title(&alpha).as_deref() == Some("Bravo") && title(&bravo).as_deref() == Some("Alpha")
    });
    assert_eq!(running.mirror.status.last_error, None);
}

fn cover_of(mirror: &LibraryMirror, track: &Path) -> Option<String> {
    mirror
        .snapshot
        .tracks
        .iter()
        .find(|t| t.path == track)
        .map(|t| t.cover_path.clone())
}

#[test]
fn image_changes_in_a_folder_give_its_tracks_their_cover_without_a_rescan() {
    let fixture = Fixture::new("cover-images");
    fixture.write("Album/song.mp3");
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    let track = fixture.path("Album/song.mp3");
    assert_eq!(cover_of(&running.mirror, &track).as_deref(), Some(""));

    let cover = fixture.path("Album/cover.jpg");
    fs::write(&cover, b"not-real-jpg").expect("add image");
    running.wait_for("an added image", |mirror| {
        cover_of(mirror, &track) == Some(cover.to_string_lossy().into_owned())
    });

    let renamed = fixture.path("Album/folder.png");
    fs::rename(&cover, &renamed).expect("rename image");
    running.wait_for("a renamed image", |mirror| {
        cover_of(mirror, &track) == Some(renamed.to_string_lossy().into_owned())
    });

    fs::remove_file(&renamed).expect("remove image");
    running.wait_for("a removed image", |mirror| {
        cover_of(mirror, &track).as_deref() == Some("")
    });
    assert_eq!(running.mirror.status.last_error, None);
}

#[test]
fn a_burst_of_changes_is_scanned_once() {
    const FILES: usize = 20;
    let fixture = Fixture::new("burst");
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    let held = hold_changes(&fixture.root);
    let pending = observe_pending(&fixture.root);
    let scans = scans_of(&fixture.root);

    // Once the folder is seen, it is watched.
    fs::create_dir(fixture.path("album")).expect("album");
    wait_for_count(&pending, "the folder seen", |count| count == 1);
    for index in 0..FILES {
        fixture.write(&format!("album/{index:02}.mp3"));
    }
    // Each file is created, and closed after writing.
    let mut seen = 0;
    wait_for_count(&pending, "every change seen", |_| {
        seen += 1;
        seen == 2 * FILES
    });
    drop(held);
    // Wakes the watcher; watching is on already.
    running.service.set_watching(true);

    running.wait_for("every file", |mirror| {
        mirror.snapshot.tracks.len() == FILES && !mirror.status.scan_in_progress
    });
    assert_eq!(scans_of(&fixture.root), scans + 1, "one scan for the burst");
}

#[test]
fn a_root_on_a_network_file_system_is_rescanned_instead_and_reported_once() {
    let fixture = Fixture::new("network");
    fixture.write("a.mp3");
    set_fs_type(&fixture.root, "nfs4");
    let mut running = fixture.start_with_watching_on(tuning(Duration::from_millis(200)), false);
    running.wait_for("the root reported", |mirror| {
        !mirror.status.unwatched_roots.is_empty()
    });
    let unwatched = running.mirror.status.unwatched_roots.clone();
    assert_eq!(unwatched.len(), 1);
    assert_eq!(unwatched[0].path, fixture.root);
    assert!(
        unwatched[0].reason.contains("nfs4"),
        "{}",
        unwatched[0].reason
    );

    // Seen by a periodic rescan.
    fixture.write("b.mp3");
    running.wait_for("the periodic rescan", |mirror| {
        mirror.paths().contains(&fixture.path("b.mp3"))
    });
    let scans = scans_of(&fixture.root);
    running.wait_for("two more periodic rescans", |mirror| {
        scans_of(&fixture.root) >= scans + 2 && !mirror.status.scan_in_progress
    });
    assert_eq!(running.unwatched_changes, 1, "reported once");
    assert_eq!(running.mirror.status.unwatched_roots, unwatched);
}

#[test]
fn a_root_past_the_watch_limit_is_unwatched_and_rescanned_instead() {
    let fixture = Fixture::new("limit");
    fixture.write("a/1.mp3");
    fixture.write("b/2.mp3");
    fixture.write("c/3.mp3");
    fail_watches(&fixture.path("b"), libc::ENOSPC);
    let watched = observe_watched(&fixture.root);
    let mut running = fixture.start(tuning(Duration::from_millis(200)));
    running.service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    running.service.set_watching(true);

    running.wait_for("the root reported", |mirror| {
        !mirror.status.unwatched_roots.is_empty()
    });
    let reason = running.mirror.status.unwatched_roots[0].reason.clone();
    assert!(reason.contains("fs.inotify.max_user_watches"), "{reason}");
    // The folders it watched before the limit are watched no more.
    let mut last = None;
    while let Ok(count) = watched.try_recv() {
        last = Some(count);
    }
    assert_eq!(last, Some(0));

    fixture.write("c/4.mp3");
    running.wait_for("the periodic rescan", |mirror| {
        mirror.paths().contains(&fixture.path("c/4.mp3"))
    });
    assert_eq!(running.unwatched_changes, 1);
}

#[test]
fn turning_watching_off_stops_updates_and_turning_it_on_catches_up() {
    let fixture = Fixture::new("toggle");
    fixture.write("a.mp3");
    let watched = observe_watched(&fixture.root);
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));

    running.service.set_watching(false);
    wait_for_count(&watched, "the watches removed", |count| count == 0);
    let scans = scans_of(&fixture.root);
    // Nothing watches the folder now, so nothing can see this.
    fixture.write("b.mp3");
    running.mirror.drain(&running.events);
    assert!(!running.has(&fixture.path("b.mp3")));
    assert_eq!(scans_of(&fixture.root), scans);

    running.service.set_watching(true);
    running.wait_for("the rescan when watching is turned on", |mirror| {
        mirror.paths().contains(&fixture.path("b.mp3")) && !mirror.status.scan_in_progress
    });
    assert_eq!(scans_of(&fixture.root), scans + 1);
    wait_for_count(&watched, "watched again", |count| count > 0);
    fixture.write("c.mp3");
    running.wait_for("a change seen again", |mirror| {
        mirror.paths().contains(&fixture.path("c.mp3"))
    });
}

#[test]
fn the_startup_rescan_finds_changes_made_while_the_library_was_closed() {
    let fixture = Fixture::new("startup");
    fixture.write("a.mp3");
    let running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    assert!(running.has(&fixture.path("a.mp3")));
    drop(running);

    fs::remove_file(fixture.path("a.mp3")).expect("remove");
    fixture.write("b.mp3");
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    assert!(
        running.has(&fixture.path("a.mp3")),
        "the stored library first"
    );
    running.service.set_watching(true);
    running.wait_for("the startup rescan", |mirror| {
        mirror.paths() == vec![fixture.path("b.mp3")] && !mirror.status.scan_in_progress
    });
}

#[test]
fn added_and_removed_roots_are_watched_and_unwatched() {
    let fixture = Fixture::new("roots");
    let watched = observe_watched(&fixture.root);
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    running.service.set_watching(true);
    running.service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    wait_for_count(&watched, "the added root watched", |count| count > 0);
    running.wait_for("the root added", |mirror| {
        mirror.snapshot.roots.len() == 1 && !mirror.status.scan_in_progress
    });

    fixture.write("a.mp3");
    running.wait_for("a change in the added root", |mirror| {
        mirror.paths().contains(&fixture.path("a.mp3"))
    });

    running
        .service
        .command(LibraryCommand::RemoveRoot(fixture.root.clone()));
    wait_for_count(&watched, "the removed root unwatched", |count| count == 0);
    running.wait_for("the root removed", |mirror| {
        mirror.snapshot.roots.is_empty()
    });
}

/// The first scan of an added root walks it only once its watches are set
/// up, so a file written before then is indexed too (#238).
#[test]
fn a_file_written_before_an_added_roots_watches_are_set_up_is_indexed() {
    let fixture = Fixture::new("added-before-watched");
    fixture.write("a.mp3");
    let watched = observe_watched(&fixture.root);
    let deferred = observe_deferred_walks(&fixture.root);
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    running.service.set_watching(true);
    let held = hold_watching(&fixture.root);
    running.service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    held.reached
        .recv_timeout(WAIT)
        .expect("the watcher about to watch the added root");
    // The first walk waits for the watches; without that, it would have
    // walked past the root's folder before this file is written.
    running.wait_for_or(&deferred, "the first scan, or its deferral", |mirror| {
        mirror.paths().contains(&fixture.path("a.mp3")) && !mirror.status.scan_in_progress
    });
    fixture.write("b.mp3");
    drop(held);
    wait_for_count(&watched, "the added root watched", |count| count > 0);

    // Seen by the watches, after the first scan.
    fixture.write("c.mp3");
    running.wait_for("a change after the watches are set up", |mirror| {
        mirror.paths().contains(&fixture.path("c.mp3")) && !mirror.status.scan_in_progress
    });
    assert_eq!(
        running.mirror.paths(),
        vec![
            fixture.path("a.mp3"),
            fixture.path("b.mp3"),
            fixture.path("c.mp3")
        ]
    );
}

/// Turning watching on rescans the roots once they are watched, so a file
/// written before the watches are set up is indexed too.
#[test]
fn a_file_written_before_the_watches_are_set_up_when_watching_is_turned_on_is_indexed() {
    let fixture = Fixture::new("enabled-before-watched");
    fixture.write("a.mp3");
    let watched = observe_watched(&fixture.root);
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    running.service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    running.wait_for("the root added", |mirror| {
        mirror.paths().contains(&fixture.path("a.mp3")) && !mirror.status.scan_in_progress
    });
    let held = hold_watching(&fixture.root);
    running.service.set_watching(true);
    held.reached
        .recv_timeout(WAIT)
        .expect("the watcher about to watch the root");
    fixture.write("b.mp3");
    drop(held);
    wait_for_count(&watched, "the root watched", |count| count > 0);

    fixture.write("c.mp3");
    running.wait_for("a change after the watches are set up", |mirror| {
        mirror.paths().contains(&fixture.path("c.mp3")) && !mirror.status.scan_in_progress
    });
    assert_eq!(
        running.mirror.paths(),
        vec![
            fixture.path("a.mp3"),
            fixture.path("b.mp3"),
            fixture.path("c.mp3")
        ]
    );
}

/// The service with watching on, and `fixture`'s root added while the
/// watcher is held before it watches the root, once the root's first walk
/// is left for later.
fn add_root_while_watching_is_held(
    fixture: &Fixture,
    running: &mut Running,
) -> super::watch_test_hooks::HeldWatching {
    let deferred = observe_deferred_walks(&fixture.root);
    running.service.set_watching(true);
    let held = hold_watching(&fixture.root);
    running.service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    deferred
        .recv_timeout(WAIT)
        .expect("the first walk left for when the root is watched");
    held
}

/// Removing a root whose first walk waits for its watches is published at
/// once, and drops the walk.
#[test]
fn removing_an_added_root_waiting_for_its_watches_is_published_at_once() {
    let fixture = Fixture::new("remove-before-watched");
    fixture.write("a.mp3");
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    let held = add_root_while_watching_is_held(&fixture, &mut running);
    running.wait_for("the root added", |mirror| mirror.snapshot.roots.len() == 1);
    assert!(running.mirror.status.scan_in_progress, "its walk is due");

    running
        .service
        .command(LibraryCommand::RemoveRoot(fixture.root.clone()));
    running.wait_for("the removal, with the watcher still held", |mirror| {
        mirror.snapshot.roots.is_empty() && !mirror.status.scan_in_progress
    });
    assert!(running.mirror.paths().is_empty());
    assert_eq!(scans_of(&fixture.root), 0);
    drop(held);
}

/// Other roots are rescanned while an added root waits for its watches.
#[test]
fn other_roots_are_rescanned_while_an_added_root_waits_for_its_watches() {
    let fixture = Fixture::new("rescan-before-watched");
    fixture.write("a.mp3");
    let other = fixture.folder.join("other");
    fs::create_dir_all(&other).expect("other root");
    let other = other.canonicalize().expect("canonical other root");
    write_test_track(&other.join("x.mp3"), b"x");
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    running.service.command(LibraryCommand::AddRoot {
        path: other.clone(),
        name: String::new(),
    });
    running.wait_for("the other root added", |mirror| {
        mirror.paths() == vec![other.join("x.mp3")] && !mirror.status.scan_in_progress
    });
    let held = add_root_while_watching_is_held(&fixture, &mut running);

    // The held watcher sees nothing; only the rescan finds this.
    write_test_track(&other.join("y.mp3"), b"y");
    running
        .service
        .command(LibraryCommand::RescanRoot(other.clone()));
    running.wait_for(
        "the other root rescanned, with the watcher still held",
        |mirror| mirror.paths().contains(&other.join("y.mp3")),
    );
    assert!(!running.has(&fixture.path("a.mp3")));
    assert_eq!(scans_of(&fixture.root), 0);

    drop(held);
    running.wait_for("the added root walked once watched", |mirror| {
        mirror.paths().contains(&fixture.path("a.mp3")) && !mirror.status.scan_in_progress
    });
    assert_eq!(scans_of(&fixture.root), 1);
}

/// Turning watching off walks an added root that waits for its watches:
/// none are needed then.
#[test]
fn turning_watching_off_walks_an_added_root_waiting_for_its_watches() {
    let fixture = Fixture::new("off-before-watched");
    fixture.write("a.mp3");
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    let held = add_root_while_watching_is_held(&fixture, &mut running);

    running.service.set_watching(false);
    running.wait_for("the walk, with the watcher still held", |mirror| {
        mirror.paths().contains(&fixture.path("a.mp3")) && !mirror.status.scan_in_progress
    });
    drop(held);
}

/// Turning watching off reaches the watcher at once, also while the worker
/// is held in a walk, between whose files it checks for commands.
#[test]
fn turning_watching_off_reaches_the_watcher_while_a_walk_is_held() {
    let fixture = Fixture::new("off-during-walk");
    fixture.write("a.mp3");
    let watched = observe_watched(&fixture.root);
    let running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    let (walking, release) = hold_walk(&fixture.root);
    running
        .service
        .command(LibraryCommand::RescanRoot(fixture.root.clone()));
    walking.recv_timeout(WAIT).expect("the walk held");

    running.service.set_watching(false);
    wait_for_count(
        &watched,
        "the watches removed, with the walk held",
        |count| count == 0,
    );
    drop(release);
}

/// Turning watching off and on while the watcher is held before it watches
/// an added root, and the worker is held in a walk of another root, leaves
/// the roots watched once, and the added root walked once.
#[test]
fn turning_watching_off_and_on_while_an_added_root_waits_watches_and_walks_it_once() {
    let fixture = Fixture::new("off-on-before-watched");
    fixture.write("a.mp3");
    let other = fixture.folder.join("other");
    fs::create_dir_all(&other).expect("other root");
    let other = other.canonicalize().expect("canonical other root");
    write_test_track(&other.join("x.mp3"), b"x");
    let watched = observe_watched(&fixture.root);
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    running.service.set_watching(true);
    running.service.command(LibraryCommand::AddRoot {
        path: other.clone(),
        name: String::new(),
    });
    running.wait_for("the other root added", |mirror| {
        mirror.paths() == vec![other.join("x.mp3")] && !mirror.status.scan_in_progress
    });
    let held = add_root_while_watching_is_held(&fixture, &mut running);
    let other_scans = scans_of(&other);
    let (walking, release) = hold_walk(&other);
    running
        .service
        .command(LibraryCommand::RescanRoot(other.clone()));
    walking
        .recv_timeout(WAIT)
        .expect("the other root's walk held");

    running.service.set_watching(false);
    drop(held);
    // The late watches are removed, with the worker still held.
    wait_for_count(&watched, "the added root watched late", |count| count > 0);
    wait_for_count(&watched, "the late watches removed", |count| count == 0);
    running.service.set_watching(true);
    let count = wait_for_count(&watched, "watched again", |count| count > 0);
    assert_eq!(count, 1, "the root's one folder");

    drop(release);
    running.wait_for("the walks and the catch-up rescans", |mirror| {
        mirror.paths().contains(&fixture.path("a.mp3"))
            && !mirror.status.scan_in_progress
            && scans_of(&other) == other_scans + 2
    });
    // The deferred walk and the catch-up rescan of the added root are one.
    assert_eq!(scans_of(&fixture.root), 1);
    assert_eq!(watched.try_iter().collect::<Vec<_>>(), Vec::<usize>::new());
}

/// The worker stops while an added root waits for its watches.
#[test]
fn the_worker_stops_while_an_added_root_waits_for_its_watches() {
    let fixture = Fixture::new("stop-before-watched");
    fixture.write("a.mp3");
    let mut running = fixture.start(tuning(Duration::from_secs(3600)));
    let held = add_root_while_watching_is_held(&fixture, &mut running);

    running.service.tx = None;
    let worker = running.service.thread.take().expect("the worker");
    assert!(worker.join_by(Instant::now() + Duration::from_secs(5)));
    assert!(!running.has(&fixture.path("a.mp3")));
    drop(held);
}

#[test]
fn stopping_the_service_stops_the_watcher_at_once() {
    let fixture = Fixture::new("stop");
    for index in 0..50 {
        fixture.write(&format!("{index}/a.mp3"));
    }
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));
    fixture.write("new.mp3");
    running.wait_for("a change", |mirror| {
        mirror.paths().contains(&fixture.path("new.mp3"))
    });

    let stopping = Instant::now();
    running.service.watcher.stop();
    assert!(running
        .service
        .watcher
        .join_by(Instant::now() + Duration::from_secs(1)));
    drop(running);
    assert!(
        stopping.elapsed() < Duration::from_secs(1),
        "stopping took {:?}",
        stopping.elapsed()
    );
}

#[test]
fn links_to_folders_made_and_removed_while_watching_are_followed() {
    let fixture = Fixture::new("links");
    let elsewhere = fixture.folder.join("elsewhere/Album");
    fs::create_dir_all(&elsewhere).expect("linked folder");
    write_test_track(&elsewhere.join("1.mp3"), b"1");
    // A root left empty would look unmounted, and keep its tracks.
    fixture.write("a.mp3");
    let mut running = fixture.start_watching(tuning(Duration::from_secs(3600)));

    std::os::unix::fs::symlink(&elsewhere, fixture.path("Album")).expect("link");
    running.wait_for("the linked folder's track", |mirror| {
        mirror.paths() == vec![fixture.path("Album/1.mp3"), fixture.path("a.mp3")]
    });
    // The folder it links to is watched.
    write_test_track(&elsewhere.join("2.mp3"), b"2");
    running.wait_for("a track added to the linked folder", |mirror| {
        mirror.paths()
            == vec![
                fixture.path("Album/1.mp3"),
                fixture.path("Album/2.mp3"),
                fixture.path("a.mp3"),
            ]
    });

    fs::remove_file(fixture.path("Album")).expect("remove the link");
    running.wait_for("the link's tracks removed", |mirror| {
        mirror.paths() == vec![fixture.path("a.mp3")] && !mirror.status.scan_in_progress
    });
    assert_eq!(running.mirror.status.last_error, None);
}
