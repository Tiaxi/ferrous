// SPDX-License-Identifier: GPL-3.0-or-later

//! The progress a scan shows: the files it processed of those it will have
//! processed when it ends, and no total while it cannot tell.

use std::sync::atomic::AtomicUsize;

use super::*;

fn test_root(name: &str) -> PathBuf {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let root = crate::paths::test_root().join(format!(
        "library-progress-{name}-{}",
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    let _ = fs::remove_dir_all(&root);
    fs::create_dir_all(&root).expect("create test root");
    root.canonicalize().expect("canonical test root")
}

fn write_tracks(root: &Path, count: usize) {
    for index in 0..count {
        write_test_track(
            &root.join(format!("{index:03}.mp3")),
            index.to_string().as_bytes(),
        );
    }
}

/// The scan progress of the events published while a scan was running.
fn published(rx: &Receiver<LibraryEvent>) -> Vec<LibraryScanProgress> {
    rx.try_iter()
        .filter(|event| event.status.scan_in_progress)
        .filter_map(|event| event.status.scan_progress)
        .collect()
}

fn fraction(processed: usize, total: usize) -> f64 {
    processed as f64 / total as f64
}

#[test]
fn a_rescan_runs_from_near_zero_to_one_and_is_full_only_once_it_ends() {
    let mut reporter = ScanProgressReporter::new(200);
    let mut fractions = Vec::new();
    // An unchanged file is processed as soon as the walk finds it, so the
    // files found are no total.
    for found in 0..=200 {
        let progress = reporter
            .emit(true, found, found, ScanStage::Walking)
            .expect("forced progress");
        let total = progress.estimated_total.expect("a rescan has an estimate");
        assert!(
            progress.processed < total,
            "{found} files of {total} while the walk goes on"
        );
        fractions.push(fraction(progress.processed, total));
    }
    // Every file is processed, but the rows of files that are gone are
    // still to remove.
    let walked = reporter
        .emit(true, 200, 200, ScanStage::Walked)
        .expect("forced progress");
    let total = walked.estimated_total.expect("a total");
    assert!(walked.processed < total, "full before the root is done");
    fractions.push(fraction(walked.processed, total));
    let end = reporter
        .emit(true, 200, 200, ScanStage::Finished)
        .expect("forced progress");
    assert_eq!(end.estimated_total, Some(200));
    fractions.push(fraction(end.processed, 200));

    assert!(fractions[0] < 0.01, "{fractions:?}");
    assert!(
        fractions[100] > 0.4 && fractions[100] < 0.6,
        "{fractions:?}"
    );
    assert!(
        fractions.windows(2).all(|pair| pair[0] <= pair[1]),
        "{fractions:?}"
    );
    assert_eq!(fractions.last(), Some(&1.0));
}

#[test]
fn a_rescan_that_finds_more_files_raises_its_total() {
    let mut reporter = ScanProgressReporter::new(10);
    for found in 0..=15 {
        let progress = reporter
            .emit(true, found, found, ScanStage::Walking)
            .expect("forced progress");
        let total = progress.estimated_total.expect("an estimate");
        assert!(total >= 10, "never below the files the root had");
        assert!(found < total, "{found} files of {total} while walking");
    }
    // Reading the last files can lag the walk; the total is what it found.
    let walked = reporter
        .emit(true, 15, 13, ScanStage::Walked)
        .expect("forced progress");
    assert_eq!(walked.estimated_total, Some(15));
}

#[test]
fn a_rescan_that_finds_fewer_files_ends_at_those_it_found() {
    let mut reporter = ScanProgressReporter::new(10);
    let walking = reporter
        .emit(true, 8, 6, ScanStage::Walking)
        .expect("forced progress");
    assert_eq!(walking.estimated_total, Some(10));
    let walked = reporter
        .emit(true, 8, 6, ScanStage::Walked)
        .expect("forced progress");
    assert_eq!(walked.estimated_total, Some(8));
    // All 8 read, and the 2 rows of files that are gone still to remove.
    let removing = reporter
        .emit(true, 8, 8, ScanStage::Walked)
        .expect("forced progress");
    assert_eq!(removing.estimated_total, Some(9));
    let end = reporter
        .emit(true, 8, 8, ScanStage::Finished)
        .expect("forced progress");
    assert_eq!(end.estimated_total, Some(8));
}

#[test]
fn a_first_scan_has_no_total_until_its_walk_ends() {
    let mut reporter = ScanProgressReporter::new(0);
    for found in 0..50 {
        let progress = reporter
            .emit(true, found, found / 2, ScanStage::Walking)
            .expect("forced progress");
        assert_eq!(progress.estimated_total, None, "{found} files found");
    }
    let walked = reporter
        .emit(true, 50, 30, ScanStage::Walked)
        .expect("forced progress");
    assert_eq!(walked.estimated_total, Some(50));
}

#[test]
fn the_eta_counts_down_to_the_total() {
    assert_eq!(scan_eta_seconds(Some(100), 40, Some(10.0)), Some(6.0));
    assert_eq!(scan_eta_seconds(None, 40, Some(10.0)), None);
    assert_eq!(scan_eta_seconds(Some(100), 40, None), None);
    assert_eq!(scan_eta_seconds(Some(100), 100, Some(10.0)), None);
}

#[test]
fn a_run_counts_the_files_of_all_its_roots() {
    let root = RootScanProgress {
        discovered: 12,
        processed: 10,
        estimated_total: Some(20),
        files_per_second: None,
    };
    let run = RunProgress {
        processed_before: 30,
        later: Some(50),
    };
    assert_eq!(run.of(&root), (42, 40, Some(100)));
    // A later root that has no rows has no estimate, so neither has the run.
    let run = RunProgress {
        processed_before: 30,
        later: None,
    };
    assert_eq!(run.of(&root), (42, 40, None));
    // Nor has it while the current root has none.
    let run = RunProgress {
        processed_before: 30,
        later: Some(50),
    };
    let first_scan = RootScanProgress {
        estimated_total: None,
        ..root
    };
    assert_eq!(run.of(&first_scan), (42, 40, None));
    // The last root is done, but the run is not: the cleanup after it
    // remains until the run says it is done.
    let run = RunProgress {
        processed_before: 30,
        later: Some(0),
    };
    let done = RootScanProgress {
        discovered: 10,
        processed: 10,
        estimated_total: Some(10),
        files_per_second: None,
    };
    assert_eq!(run.of(&done), (40, 40, Some(41)));
}

/// Checks the progress a run published: never full until its last event,
/// which shows the `files` it processed of as many.
fn assert_full_only_at_the_end(progress: &[LibraryScanProgress], files: usize) {
    let (end, before) = progress.split_last().expect("progress");
    for progress in before {
        if let Some(total) = progress.supported_files_total {
            assert!(
                progress.supported_files_processed < total,
                "full before the end: {progress:?}"
            );
        }
    }
    assert_eq!(end.supported_files_processed, files, "{end:?}");
    assert_eq!(end.supported_files_total, Some(files), "{end:?}");
}

#[test]
fn a_rescan_is_not_full_while_the_rows_of_removed_files_remain() {
    let root = test_root("removed");
    write_tracks(&root, 40);
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    scan_root_observed(&conn, &root, None, &mut |_| {}, &mut |_, _| {}).expect("first scan");
    for index in 10..40 {
        fs::remove_file(root.join(format!("{index:03}.mp3"))).expect("remove track");
    }
    let gone = root.join("039.mp3").to_string_lossy().to_string();

    // The 10 files left are unchanged, so they are processed as soon as the
    // walk finds them, before the 30 rows of the files removed go.
    let mut observed = Vec::new();
    let mut on_progress = |progress: RootScanProgress| {
        let row_left = conn
            .query_row(
                "SELECT COUNT(*) FROM tracks WHERE path = ?1",
                params![gone],
                |row| row.get::<_, i64>(0),
            )
            .expect("count rows")
            > 0;
        observed.push((progress, row_left));
    };
    let outcome =
        scan_root_observed(&conn, &root, None, &mut on_progress, &mut |_, _| {}).expect("rescan");
    assert_eq!(outcome.removed.len(), 30, "{:?}", outcome.problem);

    assert!(
        observed
            .iter()
            .any(|(progress, row_left)| *row_left && progress.processed == 10),
        "all files processed while the rows remain: {observed:?}"
    );
    for (progress, row_left) in &observed {
        if *row_left {
            let total = progress.estimated_total.expect("a rescan has a total");
            assert!(
                progress.processed < total,
                "full while the rows remain: {progress:?}"
            );
        }
    }
    let (end, row_left) = observed.last().expect("progress");
    assert!(!row_left);
    assert_eq!((end.processed, end.estimated_total), (10, Some(10)));

    let _ = fs::remove_dir_all(root);
}

#[test]
fn a_run_whose_last_root_went_offline_fills_only_once_it_ends() {
    let first = test_root("online");
    let second = test_root("offline");
    write_tracks(&first, 30);
    write_tracks(&second, 20);
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    let mut library = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    let roots = [first.clone(), second.clone()];
    run_scans(&conn, &roots, None, &mut library, &tx).expect("first scan");
    let _ = published(&rx);

    // The second root's files are gone, as from an unmounted drive: the
    // run keeps its rows and processes none of its files.
    for index in 0..20 {
        fs::remove_file(second.join(format!("{index:03}.mp3"))).expect("remove track");
    }
    let _ = run_scans(&conn, &roots, None, &mut library, &tx);
    let rescan = published(&rx);
    assert_full_only_at_the_end(&rescan, 30);
    assert!(
        rescan
            .iter()
            .any(|progress| progress.supported_files_total == Some(50)),
        "the second root counts until it is scanned: {rescan:?}"
    );

    let _ = fs::remove_dir_all(first);
    let _ = fs::remove_dir_all(second);
}

#[test]
fn a_run_of_two_roots_shows_one_total_that_fills_once() {
    let first = test_root("first");
    let second = test_root("second");
    write_tracks(&first, 30);
    write_tracks(&second, 20);
    let conn = Connection::open_in_memory().expect("db");
    init_schema(&conn).expect("schema");
    let mut library = WorkerLibrary::default();
    let (tx, rx) = unbounded();
    let roots = [first.clone(), second.clone()];

    // The first scan of both: no total while the second root has never
    // been scanned, and none for it until its walk ends.
    run_scans(&conn, &roots, None, &mut library, &tx).expect("first scan");
    let first_scan = published(&rx);
    assert!(
        first_scan
            .first()
            .is_some_and(|progress| progress.supported_files_total.is_none()),
        "{first_scan:?}"
    );
    assert!(
        first_scan.iter().any(|progress| {
            progress.current_root.as_deref() == Some(second.as_path())
                && progress.roots_completed == 1
                && progress.supported_files_processed == 30
                && progress.supported_files_total.is_none()
        }),
        "the second root starts with no total: {first_scan:?}"
    );
    for progress in &first_scan {
        if progress.current_root.as_deref() == Some(first.as_path()) {
            assert_eq!(progress.supported_files_total, None, "{progress:?}");
        }
    }
    assert_full_only_at_the_end(&first_scan, 50);

    // A rescan: the rows the roots had, from the start, and full only once
    // both are done.
    run_scans(&conn, &roots, None, &mut library, &tx).expect("rescan");
    let rescan = published(&rx);
    let start = rescan.first().expect("progress");
    assert_eq!(start.supported_files_processed, 0, "{rescan:?}");
    assert_eq!(start.supported_files_total, Some(50), "{rescan:?}");
    let mut last_processed = 0;
    for progress in &rescan {
        let total = progress
            .supported_files_total
            .expect("a rescan has a total");
        // The files, and one more while only removing rows remains.
        assert!((50..=51).contains(&total), "{progress:?}");
        assert!(progress.supported_files_processed >= last_processed);
        last_processed = progress.supported_files_processed;
    }
    assert_full_only_at_the_end(&rescan, 50);
    // Between the roots, the bar shows the first root's files of both.
    assert!(
        rescan
            .iter()
            .any(|progress| progress.roots_completed == 1
                && progress.supported_files_processed == 30),
        "{rescan:?}"
    );
    let end = rescan.last().expect("progress");
    assert_eq!(end.supported_files_processed, 50);
    assert_eq!(end.roots_completed, 2);

    let _ = fs::remove_dir_all(first);
    let _ = fs::remove_dir_all(second);
}
