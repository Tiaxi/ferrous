use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use crossbeam_channel::{unbounded, Receiver, Sender};
use lofty::file::{AudioFile, TaggedFileExt};
use lofty::prelude::Accessor;
use rusqlite::{params, Connection};
use walkdir::WalkDir;

#[derive(Debug, Clone, Default)]
pub struct LibraryTrack {
    pub path: PathBuf,
    pub title: String,
    pub artist: String,
    pub album: String,
    pub track_no: Option<u32>,
    pub duration_secs: Option<f32>,
}

#[derive(Debug, Clone, Default)]
pub struct LibrarySnapshot {
    pub roots: Vec<PathBuf>,
    pub tracks: Vec<LibraryTrack>,
    pub scan_in_progress: bool,
    pub scanned_files: usize,
    pub last_error: Option<String>,
}

#[derive(Debug, Clone)]
pub enum LibraryCommand {
    ScanRoot(PathBuf),
}

#[derive(Debug, Clone)]
pub enum LibraryEvent {
    Snapshot(LibrarySnapshot),
}

pub struct LibraryService {
    tx: Sender<LibraryCommand>,
}

impl LibraryService {
    pub fn new() -> (Self, Receiver<LibraryEvent>) {
        let (cmd_tx, cmd_rx) = unbounded::<LibraryCommand>();
        let (event_tx, event_rx) = unbounded::<LibraryEvent>();

        std::thread::spawn(move || {
            let mut snapshot = LibrarySnapshot::default();

            match open_library_db() {
                Ok(conn) => {
                    if let Err(err) = init_schema(&conn) {
                        snapshot.last_error = Some(format!("library DB init failed: {err}"));
                        let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));
                        return;
                    }
                    load_snapshot(&conn, &mut snapshot);
                    let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));

                    while let Ok(cmd) = cmd_rx.recv() {
                        match cmd {
                            LibraryCommand::ScanRoot(root) => {
                                snapshot.scan_in_progress = true;
                                snapshot.scanned_files = 0;
                                snapshot.last_error = None;
                                let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));

                                if let Err(err) = scan_root(&conn, &root, &mut snapshot) {
                                    snapshot.last_error = Some(err);
                                }

                                load_snapshot(&conn, &mut snapshot);
                                snapshot.scan_in_progress = false;
                                let _ = event_tx.send(LibraryEvent::Snapshot(snapshot.clone()));
                            }
                        }
                    }
                }
                Err(err) => {
                    snapshot.last_error = Some(format!("library DB open failed: {err}"));
                    let _ = event_tx.send(LibraryEvent::Snapshot(snapshot));
                }
            }
        });

        (Self { tx: cmd_tx }, event_rx)
    }

    pub fn command(&self, cmd: LibraryCommand) {
        let _ = self.tx.send(cmd);
    }
}

fn open_library_db() -> anyhow::Result<Connection> {
    let db_path = library_db_path()?;
    if let Some(parent) = db_path.parent() {
        fs::create_dir_all(parent)?;
    }
    let conn = Connection::open(db_path)?;
    Ok(conn)
}

fn library_db_path() -> anyhow::Result<PathBuf> {
    if let Some(xdg_home) = std::env::var_os("XDG_DATA_HOME") {
        return Ok(PathBuf::from(xdg_home)
            .join("ferrous")
            .join("library.sqlite3"));
    }
    let home = std::env::var_os("HOME")
        .ok_or_else(|| anyhow::anyhow!("HOME is not set and XDG_DATA_HOME is missing"))?;
    Ok(PathBuf::from(home)
        .join(".local")
        .join("share")
        .join("ferrous")
        .join("library.sqlite3"))
}

fn init_schema(conn: &Connection) -> anyhow::Result<()> {
    conn.execute_batch(
        r"
        CREATE TABLE IF NOT EXISTS roots (
            path TEXT PRIMARY KEY,
            added_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS tracks (
            path TEXT PRIMARY KEY,
            title TEXT NOT NULL,
            artist TEXT NOT NULL,
            album TEXT NOT NULL,
            track_no INTEGER,
            duration_secs REAL,
            mtime_ns INTEGER NOT NULL,
            size_bytes INTEGER NOT NULL,
            indexed_at INTEGER NOT NULL
        );

        CREATE INDEX IF NOT EXISTS idx_tracks_artist ON tracks(artist);
        CREATE INDEX IF NOT EXISTS idx_tracks_album ON tracks(album);
        CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title);
        ",
    )?;
    // Migration for existing DBs created before track_no support.
    let _ = conn.execute("ALTER TABLE tracks ADD COLUMN track_no INTEGER", []);
    Ok(())
}

