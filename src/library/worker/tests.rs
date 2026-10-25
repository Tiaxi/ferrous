// SPDX-License-Identifier: GPL-3.0-or-later

use std::fs;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::time::Instant;

use rusqlite::Connection;

use super::*;
use crate::library::scan::scan_test_hooks::{hold_read, scans_of};
use crate::library::scan::ScanTuning;
use crate::library::schema::{user_version, SCHEMA_VERSION};
use crate::library::test_fixtures::{build_version_0_library, schema_objects};
use crate::library::LibraryService;

fn root(path: &str) -> PathBuf {
    PathBuf::from(path)
}

fn rescan(path: &str) -> LibraryCommand {
    LibraryCommand::RescanRoot(root(path))
}

fn add(path: &str, name: &str) -> LibraryCommand {
    LibraryCommand::AddRoot {
        path: root(path),
        name: name.to_string(),
    }
}

fn rename(path: &str, name: &str) -> LibraryCommand {
    LibraryCommand::RenameRoot {
        path: root(path),
        name: name.to_string(),
    }
}

fn queued(queue: &CommandQueue) -> (Vec<LibraryCommand>, Vec<LibraryCommand>) {
    (
        queue.priority.iter().cloned().collect(),
        queue.normal.iter().cloned().collect(),
    )
}

#[test]
fn repeated_rescans_of_a_queued_root_merge() {
    let mut queue = CommandQueue::default();
    queue.push(rescan("/a"), None);
    queue.push(rescan("/b"), None);
    queue.push(rescan("/a"), None);
    queue.push(rescan("/b"), None);

    assert_eq!(queued(&queue), (vec![], vec![rescan("/a"), rescan("/b")]));
}

#[test]
fn rescans_of_a_root_that_a_running_scan_has_yet_to_finish_merge_with_it() {
    let mut queue = CommandQueue::default();
    let remaining = [root("/c")];
    let running = RunningScan {
        all: true,
        scanning: Path::new("/b"),
        remaining: &remaining,
    };
    queue.push(rescan("/b"), Some(running));
    queue.push(rescan("/c"), Some(running));
    queue.push(LibraryCommand::RescanAll, Some(running));
    assert_eq!(queued(&queue), (vec![], vec![]));

    // A root the scan has finished is scanned again.
    queue.push(rescan("/a"), Some(running));
    assert_eq!(queued(&queue), (vec![], vec![rescan("/a")]));
}

#[test]
fn a_rescan_of_every_root_replaces_queued_rescans_of_single_roots() {
    let mut queue = CommandQueue::default();
    queue.push(rescan("/a"), None);
    queue.push(add("/new", "New"), None);
    queue.push(LibraryCommand::RescanAll, None);
    queue.push(rescan("/b"), None);
    queue.push(LibraryCommand::RescanAll, None);

    assert_eq!(
        queued(&queue),
        (vec![], vec![add("/new", "New"), LibraryCommand::RescanAll])
    );
}

#[test]
fn removing_a_root_drops_its_queued_commands_and_runs_first() {
    let mut queue = CommandQueue::default();
    queue.push(rescan("/a"), None);
    queue.push(add("/b", ""), None);
    queue.push(rename("/a", "Renamed"), None);
    queue.push(rescan("/b"), None);
    queue.push(LibraryCommand::RemoveRoot(root("/a")), None);
    queue.push(LibraryCommand::RemoveRoot(root("/a")), None);

    assert_eq!(
        queued(&queue),
        (
            vec![LibraryCommand::RemoveRoot(root("/a"))],
            vec![add("/b", "")]
        )
    );
    assert_eq!(
        queue.pop(false),
        Some(LibraryCommand::RemoveRoot(root("/a")))
    );
}

#[test]
fn renaming_a_root_that_is_not_added_yet_waits_for_the_add() {
    let mut queue = CommandQueue::default();
    queue.push(LibraryCommand::ScanRoot(root("/a")), None);
    queue.push(rename("/a", "Name"), None);
    queue.push(rename("/b", "Other"), None);
    queue.push(add("/a", "Again"), None);

    assert_eq!(
        queued(&queue),
        (
            vec![rename("/b", "Other")],
            vec![add("/a", "Again"), rename("/a", "Name")]
        )
    );
}

