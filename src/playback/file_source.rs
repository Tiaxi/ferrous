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
//! flush ends that wait at once (`unlock`): the source returns `FLUSHING`,
//! and the pipeline seeks, stops, or ends its run without the read. The
//! reader finishes the read on its own and drops the data. The reader also
//! closes the file, which on a network share can wait for the server too.
//!
//! The sources of one file share its reader, which reads for them in turn,
//! so a file whose read does not return holds one thread, however often it
//! is played again meanwhile. The reader ends once no source has the file
//! open and it has nothing left to do.
//!
//! A source that stops while its read is in progress leaves the read behind.
//! The pipeline's `Releases` keep track of such reads, so that the engine
//! can tell the user about one that does not return, and at shutdown.
//!
//! The source opens its file on the thread that starts it, as `filesrc`
//! does, because the pipeline needs the file's size and type then.

use std::collections::{HashMap, VecDeque};
use std::fs::File;
use std::io::{self, Read};
use std::os::unix::fs::{FileExt, MetadataExt};
use std::path::{Path, PathBuf};
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

/// The reads of stopped sources that have not returned yet, so that the
/// engine can name the files it cannot release.
#[derive(Default)]
pub(crate) struct Releases {
    state: Mutex<ReleasesState>,
    /// Signalled when a read returns.
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
    /// Whether the engine reported the read.
    reported: bool,
}

/// A read that went on after its source stopped, pending until dropped.
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

    /// The files of reads that have been pending for `after` at `now` and
    /// were not reported yet. They count as reported from now on.
    pub(crate) fn overdue(&self, now: Instant, after: Duration) -> Vec<PathBuf> {
        lock(&self.state)
            .pending
            .iter_mut()
            .filter(|pending| !pending.reported && now.duration_since(pending.since) >= after)
            .map(|pending| {
                pending.reported = true;
                pending.path.clone()
            })
            .collect()
    }

    /// Wait until no read is pending, or until `deadline`. Returns the files
    /// of the reads still pending then.
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
        state
            .pending
            .iter()
            .map(|pending| pending.path.clone())
            .collect()
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
                    "[ferrous] the read of {} returned after {:.1} s",
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

/// The reader of each file that a source has open, or that has reads or a
/// close left.
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
    /// Sources that have the file open.
    users: usize,
}

enum Job {
    Read(ReadJob),
    /// A source has stopped and drops its file, which this closes unless a
    /// read still uses it.
    Close(Arc<File>),
}

/// A read for a source, whose streaming thread waits for it.
struct ReadJob {
    file: Arc<File>,
    path: Arc<Path>,
    /// Whether the file is read at `offset`, or else in sequence.
    seekable: bool,
    offset: u64,
    length: u32,
    wait: Arc<Wait>,
    serial: u64,
}