fn load_snapshot(conn: &Connection, snapshot: &mut LibrarySnapshot) {
    snapshot.roots.clear();
    snapshot.tracks.clear();

    if let Ok(mut stmt) = conn.prepare("SELECT path FROM roots ORDER BY path") {
        if let Ok(rows) = stmt.query_map([], |row| row.get::<_, String>(0)) {
            for row in rows.flatten() {
                snapshot.roots.push(PathBuf::from(row));
            }
        }
    }

    if let Ok(mut stmt) = conn.prepare(
        r"
        SELECT path, title, artist, album, track_no, duration_secs
        FROM tracks
        ORDER BY
            CASE WHEN artist = '' THEN 1 ELSE 0 END,
            artist COLLATE NOCASE,
            album COLLATE NOCASE,
            CASE WHEN track_no IS NULL THEN 1 ELSE 0 END,
            track_no ASC,
            path COLLATE NOCASE
        ",
    ) {
        if let Ok(rows) = stmt.query_map([], |row| {
            Ok(LibraryTrack {
                path: PathBuf::from(row.get::<_, String>(0)?),
                title: row.get::<_, String>(1)?,
                artist: row.get::<_, String>(2)?,
                album: row.get::<_, String>(3)?,
                track_no: row
                    .get::<_, Option<i64>>(4)?
                    .and_then(|v| u32::try_from(v).ok()),
                duration_secs: row.get::<_, Option<f32>>(5)?,
            })
        }) {
            for row in rows.flatten() {
                snapshot.tracks.push(row);
            }
        }
    }
}

fn scan_root(conn: &Connection, root: &Path, snapshot: &mut LibrarySnapshot) -> Result<(), String> {
    let root = root
        .canonicalize()
        .map_err(|e| format!("failed to access '{}': {e}", root.display()))?;
    if !root.is_dir() {
        return Err(format!("'{}' is not a directory", root.display()));
    }

    let root_str = root.to_string_lossy().to_string();
    let now = unix_ts_i64();
    conn.execute(
        "INSERT OR IGNORE INTO roots(path, added_at) VALUES (?1, ?2)",
        params![root_str, now],
    )
    .map_err(|e| format!("failed to save root '{}': {e}", root.display()))?;

    let mut existing: HashMap<String, (i64, i64)> = HashMap::new();
    if let Ok(mut stmt) = conn.prepare(
        "SELECT path, mtime_ns, size_bytes FROM tracks WHERE path LIKE ?1 || '/%' OR path = ?1",
    ) {
        let mapped = stmt.query_map(params![root.to_string_lossy().to_string()], |row| {
            Ok((
                row.get::<_, String>(0)?,
                row.get::<_, i64>(1)?,
                row.get::<_, i64>(2)?,
            ))
        });
        if let Ok(rows) = mapped {
            for item in rows.flatten() {
                existing.insert(item.0, (item.1, item.2));
            }
        }
    }

    let mut seen_paths: HashSet<String> = HashSet::new();
    let tx = match conn.unchecked_transaction() {
        Ok(tx) => tx,
        Err(e) => return Err(format!("failed to begin transaction: {e}")),
    };

    for entry in WalkDir::new(&root)
        .follow_links(false)
        .into_iter()
        .filter_map(std::result::Result::ok)
    {
        if !entry.file_type().is_file() {
            continue;
        }
        let path = entry.path();
        if !is_supported_audio(path) {
            continue;
        }

        let metadata = match fs::metadata(path) {
            Ok(v) => v,
            Err(_) => continue,
        };
        let size_bytes = metadata.len() as i64;
        let mtime_ns = metadata
            .modified()
            .ok()
            .and_then(|t| t.duration_since(UNIX_EPOCH).ok())
            .map_or(0, |d| d.as_nanos() as i64);

        let path_string = path.to_string_lossy().to_string();
        seen_paths.insert(path_string.clone());

        let needs_update = existing
            .get(&path_string)
            .map_or(true, |(old_mtime, old_size)| {
                *old_mtime != mtime_ns || *old_size != size_bytes
            });

        if needs_update {
            let indexed = read_track_info(path);
            if tx
                .execute(
                    r"
                    INSERT INTO tracks(path, title, artist, album, track_no, duration_secs, mtime_ns, size_bytes, indexed_at)
                    VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
                    ON CONFLICT(path) DO UPDATE SET
                        title=excluded.title,
                        artist=excluded.artist,
                        album=excluded.album,
                        track_no=excluded.track_no,
                        duration_secs=excluded.duration_secs,
                        mtime_ns=excluded.mtime_ns,
                        size_bytes=excluded.size_bytes,
                        indexed_at=excluded.indexed_at
                    ",
                    params![
                        path_string,
                        indexed.title,
                        indexed.artist,
                        indexed.album,
                        indexed.track_no.map(i64::from),
                        indexed.duration_secs,
                        mtime_ns,
                        size_bytes,
                        now
                    ],
                )
                .is_err()
            {
                continue;
            }
        }

        snapshot.scanned_files = snapshot.scanned_files.saturating_add(1);
    }

    let stale: Vec<String> = existing
        .into_keys()
        .filter(|p| !seen_paths.contains(p))
        .collect();
    for p in stale {
        let _ = tx.execute("DELETE FROM tracks WHERE path=?1", params![p]);
    }

    tx.commit()
        .map_err(|e| format!("failed to finalize scan transaction: {e}"))?;

    Ok(())
}