#[test]
fn shutting_down_drops_queued_rescans_but_runs_other_commands() {
    let mut queue = CommandQueue::default();
    queue.push(rescan("/a"), None);
    queue.push(add("/b", "B"), None);
    queue.push(LibraryCommand::RescanAll, None);
    queue.push(rename("/c", "C"), None);

    assert_eq!(queue.pop(true), Some(rename("/c", "C")));
    assert_eq!(queue.pop(true), Some(add("/b", "B")));
    assert_eq!(queue.pop(true), None);
}

/// A library folder with stub tracks, and a database beside it.
struct Fixture {
    folder: PathBuf,
    db: PathBuf,
    root: PathBuf,
}

impl Fixture {
    fn new(name: &str, tracks: &[&str]) -> Self {
        static NEXT: AtomicUsize = AtomicUsize::new(0);
        let folder = crate::paths::test_root().join(format!(
            "library-worker-{name}-{}",
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let _ = fs::remove_dir_all(&folder);
        let root = folder.join("music");
        fs::create_dir_all(&root).expect("library folder");
        let root = root.canonicalize().expect("canonical root");
        for track in tracks {
            fs::write(root.join(track), track.as_bytes()).expect("track");
        }
        Self {
            db: folder.join("library.sqlite3"),
            folder,
            root,
        }
    }

    fn track(&self, name: &str) -> PathBuf {
        self.root.join(name)
    }

    /// The service, with its first snapshot read.
    fn start(&self) -> (LibraryService, Events) {
        let (service, events) = LibraryService::with_database(
            self.db.clone(),
            Duration::from_secs(5),
            ScanTuning {
                batch_rows: 2,
                batch_interval: Duration::from_secs(60),
                workers: 2,
            },
        );
        let mut events = Events::new(events);
        next_snapshot(&mut events, |_| true);
        (service, events)
    }

    fn conn(&self) -> Connection {
        Connection::open(&self.db).expect("open test library")
    }

    fn indexed(&self) -> Vec<PathBuf> {
        let conn = self.conn();
        let mut stmt = conn
            .prepare("SELECT path FROM tracks ORDER BY path")
            .expect("prepare");
        stmt.query_map([], |row| row.get::<_, String>(0))
            .expect("query")
            .map(|path| PathBuf::from(path.expect("path")))
            .collect()
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.folder);
    }
}

/// The library as the worker's events have it after one of them.
#[derive(Debug, Clone)]
struct View {
    roots: Vec<crate::library::LibraryRoot>,
    tracks: Vec<std::sync::Arc<crate::library::LibraryTrack>>,
    scan_in_progress: bool,
    last_error: Option<String>,
}

/// The worker's events, applied as they are received.
struct Events {
    rx: Receiver<LibraryEvent>,
    mirror: crate::library::LibraryMirror,
}

impl Events {
    fn new(rx: Receiver<LibraryEvent>) -> Self {
        Self {
            rx,
            mirror: crate::library::LibraryMirror::default(),
        }
    }

    fn view(&self) -> View {
        View {
            roots: self.mirror.snapshot.roots.clone(),
            tracks: self.mirror.snapshot.tracks.clone(),
            scan_in_progress: self.mirror.status.scan_in_progress,
            last_error: self.mirror.status.last_error.clone(),
        }
    }
}

/// The library after the first event that leaves it satisfying `wanted`,
/// waiting up to 20 s.
fn next_snapshot(events: &mut Events, mut wanted: impl FnMut(&View) -> bool) -> View {
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        match events.rx.recv_deadline(deadline) {
            Ok(event) => {
                events.mirror.apply(&event);
                let view = events.view();
                if wanted(&view) {
                    return view;
                }
            }
            Err(error) => panic!("no such snapshot: {error}"),
        }
    }
}

fn idle_with_tracks(snapshot: &View, count: usize) -> bool {
    !snapshot.scan_in_progress && snapshot.tracks.len() == count
}

