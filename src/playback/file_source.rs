// SPDX-License-Identifier: GPL-3.0-or-later

//! The file source of the `playbin3` pipelines, whose reads a flush
//! interrupts.
//!
//! `filesrc` reads its file on the streaming thread that asks for the data.
//! A `read()` that the kernel does not return, from a network share that
//! stopped answering, a drive that does not wake up, or a FUSE file system
//! that hangs, keeps that thread in the source, and `filesrc` cannot let go
//! of it. A flushing seek and the state change that ends a run both wait
//! for the streaming threads, so they waited for the read, and the playback
//! engine, which makes them, stopped handling commands until it returned.
//!
//! This source, registered above `filesrc` for `file://` URIs, reads on a
//! reader thread instead, and the streaming thread waits for the read. A
//! flush ends that wait at once (`unlock`), and so does the source's stop:
//! the source returns `FLUSHING`, and the pipeline seeks, stops, or ends its
//! run without the read. The reader finishes the read on its own and drops
//! the data.
//!
//! The reader owns the files that its sources opened, and only the reader
//! touches them after the open: it reads them, asks for their size, and
//! closes them, as closing a file on a network share can wait for the server
//! too. A streaming thread asks for a read by the source's number, so it
//! never holds the file, and never closes it.
//!
//! The sources of one file share its reader, which works for them in turn,
//! so a file whose read does not return holds one thread, however often it
//! is played again meanwhile. The reader ends once no source has the file
//! open and it has nothing left to do.
//!
//! A source that stops leaves its file to the reader to close, behind a read
//! in progress. The pipeline's `Releases` keep track of such files until
//! they are closed, so that the engine can tell the user about one that is
//! not, and at shutdown.
//!
//! The source opens its file on the thread that starts it, as `filesrc`
//! does, because the pipeline needs the file's size and type then. A file
//! that grows while it plays, such as a download, is read on: when the
//! pipeline reaches the end that the source knows, the source asks the
//! reader for the size again, as `filesrc` asks the file system.

use std::collections::{HashMap, HashSet, VecDeque};
use std::fs::File;
use std::io::{self, Read};
use std::os::unix::fs::{FileExt, MetadataExt};
use std::path::{Path, PathBuf};
use std::sync::atomic::AtomicU64;
use std::sync::{Arc, Condvar, LazyLock, Mutex, MutexGuard, OnceLock, PoisonError};
use std::time::{Duration, Instant};

use gst::glib;
use gstreamer as gst;
use gstreamer_base as gst_base;
use gstreamer_base::prelude::*;
use gstreamer_base::subclass::prelude::*;

/// Name of the source's element factory.
pub(crate) const FACTORY_NAME: &str = "ferrousfilesrc";

/// Lock `mutex`, also after a thread panicked while holding it: the state
/// that each lock guards stays consistent between its statements.
fn lock<T>(mutex: &Mutex<T>) -> MutexGuard<'_, T> {
    mutex.lock().unwrap_or_else(PoisonError::into_inner)
}

/// Make `file://` URIs read with this source rather than `filesrc`, in
/// every pipeline of the process that `GStreamer` creates sources for.
pub(crate) fn register() {
    static REGISTERED: OnceLock<()> = OnceLock::new();
    REGISTERED.get_or_init(|| {
        // Pipelines go on reading with `filesrc` without it.
        if let Err(err) = gst::Element::register(
            None,
            FACTORY_NAME,
            gst::Rank::PRIMARY + 1,
            FileSource::static_type(),
        ) {
            eprintln!("[ferrous] the file source could not be registered: {err}");
        }
    });
}

glib::wrapper! {
    /// A `filesrc` whose reads a flush interrupts; see the module.
    pub(crate) struct FileSource(ObjectSubclass<imp::FileSource>)
        @extends gst_base::BaseSrc, gst::Element, gst::Object,
        @implements gst::URIHandler;
}

/// The files of stopped sources that have not been closed yet, behind a
/// read that has not returned or in a close that has not, so that the
/// engine can name the files it cannot release.
#[derive(Default)]
pub(crate) struct Releases {
    state: Mutex<ReleasesState>,
    /// Signalled when a file is closed.
    released: Condvar,
}

#[derive(Default)]
struct ReleasesState {
    next_id: u64,
    pending: Vec<PendingRelease>,
}

struct PendingRelease {
    id: u64,
    path: PathBuf,
    /// When the source stopped.
    since: Instant,
    /// Whether the engine reported the file.
    reported: bool,
}

/// The file of a stopped source, pending until dropped once it is closed.
struct Release {
    releases: Arc<Releases>,
    id: u64,
}

impl Releases {
    /// Give every source with which `playbin` reads a file these releases.
    pub(crate) fn track(self: &Arc<Self>, playbin: &gst::Element) {
        let releases = Arc::clone(self);
        playbin.connect("source-setup", false, move |values| {
            let source = values.get(1)?.get::<gst::Element>().ok()?;
            if let Some(source) = source.downcast_ref::<FileSource>() {
                source.imp().set_releases(Arc::clone(&releases));
            }
            None
        });
    }

    fn begin(self: &Arc<Self>, path: &Path) -> Release {
        let mut state = lock(&self.state);
        state.next_id += 1;
        let id = state.next_id;
        state.pending.push(PendingRelease {
            id,
            path: path.to_path_buf(),
            since: Instant::now(),
            reported: false,
        });
        Release {
            releases: Arc::clone(self),
            id,
        }
    }

    /// The files that have been pending for `after` at `now` and were not
    /// reported yet, each once, also when several sources left it. They
    /// count as reported from now on.
    pub(crate) fn overdue(&self, now: Instant, after: Duration) -> Vec<PathBuf> {
        let mut state = lock(&self.state);
        let mut reported: HashSet<PathBuf> = state
            .pending
            .iter()
            .filter(|pending| pending.reported)
            .map(|pending| pending.path.clone())
            .collect();
        let mut overdue = Vec::new();
        for pending in state
            .pending
            .iter_mut()
            .filter(|pending| !pending.reported && now.duration_since(pending.since) >= after)
        {
            pending.reported = true;
            if reported.insert(pending.path.clone()) {
                overdue.push(pending.path.clone());
            }
        }
        overdue
    }