fn is_supported_audio(path: &Path) -> bool {
    let Some(ext) = path.extension().and_then(|e| e.to_str()) else {
        return false;
    };
    matches!(ext.to_ascii_lowercase().as_str(), "mp3" | "flac")
}

struct IndexedTrack {
    title: String,
    artist: String,
    album: String,
    track_no: Option<u32>,
    duration_secs: Option<f32>,
}

fn read_track_info(path: &Path) -> IndexedTrack {
    let mut out = IndexedTrack {
        title: path
            .file_stem()
            .and_then(|s| s.to_str())
            .unwrap_or_default()
            .to_owned(),
        artist: String::new(),
        album: String::new(),
        track_no: None,
        duration_secs: None,
    };

    if let Ok(tagged) = lofty::read_from_path(path) {
        if let Some(tag) = tagged.primary_tag().or_else(|| tagged.first_tag()) {
            if let Some(title) = tag.title() {
                out.title = title.into_owned();
            }
            if let Some(artist) = tag.artist() {
                out.artist = artist.into_owned();
            }
            if let Some(album) = tag.album() {
                out.album = album.into_owned();
            }
            out.track_no = tag.track();
        }
        out.duration_secs = Some(tagged.properties().duration().as_secs_f32());
    }
    out
}

fn unix_ts_i64() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs() as i64)
        .unwrap_or(0)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn test_dir(name: &str) -> PathBuf {
        let mut p = std::env::temp_dir();
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        p.push(format!(
            "ferrous-libtest-{name}-{}-{nanos}",
            std::process::id()
        ));
        p
    }

    #[test]
    fn supported_audio_extensions_are_detected() {
        assert!(is_supported_audio(Path::new("a.mp3")));
        assert!(is_supported_audio(Path::new("a.flac")));
        assert!(!is_supported_audio(Path::new("a.wav")));
        assert!(!is_supported_audio(Path::new("a")));
    }

    #[test]
    fn scan_root_indexes_supported_files_only() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("index");
        fs::create_dir_all(&root).expect("mkdir");

        let mp3 = root.join("song1.mp3");
        let flac = root.join("song2.flac");
        let txt = root.join("notes.txt");
        fs::File::create(&mp3)
            .and_then(|mut f| f.write_all(b"not-real-mp3"))
            .expect("create mp3");
        fs::File::create(&flac)
            .and_then(|mut f| f.write_all(b"not-real-flac"))
            .expect("create flac");
        fs::File::create(&txt)
            .and_then(|mut f| f.write_all(b"ignore me"))
            .expect("create txt");

        let mut snapshot = LibrarySnapshot::default();
        scan_root(&conn, &root, &mut snapshot).expect("scan");
        load_snapshot(&conn, &mut snapshot);

        let paths: Vec<PathBuf> = snapshot.tracks.iter().map(|t| t.path.clone()).collect();
        assert!(paths.iter().any(|p| p.ends_with("song1.mp3")));
        assert!(paths.iter().any(|p| p.ends_with("song2.flac")));
        assert!(!paths.iter().any(|p| p.ends_with("notes.txt")));

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn scan_root_removes_stale_deleted_tracks() {
        let conn = Connection::open_in_memory().expect("db");
        init_schema(&conn).expect("schema");

        let root = test_dir("stale");
        fs::create_dir_all(&root).expect("mkdir");
        let mp3 = root.join("song1.mp3");
        fs::File::create(&mp3)
            .and_then(|mut f| f.write_all(b"not-real-mp3"))
            .expect("create mp3");

        let mut snapshot = LibrarySnapshot::default();
        scan_root(&conn, &root, &mut snapshot).expect("initial scan");
        load_snapshot(&conn, &mut snapshot);
        assert_eq!(snapshot.tracks.len(), 1);

        fs::remove_file(&mp3).expect("remove mp3");
        scan_root(&conn, &root, &mut snapshot).expect("rescan");
        load_snapshot(&conn, &mut snapshot);
        assert!(snapshot.tracks.is_empty());

        let _ = fs::remove_dir_all(root);
    }
}