#[test]
fn removing_a_root_during_its_scan_does_not_wait_for_the_scan() {
    let fixture = Fixture::new("remove", &["a.mp3", "b.mp3", "c.mp3", "d.mp3", "e.mp3"]);
    let (started, release) = hold_read(&fixture.track("c.mp3"));
    let (service, mut events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: "Music".to_string(),
    });
    started
        .recv_timeout(Duration::from_secs(20))
        .expect("the scan reads the held file");

    service.command(LibraryCommand::RemoveRoot(fixture.root.clone()));

    // The read is still held: the scan has not finished.
    let removed = next_snapshot(&mut events, |snapshot| snapshot.roots.is_empty());
    assert!(removed.tracks.is_empty(), "{:?}", removed.tracks);
    assert_eq!(removed.last_error, None);
    drop(release);
    let done = next_snapshot(&mut events, |snapshot| !snapshot.scan_in_progress);
    assert!(done.roots.is_empty());
    assert_eq!(done.last_error, None);
    drop(service);
    assert_eq!(fixture.indexed(), Vec::<PathBuf>::new());
    let roots: i64 = fixture
        .conn()
        .query_row("SELECT COUNT(*) FROM roots", [], |row| row.get(0))
        .expect("roots");
    assert_eq!(roots, 0, "the stopped scan did not add the root back");
}

#[test]
fn renaming_a_root_during_its_scan_applies_at_once() {
    let fixture = Fixture::new("rename", &["a.mp3", "b.mp3", "c.mp3"]);
    let (started, release) = hold_read(&fixture.track("b.mp3"));
    let (service, mut events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: "Old".to_string(),
    });
    started
        .recv_timeout(Duration::from_secs(20))
        .expect("the scan reads the held file");

    service.command(LibraryCommand::RenameRoot {
        path: fixture.root.clone(),
        name: "New".to_string(),
    });

    let renamed = next_snapshot(&mut events, |snapshot| {
        snapshot
            .roots
            .first()
            .is_some_and(|root| root.name == "New")
    });
    assert!(renamed.scan_in_progress, "the scan is still running");
    drop(release);
    let done = next_snapshot(&mut events, |snapshot| idle_with_tracks(snapshot, 3));
    assert_eq!(done.roots[0].name, "New");
    assert_eq!(done.last_error, None);
}