    /// Wait until no file is pending, or until `deadline`. Returns the
    /// files still pending then, each once.
    pub(crate) fn wait_released(&self, deadline: Instant) -> Vec<PathBuf> {
        let mut state = lock(&self.state);
        while !state.pending.is_empty() {
            let Some(left) = deadline.checked_duration_since(Instant::now()) else {
                break;
            };
            state = self
                .released
                .wait_timeout(state, left)
                .unwrap_or_else(PoisonError::into_inner)
                .0;
        }
        let mut files = Vec::new();
        for pending in &state.pending {
            if !files.contains(&pending.path) {
                files.push(pending.path.clone());
            }
        }
        files
    }
}

impl Drop for Release {
    fn drop(&mut self) {
        let mut state = lock(&self.releases.state);
        if let Some(index) = state
            .pending
            .iter()
            .position(|pending| pending.id == self.id)
        {
            let pending = state.pending.swap_remove(index);
            if pending.reported {
                eprintln!(
                    "[ferrous] {} was closed after {:.1} s",
                    pending.path.display(),
                    pending.since.elapsed().as_secs_f64()
                );
            }
        }
        drop(state);
        self.releases.released.notify_all();
    }
}

/// The identity of a file, which its sources share a reader by.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
struct FileKey {
    device: u64,
    inode: u64,
}

impl FileKey {
    fn of(metadata: &std::fs::Metadata) -> Self {
        Self {
            device: metadata.dev(),
            inode: metadata.ino(),
        }
    }
}

/// Numbers each start of a source, which the reader holds its file under.
static NEXT_HANDLE: AtomicU64 = AtomicU64::new(1);

/// The reader of each file that a source has open, or that has work left.
static READERS: LazyLock<Mutex<HashMap<FileKey, Arc<Reader>>>> = LazyLock::new(Mutex::default);

/// Reads a file for its sources, on a thread of its own. Lock order: the
/// table of readers, then a reader's state, then the `Wait` of a source.
struct Reader {
    key: FileKey,
    state: Mutex<ReaderState>,
    /// Signalled when a job is queued.
    work: Condvar,
}

struct ReaderState {
    jobs: VecDeque<Job>,
    /// The file that each started source opened, by its handle. Only the
    /// reader's thread clones a file, for as long as it uses it, so the
    /// last reference, which closes the file, is always dropped there.
    files: HashMap<u64, Arc<File>>,
}

enum Job {
    Ask(Request),
    /// A source has stopped. Close its file, then end its release.
    Close {
        handle: u64,
        path: Arc<Path>,
        release: Option<Release>,
    },
}

/// What the streaming thread of a source asks the reader for, and waits for.
struct Request {
    handle: u64,
    /// The file's path, which test holds go by.
    #[cfg(test)]
    path: Arc<Path>,
    /// Whether the file is read at an offset, or else in sequence.
    seekable: bool,
    op: Op,
    wait: Arc<Wait>,
    serial: u64,
}

#[derive(Debug, Clone, Copy)]
enum Op {
    Read { offset: u64, length: u32 },
    Size,
}

/// What the reader answered.
enum Answer {
    Read(io::Result<gst::Buffer>),
    Size(io::Result<u64>),
}

impl Reader {
    /// The reader of the file `key`, which it gives `file`, opened by a
    /// source as `handle`. Starts the reader's thread if no reader reads the
    /// file. Returns whether it started one.
    fn join(key: FileKey, handle: u64, file: File) -> io::Result<(Arc<Self>, bool)> {
        let mut readers = lock(&READERS);
        if let Some(reader) = readers.get(&key) {
            lock(&reader.state).files.insert(handle, Arc::new(file));
            return Ok((Arc::clone(reader), false));
        }
        let reader = Arc::new(Self {
            key,
            state: Mutex::new(ReaderState {
                jobs: VecDeque::new(),
                files: HashMap::from([(handle, Arc::new(file))]),
            }),
            work: Condvar::new(),
        });
        let thread = Arc::clone(&reader);
        std::thread::Builder::new()
            .name("ferrous-read".to_string())
            .spawn(move || thread.run())?;
        readers.insert(key, Arc::clone(&reader));
        Ok((reader, true))
    }

    fn push(&self, job: Job) {
        lock(&self.state).jobs.push_back(job);
        self.work.notify_one();
    }

    fn run(self: Arc<Self>) {
        loop {
            let job = {
                let mut state = lock(&self.state);
                loop {
                    if let Some(job) = state.jobs.pop_front() {
                        break Some(job);
                    }
                    if state.files.is_empty() {
                        break None;
                    }
                    state = self
                        .work
                        .wait(state)
                        .unwrap_or_else(PoisonError::into_inner);
                }
            };
            match job {
                Some(Job::Ask(request)) => request.run(&self),
                Some(Job::Close {
                    handle,
                    path,
                    release,
                }) => {
                    let file = lock(&self.state).files.remove(&handle);
                    #[cfg(test)]
                    test_support::wait_if_held(&path, test_support::Point::Close);
                    drop(file);
                    #[cfg(test)]
                    test_support::closed(&path);
                    drop(path);
                    drop(release);
                }
                None if self.retire() => return,
                None => {}
            }
        }
    }

    /// Leave the table if no source has the file open and nothing is left
    /// to do. Returns false if a source joined meanwhile.
    fn retire(self: &Arc<Self>) -> bool {
        let mut readers = lock(&READERS);
        let state = lock(&self.state);
        if !state.files.is_empty() || !state.jobs.is_empty() {
            return false;
        }
        if readers
            .get(&self.key)
            .is_some_and(|reader| Arc::ptr_eq(reader, self))
        {
            readers.remove(&self.key);
        }
        true
    }
}

impl Request {
    /// Answer the request, unless the source no longer waits for it or has
    /// closed its file.
    fn run(self, reader: &Reader) {
        if lock(&self.wait.state).wanted != self.serial {
            return;
        }
        let Some(file) = lock(&reader.state).files.get(&self.handle).cloned() else {
            return;
        };
        #[cfg(test)]
        test_support::wait_if_held(&self.path, test_support::Point::Read);
        let answer = match self.op {
            Op::Read { offset, length } => Answer::Read(self.read(&file, offset, length)),
            Op::Size => Answer::Size(file.metadata().map(|metadata| metadata.len())),
        };
        drop(file);
        {
            let mut wait = lock(&self.wait.state);
            if wait.wanted == self.serial {
                wait.answer = Some(answer);
            }
        }
        self.wait.changed.notify_all();
    }

