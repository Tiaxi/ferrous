// SPDX-License-Identifier: GPL-3.0-or-later

//! Files that cannot be read fully when the index reads them again outside a
//! full scan: after a tag edit, a rename, a cover change, and the search-tag
//! backfill, and for the external-track cache.

use super::*;
use crate::metadata::{encoded_picture, test_cache, write_wav_with_picture};
use crate::raw_audio::write_test_apev2_file;
use crate::vorbis_comment_scan::fixtures::{comment_block, flac};

const SAVED_TAGS: [&[u8]; 3] = [
    b"TITLE=Saved Title",
    b"ARTIST=Saved Artist",
    b"ALBUM=Saved Album",
];

fn saved_tags() -> (String, String, String) {
    (
        "Saved Title".to_string(),
        "Saved Artist".to_string(),
        "Saved Album".to_string(),
    )
}

fn test_dir(name: &str) -> PathBuf {
    static NEXT_ID: AtomicU64 = AtomicU64::new(0);
    let dir = std::env::temp_dir().join(format!(
        "ferrous-refresh-{name}-{}-{}",
        std::process::id(),
        NEXT_ID.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&dir);
    fs::create_dir_all(&dir).expect("create test dir");
    dir.canonicalize().expect("canonical test dir")
}

fn test_conn() -> Connection {
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    conn
}

/// A FLAC file at `path`, indexed with [`SAVED_TAGS`] and cached as an
/// external track too, then given a `COVERART` picture too short for lofty
/// 0.24, which panics on any read of it. Returns the file state indexed.
fn index_then_break(conn: &mut Connection, root: &Path, path: &Path) -> TrackFileFingerprint {
    fs::write(path, flac(&[&comment_block(&SAVED_TAGS)])).expect("write track");
    insert_indexed_tracks(conn, root, &[path.to_path_buf()]);
    let indexed = track_file_fingerprint(path).expect("fingerprint");
    store_external_track_cache_in_conn(conn, path, indexed, &read_track_info(path))
        .expect("cache row");
    let mut broken = SAVED_TAGS.to_vec();
    broken.push(b"COVERART=AAAA");
    fs::write(path, flac(&[&comment_block(&broken)])).expect("break track");
    // The fixture must still make lofty panic, or these tests prove nothing.
    assert!(std::panic::catch_unwind(|| lofty::read_from_path(path).is_ok()).is_err());
    assert_ne!(track_file_fingerprint(path), Some(indexed));
    indexed
}

/// The `(title, artist, album)` of the library row of `path`.
fn row_tags(conn: &Connection, path: &Path) -> (String, String, String) {
    conn.query_row(
        "SELECT title, artist, album FROM tracks WHERE path = ?1",
        params![path.to_string_lossy()],
        |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
    )
    .expect("track row")
}

/// The file state the library row of `path` was read at.
fn row_state(conn: &Connection, path: &Path) -> TrackFileFingerprint {
    conn.query_row(
        "SELECT mtime_ns, size_bytes FROM tracks WHERE path = ?1",
        params![path.to_string_lossy()],
        |row| {
            Ok(TrackFileFingerprint {
                mtime_ns: row.get(0)?,
                size_bytes: row.get(1)?,
            })
        },
    )
    .expect("track row")
}

/// The `(title, cover_path)` and file state of the external-track cache row
/// of `path`.
fn external_row_of(conn: &Connection, path: &Path) -> ((String, String), TrackFileFingerprint) {
    let (indexed, fingerprint) = external_row(conn, &path.to_string_lossy())
        .expect("query")
        .expect("cache row");
    ((indexed.title, indexed.cover_path), fingerprint)
}

/// The row's `(cover_path, cover_checked)`.
fn cover_row(conn: &Connection, path: &Path) -> (String, bool) {
    conn.query_row(
        "SELECT cover_path, cover_checked FROM tracks WHERE path = ?1",
        params![path.to_string_lossy()],
        |row| Ok((row.get(0)?, row.get::<_, i64>(1)? != 0)),
    )
    .expect("track row")
}

fn assert_reports_tags(problem: Option<&String>, path: &Path) {
    let problem = problem.expect("the unreadable file is reported");
    assert!(
        problem.contains("could not read the tags of 1 file; kept the tags indexed before"),
        "{problem}"
    );
    assert!(problem.contains(&path.display().to_string()), "{problem}");
    assert!(problem.contains("the tag reader for"), "{problem}");
}

fn assert_reports_cover(problem: Option<&String>) {
    let problem = problem.expect("the uncached cover is reported");
    assert!(
        problem
            .contains("could not cache the embedded covers of 1 file; kept their previous covers"),
        "{problem}"
    );
}

#[test]
fn a_metadata_refresh_keeps_the_indexed_tags_of_a_file_whose_reader_panics() {
    let mut conn = test_conn();
    let cache = test_cache("refresh-panic");
    let dir = test_dir("refresh-panic");
    let broken = dir.join("broken.flac");
    let indexed_state = index_then_break(&mut conn, &dir, &broken);
    // Lofty cannot read raw surround files at all; they take their tags from
    // an appended APEv2 tag, which a refresh still reads.
    let surround = dir.join("surround.ac3");
    write_test_apev2_file(&surround, &[("Title", "Old")], true);
    insert_indexed_tracks(&mut conn, &dir, std::slice::from_ref(&surround));
    write_test_apev2_file(&surround, &[("Title", "Harvest")], true);

    let refresh = refresh_indexed_metadata_in_conn(
        &mut conn,
        &[broken.clone(), surround.clone()],
        Some(&cache),
    )
    .expect("refresh");

    assert_eq!(row_tags(&conn, &broken), saved_tags());
    assert_eq!(refresh.indexed[&broken].title, "Saved Title");
    // The row keeps its old file state, so the next scan reads it again.
    assert_eq!(row_state(&conn, &broken), indexed_state);
    // The cache row is not replaced either.
    let (cached, cached_state) = external_row_of(&conn, &broken);
    assert_eq!(
        (cached.0.as_str(), cached_state),
        ("Saved Title", indexed_state)
    );
    assert_reports_tags(refresh.problem.as_ref(), &broken);
    assert_eq!(row_tags(&conn, &surround).0, "Harvest");
    let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_rename_keeps_the_indexed_tags_of_a_file_whose_reader_panics() {
    let mut conn = test_conn();
    let cache = test_cache("rename-panic");
    let dir = test_dir("rename-panic");
    let old = dir.join("01 - broken.flac");
    let indexed_state = index_then_break(&mut conn, &dir, &old);
    let new = dir.join("02 - broken.flac");
    let rowid = |conn: &Connection, path: &Path| -> i64 {
        conn.query_row(
            "SELECT rowid FROM tracks WHERE path = ?1",
            params![path.to_string_lossy()],
            |row| row.get(0),
        )
        .expect("track row")
    };
    let before = rowid(&conn, &old);
    let renames = [(old.clone(), new.clone())];

    let refresh = rename_with_journal(
        &mut conn,
        &renames,
        DiskRenameJournal::new(&renames),
        Some(&cache),
    )
    .expect("rename");

    assert!(new.is_file() && !old.exists());
    assert_eq!(rowid(&conn, &new), before, "the row follows its file");
    assert_eq!(row_tags(&conn, &new), saved_tags());
    assert_eq!(row_state(&conn, &new), indexed_state);
    assert_eq!(refresh.indexed[&new].title, "Saved Title");
    let (cached, cached_state) = external_row_of(&conn, &new);
    assert_eq!(
        (cached.0.as_str(), cached_state),
        ("Saved Title", indexed_state)
    );
    assert!(external_row(&conn, &old.to_string_lossy())
        .expect("query")
        .is_none());
    assert_reports_tags(refresh.problem.as_ref(), &new);
    let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_cover_refresh_keeps_the_row_of_a_file_whose_reader_panics() {
    let mut conn = test_conn();
    let cache = test_cache("cover-refresh-panic");
    let dir = test_dir("cover-refresh-panic");
    let broken = dir.join("broken.flac");
    let indexed_state = index_then_break(&mut conn, &dir, &broken);
    conn.execute(
        "UPDATE tracks SET cover_path = '/covers/saved.jpg' WHERE path = ?1",
        params![broken.to_string_lossy()],
    )
    .expect("saved cover");

    let refresh = refresh_cover_paths_in_conn(&conn, std::slice::from_ref(&broken), Some(&cache))
        .expect("refresh");

    assert_eq!(row_tags(&conn, &broken), saved_tags());
    assert_eq!(
        cover_row(&conn, &broken),
        ("/covers/saved.jpg".to_string(), true)
    );
    assert_eq!(refresh.indexed[&broken].cover_path, "/covers/saved.jpg");
    assert_eq!(external_row_of(&conn, &broken).1, indexed_state);
    assert_reports_tags(refresh.problem.as_ref(), &broken);
    let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn the_search_tag_backfill_leaves_and_reports_a_file_whose_reader_panics() {
    let mut conn = test_conn();
    let dir = test_dir("backfill-panic");
    let broken = dir.join("broken.flac");
    index_then_break(&mut conn, &dir, &broken);
    conn.execute("UPDATE tracks SET search_tags = NULL", [])
        .expect("a row from before search tags");
    let mut snapshot = WorkerLibrary::default();
    snapshot.reload(&conn);
    let (event_tx, _event_rx) = unbounded();

    let problem = backfill_search_tags(&conn, &mut snapshot, &event_tx).expect("backfill");

    assert_reports_tags(problem.as_ref(), &broken);
    assert_eq!(row_tags(&conn, &broken), saved_tags());
    // Still without search tags, so the next scan reads the file again.
    assert!(super::super::schema::needs_search_tag_backfill(&conn));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn an_inner_root_reclaiming_a_track_whose_reader_panics_keeps_its_row() {
    let conn = test_conn();
    let outer = test_dir("reclaim-panic");
    let inner = outer.join("inner");
    fs::create_dir_all(&inner).expect("mkdir inner");
    let track = inner.join("01.flac");
    fs::write(&track, crate::panic_guard::fixtures::short_coverart_flac()).expect("write track");
    // A database from before nested roots were rejected: the outer root
    // still owns the inner folder's track, and the inner root was added
    // without a rescan.
    insert_root(&conn, &outer, "").expect("outer root");
    insert_root(&conn, &inner, "").expect("legacy inner root");
    conn.execute(
        "INSERT INTO tracks(path, root_path, title, artist, album, cover_checked, \
                            mtime_ns, size_bytes, indexed_at) \
         VALUES (?1, ?2, 'Saved Title', 'Saved Artist', 'Saved Album', 1, 1, 1, 1)",
        params![track.to_string_lossy(), outer.to_string_lossy()],
    )
    .expect("row owned by the outer root");

    let outcome = scan_root_observed(
        &conn,
        &inner,
        EmbeddedCoverCache::shared(),
        &mut |_| {},
        &mut |_, _| {},
    )
    .expect("scan");

    let problem = outcome.problem.expect("the unreadable file is reported");
    assert!(
        problem.contains("could not read the tags of 1 file"),
        "{problem}"
    );
    assert_eq!(row_tags(&conn, &track), saved_tags());
    let _ = fs::remove_dir_all(outer);
}

/// A WAV file with a picture whose cache entry is blocked, and a library row
/// and an external-track cache row for it from an older file state, with
/// `previous` as their cover. Returns the file and the blocked entry.
fn track_with_blocked_cover(
    conn: &Connection,
    cache: &EmbeddedCoverCache,
    dir: &Path,
    seed: u8,
    previous: &str,
) -> (PathBuf, PathBuf) {
    let picture = encoded_picture(seed, image::ImageFormat::Png);
    let track = dir.join("01.wav");
    write_wav_with_picture(&track, &picture);
    conn.execute(
        "INSERT INTO tracks(path, root_path, title, artist, album, cover_path, cover_checked, \
                            mtime_ns, size_bytes, indexed_at) \
         VALUES (?1, ?2, 'Old', '', '', ?3, 1, 1, 1, 1)",
        params![track.to_string_lossy(), dir.to_string_lossy(), previous],
    )
    .expect("indexed row");
    let old_state = TrackFileFingerprint {
        mtime_ns: 1,
        size_bytes: 1,
    };
    let mut cached = untagged_track(&track);
    cached.cover_path = previous.to_string();
    store_external_track_cache_in_conn(conn, &track, old_state, &cached).expect("cache row");
    // A directory where the entry belongs makes writing it fail.
    let entry = cache
        .dir()
        .join(format!("{:x}.png", md5::compute(&picture)));
    fs::create_dir_all(&entry).expect("block the entry");
    (track, entry)
}

fn assert_cover_kept(conn: &Connection, track: &Path, previous: &str, refresh: &TrackRefresh) {
    assert_eq!(cover_row(conn, track), (previous.to_string(), false));
    assert_eq!(refresh.indexed[track].cover_path, previous);
    let ((_, cover), state) = external_row_of(conn, track);
    assert_eq!(
        (cover.as_str(), state.mtime_ns),
        (previous, 1),
        "the cache row is not replaced"
    );
    assert_reports_cover(refresh.problem.as_ref());
}

fn assert_cover_cached(conn: &Connection, track: &Path, entry: &Path, refresh: &TrackRefresh) {
    let entry = entry.to_string_lossy().into_owned();
    assert_eq!(cover_row(conn, track), (entry.clone(), true));
    assert_eq!(refresh.indexed[track].cover_path, entry);
    assert_eq!(external_row_of(conn, track).0 .1, entry);
    assert!(refresh.problem.is_none(), "{:?}", refresh.problem);
}

#[test]
fn a_metadata_refresh_keeps_the_previous_cover_when_the_picture_cannot_be_cached() {
    let mut conn = test_conn();
    let cache = test_cache("refresh-blocked-entry");
    let dir = test_dir("refresh-blocked-cover");
    let previous = "/covers/previous.jpg";
    let (track, entry) = track_with_blocked_cover(&conn, &cache, &dir, 40, previous);
    let paths = [track.clone()];

    let refresh =
        refresh_indexed_metadata_in_conn(&mut conn, &paths, Some(&cache)).expect("refresh");

    assert_cover_kept(&conn, &track, previous, &refresh);
    fs::remove_dir(&entry).expect("unblock the entry");
    let refresh = refresh_indexed_metadata_in_conn(&mut conn, &paths, Some(&cache)).expect("retry");
    assert_cover_cached(&conn, &track, &entry, &refresh);
    let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_rename_keeps_the_previous_cover_when_the_picture_cannot_be_cached() {
    let mut conn = test_conn();
    let cache = test_cache("rename-blocked-entry");
    let dir = test_dir("rename-blocked-cover");
    let previous = "/covers/previous.jpg";
    let (old, entry) = track_with_blocked_cover(&conn, &cache, &dir, 41, previous);
    let new = dir.join("02.wav");
    let renames = [(old.clone(), new.clone())];

    let refresh = rename_with_journal(
        &mut conn,
        &renames,
        DiskRenameJournal::new(&renames),
        Some(&cache),
    )
    .expect("rename");

    assert_cover_kept(&conn, &new, previous, &refresh);
    fs::remove_dir(&entry).expect("unblock the entry");
    let renames = [(new.clone(), old.clone())];
    let refresh = rename_with_journal(
        &mut conn,
        &renames,
        DiskRenameJournal::new(&renames),
        Some(&cache),
    )
    .expect("rename back");
    assert_cover_cached(&conn, &old, &entry, &refresh);
    let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn a_cover_refresh_keeps_the_previous_cover_when_the_picture_cannot_be_cached() {
    let conn = test_conn();
    let cache = test_cache("cover-refresh-blocked-entry");
    let dir = test_dir("cover-refresh-blocked-cover");
    let previous = "/covers/previous.jpg";
    let (track, entry) = track_with_blocked_cover(&conn, &cache, &dir, 42, previous);
    let paths = [track.clone()];

    let refresh = refresh_cover_paths_in_conn(&conn, &paths, Some(&cache)).expect("refresh");

    assert_cover_kept(&conn, &track, previous, &refresh);
    fs::remove_dir(&entry).expect("unblock the entry");
    let refresh = refresh_cover_paths_in_conn(&conn, &paths, Some(&cache)).expect("retry");
    assert_cover_cached(&conn, &track, &entry, &refresh);
    let _ = fs::remove_dir_all(cache.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}

#[test]
fn the_external_track_cache_keeps_its_row_for_a_read_that_failed() {
    let cache = ExternalTrackCache::in_memory();
    let conn = cache.0.as_ref().expect("cache db");
    let covers = test_cache("external-cache");
    let dir = test_dir("external-cache");
    let read = |path: &Path| ExternalTrackRead(read_track_catching_panic(path, Some(&covers)));

    // Tags that cannot be read show the cached tags.
    let broken = dir.join("broken.flac");
    fs::write(&broken, flac(&[&comment_block(&SAVED_TAGS)])).expect("write track");
    let saved_state = track_file_fingerprint(&broken).expect("fingerprint");
    let shown = cache.store(&broken, saved_state, read(&broken));
    assert_eq!(shown.title, "Saved Title");
    let mut broken_tags = SAVED_TAGS.to_vec();
    broken_tags.push(b"COVERART=AAAA");
    fs::write(&broken, flac(&[&comment_block(&broken_tags)])).expect("break track");
    let broken_state = track_file_fingerprint(&broken).expect("fingerprint");

    let shown = cache.store(&broken, broken_state, read(&broken));

    assert_eq!(
        (
            shown.title.as_str(),
            shown.artist.as_str(),
            shown.album.as_str()
        ),
        ("Saved Title", "Saved Artist", "Saved Album")
    );
    assert_eq!(
        external_row_of(conn, &broken).1,
        saved_state,
        "not replaced"
    );

    // A picture that cannot be cached shows the cached cover.
    let previous = "/covers/previous.jpg";
    let (track, entry) = track_with_blocked_cover(conn, &covers, &dir, 43, previous);
    let state = track_file_fingerprint(&track).expect("fingerprint");

    let shown = cache.store(&track, state, read(&track));

    assert_eq!(shown.cover_path, previous);
    assert_eq!(external_row_of(conn, &track).1.mtime_ns, 1, "not replaced");
    fs::remove_dir(&entry).expect("unblock the entry");
    let shown = cache.store(&track, state, read(&track));
    let entry = entry.to_string_lossy().into_owned();
    assert_eq!(shown.cover_path, entry);
    assert_eq!(
        load_external_track_cache_from_conn(conn, &track, state).map(|row| row.cover_path),
        Some(entry)
    );
    let _ = fs::remove_dir_all(covers.dir().parent().expect("cache root"));
    let _ = fs::remove_dir_all(dir);
}
