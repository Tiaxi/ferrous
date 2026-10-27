// SPDX-License-Identifier: GPL-3.0-or-later

//! Opening the library database for the library worker: schema migrations,
//! write-ahead logging, and recovery from a damaged file.

use std::fs;
use std::path::{Path, PathBuf};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use rusqlite::{Connection, ErrorCode};

use crate::no_replace::rename_no_replace;

use super::schema::{open_library_db_at, prepare_database};

/// The files `SQLite` keeps beside a database while it is in use. They hold
/// committed data that may not be in the main file yet, so they move with it.
const SIDE_FILE_SUFFIXES: [&str; 3] = ["-journal", "-wal", "-shm"];

/// The library worker's connection, and what the user should know about how
/// it was opened.
pub(super) struct OpenedLibrary {
    pub(super) conn: Connection,
    /// A problem the user should see, such as a damaged database that was
    /// moved aside, or write-ahead logging that could not be enabled.
    pub(super) notice: Option<String>,
}

/// Opens the database at `path` for the library worker and migrates it to
/// the current schema.
///
/// A file that `SQLite` reports as damaged, and that fails an integrity
/// check, is moved aside under a timestamped name, and the worker starts
/// with a new, empty database; the notice tells the user where the old one
/// is. Every other failure, such as a lock held by another program or a file
/// that cannot be read, leaves the file where it is and is returned.
pub(super) fn open_worker_db(path: &Path, busy_timeout: Duration) -> Result<OpenedLibrary, String> {
    let error = match open_and_migrate(path, busy_timeout) {
        Ok(opened) => return Ok(opened),
        Err(error) => error,
    };
    if !is_corruption(&error) || !fails_integrity_check(path, busy_timeout) {
        return Err(format!(
            "could not open the library database {}: {error:#}",
            path.display()
        ));
    }
    let kept = move_aside(path, SystemTime::now()).map_err(|move_error| {
        format!(
            "the library database {} is damaged ({error:#}), and could not be moved aside: \
             {move_error}",
            path.display()
        )
    })?;
    let mut opened = open_and_migrate(path, busy_timeout).map_err(|reopen_error| {
        format!(
            "the library database {} was damaged and was moved to {}, but a new one could \
             not be created: {reopen_error:#}",
            path.display(),
            kept.display()
        )
    })?;
    let moved = format!(
        "the library database was damaged ({error:#}). Ferrous moved it to {} and started \
         a new, empty library; add your library folders again to rebuild it",
        kept.display()
    );
    opened.notice = Some(match opened.notice.take() {
        Some(notice) => format!("{moved}; {notice}"),
        None => moved,
    });
    Ok(opened)
}

fn open_and_migrate(path: &Path, busy_timeout: Duration) -> anyhow::Result<OpenedLibrary> {
    let conn = open_library_db_at(path, busy_timeout)?;
    let notice = prepare_database(&conn)?;
    Ok(OpenedLibrary { conn, notice })
}

/// Copies the write-ahead log into the database file as far as no reader
/// needs it, without waiting for any. Runs when the library worker stops.
pub(super) fn checkpoint(conn: &Connection) -> rusqlite::Result<()> {
    conn.query_row("PRAGMA wal_checkpoint(PASSIVE)", [], |_| Ok(()))
}

/// Whether `error` is `SQLite` reporting a damaged database, rather than a
/// lock, a permission, or an I/O problem.
fn is_corruption(error: &anyhow::Error) -> bool {
    error.chain().any(|cause| {
        cause
            .downcast_ref::<rusqlite::Error>()
            .and_then(rusqlite::Error::sqlite_error_code)
            .is_some_and(|code| {
                matches!(code, ErrorCode::NotADatabase | ErrorCode::DatabaseCorrupt)
            })
    })
}

/// Confirms a damaged database with `SQLite`'s integrity check, on a
/// connection of its own, so that no other error moves the file aside.
fn fails_integrity_check(path: &Path, busy_timeout: Duration) -> bool {
    let Ok(conn) = open_library_db_at(path, busy_timeout) else {
        return false;
    };
    match conn.query_row("PRAGMA quick_check(1)", [], |row| row.get::<_, String>(0)) {
        Ok(result) => result != "ok",
        Err(error) => error.sqlite_error_code().is_some_and(|code| {
            matches!(code, ErrorCode::NotADatabase | ErrorCode::DatabaseCorrupt)
        }),
    }
}

/// Moves the database at `path`, with its side files, into a new folder
/// beside it named after the time, and returns the folder. Nothing is
/// deleted, and no existing file is replaced.
///
/// The side files move first: a write-ahead log or journal left at `path`
/// would be applied to the new database. If one cannot move, the database
/// stays where it is.
fn move_aside(path: &Path, now: SystemTime) -> std::io::Result<PathBuf> {
    let parent = path.parent().unwrap_or_else(|| Path::new("."));
    let name = path
        .file_name()
        .ok_or_else(|| std::io::Error::other("the library database path has no file name"))?;
    let stamp = utc_timestamp(now);
    let folder = (0..100)
        .map(|attempt| {
            let mut folder_name = name.to_os_string();
            folder_name.push(format!(".damaged-{stamp}"));
            if attempt > 0 {
                folder_name.push(format!("-{attempt}"));
            }
            parent.join(folder_name)
        })
        .find(|folder| fs::create_dir(folder).is_ok())
        .ok_or_else(|| std::io::Error::other("could not create a folder for the damaged copy"))?;
    for suffix in SIDE_FILE_SUFFIXES {
        let mut side_name = name.to_os_string();
        side_name.push(suffix);
        let side = parent.join(&side_name);
        if fs::symlink_metadata(&side).is_ok() {
            rename_no_replace(&side, &folder.join(&side_name))?;
        }
    }
    rename_no_replace(path, &folder.join(name))?;
    Ok(folder)
}

/// `now` in UTC as `YYYYMMDD-HHMMSS`.
fn utc_timestamp(now: SystemTime) -> String {
    let secs = now
        .duration_since(UNIX_EPOCH)
        .map_or(0, |elapsed| elapsed.as_secs());
    let days = i64::try_from(secs / 86_400).unwrap_or(0);
    let of_day = secs % 86_400;
    let (year, month, day) = civil_from_days(days);
    format!(
        "{year:04}{month:02}{day:02}-{:02}{:02}{:02}",
        of_day / 3600,
        of_day % 3600 / 60,
        of_day % 60
    )
}

/// The proleptic Gregorian date `days` after 1970-01-01, after Howard
/// Hinnant's `civil_from_days`.
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z.rem_euclid(146_097);
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = if mp < 10 { mp + 3 } else { mp - 9 };
    let year = yoe + era * 400 + i64::from(month <= 2);
    (
        year,
        u32::try_from(month).unwrap_or(1),
        u32::try_from(day).unwrap_or(1),
    )
}

#[cfg(test)]
mod tests;