    /// Read up to `length` bytes at `offset`, fewer only at the end of the
    /// file.
    fn read(&self, file: &File, offset: u64, length: u32) -> io::Result<gst::Buffer> {
        let mut buffer =
            gst::Buffer::with_size(length as usize).map_err(|err| io::Error::other(err.message))?;
        let buffer_mut = buffer
            .get_mut()
            .ok_or_else(|| io::Error::other("a new buffer is shared"))?;
        let filled = {
            let mut map = buffer_mut
                .map_writable()
                .map_err(|err| io::Error::other(err.message))?;
            let data = map.as_mut_slice();
            let mut filled = 0;
            while filled < data.len() {
                let read = if self.seekable {
                    file.read_at(&mut data[filled..], offset.saturating_add(filled as u64))
                } else {
                    (&*file).read(&mut data[filled..])
                };
                match read {
                    Ok(0) => break,
                    Ok(count) => filled += count,
                    Err(err) if err.kind() == io::ErrorKind::Interrupted => {}
                    Err(err) => return Err(err),
                }
            }
            filled
        };
        buffer_mut.set_size(filled);
        // A file that is read in sequence may be given no offset.
        if offset != u64::MAX {
            buffer_mut.set_offset(offset);
            buffer_mut.set_offset_end(offset.saturating_add(filled as u64));
        }
        Ok(buffer)
    }
}

/// Where the streaming thread of a source waits for the reader.
#[derive(Default)]
struct Wait {
    state: Mutex<WaitState>,
    /// Signalled when the reader answers, when the source flushes, and when
    /// it stops.
    changed: Condvar,
}

#[derive(Default)]
struct WaitState {
    /// The source flushes: its streaming thread waits for nothing.
    flushing: bool,
    /// The handle of the started source, or 0 once it has stopped. A
    /// streaming thread that asks the stopped source for data, which
    /// `basesrc` can let in after the flush of the stop has ended and before
    /// it stops the source, waits for nothing either.
    handle: u64,
    /// Serial of the request that the streaming thread waits for, or 0.
    wanted: u64,
    /// Serial of the latest request.
    serial: u64,
    /// The answer to the wanted request.
    answer: Option<Answer>,
}

/// The file of a started source, which its reader holds.
struct Open {
    handle: u64,
    path: Arc<Path>,
    reader: Arc<Reader>,
    seekable: bool,
    /// The size of the file when it was opened, or when the reader was
    /// last asked.
    size: u64,
    /// The pipeline has not asked for the size since the source started.
    /// It asks right after, and is told the size the open found.
    size_unasked: bool,
    /// Given to the reader with the close, which ends it.
    release: Option<Release>,
}

impl Drop for Open {
    fn drop(&mut self) {
        self.reader.push(Job::Close {
            handle: self.handle,
            path: Arc::clone(&self.path),
            release: self.release.take(),
        });
    }
}

mod imp {
    use super::{lock, Answer, FileKey, Job, Op, Open, Reader, Releases, Request, Wait};
    use super::{FACTORY_NAME, NEXT_HANDLE};

    use std::fs::File;
    use std::io;
    use std::os::unix::fs::FileTypeExt;
    use std::path::{Path, PathBuf};
    use std::sync::atomic::Ordering;
    use std::sync::{Arc, LazyLock, Mutex, PoisonError};

    use gst::glib;
    use gstreamer as gst;
    use gstreamer_base as gst_base;
    use gstreamer_base::prelude::*;
    use gstreamer_base::subclass::base_src::CreateSuccess;
    use gstreamer_base::subclass::prelude::*;

    #[derive(Default)]
    pub(crate) struct FileSource {
        location: Mutex<Option<PathBuf>>,
        open: Mutex<Option<Open>>,
        wait: Arc<Wait>,
        releases: Mutex<Option<Arc<Releases>>>,
    }

    impl FileSource {
        pub(super) fn set_releases(&self, releases: Arc<Releases>) {
            *lock(&self.releases) = Some(releases);
        }

        fn set_location(&self, location: Option<PathBuf>) -> Result<(), glib::Error> {
            if self.obj().current_state() > gst::State::Ready {
                return Err(glib::Error::new(
                    gst::URIError::BadState,
                    "the location of a file source cannot change while it reads its file",
                ));
            }
            *lock(&self.location) = location;
            Ok(())
        }

        /// Open the file at `path`, and give it to its reader.
        fn open(path: &Path) -> Result<Open, gst::ErrorMessage> {
            let file = File::open(path).map_err(|err| match err.kind() {
                io::ErrorKind::NotFound => gst::error_msg!(
                    gst::ResourceError::NotFound,
                    ["No such file \"{}\"", path.display()]
                ),
                _ => gst::error_msg!(
                    gst::ResourceError::OpenRead,
                    ("Could not open file \"{}\" for reading.", path.display()),
                    ["{err}"]
                ),
            })?;
            let metadata = file.metadata().map_err(|err| {
                gst::error_msg!(
                    gst::ResourceError::OpenRead,
                    ("Could not get info on \"{}\".", path.display()),
                    ["{err}"]
                )
            })?;
            let kind = metadata.file_type();
            if kind.is_dir() {
                return Err(gst::error_msg!(
                    gst::ResourceError::OpenRead,
                    ("\"{}\" is a directory.", path.display())
                ));
            }
            if kind.is_socket() {
                return Err(gst::error_msg!(
                    gst::ResourceError::OpenRead,
                    ("File \"{}\" is a socket.", path.display())
                ));
            }
            let handle = NEXT_HANDLE.fetch_add(1, Ordering::Relaxed);
            let (reader, started) =
                Reader::join(FileKey::of(&metadata), handle, file).map_err(|err| {
                    gst::error_msg!(
                        gst::ResourceError::OpenRead,
                        ("Could not open file \"{}\" for reading.", path.display()),
                        ["no thread to read it: {err}"]
                    )
                })?;
            #[cfg(test)]
            if started {
                super::test_support::started(path);
            }
            #[cfg(not(test))]
            let _ = started;
            Ok(Open {
                handle,
                path: Arc::from(path),
                reader,
                // Only a regular file has a size and can be read anywhere.
                seekable: kind.is_file(),
                size: metadata.len(),
                size_unasked: true,
                release: None,
            })
        }

