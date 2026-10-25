// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::{Path, PathBuf};
use std::sync::Arc;

use crossbeam_channel::unbounded;
use rusqlite::{params, Connection};

use super::*;
use crate::library::schema::init_schema;

fn track(path: &str, title: &str) -> LibraryTrack {
    LibraryTrack {
        path: PathBuf::from(path),
        root_path: PathBuf::from("/music"),
        title: title.to_string(),
        ..LibraryTrack::default()
    }
}

fn shared(path: &str, title: &str) -> Arc<LibraryTrack> {
    Arc::new(track(path, title))
}

/// Positions by path in `snapshot`, as an index would find them.
fn positions_in(snapshot: &LibrarySnapshot) -> impl Fn(&Path) -> Vec<usize> + '_ {
    move |path| {
        snapshot
            .tracks
            .iter()
            .enumerate()
            .filter(|(_, track)| track.path == path)
            .map(|(index, _)| index)
            .collect()
    }
}

fn apply(snapshot: &mut LibrarySnapshot, changes: &LibraryChanges) -> AppliedChanges {
    let before = snapshot.clone();
    snapshot.apply_changes(changes, positions_in(&before))
}

fn titles(snapshot: &LibrarySnapshot) -> Vec<(String, String)> {
    let mut titles: Vec<(String, String)> = snapshot
        .tracks
        .iter()
        .map(|track| {
            (
                track.path.to_string_lossy().into_owned(),
                track.title.clone(),
            )
        })
        .collect();
    titles.sort();
    titles
}

#[test]
fn applying_changes_replaces_in_place_appends_and_removes() {
    let mut snapshot = LibrarySnapshot::default();
    let first = apply(
        &mut snapshot,
        &LibraryChanges {
            roots: None,
            upserted: vec![
                shared("/m/a", "A"),
                shared("/m/b", "B"),
                shared("/m/c", "C"),
            ],
            removed: Vec::new(),
        },
    );
    assert!(first.changed && first.positions_kept);
    assert_eq!(first.appended_from, 0);
    let kept = Arc::clone(&snapshot.tracks[0]);

    // An edit and an addition keep every position.
    let edit = apply(
        &mut snapshot,
        &LibraryChanges {
            roots: None,
            upserted: vec![shared("/m/b", "B2"), shared("/m/d", "D")],
            removed: Vec::new(),
        },
    );
    assert!(edit.changed && edit.positions_kept);
    assert_eq!(edit.appended_from, 3);
    assert_eq!(snapshot.tracks[1].title, "B2");
    assert_eq!(snapshot.tracks[3].path, PathBuf::from("/m/d"));
    assert!(
        Arc::ptr_eq(&snapshot.tracks[0], &kept),
        "unchanged tracks are shared"
    );

    let removal = apply(
        &mut snapshot,
        &LibraryChanges {
            roots: None,
            upserted: Vec::new(),
            removed: vec![PathBuf::from("/m/a"), PathBuf::from("/m/missing")],
        },
    );
    assert!(removal.changed && !removal.positions_kept);
    assert_eq!(
        titles(&snapshot),
        [("/m/b", "B2"), ("/m/c", "C"), ("/m/d", "D")]
            .map(|(path, title)| (path.to_string(), title.to_string()))
    );
}

#[test]
fn an_upsert_equal_to_the_track_changes_nothing() {
    let mut snapshot = LibrarySnapshot::default();
    apply(
        &mut snapshot,
        &LibraryChanges {
            upserted: vec![shared("/m/a", "A")],
            ..LibraryChanges::default()
        },
    );
    let revision = snapshot.search_revision;

    let applied = apply(
        &mut snapshot,
        &LibraryChanges {
            upserted: vec![shared("/m/a", "A")],
            ..LibraryChanges::default()
        },
    );

    assert!(!applied.changed);
    assert_eq!(snapshot.search_revision, revision);
}

#[test]
fn an_upsert_leaves_one_track_at_a_path_a_copy_got_twice() {
    let mut snapshot = LibrarySnapshot::new(
        Vec::new(),
        vec![
            track("/m/a", "Old"),
            track("/m/b", "B"),
            track("/m/a", "Older"),
        ],
    );

    apply(
        &mut snapshot,
        &LibraryChanges {
            upserted: vec![shared("/m/a", "New")],
            ..LibraryChanges::default()
        },
    );

    assert_eq!(
        titles(&snapshot),
        [("/m/a", "New"), ("/m/b", "B")].map(|(path, title)| (path.to_string(), title.to_string()))
    );
}