impl Reader {
    /// The reader of the file `key` for one more source, which starts its
    /// thread if no reader reads the file. Returns whether it started one.
    fn join(key: FileKey) -> io::Result<(Arc<Self>, bool)> {
        let mut readers = lock(&READERS);
        if let Some(reader) = readers.get(&key) {
            lock(&reader.state).users += 1;
            return Ok((Arc::clone(reader), false));
        }
        let reader = Arc::new(Self {
            key,
            state: Mutex::new(ReaderState {
                jobs: VecDeque::new(),
                users: 1,
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
                    if state.users == 0 {
                        break None;
                    }
                    state = self
                        .work
                        .wait(state)
                        .unwrap_or_else(PoisonError::into_inner);
                }
            };
            match job {
                Some(Job::Read(read)) => read.run(),
                Some(Job::Close(file)) => {
                    drop(file);
                    lock(&self.state).users -= 1;
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
        if state.users > 0 || !state.jobs.is_empty() {
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

impl ReadJob {
    /// Read for the source, unless it no longer waits for this read.
    fn run(self) {
        {
            let mut wait = lock(&self.wait.state);
            if wait.wanted != self.serial {
                return;
            }
            wait.reading += 1;
        }
        #[cfg(test)]
        test_support::wait_if_held(&self.path);
        let result = self.read();
        let release = {
            let mut wait = lock(&self.wait.state);
            wait.reading -= 1;
            if wait.wanted == self.serial {
                wait.done = Some(result);
            }
            if wait.reading == 0 {
                wait.release.take()
            } else {
                None
            }
        };
        self.wait.changed.notify_all();
        drop(release);
    }

    /// Read up to `length` bytes, fewer only at the end of the file.
    fn read(&self) -> io::Result<gst::Buffer> {
        let mut buffer = gst::Buffer::with_size(self.length as usize)
            .map_err(|err| io::Error::other(err.message))?;
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
                    self.file.read_at(
                        &mut data[filled..],
                        self.offset.saturating_add(filled as u64),
                    )
                } else {
                    (&*self.file).read(&mut data[filled..])
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
        if self.offset != u64::MAX {
            buffer_mut.set_offset(self.offset);
            buffer_mut.set_offset_end(self.offset.saturating_add(filled as u64));
        }
        Ok(buffer)
    }
}

/// Where the streaming thread of a source waits for its read.
#[derive(Default)]
struct Wait {
    state: Mutex<WaitState>,
    /// Signalled when a read returns and when the source flushes.
    changed: Condvar,
}

#[derive(Default)]
struct WaitState {
    /// The source flushes: its streaming thread waits for no read.
    flushing: bool,
    /// The source has stopped. The thread of a reader that pulls from it,
    /// which `basesrc` lets in again once the flush of its stop has ended
    /// and before it stops the source, waits for no read either.
    stopped: bool,
    /// Serial of the read that the streaming thread waits for, or 0.
    wanted: u64,
    /// Serial of the latest read.
    serial: u64,
    /// The result of the wanted read.
    done: Option<io::Result<gst::Buffer>>,
    /// Reads in progress for the source: one, or one per file when the
    /// source was started again on another file while a read of the
    /// previous one had not returned.
    reading: usize,
    /// The read that went on after the source stopped, until it returns.
    release: Option<Release>,
}

/// The file of a started source.
struct Open {
    /// Given to the reader to close when the source stops.
    file: Option<Arc<File>>,
    path: Arc<Path>,
    reader: Arc<Reader>,
    seekable: bool,
    size: u64,
}

impl Drop for Open {
    fn drop(&mut self) {
        if let Some(file) = self.file.take() {
            self.reader.push(Job::Close(file));
        }
    }
}

mod imp {
    use super::{lock, FileKey, Job, Open, ReadJob, Reader, Releases, Wait, FACTORY_NAME};

    use std::fs::File;
    use std::io;
    use std::os::unix::fs::FileTypeExt;
    use std::path::{Path, PathBuf};
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
            let (reader, started) = Reader::join(FileKey::of(&metadata)).map_err(|err| {
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
                file: Some(Arc::new(file)),
                path: Arc::from(path),
                reader,
                // Only a regular file has a size and can be read anywhere.
                seekable: kind.is_file(),
                size: metadata.len(),
            })
        }

        /// Read `length` bytes at `offset` on the reader, unless the source
        /// flushes first.
        fn read(&self, offset: u64, length: u32) -> Result<gst::Buffer, gst::FlowError> {
            let (reader, mut job) = {
                let open = lock(&self.open);
                let Some((open, file)) = open
                    .as_ref()
                    .and_then(|open| Some((open, Arc::clone(open.file.as_ref()?))))
                else {
                    return Err(gst::FlowError::Flushing);
                };
                let job = ReadJob {
                    file,
                    path: Arc::clone(&open.path),
                    seekable: open.seekable,
                    offset,
                    length,
                    wait: Arc::clone(&self.wait),
                    serial: 0,
                };
                (Arc::clone(&open.reader), job)
            };
            {
                let mut wait = lock(&self.wait.state);
                if wait.flushing || wait.stopped {
                    return Err(gst::FlowError::Flushing);
                }
                wait.serial += 1;
                wait.wanted = wait.serial;
                wait.done = None;
                job.serial = wait.serial;
            }
            let serial = job.serial;
            let path = Arc::clone(&job.path);
            reader.push(Job::Read(job));
            let result = {
                let mut wait = lock(&self.wait.state);
                loop {
                    if wait.flushing || wait.stopped {
                        // The reader drops the read when it returns.
                        wait.wanted = 0;
                        return Err(gst::FlowError::Flushing);
                    }
                    if wait.wanted == serial {
                        if let Some(done) = wait.done.take() {
                            wait.wanted = 0;
                            break done;
                        }
                    }
                    wait = self
                        .wait
                        .changed
                        .wait(wait)
                        .unwrap_or_else(PoisonError::into_inner);
                }
            };
            result.map_err(|err| {
                gst::element_imp_error!(
                    self,
                    gst::ResourceError::Read,
                    ["Could not read \"{}\": {err}", path.display()]
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
            // A file that grows is not followed, so a size query need not
            // wait for the file system.
            self.obj().set_dynamic_size(false);
            *lock(&self.open) = Some(open);
            lock(&self.wait.state).stopped = false;
            Ok(())
        }

        fn stop(&self) -> Result<(), gst::ErrorMessage> {
            let Some(open) = lock(&self.open).take() else {
                return Ok(());
            };
            let releases = lock(&self.releases).clone();
            {
                let mut wait = lock(&self.wait.state);
                wait.stopped = true;
                wait.wanted = 0;
                wait.done = None;
                if let (true, None, Some(releases)) =
                    (wait.reading > 0, wait.release.as_ref(), releases)
                {
                    wait.release = Some(releases.begin(&open.path));
                }
            }
            self.wait.changed.notify_all();
            drop(open);
            Ok(())
        }

        fn is_seekable(&self) -> bool {
            lock(&self.open).as_ref().is_some_and(|open| open.seekable)
        }

        fn size(&self) -> Option<u64> {
            lock(&self.open)
                .as_ref()
                .filter(|open| open.seekable)
                .map(|open| open.size)
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

/// Holds reads in the reader, as a `read()` that the kernel does not return
/// would, and tells which readers run.
#[cfg(test)]
pub(crate) mod test_support {
    use std::collections::HashMap;
    use std::path::{Path, PathBuf};
    use std::sync::{Arc, Condvar, LazyLock, Mutex};

    use super::{lock, FileKey, READERS};

    /// Holds every read of a file from when it is created until released.
    pub(crate) struct ReadHold {
        path: PathBuf,
        state: Mutex<HoldState>,
        changed: Condvar,
    }

    #[derive(Default)]
    struct HoldState {
        released: bool,
        /// Reads held now.
        waiting: usize,
    }

    static HOLDS: LazyLock<Mutex<Vec<Arc<ReadHold>>>> = LazyLock::new(Mutex::default);

    /// Readers started for each file, by the path of the source that
    /// started it.
    static STARTED: LazyLock<Mutex<HashMap<PathBuf, usize>>> = LazyLock::new(Mutex::default);

    /// Hold every read of the file at `path` from now on.
    pub(crate) fn hold_reads(path: &Path) -> Arc<ReadHold> {
        let hold = Arc::new(ReadHold {
            path: path.to_path_buf(),
            state: Mutex::default(),
            changed: Condvar::new(),
        });
        lock(&HOLDS).push(Arc::clone(&hold));
        hold
    }

    impl ReadHold {
        /// Whether a read is held now.
        pub(crate) fn holds_a_read(&self) -> bool {
            lock(&self.state).waiting > 0
        }

        /// Let the held reads, and every later one, return.
        pub(crate) fn release(&self) {
            lock(&HOLDS).retain(|hold| !std::ptr::eq(&**hold, self));
            lock(&self.state).released = true;
            self.changed.notify_all();
        }
    }

    impl Drop for ReadHold {
        fn drop(&mut self) {
            lock(&self.state).released = true;
            self.changed.notify_all();
        }
    }

    pub(super) fn wait_if_held(path: &Path) {
        let holds: Vec<_> = lock(&HOLDS)
            .iter()
            .filter(|hold| hold.path == path)
            .cloned()
            .collect();
        for hold in holds {
            let mut state = lock(&hold.state);
            state.waiting += 1;
            while !state.released {
                state = hold.changed.wait(state).unwrap();
            }
            state.waiting -= 1;
        }
    }

    pub(super) fn started(path: &Path) {
        *lock(&STARTED).entry(path.to_path_buf()).or_default() += 1;
    }

    fn key(path: &Path) -> FileKey {
        FileKey::of(&std::fs::metadata(path).expect("file metadata"))
    }

    /// How many readers have been started for the file at `path`.
    pub(crate) fn readers_started(path: &Path) -> usize {
        lock(&STARTED).get(path).copied().unwrap_or(0)
    }

    /// Whether a reader reads the file at `path`.
    pub(crate) fn reader_runs(path: &Path) -> bool {
        lock(&READERS).contains_key(&key(path))
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

    use super::test_support::{hold_reads, reader_runs, readers_started};
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
        let src = source(&path);
        src.set_state(gst::State::Ready).unwrap();
        let pad = src.static_pad("src").unwrap();
        pad.activate_mode(gst::PadMode::Pull, true).unwrap();
        let pull = |pad: &gst::Pad| {
            let (done_tx, done) = mpsc::channel();
            let pad = pad.clone();
            std::thread::spawn(move || {
                let _ = done_tx.send(pad.range(0, 100));
            });
            done
        };
        let imp = src.downcast_ref::<super::FileSource>().unwrap().imp();
        let first = pull(&pad);
        wait_until("a read is held", || hold.holds_a_read());
        // `basesrc` stops a source in pull mode so: it flushes, which ends
        // the wait, and lets the readers in again before it stops it.
        imp.unlock().unwrap();
        assert_eq!(
            first.recv_timeout(TIMEOUT).unwrap(),
            Err(gst::FlowError::Flushing)
        );
        imp.unlock_stop().unwrap();
        let second = pull(&pad);
        assert!(second.recv_timeout(Duration::from_millis(50)).is_err());
        imp.stop().unwrap();
        assert_eq!(
            second.recv_timeout(TIMEOUT).unwrap(),
            Err(gst::FlowError::Flushing)
        );

        hold.release();
        pad.activate_mode(gst::PadMode::Pull, false).unwrap();
        src.set_state(gst::State::Null).unwrap();
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