        /// Ask the reader for `op` on the file, and wait for the answer,
        /// unless the source flushes or stops first.
        fn ask(&self, op: Op) -> Result<Answer, gst::FlowError> {
            let (reader, mut request) = {
                let open = lock(&self.open);
                let Some(open) = open.as_ref() else {
                    return Err(gst::FlowError::Flushing);
                };
                let request = Request {
                    handle: open.handle,
                    #[cfg(test)]
                    path: Arc::clone(&open.path),
                    seekable: open.seekable,
                    op,
                    wait: Arc::clone(&self.wait),
                    serial: 0,
                };
                (Arc::clone(&open.reader), request)
            };
            #[cfg(test)]
            super::test_support::wait_if_held(&request.path, super::test_support::Point::Ask);
            let handle = request.handle;
            {
                let mut wait = lock(&self.wait.state);
                if wait.flushing || wait.handle != handle {
                    return Err(gst::FlowError::Flushing);
                }
                wait.serial += 1;
                wait.wanted = wait.serial;
                wait.answer = None;
                request.serial = wait.serial;
            }
            let serial = request.serial;
            reader.push(Job::Ask(request));
            let mut wait = lock(&self.wait.state);
            loop {
                // A stop also drops the request. The reader drops a request
                // that it reads for when it returns.
                if wait.flushing || wait.handle != handle || wait.wanted != serial {
                    if wait.wanted == serial {
                        wait.wanted = 0;
                    }
                    return Err(gst::FlowError::Flushing);
                }
                if let Some(answer) = wait.answer.take() {
                    wait.wanted = 0;
                    return Ok(answer);
                }
                wait = self
                    .wait
                    .changed
                    .wait(wait)
                    .unwrap_or_else(PoisonError::into_inner);
            }
        }

        /// Read `length` bytes at `offset` on the reader.
        fn read(&self, offset: u64, length: u32) -> Result<gst::Buffer, gst::FlowError> {
            let Answer::Read(result) = self.ask(Op::Read { offset, length })? else {
                return Err(gst::FlowError::Error);
            };
            result.map_err(|err| {
                let path = lock(&self.open)
                    .as_ref()
                    .map(|open| open.path.display().to_string())
                    .unwrap_or_default();
                gst::element_imp_error!(
                    self,
                    gst::ResourceError::Read,
                    ["Could not read \"{path}\": {err}"]
                );
                gst::FlowError::Error
            })
        }
    }

    impl Drop for FileSource {
        fn drop(&mut self) {
            // A source that was not stopped closes its file all the same.
            drop(
                self.open
                    .get_mut()
                    .unwrap_or_else(PoisonError::into_inner)
                    .take(),
            );
        }
    }

    #[glib::object_subclass]
    impl ObjectSubclass for FileSource {
        const NAME: &'static str = "FerrousFileSrc";
        type Type = super::FileSource;
        type ParentType = gst_base::BaseSrc;
        type Interfaces = (gst::URIHandler,);
    }

    impl ObjectImpl for FileSource {
        fn properties() -> &'static [glib::ParamSpec] {
            static PROPERTIES: LazyLock<Vec<glib::ParamSpec>> = LazyLock::new(|| {
                vec![glib::ParamSpecString::builder("location")
                    .nick("File Location")
                    .blurb("Location of the file to read")
                    .mutable_ready()
                    .build()]
            });
            PROPERTIES.as_ref()
        }

        fn set_property(&self, _id: usize, value: &glib::Value, pspec: &glib::ParamSpec) {
            if pspec.name() == "location" {
                let location = value.get::<Option<String>>().ok().flatten();
                if let Err(err) = self.set_location(location.map(PathBuf::from)) {
                    eprintln!("[ferrous] {FACTORY_NAME}: {err}");
                }
            }
        }

        fn property(&self, _id: usize, pspec: &glib::ParamSpec) -> glib::Value {
            match pspec.name() {
                "location" => lock(&self.location)
                    .as_ref()
                    .map(|path| path.to_string_lossy().into_owned())
                    .to_value(),
                _ => None::<String>.to_value(),
            }
        }

