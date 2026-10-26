// SPDX-License-Identifier: GPL-3.0-or-later

//! Covers of tracks whose files did not change: scans check them again when
//! the images in their folder change.

use std::sync::atomic::AtomicUsize;

use super::*;
use crate::library::worker::{handle_command, Control};
use crate::library::LibraryCommand;
use crate::metadata::{encoded_picture, write_wav_with_picture};

fn test_root(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let dir = crate::paths::test_root().join(format!(
        "library-covers-{name}-{}",
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&dir);
    let root = dir.join("music");
    fs::create_dir_all(&root).expect("create test root");
    root.canonicalize().expect("canonical test root")
}

fn write(path: &Path, bytes: &[u8]) {
    fs::create_dir_all(path.parent().expect("parent")).expect("folder");
    fs::write(path, bytes).expect("write fixture");
}

/// The library worker's state for one test: its database, its copy of the
/// library, and the channel it publishes on.
struct Worker {
    conn: Connection,
    library: WorkerLibrary,
    events: Receiver<LibraryEvent>,
    tx: Sender<LibraryEvent>,
    root: PathBuf,
}

impl Worker {
    /// A worker with `root` added and scanned.
    fn scanned(root: &Path) -> Self {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");
        let (tx, events) = unbounded();
        let mut worker = Self {
            conn,
            library: WorkerLibrary::default(),
            events,
            tx,
            root: root.to_path_buf(),
        };
        worker.run(LibraryCommand::AddRoot {
            path: root.to_path_buf(),
            name: String::new(),
        });
        worker
    }

    fn run(&mut self, command: LibraryCommand) {
        handle_command(
            &self.conn,
            command,
            &mut self.library,
            &self.tx,
            &mut Control::detached(),
        );
        assert_eq!(self.library.status.last_error, None);
    }

    fn rescan(&mut self) {
        self.run(LibraryCommand::RescanRoot(self.root.clone()));
    }

    fn scan_paths(&mut self, paths: &[PathBuf]) {
        self.run(LibraryCommand::ScanPaths {
            root: self.root.clone(),
            paths: paths.to_vec(),
        });
    }

    /// The `(cover_path, cover_stamp)` of the row of `path`, after checking
    /// that the worker's copy, which it publishes, has the same.
    fn cover(&self, path: &Path) -> (String, u64) {
        let stored = self
            .conn
            .query_row(
                "SELECT cover_path, cover_stamp FROM tracks WHERE path = ?1",
                params![path.to_string_lossy()],
                |row| Ok((row.get::<_, String>(0)?, stamp_from_column(row.get(1)?))),
            )
            .expect("track row");
        let published = self
            .library
            .tracks()
            .iter()
            .find(|track| track.path == path)
            .map(|track| (track.cover_path.clone(), track.cover_stamp))
            .expect("published track");
        assert_eq!(published, stored, "the published row matches the database");
        stored
    }

    /// The paths of the rows that events since the last call published.
    fn published_rows(&self) -> Vec<PathBuf> {
        self.events
            .try_iter()
            .flat_map(|event| event.changes.upserted)
            .map(|track| track.path.clone())
            .collect()
    }
}

fn path_string(path: &Path) -> String {
    path.to_string_lossy().into_owned()
}

#[test]
fn a_rescan_picks_up_a_cover_image_added_after_the_first_scan() {
    let root = test_root("added");
    let track = root.join("Album/song.mp3");
    write(&track, b"not-real-mp3");
    let mut worker = Worker::scanned(&root);
    let (cover, first_stamp) = worker.cover(&track);
    assert_eq!(cover, "");
    assert_ne!(first_stamp, 0, "the folder without images has a stamp");
    worker.published_rows();

    let image = root.join("Album/cover.jpg");
    write(&image, b"not-real-jpg");
    // The audio file is the same, so checking its cover must not read it.
    scan_test_hooks::panic_on_read(&track);
    let before = std::time::Instant::now();
    worker.rescan();

    let (cover, stamp) = worker.cover(&track);
    assert_eq!(cover, path_string(&image));
    assert_ne!(stamp, first_stamp);
    let read_at = worker
        .library
        .tracks()
        .iter()
        .find(|row| row.path == track)
        .and_then(|row| row.cover_read_at.0);
    assert!(
        read_at.is_some_and(|read_at| read_at >= before),
        "the published row says when the scan read the folder"
    );
    assert_eq!(worker.published_rows(), std::slice::from_ref(&track));

    worker.rescan();
    assert_eq!(worker.cover(&track), (cover, stamp));
    assert!(
        worker.published_rows().is_empty(),
        "a folder that did not change again writes nothing"
    );
}

#[test]
fn renaming_or_removing_the_cover_image_changes_the_cover_at_the_next_scan() {
    let root = test_root("renamed");
    let track = root.join("Album/song.mp3");
    write(&track, b"not-real-mp3");
    write(&root.join("Album/cover.jpg"), b"cover");
    write(&root.join("Album/back.jpg"), b"back");
    let mut worker = Worker::scanned(&root);
    assert_eq!(
        worker.cover(&track).0,
        path_string(&root.join("Album/cover.jpg"))
    );

    fs::rename(root.join("Album/cover.jpg"), root.join("Album/Front.PNG")).expect("rename");
    worker.rescan();
    assert_eq!(
        worker.cover(&track).0,
        path_string(&root.join("Album/Front.PNG"))
    );

    fs::remove_file(root.join("Album/Front.PNG")).expect("remove");
    worker.rescan();
    assert_eq!(
        worker.cover(&track).0,
        path_string(&root.join("Album/back.jpg"))
    );

    // With no image left, only the file can tell whether it has a picture;
    // this one has none.
    fs::remove_file(root.join("Album/back.jpg")).expect("remove");
    worker.rescan();
    assert_eq!(worker.cover(&track).0, "");
}

#[test]
fn replacing_the_cover_image_at_the_same_name_gives_it_a_new_stamp() {
    let root = test_root("replaced");
    let track = root.join("Album/song.mp3");
    let image = root.join("Album/cover.jpg");
    write(&track, b"not-real-mp3");
    write(&image, b"old cover");
    let mut worker = Worker::scanned(&root);
    let (cover, stamp) = worker.cover(&track);

    // The way artwork apply and image editors replace a file: a new file
    // renamed over it.
    let replacement = root.join("Album/.cover.jpg.tmp");
    write(&replacement, b"the new cover, which is longer");
    fs::rename(&replacement, &image).expect("replace");
    scan_test_hooks::panic_on_read(&track);
    worker.rescan();

    let (new_cover, new_stamp) = worker.cover(&track);
    assert_eq!(new_cover, cover, "the same file holds the cover");
    assert_ne!(
        new_stamp, stamp,
        "views load the new image for the new stamp"
    );
}

#[test]
fn a_folder_that_loses_its_images_shows_the_embedded_picture() {
    let root = test_root("embedded");
    let track = root.join("Album/song.wav");
    fs::create_dir_all(track.parent().expect("parent")).expect("folder");
    write_wav_with_picture(&track, &encoded_picture(7, image::ImageFormat::Png));
    let image = root.join("Album/cover.jpg");
    write(&image, b"not-real-jpg");
    let mut worker = Worker::scanned(&root);
    assert_eq!(
        worker.cover(&track).0,
        path_string(&image),
        "the folder image comes before the embedded picture"
    );

    fs::remove_file(&image).expect("remove");
    worker.rescan();

    let (cover, _) = worker.cover(&track);
    let cache = EmbeddedCoverCache::shared().expect("test cover cache");
    assert!(
        Path::new(&cover).starts_with(cache.dir()),
        "the embedded picture's cache entry: {cover}"
    );
}

#[test]
fn a_scan_of_a_changed_folder_checks_the_covers_of_its_tracks_only() {
    let root = test_root("scoped");
    let changed = root.join("A/song.mp3");
    let other = root.join("B/song.mp3");
    write(&changed, b"not-real-mp3");
    write(&other, b"not-real-mp3");
    let mut worker = Worker::scanned(&root);
    worker.published_rows();

    write(&root.join("A/folder.png"), b"not-real-png");
    write(&root.join("B/folder.png"), b"not-real-png");
    scan_test_hooks::panic_on_read(&changed);
    // What the watcher sends for an image: its folder.
    worker.scan_paths(&[root.join("A")]);

    assert_eq!(
        worker.cover(&changed).0,
        path_string(&root.join("A/folder.png"))
    );
    assert_eq!(
        worker.cover(&other).0,
        "",
        "a folder outside the scan waits"
    );
    assert_eq!(worker.published_rows(), [changed]);
}

#[test]
fn rows_from_before_cover_stamps_get_the_current_rule_once() {
    let root = test_root("legacy");
    let track = root.join("Album/song.mp3");
    write(&track, b"not-real-mp3");
    write(&root.join("Album/back.jpg"), b"back");
    write(&root.join("Album/Cover.JPG"), b"cover");
    let mut worker = Worker::scanned(&root);
    // A row chosen by the rule before #139, which took `back.jpg`, and from
    // before cover stamps, as the version 2 migration leaves it.
    worker
        .conn
        .execute(
            "UPDATE tracks SET cover_path = ?2, cover_stamp = NULL WHERE path = ?1",
            params![
                path_string(&track),
                path_string(&root.join("Album/back.jpg"))
            ],
        )
        .expect("legacy row");
    worker.run(LibraryCommand::ReadRows(vec![track.clone()]));
    assert_eq!(worker.cover(&track).1, 0);
    worker.published_rows();

    scan_test_hooks::panic_on_read(&track);
    worker.rescan();
    let (cover, stamp) = worker.cover(&track);
    assert_eq!(cover, path_string(&root.join("Album/Cover.JPG")));
    assert_ne!(stamp, 0);
    assert_eq!(worker.published_rows(), std::slice::from_ref(&track));

    worker.rescan();
    assert!(worker.published_rows().is_empty(), "the check happens once");
}

#[test]
fn a_cover_check_does_not_overwrite_a_cover_written_since_the_scan_saw_the_row() {
    let root = test_root("guarded");
    let track = root.join("Album/song.mp3");
    write(&track, b"not-real-mp3");
    let worker = Worker::scanned(&root);
    write(&root.join("Album/cover.jpg"), b"not-real-jpg");
    let key = path_string(&track);
    let existing = load_existing_tracks_for_root(&worker.conn, &path_string(&root));
    let fingerprint = track_file_fingerprint(&track).expect("fingerprint");

    // Artwork apply wrote another cover after the scan read the row.
    worker
        .conn
        .execute(
            "UPDATE tracks SET cover_path = '/elsewhere/applied.png', cover_stamp = 5 \
             WHERE path = ?1",
            params![key],
        )
        .expect("applied cover");
    let written = write_batch(
        &worker.conn,
        &path_string(&root),
        0,
        &SeenRows::new(&existing, &HashMap::new()),
        vec![ScanRow::Cover(CoverUpdate {
            task: MetadataTask {
                path: track.clone(),
                path_string: key.clone(),
                mtime_ns: fingerprint.mtime_ns,
                size_bytes: fingerprint.size_bytes,
                file_id: None,
            },
            cover_path: Some(path_string(&root.join("Album/cover.jpg"))),
            stamp: 9,
            read_at: CoverReadAt::default(),
        })],
    )
    .expect("batch");

    assert!(written.is_empty());
    let stored: (String, i64) = worker
        .conn
        .query_row(
            "SELECT cover_path, cover_stamp FROM tracks WHERE path = ?1",
            params![key],
            |row| Ok((row.get(0)?, row.get(1)?)),
        )
        .expect("row");
    assert_eq!(stored, ("/elsewhere/applied.png".to_string(), 5));
}