#[test]
fn merged_changes_apply_as_the_changes_in_order() {
    let base = LibrarySnapshot::new(Vec::new(), vec![track("/m/a", "A"), track("/m/b", "B")]);
    let steps = [
        LibraryChanges {
            upserted: vec![shared("/m/c", "C"), shared("/m/a", "A2")],
            removed: vec![PathBuf::from("/m/b")],
            ..LibraryChanges::default()
        },
        LibraryChanges {
            roots: Some(vec![LibraryRoot {
                path: PathBuf::from("/m"),
                name: String::new(),
            }]),
            upserted: vec![shared("/m/b", "B again"), shared("/m/c", "C2")],
            removed: vec![PathBuf::from("/m/a")],
        },
        LibraryChanges {
            upserted: vec![shared("/m/a", "A3")],
            removed: vec![PathBuf::from("/m/c")],
            ..LibraryChanges::default()
        },
    ];

    let mut in_order = base.clone();
    for step in &steps {
        apply(&mut in_order, step);
    }
    let mut merged = LibraryChanges::default();
    for step in steps {
        merged.merge(step);
    }
    let mut at_once = base;
    apply(&mut at_once, &merged);

    assert_eq!(titles(&at_once), titles(&in_order));
    assert_eq!(at_once.roots, in_order.roots);
    assert_eq!(
        titles(&at_once),
        [("/m/a", "A3"), ("/m/b", "B again")]
            .map(|(path, title)| (path.to_string(), title.to_string()))
    );
}

fn memory_db() -> Connection {
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    conn
}

fn insert(conn: &Connection, path: &str, title: &str) {
    conn.execute(
        "INSERT INTO tracks(path, root_path, title, artist, album, mtime_ns, size_bytes, indexed_at) \
         VALUES (?1, '/music', ?2, '', '', 0, 0, 0) \
         ON CONFLICT(path) DO UPDATE SET title = excluded.title",
        params![path, title],
    )
    .expect("insert track");
}

fn events(rx: &crossbeam_channel::Receiver<LibraryEvent>) -> Vec<LibraryEvent> {
    rx.try_iter().collect()
}

#[test]
fn a_reload_publishes_only_the_rows_that_changed() {
    let conn = memory_db();
    for index in 0..100 {
        insert(&conn, &format!("/music/{index:03}.flac"), "Song");
    }
    let mut library = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    library.reload(&conn);
    library.publish(&tx);
    let first = events(&rx);
    assert_eq!(first.len(), 1);
    assert_eq!(
        first[0].changes.upserted.len(),
        100,
        "the first event has every row"
    );

    // Another connection edits one row and deletes another.
    insert(&conn, "/music/007.flac", "Edited");
    conn.execute("DELETE FROM tracks WHERE path = '/music/042.flac'", [])
        .expect("delete");
    library.reload(&conn);
    library.publish(&tx);

    let second = events(&rx);
    assert_eq!(second.len(), 1);
    let changes = &second[0].changes;
    assert!(changes.roots.is_none());
    assert_eq!(changes.upserted.len(), 1);
    assert_eq!(changes.upserted[0].title, "Edited");
    assert_eq!(changes.removed, [PathBuf::from("/music/042.flac")]);
}

#[test]
fn nothing_is_published_when_neither_rows_nor_status_changed() {
    let conn = memory_db();
    insert(&conn, "/music/a.flac", "A");
    let mut library = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    library.reload(&conn);
    library.publish(&tx);
    assert_eq!(events(&rx).len(), 1);

    library.reload(&conn);
    library.publish(&tx);
    assert!(events(&rx).is_empty());

    library.status.scan_in_progress = true;
    library.publish(&tx);
    let progress = events(&rx);
    assert_eq!(progress.len(), 1);
    assert!(progress[0].changes.is_empty(), "progress only");
    assert!(progress[0].status.scan_in_progress);
}

#[test]
fn reading_rows_publishes_them_even_when_the_worker_had_them() {
    let conn = memory_db();
    insert(&conn, "/music/a.flac", "A");
    insert(&conn, "/music/b.flac", "B");
    let mut library = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    library.reload(&conn);
    library.publish(&tx);
    events(&rx);

    library
        .read_rows(
            &conn,
            &[
                PathBuf::from("/music/a.flac"),
                PathBuf::from("/music/renamed-away.flac"),
            ],
        )
        .expect("read rows");
    library.publish(&tx);

    let read = events(&rx);
    assert_eq!(read.len(), 1);
    let changes = &read[0].changes;
    assert_eq!(changes.upserted.len(), 1);
    assert_eq!(changes.upserted[0].path, PathBuf::from("/music/a.flac"));
    assert_eq!(
        changes.removed,
        [PathBuf::from("/music/renamed-away.flac")],
        "a path the database lacks is removed from any copy that has it"
    );
}