        fn constructed(&self) {
            self.parent_constructed();
            self.obj().set_format(gst::Format::Bytes);
        }
    }

    impl GstObjectImpl for FileSource {}

    impl ElementImpl for FileSource {
        fn metadata() -> Option<&'static gst::subclass::ElementMetadata> {
            static METADATA: LazyLock<gst::subclass::ElementMetadata> = LazyLock::new(|| {
                gst::subclass::ElementMetadata::new(
                    "File Source",
                    "Source/File",
                    "Read from arbitrary point in a file, on a thread that a flush does not wait for",
                    "Ferrous",
                )
            });
            Some(&*METADATA)
        }

        fn pad_templates() -> &'static [gst::PadTemplate] {
            static TEMPLATES: LazyLock<Vec<gst::PadTemplate>> = LazyLock::new(|| {
                gst::PadTemplate::new(
                    "src",
                    gst::PadDirection::Src,
                    gst::PadPresence::Always,
                    &gst::Caps::new_any(),
                )
                .map(|template| vec![template])
                .unwrap_or_default()
            });
            TEMPLATES.as_ref()
        }
    }

    impl BaseSrcImpl for FileSource {
        fn start(&self) -> Result<(), gst::ErrorMessage> {
            let Some(path) = lock(&self.location).clone() else {
                return Err(gst::error_msg!(
                    gst::ResourceError::NotFound,
                    ("No file name specified for reading.")
                ));
            };
            let open = Self::open(&path)?;
            let handle = open.handle;
            // The pipeline asks for the size again only when it reaches the
            // end it knows, never for a duration query, which the engine's
            // thread makes too.
            self.obj().set_dynamic_size(false);
            *lock(&self.open) = Some(open);
            lock(&self.wait.state).handle = handle;
            Ok(())
        }

        fn stop(&self) -> Result<(), gst::ErrorMessage> {
            let Some(mut open) = lock(&self.open).take() else {
                return Ok(());
            };
            {
                let mut wait = lock(&self.wait.state);
                wait.handle = 0;
                wait.wanted = 0;
                wait.answer = None;
            }
            self.wait.changed.notify_all();
            // The file is released once the reader has closed it, after a
            // read in progress.
            open.release = lock(&self.releases)
                .as_ref()
                .map(|releases| releases.begin(&open.path));
            drop(open);
            Ok(())
        }

        fn is_seekable(&self) -> bool {
            lock(&self.open).as_ref().is_some_and(|open| open.seekable)
        }

        fn size(&self) -> Option<u64> {
            let known = {
                let mut open = lock(&self.open);
                let open = open.as_mut().filter(|open| open.seekable)?;
                if std::mem::take(&mut open.size_unasked) || open.size == 0 {
                    // Right after the start, or of a file that held nothing
                    // to play. A duration query asks then, on any thread.
                    return Some(open.size);
                }
                open.size
            };
            // The pipeline has reached the end it knows, on its streaming
            // thread: the file may have grown since.
            let Ok(Answer::Size(Ok(size))) = self.ask(Op::Size) else {
                return Some(known);
            };
            if let Some(open) = lock(&self.open).as_mut() {
                open.size = size;
            }
            Some(size)
        }

        fn create(
            &self,
            offset: u64,
            buffer: Option<&mut gst::BufferRef>,
            length: u32,
        ) -> Result<CreateSuccess, gst::FlowError> {
            let read = self.read(offset, length)?;
            if read.size() == 0 {
                return Err(gst::FlowError::Eos);
            }
            let Some(buffer) = buffer else {
                return Ok(CreateSuccess::NewBuffer(read));
            };
            let size = read.size();
            {
                let data = read.map_readable().map_err(|_| gst::FlowError::Error)?;
                let mut out = buffer.map_writable().map_err(|_| gst::FlowError::Error)?;
                out.get_mut(..size)
                    .ok_or(gst::FlowError::Error)?
                    .copy_from_slice(&data);
            }
            buffer.set_size(size);
            buffer.set_offset(read.offset());
            buffer.set_offset_end(read.offset_end());
            Ok(CreateSuccess::FilledBuffer)
        }

        fn unlock(&self) -> Result<(), gst::ErrorMessage> {
            lock(&self.wait.state).flushing = true;
            self.wait.changed.notify_all();
            Ok(())
        }

        fn unlock_stop(&self) -> Result<(), gst::ErrorMessage> {
            lock(&self.wait.state).flushing = false;
            Ok(())
        }
    }

    impl URIHandlerImpl for FileSource {
        const URI_TYPE: gst::URIType = gst::URIType::Src;

        fn protocols() -> &'static [&'static str] {
            &["file"]
        }

        fn uri(&self) -> Option<String> {
            let location = lock(&self.location).clone()?;
            glib::filename_to_uri(location, None).ok().map(Into::into)
        }

        fn set_uri(&self, uri: &str) -> Result<(), glib::Error> {
            // Some applications ask whether an element takes file URIs so.
            if uri == "file://" {
                return self.set_location(None);
            }
            let (location, host) = glib::filename_from_uri(uri)?;
            if let Some(host) = host.filter(|host| host.as_str() != "localhost") {
                return Err(glib::Error::new(
                    gst::URIError::BadUri,
                    &format!("File URI with invalid hostname '{host}'"),
                ));
            }
            self.set_location(Some(location))
        }
    }
}

/// Holds the work of the reader where the kernel could, and the streaming
/// thread before it asks the reader, and tells which readers ran and which
/// threads closed files.
#[cfg(test)]
pub(crate) mod test_support {
    use std::collections::HashMap;
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, Condvar, LazyLock, Mutex};

    use super::{lock, FileKey, READERS};

    /// Where a hold holds.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub(crate) enum Point {
        /// A streaming thread that has found the source's file and is about
        /// to ask the reader.
        Ask,
        /// The reader, about to read or to ask for the size.
        Read,
        /// The reader, about to close a file.
        Close,
    }

    /// Holds the threads that reach `point` for a file, from its creation
    /// until released.
    pub(crate) struct Hold {
        path: PathBuf,
        point: Point,
        state: Mutex<HoldState>,
        changed: Condvar,
    }

    #[derive(Default)]
    struct HoldState {
        released: bool,
        /// Threads held now.
        waiting: usize,
    }

    static HOLDS: LazyLock<Mutex<Vec<Arc<Hold>>>> = LazyLock::new(Mutex::default);

    /// Readers started for each file, by the path of the source that
    /// started it.
    static STARTED: LazyLock<Mutex<HashMap<PathBuf, usize>>> = LazyLock::new(Mutex::default);

    /// The threads that closed each file, by its path, in order.
    static CLOSED: LazyLock<Mutex<HashMap<PathBuf, Vec<String>>>> = LazyLock::new(Mutex::default);

    /// Hold the threads that reach `point` for the file at `path`.
    pub(crate) fn hold(path: &Path, point: Point) -> Arc<Hold> {
        let hold = Arc::new(Hold {
            path: path.to_path_buf(),
            point,
            state: Mutex::default(),
            changed: Condvar::new(),
        });
        lock(&HOLDS).push(Arc::clone(&hold));
        hold
    }

    /// Hold every read of the file at `path`, as a drive or network share
    /// that stopped answering would.
    pub(crate) fn hold_reads(path: &Path) -> Arc<Hold> {
        hold(path, Point::Read)
    }

    impl Hold {
        /// Whether a thread is held now.
        pub(crate) fn holds(&self) -> bool {
            lock(&self.state).waiting > 0
        }

        /// Whether a read is held now.
        pub(crate) fn holds_a_read(&self) -> bool {
            self.holds()
        }

        /// Let the held threads, and every later one, go.
        pub(crate) fn release(&self) {
            lock(&HOLDS).retain(|hold| !std::ptr::eq(&**hold, self));
            lock(&self.state).released = true;
            self.changed.notify_all();
        }
    }

    impl Drop for Hold {
        fn drop(&mut self) {
            lock(&self.state).released = true;
            self.changed.notify_all();
        }
    }

    pub(super) fn wait_if_held(path: &Path, point: Point) {
        let holds: Vec<_> = lock(&HOLDS)
            .iter()
            .filter(|hold| hold.point == point && hold.path == path)
            .cloned()
            .collect();
        for hold in holds {
            let mut state = lock(&hold.state);
            state.waiting += 1;
            hold.changed.notify_all();
            while !state.released {
                state = hold.changed.wait(state).unwrap();
            }
            state.waiting -= 1;
        }
    }

    pub(super) fn started(path: &Path) {
        *lock(&STARTED).entry(path.to_path_buf()).or_default() += 1;
    }

    pub(super) fn closed(path: &Path) {
        let thread = std::thread::current().name().unwrap_or("").to_string();
        lock(&CLOSED)
            .entry(path.to_path_buf())
            .or_default()
            .push(thread);
    }

    /// How many readers have been started for the file at `path`.
    pub(crate) fn readers_started(path: &Path) -> usize {
        lock(&STARTED).get(path).copied().unwrap_or(0)
    }

    /// The names of the threads that closed the file at `path`.
    pub(crate) fn closed_on(path: &Path) -> Vec<String> {
        lock(&CLOSED).get(path).cloned().unwrap_or_default()
    }

    /// Whether a reader reads the file at `path`.
    pub(crate) fn reader_runs(path: &Path) -> bool {
        let key = FileKey::of(&std::fs::metadata(path).expect("file metadata"));
        lock(&READERS).contains_key(&key)
    }
}