#[test]
fn rescans_requested_during_a_scan_of_the_root_merge_with_it() {
    let fixture = Fixture::new("merge", &["a.mp3", "b.mp3", "c.mp3"]);
    let (service, mut events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    next_snapshot(&mut events, |snapshot| idle_with_tracks(snapshot, 3));
    fs::write(fixture.track("b.mp3"), b"changed").expect("change a track");
    let (started, release) = hold_read(&fixture.track("b.mp3"));
    service.command(LibraryCommand::RescanRoot(fixture.root.clone()));
    started
        .recv_timeout(Duration::from_secs(20))
        .expect("the rescan reads the changed file");

    for _ in 0..3 {
        service.command(LibraryCommand::RescanRoot(fixture.root.clone()));
    }
    // Applied while the scan runs, so the rescans before it were received
    // then too.
    service.command(LibraryCommand::RenameRoot {
        path: fixture.root.clone(),
        name: "Seen".to_string(),
    });
    next_snapshot(&mut events, |snapshot| {
        snapshot.scan_in_progress
            && snapshot
                .roots
                .first()
                .is_some_and(|root| root.name == "Seen")
    });
    // Queued after any rescan that did not merge.
    let missing = fixture.folder.join("not-a-root");
    service.command(LibraryCommand::RescanRoot(missing));
    drop(release);

    let done = next_snapshot(&mut events, |snapshot| {
        snapshot
            .last_error
            .as_deref()
            .is_some_and(|error| error.contains("is not configured"))
    });
    assert_eq!(done.tracks.len(), 3);
    assert_eq!(scans_of(&fixture.root), 2, "the add and one rescan");
}

#[test]
fn stopping_the_service_stops_a_scan_and_keeps_its_committed_batches() {
    let fixture = Fixture::new("shutdown", &["a.mp3", "b.mp3", "c.mp3", "d.mp3"]);
    let (started, release) = hold_read(&fixture.track("d.mp3"));
    let (service, events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    started
        .recv_timeout(Duration::from_secs(20))
        .expect("the scan reads the held file");
    // Batches are published at intervals; wait for the first in the database.
    let deadline = Instant::now() + Duration::from_secs(20);
    while fixture.indexed().len() < 2 {
        assert!(Instant::now() < deadline, "no batch was committed");
        std::thread::sleep(Duration::from_millis(10));
    }
    drop(events);

    let stopping = Instant::now();
    drop(service);

    assert!(
        stopping.elapsed() < Duration::from_secs(1),
        "stopping took {:?}",
        stopping.elapsed()
    );
    let indexed = fixture.indexed();
    assert_eq!(indexed.len(), 2, "one batch: {indexed:?}");
    assert!(!indexed.contains(&fixture.track("d.mp3")));
    assert!(
        !fixture.folder.join("library.sqlite3-wal").exists(),
        "the log was checkpointed when the database closed"
    );
    drop(release);
}

#[test]
fn the_worker_migrates_a_version_0_library_and_publishes_all_of_it() {
    let fixture = Fixture::new("migrate", &[]);
    let roots = build_version_0_library(&fixture.db, 200);
    let (service, events) = LibraryService::with_database(
        fixture.db.clone(),
        Duration::from_secs(5),
        ScanTuning {
            batch_rows: 2,
            batch_interval: Duration::from_secs(60),
            workers: 2,
        },
    );

    let mut events = Events::new(events);
    let first = next_snapshot(&mut events, |_| true);

    assert_eq!(first.last_error, None);
    assert_eq!(
        first
            .roots
            .iter()
            .map(|root| root.path.clone())
            .collect::<Vec<_>>(),
        {
            let mut paths: Vec<PathBuf> = roots.iter().map(|(path, _)| path.clone()).collect();
            paths.sort_by_key(|path| path.to_string_lossy().to_lowercase());
            paths
        }
    );
    assert_eq!(first.tracks.len(), 600);
    assert_eq!(first.roots[0].name, "Spaced name");
    drop(service);
    let conn = fixture.conn();
    assert_eq!(user_version(&conn).expect("version"), SCHEMA_VERSION);
    assert!(schema_objects(&conn)
        .iter()
        .all(|(_, name, _)| !name.starts_with("tracks_fts")));
}

#[test]
fn the_worker_reports_a_damaged_database_that_it_moved_aside() {
    let fixture = Fixture::new("damaged", &[]);
    fs::write(&fixture.db, b"not a database".repeat(400)).expect("damaged file");
    let (service, events) = LibraryService::with_database(
        fixture.db.clone(),
        Duration::from_secs(5),
        ScanTuning {
            batch_rows: 2,
            batch_interval: Duration::from_secs(60),
            workers: 2,
        },
    );

    let mut events = Events::new(events);
    let first = next_snapshot(&mut events, |_| true);

    let error = first.last_error.expect("the move is shown");
    assert!(error.contains("was damaged"), "{error}");
    assert!(error.contains(".damaged-"), "{error}");
    assert!(first.roots.is_empty());
    drop(service);
}

/// The events until the worker is idle again after a command.
fn events_until_idle(events: &mut Events) -> Vec<LibraryEvent> {
    let deadline = Instant::now() + Duration::from_secs(20);
    let mut received = Vec::new();
    loop {
        let event = events
            .rx
            .recv_deadline(deadline)
            .expect("the command finishes");
        events.mirror.apply(&event);
        let idle = !event.status.scan_in_progress;
        received.push(event);
        if idle && received.len() > 1 {
            return received;
        }
    }
}

#[test]
fn an_unchanged_rescan_publishes_its_progress_and_no_rows() {
    let names: Vec<String> = (0..40).map(|index| format!("{index:02}.mp3")).collect();
    let names: Vec<&str> = names.iter().map(String::as_str).collect();
    let fixture = Fixture::new("unchanged-rescan", &names);
    let (service, mut events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    next_snapshot(&mut events, |snapshot| idle_with_tracks(snapshot, 40));
    events.mirror.drain(&events.rx);

    service.command(LibraryCommand::RescanRoot(fixture.root.clone()));
    let rescan = events_until_idle(&mut events);

    assert!(
        rescan.iter().any(|event| event.status.scan_in_progress),
        "the rescan's progress is published"
    );
    for event in &rescan {
        assert!(
            event.changes.is_empty(),
            "an unchanged rescan sends no rows: {:?}",
            event.changes
        );
    }
    assert_eq!(events.mirror.snapshot.tracks.len(), 40);
}

#[test]
fn a_rescan_publishes_only_the_rows_that_changed() {
    let names: Vec<String> = (0..40).map(|index| format!("{index:02}.mp3")).collect();
    let names: Vec<&str> = names.iter().map(String::as_str).collect();
    let fixture = Fixture::new("changed-rescan", &names);
    let (service, mut events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    next_snapshot(&mut events, |snapshot| idle_with_tracks(snapshot, 40));
    events.mirror.drain(&events.rx);
    // The scan reads this file again, but its row stays the same.
    fs::write(fixture.track("07.mp3"), b"changed, and longer").expect("change a track");
    fs::remove_file(fixture.track("08.mp3")).expect("remove a track");
    fs::write(fixture.track("new.mp3"), b"new").expect("add a track");

    service.command(LibraryCommand::RescanRoot(fixture.root.clone()));
    let rescan = events_until_idle(&mut events);

    let mut upserted: Vec<PathBuf> = rescan
        .iter()
        .flat_map(|event| {
            event
                .changes
                .upserted
                .iter()
                .map(|track| track.path.clone())
        })
        .collect();
    upserted.sort();
    let removed: Vec<PathBuf> = rescan
        .iter()
        .flat_map(|event| event.changes.removed.iter().cloned())
        .collect();
    assert_eq!(upserted, [fixture.track("new.mp3")]);
    assert_eq!(removed, [fixture.track("08.mp3")]);
    assert!(rescan.iter().all(|event| event.changes.roots.is_none()));
    assert_eq!(events.mirror.paths(), fixture.indexed());
}

#[test]
fn reading_rows_again_corrects_a_copy_that_diverged() {
    let fixture = Fixture::new("read-rows", &["a.mp3", "b.mp3"]);
    let (service, mut events) = fixture.start();
    service.command(LibraryCommand::AddRoot {
        path: fixture.root.clone(),
        name: String::new(),
    });
    next_snapshot(&mut events, |snapshot| idle_with_tracks(snapshot, 2));
    events.mirror.drain(&events.rx);
    // A copy that applied an older change after a newer one: a's details
    // are stale, and it has a track the database no longer has.
    let a = fixture.track("a.mp3");
    let stale = fixture.track("stale.mp3");
    events.mirror.snapshot.apply_changes(
        &crate::library::LibraryChanges {
            upserted: vec![
                std::sync::Arc::new(crate::library::LibraryTrack {
                    path: a.clone(),
                    title: "Stale".to_string(),
                    ..crate::library::LibraryTrack::default()
                }),
                std::sync::Arc::new(crate::library::LibraryTrack {
                    path: stale.clone(),
                    ..crate::library::LibraryTrack::default()
                }),
            ],
            ..crate::library::LibraryChanges::default()
        },
        |path| {
            if path == a {
                vec![0]
            } else {
                Vec::new()
            }
        },
    );

    service.command(LibraryCommand::ReadRows(vec![a.clone(), stale]));
    let corrected = next_snapshot(&mut events, |snapshot| snapshot.tracks.len() == 2);

    let read = corrected
        .tracks
        .iter()
        .find(|track| track.path == a)
        .expect("a is still there");
    assert_ne!(read.title, "Stale");
    assert_eq!(events.mirror.paths(), fixture.indexed());
}