#[cfg(test)]
mod tests {
    use std::path::{Path, PathBuf};
    use std::sync::{mpsc, Arc, Mutex};
    use std::time::{Duration, Instant};

    use gst::prelude::*;
    use gst::subclass::prelude::ObjectSubclassIsExt;
    use gstreamer as gst;

    use super::test_support::{closed_on, hold, hold_reads, reader_runs, readers_started, Point};
    use super::{register, Releases, FACTORY_NAME};

    /// How long a test waits for a thread to get somewhere.
    const TIMEOUT: Duration = Duration::from_secs(10);

    struct Fixture {
        dir: PathBuf,
    }

    impl Fixture {
        fn new(tag: &str) -> Self {
            gst::init().expect("gstreamer initialization");
            register();
            let dir = std::env::temp_dir()
                .join(format!("ferrous-file-source-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            Self { dir }
        }

        /// A file of `len` bytes that count up.
        fn file(&self, name: &str, len: usize) -> (PathBuf, Vec<u8>) {
            let bytes: Vec<u8> = (0..len).map(|index| (index % 251) as u8).collect();
            let path = self.dir.join(name);
            std::fs::write(&path, &bytes).unwrap();
            (path, bytes)
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.dir);
        }
    }

    fn wait_until(what: &str, done: impl Fn() -> bool) {
        let deadline = Instant::now() + TIMEOUT;
        while !done() {
            assert!(Instant::now() < deadline, "timed out: {what}");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn source(path: &Path) -> gst::Element {
        gst::Element::make_from_uri(gst::URIType::Src, &glib_uri(path), None).expect("file source")
    }

    fn glib_uri(path: &Path) -> String {
        gst::glib::filename_to_uri(path, None).unwrap().into()
    }

    /// A pipeline that pushes the file at `path` to a sink, which collects
    /// the bytes.
    fn pushing(path: &Path) -> (gst::Pipeline, gst::Element, Arc<Mutex<Vec<u8>>>) {
        let pipeline = gst::Pipeline::new();
        let src = source(path);
        let sink = gst::ElementFactory::make("fakesink")
            .property("signal-handoffs", true)
            .build()
            .unwrap();
        let received = Arc::new(Mutex::new(Vec::new()));
        let collected = Arc::clone(&received);
        sink.connect("handoff", false, move |values| {
            let buffer = values[1].get::<gst::Buffer>().unwrap();
            collected
                .lock()
                .unwrap()
                .extend_from_slice(&buffer.map_readable().unwrap());
            None
        });
        pipeline.add_many([&src, &sink]).unwrap();
        src.link(&sink).unwrap();
        (pipeline, src, received)
    }

    /// Give the source of `src` its own `Releases`.
    fn track(src: &gst::Element) -> Arc<Releases> {
        let releases = Arc::new(Releases::default());
        src.downcast_ref::<super::FileSource>()
            .unwrap()
            .imp()
            .set_releases(Arc::clone(&releases));
        releases
    }

    /// Append `bytes` to the file at `path`, as a download would.
    fn append(path: &Path, bytes: &[u8]) {
        use std::io::Write;
        std::fs::OpenOptions::new()
            .append(true)
            .open(path)
            .unwrap()
            .write_all(bytes)
            .unwrap();
    }

    /// Pull `length` bytes at `offset` from `pad` on another thread.
    fn pull(
        pad: &gst::Pad,
        offset: u64,
        length: u32,
    ) -> mpsc::Receiver<Result<gst::Buffer, gst::FlowError>> {
        let (done_tx, done) = mpsc::channel();
        let pad = pad.clone();
        std::thread::spawn(move || {
            let _ = done_tx.send(pad.range(offset, length));
        });
        done
    }

    /// The source of the file at `path`, started in pull mode.
    fn pulled(path: &Path) -> (gst::Element, gst::Pad) {
        let src = source(path);
        src.set_state(gst::State::Ready).unwrap();
        let pad = src.static_pad("src").unwrap();
        pad.activate_mode(gst::PadMode::Pull, true).unwrap();
        (src, pad)
    }

    fn stop_pulling(src: &gst::Element, pad: &gst::Pad) {
        pad.activate_mode(gst::PadMode::Pull, false).unwrap();
        src.set_state(gst::State::Null).unwrap();
    }

    /// Whether every close of the file at `path` ran on its reader.
    #[track_caller]
    fn assert_closed_by_the_reader(path: &Path, closes: usize) {
        let threads = closed_on(path);
        assert_eq!(threads.len(), closes, "{threads:?}");
        assert!(
            threads.iter().all(|thread| thread == "ferrous-read"),
            "{threads:?}"
        );
    }

    /// Set `pipeline` to `state` on another thread. Returns whether the
    /// state change returned in time.
    fn set_state_in_time(pipeline: &gst::Pipeline, state: gst::State) -> bool {
        let (done_tx, done) = mpsc::channel();
        let pipeline = pipeline.clone();
        std::thread::spawn(move || {
            let _ = pipeline.set_state(state);
            let _ = done_tx.send(());
        });
        done.recv_timeout(TIMEOUT).is_ok()
    }

    #[test]
    fn file_uris_are_read_by_the_source() {
        let fixture = Fixture::new("uri");
        let (path, _) = fixture.file("a.bin", 10);
        let src = source(&path);
        assert_eq!(src.factory().unwrap().name(), FACTORY_NAME);
        assert_eq!(
            src.property::<Option<String>>("location").as_deref(),
            path.to_str()
        );
        let handler = src.dynamic_cast_ref::<gst::URIHandler>().unwrap();
        assert_eq!(handler.uri().as_deref(), Some(glib_uri(&path).as_str()));
    }

    #[test]
    fn a_pushed_file_arrives_whole() {
        let fixture = Fixture::new("push");
        let (path, bytes) = fixture.file("a.bin", 100_000);
        let (pipeline, _, received) = pushing(&path);
        pipeline.set_state(gst::State::Playing).unwrap();
        let bus = pipeline.bus().unwrap();
        let message = bus
            .timed_pop_filtered(
                gst::ClockTime::from_seconds(10),
                &[gst::MessageType::Eos, gst::MessageType::Error],
            )
            .expect("end of stream");
        assert!(
            matches!(message.view(), gst::MessageView::Eos(_)),
            "{message:?}"
        );
        pipeline.set_state(gst::State::Null).unwrap();
        assert_eq!(*received.lock().unwrap(), bytes);
    }

    #[test]
    fn a_pulled_file_is_read_where_asked() {
        let fixture = Fixture::new("pull");
        let (path, bytes) = fixture.file("a.bin", 10_000);
        let src = source(&path);
        src.set_state(gst::State::Ready).unwrap();
        let pad = src.static_pad("src").unwrap();
        pad.activate_mode(gst::PadMode::Pull, true).unwrap();
        let buffer = pad.range(5_000, 100).unwrap();
        assert_eq!(&*buffer.map_readable().unwrap(), &bytes[5_000..5_100]);
        assert_eq!(buffer.offset(), 5_000);
        // The end of the file shortens a read, and ends the stream past it.
        let buffer = pad.range(9_950, 100).unwrap();
        assert_eq!(&*buffer.map_readable().unwrap(), &bytes[9_950..]);
        assert_eq!(pad.range(10_000, 100), Err(gst::FlowError::Eos));
        pad.activate_mode(gst::PadMode::Pull, false).unwrap();
        src.set_state(gst::State::Null).unwrap();
        wait_until("the reader ends", || !reader_runs(&path));
    }

    #[test]
    fn missing_files_and_directories_are_errors() {
        let fixture = Fixture::new("errors");
        let missing = fixture.dir.join("missing.bin");
        for (path, not_found) in [(missing, true), (fixture.dir.clone(), false)] {
            let (pipeline, _, _) = pushing(&path);
            assert!(pipeline.set_state(gst::State::Paused).is_err());
            let message = pipeline
                .bus()
                .unwrap()
                .timed_pop_filtered(gst::ClockTime::from_seconds(1), &[gst::MessageType::Error])
                .unwrap_or_else(|| panic!("no error for {}", path.display()));
            let gst::MessageView::Error(error) = message.view() else {
                unreachable!();
            };
            assert_eq!(
                error.error().matches(gst::ResourceError::NotFound),
                not_found,
                "{error:?}"
            );
            assert!(
                not_found || error.error().matches(gst::ResourceError::OpenRead),
                "{error:?}"
            );
            pipeline.set_state(gst::State::Null).unwrap();
        }
    }

    #[test]
    fn a_read_that_does_not_return_holds_no_state_change() {
        let fixture = Fixture::new("held");
        let (path, bytes) = fixture.file("a.bin", 100_000);
        let hold = hold_reads(&path);
        let (pipeline, src, received) = pushing(&path);
        let releases = Arc::new(Releases::default());
        src.downcast_ref::<super::FileSource>()
            .unwrap()
            .imp()
            .set_releases(Arc::clone(&releases));
        let _ = pipeline.set_state(gst::State::Playing);
        wait_until("a read is held", || hold.holds_a_read());

        // A flushing seek and the end of the run do not wait for the read.
        assert!(
            set_state_in_time(&pipeline, gst::State::Null),
            "the pipeline stopped while its read was held"
        );
        assert!(hold.holds_a_read());
        assert!(received.lock().unwrap().is_empty());
        // The source left its read behind.
        assert_eq!(
            releases.overdue(Instant::now(), Duration::ZERO),
            vec![path.clone()]
        );
        // Reported once.
        assert!(releases.overdue(Instant::now(), Duration::ZERO).is_empty());
        assert_eq!(
            releases.wait_released(Instant::now() + Duration::from_millis(50)),
            vec![path.clone()]
        );

        // The file plays again once its read has returned.
        hold.release();
        assert!(releases.wait_released(Instant::now() + TIMEOUT).is_empty());
        wait_until("the reader ends", || !reader_runs(&path));
        assert_closed_by_the_reader(&path, 1);
        pipeline.set_state(gst::State::Playing).unwrap();
        pipeline
            .bus()
            .unwrap()
            .timed_pop_filtered(gst::ClockTime::from_seconds(10), &[gst::MessageType::Eos])
            .expect("end of stream");
        pipeline.set_state(gst::State::Null).unwrap();
        assert_eq!(*received.lock().unwrap(), bytes);
    }

    #[test]
    fn a_read_asked_for_after_the_flush_of_a_stop_ends_with_the_stop() {
        use gstreamer_base::subclass::prelude::BaseSrcImpl;

        let fixture = Fixture::new("stop-race");
        let (path, _) = fixture.file("a.bin", 10_000);
        let hold = hold_reads(&path);
        let (src, pad) = pulled(&path);
        let imp = src.downcast_ref::<super::FileSource>().unwrap().imp();
        let first = pull(&pad, 0, 100);
        wait_until("a read is held", || hold.holds_a_read());
        // `basesrc` stops a source in pull mode so: it flushes, which ends
        // the wait, and lets the readers in again before it stops it.
        imp.unlock().unwrap();
        assert_eq!(
            first.recv_timeout(TIMEOUT).unwrap(),
            Err(gst::FlowError::Flushing)
        );
        imp.unlock_stop().unwrap();
        let second = pull(&pad, 0, 100);
        assert!(second.recv_timeout(Duration::from_millis(50)).is_err());
        imp.stop().unwrap();
        assert_eq!(
            second.recv_timeout(TIMEOUT).unwrap(),
            Err(gst::FlowError::Flushing)
        );

        hold.release();
        stop_pulling(&src, &pad);
        wait_until("the reader ends", || !reader_runs(&path));
        assert_closed_by_the_reader(&path, 1);
    }

    #[test]
    fn a_pull_that_goes_on_after_its_source_stopped_leaves_the_close_to_the_reader() {
        use gstreamer_base::subclass::prelude::BaseSrcImpl;

        let fixture = Fixture::new("late-pull");
        let (path, _) = fixture.file("a.bin", 10_000);
        let (src, pad) = pulled(&path);
        let imp = src.downcast_ref::<super::FileSource>().unwrap().imp();
        // The streaming thread has found the source's file and is
        // descheduled before it asks the reader.
        let paused = hold(&path, Point::Ask);
        let late = pull(&pad, 0, 100);
        wait_until("the pull is held", || paused.holds());
        // `basesrc` stops the source meanwhile.
        imp.unlock().unwrap();
        imp.unlock_stop().unwrap();
        imp.stop().unwrap();
        wait_until("the reader ends", || !reader_runs(&path));
        assert_closed_by_the_reader(&path, 1);

        paused.release();
        assert_eq!(
            late.recv_timeout(TIMEOUT).unwrap(),
            Err(gst::FlowError::Flushing)
        );
        assert_closed_by_the_reader(&path, 1);
        stop_pulling(&src, &pad);
    }

    #[test]
    fn a_close_that_does_not_return_keeps_the_file_unreleased() {
        let fixture = Fixture::new("held-close");
        let (path, bytes) = fixture.file("a.bin", 10_000);
        let (pipeline, src, received) = pushing(&path);
        let releases = track(&src);
        // The file plays to its end, so no read is in progress at the stop.
        pipeline.set_state(gst::State::Playing).unwrap();
        pipeline
            .bus()
            .unwrap()
            .timed_pop_filtered(gst::ClockTime::from_seconds(10), &[gst::MessageType::Eos])
            .expect("end of stream");
        assert_eq!(*received.lock().unwrap(), bytes);
        let closing = hold(&path, Point::Close);
        assert!(set_state_in_time(&pipeline, gst::State::Null));
        wait_until("the close is held", || closing.holds());

        assert_eq!(
            releases.overdue(Instant::now(), Duration::ZERO),
            vec![path.clone()]
        );
        assert_eq!(
            releases.wait_released(Instant::now() + Duration::from_millis(50)),
            vec![path.clone()]
        );
        closing.release();
        assert!(releases.wait_released(Instant::now() + TIMEOUT).is_empty());
        assert_closed_by_the_reader(&path, 1);
    }

    #[test]
    fn a_file_whose_read_returned_stays_unreleased_until_it_is_closed() {
        let fixture = Fixture::new("read-then-close");
        let (path, _) = fixture.file("a.bin", 100_000);
        let reads = hold_reads(&path);
        let closing = hold(&path, Point::Close);
        let (pipeline, src, _) = pushing(&path);
        let releases = track(&src);
        let _ = pipeline.set_state(gst::State::Playing);
        wait_until("a read is held", || reads.holds());
        assert!(set_state_in_time(&pipeline, gst::State::Null));

        reads.release();
        wait_until("the close is held", || closing.holds());
        assert_eq!(
            releases.wait_released(Instant::now() + Duration::from_millis(50)),
            vec![path.clone()]
        );
        closing.release();
        assert!(releases.wait_released(Instant::now() + TIMEOUT).is_empty());
        wait_until("the reader ends", || !reader_runs(&path));
        assert_closed_by_the_reader(&path, 1);
    }

    #[test]
    fn a_file_that_grows_is_read_on() {
        let fixture = Fixture::new("growing");
        let (path, bytes) = fixture.file("a.bin", 8_192);
        let more: Vec<u8> = bytes.iter().map(|byte| byte ^ 0xFF).collect();

        // Pulled: past the end the source knew, and from it.
        let (src, pad) = pulled(&path);
        assert_eq!(
            &*pad.range(0, 4_096).unwrap().map_readable().unwrap(),
            &bytes[..4_096]
        );
        append(&path, &more[..4_096]);
        let buffer = pad.range(4_096, 8_192).unwrap();
        assert_eq!(buffer.size(), 8_192);
        assert_eq!(&buffer.map_readable().unwrap()[4_096..], &more[..4_096]);
        append(&path, &more[4_096..]);
        assert_eq!(
            &*pad.range(12_288, 8_192).unwrap().map_readable().unwrap(),
            &more[4_096..]
        );
        assert_eq!(pad.range(16_384, 100), Err(gst::FlowError::Eos));
        stop_pulling(&src, &pad);

        // Pushed: the file grows after the source opened it.
        let (path, bytes) = fixture.file("b.bin", 8_192);
        let reads = hold_reads(&path);
        let (pipeline, _, received) = pushing(&path);
        let _ = pipeline.set_state(gst::State::Playing);
        wait_until("a read is held", || reads.holds());
        append(&path, &more);
        reads.release();
        pipeline
            .bus()
            .unwrap()
            .timed_pop_filtered(gst::ClockTime::from_seconds(10), &[gst::MessageType::Eos])
            .expect("end of stream");
        pipeline.set_state(gst::State::Null).unwrap();
        assert_eq!(*received.lock().unwrap(), [bytes, more].concat());
    }

    #[test]
    fn asking_for_the_size_ends_with_a_flush() {
        use gstreamer_base::subclass::prelude::BaseSrcImpl;

        let fixture = Fixture::new("held-size");
        let (path, _) = fixture.file("a.bin", 8_192);
        let (src, pad) = pulled(&path);
        let imp = src.downcast_ref::<super::FileSource>().unwrap().imp();
        let reads = hold_reads(&path);
        // A pull that reaches the end of the file asks for its size first.
        let at_end = pull(&pad, 4_096, 4_096);
        wait_until("the size is asked for", || reads.holds());
        imp.unlock().unwrap();
        assert_eq!(
            at_end.recv_timeout(TIMEOUT).unwrap(),
            Err(gst::FlowError::Flushing)
        );
        imp.unlock_stop().unwrap();
        reads.release();
        stop_pulling(&src, &pad);
        wait_until("the reader ends", || !reader_runs(&path));
    }

    #[test]
    fn sources_of_a_file_whose_read_does_not_return_share_its_reader() {
        let fixture = Fixture::new("shared");
        let (path, _) = fixture.file("a.bin", 100_000);
        let hold = hold_reads(&path);
        let mut pipelines = Vec::new();
        for _ in 0..5 {
            let (pipeline, _, _) = pushing(&path);
            let _ = pipeline.set_state(gst::State::Playing);
            wait_until("a read is held", || hold.holds_a_read());
            assert!(set_state_in_time(&pipeline, gst::State::Null));
            pipelines.push(pipeline);
        }
        assert_eq!(readers_started(&path), 1);
        assert!(reader_runs(&path));
        hold.release();
        wait_until("the reader ends", || !reader_runs(&path));
        assert_eq!(readers_started(&path), 1);
    }
}
