// SPDX-License-Identifier: GPL-3.0-or-later

//! Activation of the sources of `playbin3` pipelines that cannot deadlock
//! with their own type finding.
//!
//! A `playbin3` pipeline connects a `SourceActivation`, makes each of its
//! state changes in `set_state`, and hands over each successor in
//! about-to-finish in `hand_over`. The playback engine does, and so must
//! every other pipeline whose sources are `urisourcebin`s, tests included.
//!
//! Once the type finder of a `urisourcebin` has found the type of its file,
//! `urisourcebin` adds a `parsebin` for it and sets its state, in the
//! `have-type` signal on the type finder's streaming thread. A state change
//! of the pipeline that another thread makes meanwhile reaches the new
//! `parsebin`, because bins always set the state of their child bins. Each
//! thread then waits for a lock that the other holds: the state change
//! activates the `parsebin`, which needs the type finder's stream lock, and
//! the type finder waits for the `parsebin`'s state lock. Under load the
//! threads are slow enough for that to happen. See
//! <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/4472>, open
//! in 1.28.
//!
//! That setup, the `have-type` emission, only adds and links elements and
//! sets their state. It reads no data: the type finder has read what it
//! needed before, and the new `parsebin` reads on its own thread. So a
//! state change waits for the setups in progress, which take a moment, but
//! never for a source that reads its file to find its type. A setup in turn
//! waits for a state change to Paused or Playing in progress, which needs
//! no lock that a type finder holds before its setup. A setup that would
//! start during a state change to Ready or Null, which waits for the type
//! finder's streaming thread to stop, is skipped instead: that state change
//! ends the run and removes the source.
//!
//! A source also finds its type only once the state change or handover that
//! activates it has returned, because the activating thread goes on to
//! change the state of what the source sets up.
//!
//! `GStreamer` also changes the state of the pipeline on a thread of its
//! own, which no state change here can wait for: a pipeline asked to play
//! before it has prerolled goes on to Playing from a pool thread once it
//! has, and so does one that prerolls again after a flushing seek. A
//! successor handed over meanwhile, as a short track's is while the run
//! prerolls, can find its type just then. So a setup also holds the state
//! lock of its `urisourcebin`, which every state change of the source
//! takes: one that would reach the source meanwhile waits for the setup,
//! and one that is changing the source's state already finishes first,
//! which it can because the `parsebin` it would deadlock on is not there
//! yet.
//!
//! Connecting also orders the stream collection of each stream that a
//! source parses ahead of the stream's data (see `collection_order`),
//! reports each stream that a `parsebin` holds after exposing it (see
//! `parsebin_stall`), and makes the sources read their files on a thread
//! that a flush does not wait for (see `file_source`).

use std::cell::RefCell;
use std::marker::PhantomData;
use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};

use gst::glib;
use gst::prelude::*;
use gstreamer as gst;

/// Orders the setup that the sources of a `playbin3` pipeline make for the
/// type of their file against the pipeline's state changes and the
/// successors it is handed.
pub(crate) struct SourceActivation {
    state: Mutex<ActivationState>,
    /// Signalled when a source has set up what it found, and when a state
    /// change has returned.
    settled: Condvar,
}

#[derive(Default)]
struct ActivationState {
    /// State changes and handovers in progress.
    activations: usize,
    /// State changes to Paused or Playing in progress.
    rising: usize,
    /// State changes to Ready or Null that are in progress or wait to
    /// start. They end the run.
    ending: usize,
    /// The source pads of type finders held at the start of their stream,
    /// before they find the type, with the probes that hold them, until no
    /// activation is in progress.
    held: Vec<(gst::Pad, gst::PadProbeId)>,
    /// Type finders whose `urisourcebin` sets up a `parsebin` for the type
    /// they found.
    setting_up: Vec<glib::WeakRef<gst::Element>>,
    /// State changes that wait for a setup to end.
    #[cfg(test)]
    waiting: usize,
    /// Setups that wait for a state change to return.
    #[cfg(test)]
    setups_waiting: usize,
    /// Setups skipped because the run ended.
    #[cfg(test)]
    skipped: usize,
}

impl ActivationState {
    fn setting_up(&self) -> bool {
        self.setting_up
            .iter()
            .any(|typefind| typefind.upgrade().is_some())
    }
}

impl SourceActivation {
    /// Order the setup of every source that `urisourcebin` creates in
    /// `playbin`, and hold its type finding until the source is activated.
    /// The type finder of a source announces the stream before it finds the
    /// type, so a probe holds it there. That holds both when the type finder
    /// reads the file itself and when the file source pushes the file to
    /// it, as for a raw surround file that `raw_audio`'s byte filter offers
    /// in push mode only.
    pub(crate) fn connect(playbin: &gst::Element) -> Arc<Self> {
        super::collection_order::connect(playbin);
        super::parsebin_stall::connect(playbin);
        super::file_source::register();
        let activation = Arc::new(Self {
            state: Mutex::default(),
            settled: Condvar::new(),
        });
        let added = Arc::clone(&activation);
        // Runs on the thread that adds the type finder, before it starts
        // and before `urisourcebin` connects to its `have-type` signal.
        playbin.connect("deep-element-added", false, move |values| {
            added.watch(&source_type_finder(values)?);
            None
        });
        let removed = Arc::clone(&activation);
        playbin.connect("deep-element-removed", false, move |values| {
            removed.set_up(&source_type_finder(values)?);
            None
        });
        activation
    }

    /// Set `playbin` to `state` once no source sets up what it found.
    /// Sources that reach the start of their stream meanwhile find their
    /// type once the state change has returned.
    ///
    /// The wait takes as long as a setup does, a moment: a setup reads no
    /// data and needs no lock that a thread outside a state change holds.
    pub(crate) fn set_state(
        &self,
        playbin: &gst::Element,
        state: gst::State,
    ) -> Result<gst::StateChangeSuccess, gst::StateChangeError> {
        self.change(state <= gst::State::Ready, || playbin.set_state(state))
    }

    /// Run `change`, a state change of the pipeline, to Ready or Null if
    /// `ends_run`, else to Paused or Playing.
    fn change<R>(&self, ends_run: bool, change: impl FnOnce() -> R) -> R {
        let mut guard = self.lock();
        guard.activations += 1;
        if ends_run {
            // Setups that would start from now on are skipped, also those
            // that wait for a state change of another thread.
            guard.ending += 1;
            self.settled.notify_all();
        }
        #[cfg(test)]
        {
            guard.waiting += 1;
        }
        while guard.setting_up() {
            guard = self
                .settled
                .wait(guard)
                .unwrap_or_else(PoisonError::into_inner);
        }
        #[cfg(test)]
        {
            guard.waiting -= 1;
        }
        if !ends_run {
            guard.rising += 1;
        }
        drop(guard);
        let result = change();
        {
            let mut guard = self.lock();
            if ends_run {
                guard.ending -= 1;
            } else {
                guard.rising -= 1;
            }
        }
        self.settled.notify_all();
        self.activated();
        result
    }

    /// Run `hand_over`, which hands the pipeline a successor and activates
    /// its source. The source finds the type of its file once `hand_over`
    /// has returned. Unlike a state change, a handover changes the state of
    /// the successor's source only, so it waits for no other source and
    /// holds no setup of another source.
    pub(crate) fn hand_over<R>(&self, hand_over: impl FnOnce() -> R) -> R {
        self.lock().activations += 1;
        let result = hand_over();
        self.activated();
        result
    }

    /// An activation has returned. Let the held type finders go once no
    /// activation is in progress.
    fn activated(&self) {
        let released = {
            let mut state = self.lock();
            state.activations -= 1;
            if state.activations == 0 {
                std::mem::take(&mut state.held)
            } else {
                Vec::new()
            }
        };
        for (pad, probe) in released {
            pad.remove_probe(probe);
        }
    }

    /// Hold `typefind` at the start of its stream while an activation is in
    /// progress, and order the setup of the type it finds against the
    /// state changes.
    fn watch(self: &Arc<Self>, typefind: &gst::Element) {
        let Some(src) = typefind.static_pad("src") else {
            return;
        };
        let activation = Arc::clone(self);
        src.add_probe(
            gst::PadProbeType::BLOCK | gst::PadProbeType::DATA_DOWNSTREAM,
            move |pad, info| {
                let mut state = activation.lock();
                if state.activations == 0 {
                    return gst::PadProbeReturn::Remove;
                }
                match info.id.take() {
                    Some(probe) => {
                        state.held.push((pad.clone(), probe));
                        gst::PadProbeReturn::Ok
                    }
                    None => gst::PadProbeReturn::Pass,
                }
            },
        );
        // Before the handler of `urisourcebin`, which sets up the
        // `parsebin`.
        let activation = Arc::clone(self);
        typefind.connect("have-type", false, move |values| {
            let typefind = values.first()?.get::<gst::Element>().ok()?;
            if activation.setting_up(&typefind) {
                SourceStateLock::hold(&typefind);
            } else {
                typefind.stop_signal_emission_by_name("have-type");
            }
            None
        });
        // After it, which the handler of `urisourcebin` lets run: it does
        // not stop the emission.
        let activation = Arc::clone(self);
        typefind.connect("have-type", true, move |values| {
            let typefind = values.first()?.get::<gst::Element>().ok()?;
            SourceStateLock::release(&typefind);
            activation.set_up(&typefind);
            None
        });
    }

    /// `typefind` has found the type of its file, and its `urisourcebin`
    /// is about to set up a `parsebin` for it. Wait for a state change to
    /// Paused or Playing in progress. Returns false, for the setup to be
    /// skipped, if a state change ends the run: that state change waits
    /// for the type finder's streaming thread, which is this thread, to
    /// stop.
    fn setting_up(&self, typefind: &gst::Element) -> bool {
        let mut state = self.lock();
        loop {
            if state.ending > 0 {
                #[cfg(test)]
                {
                    state.skipped += 1;
                }
                return false;
            }
            if state.rising == 0 {
                break;
            }
            #[cfg(test)]
            {
                state.setups_waiting += 1;
            }
            state = self
                .settled
                .wait(state)
                .unwrap_or_else(PoisonError::into_inner);
            #[cfg(test)]
            {
                state.setups_waiting -= 1;
            }
        }
        state.setting_up.push(typefind.downgrade());
        true
    }

    /// `urisourcebin` has set up what `typefind` found, or removed it.
    fn set_up(&self, typefind: &gst::Element) {
        self.lock().setting_up.retain(|setting_up| {
            setting_up
                .upgrade()
                .is_some_and(|setting_up| setting_up != *typefind)
        });
        self.settled.notify_all();
    }

    fn lock(&self) -> MutexGuard<'_, ActivationState> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Whether a state change waits for a source to set up what it found.
    #[cfg(test)]
    pub(crate) fn state_change_waits(&self) -> bool {
        let state = self.lock();
        state.waiting > 0 && state.setting_up()
    }

    /// How many setups were skipped because the run ended.
    #[cfg(test)]
    fn skipped_setups(&self) -> usize {
        self.lock().skipped
    }

    /// Whether a source waits for a state change to return before it sets
    /// up what it found.
    #[cfg(test)]
    pub(crate) fn setup_waits(&self) -> bool {
        self.lock().setups_waiting > 0
    }

    /// Whether a state change to Ready or Null is in progress or waits to
    /// start.
    #[cfg(test)]
    pub(crate) fn run_ends(&self) -> bool {
        self.lock().ending > 0
    }
}

thread_local! {
    /// The state locks that the setups on this thread hold.
    static SOURCE_STATE_LOCKS: RefCell<Vec<SourceStateLock>> = const { RefCell::new(Vec::new()) };
}

/// The state lock of the `urisourcebin` of a type finder, held on the type
/// finder's streaming thread while the `urisourcebin` sets up what the type
/// finder found, so that no state change of the source runs meanwhile. A
/// recursive mutex that the thread which locked it must unlock, so it never
/// leaves that thread.
struct SourceStateLock {
    typefind: gst::Element,
    source: gst::Element,
    _thread: PhantomData<*const ()>,
}

impl SourceStateLock {
    /// Lock the state of the `urisourcebin` of `typefind`, waiting for a
    /// state change of it in progress, until `release`.
    fn hold(typefind: &gst::Element) {
        let Some(source) = typefind.parent().and_downcast::<gst::Element>() else {
            return;
        };
        // SAFETY: `source` is a live `GstElement`, kept alive by the
        // reference this holds until the lock is dropped, and its state lock
        // is a `GRecMutex` initialized with the element, which GStreamer
        // documents for use by code outside the element (`GST_STATE_LOCK`).
        // It is unlocked by `Drop` on this thread, which locked it.
        unsafe {
            gst::glib::ffi::g_rec_mutex_lock(&raw mut (*source.as_ptr()).state_lock);
        }
        let lock = Self {
            typefind: typefind.clone(),
            source,
            _thread: PhantomData,
        };
        SOURCE_STATE_LOCKS.with(|locks| locks.borrow_mut().push(lock));
    }

    /// Unlock what `hold` locked for `typefind`.
    fn release(typefind: &gst::Element) {
        let lock = SOURCE_STATE_LOCKS.with(|locks| {
            let mut locks = locks.borrow_mut();
            let held = locks.iter().rposition(|lock| lock.typefind == *typefind)?;
            Some(locks.remove(held))
        });
        drop(lock);
    }
}

impl Drop for SourceStateLock {
    fn drop(&mut self) {
        // SAFETY: `hold` locked this state lock on this thread, since the
        // lock never leaves the thread, and `self.source` keeps it alive.
        unsafe {
            gst::glib::ffi::g_rec_mutex_unlock(&raw mut (*self.source.as_ptr()).state_lock);
        }
    }
}

/// The type finder of a `urisourcebin` that a `deep-element-added` or
/// `deep-element-removed` signal with `values` reports, if it reports one.
fn source_type_finder(values: &[glib::Value]) -> Option<gst::Element> {
    let bin = values.get(1)?.get::<gst::Bin>().ok()?;
    let element = values.get(2)?.get::<gst::Element>().ok()?;
    let factory_is = |element: &gst::Element, name: &str| {
        element
            .factory()
            .is_some_and(|factory| factory.name() == name)
    };
    (factory_is(&element, "typefind") && factory_is(bin.upcast_ref(), "urisourcebin"))
        .then_some(element)
}

/// Races the type finding of a source against the thread that activates it,
/// for tests of pipelines that use the hold.
#[cfg(test)]
pub(crate) mod race {
    use std::path::Path;
    use std::sync::{Arc, Mutex};
    use std::time::{Duration, Instant};

    use gst::prelude::*;
    use gstreamer as gst;

    /// How long `race_type_finding_with_activation` holds the activating
    /// thread at most.
    const RACE_TIMEOUT: Duration = Duration::from_secs(10);

    /// What came first while the thread that activated the source of a file
    /// was held in the source's state change.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub(crate) enum ActivationRace {
        /// The type finding of the source was held back.
        TypeFindingHeld,
        /// `urisourcebin` found the type of the file and set up a
        /// `parsebin`, which can deadlock with the activating thread.
        ParsebinSetUp,
        /// Neither happened within `RACE_TIMEOUT`.
        Neither,
    }

    /// Hold the thread that activates the source of `file` when the source
    /// becomes Paused, which is while its `urisourcebin` changes state,
    /// until the type finding of the source is held back or `urisourcebin`
    /// has set up a `parsebin` for it. Returns which came first.
    pub(crate) fn race_type_finding_with_activation(
        playbin: &gst::Element,
        file: &Path,
    ) -> Arc<Mutex<Option<ActivationRace>>> {
        let outcome = Arc::new(Mutex::new(None));
        let recorded = Arc::clone(&outcome);
        let file = file.to_path_buf();
        let bus = playbin.bus().unwrap();
        bus.enable_sync_message_emission();
        bus.connect_sync_message(None, move |_, message| {
            let gst::MessageView::StateChanged(change) = message.view() else {
                return;
            };
            let Some(source) = message
                .src()
                .and_then(|src| src.downcast_ref::<gst::Element>())
                .filter(|source| {
                    source.factory().is_some_and(|factory| {
                        factory.name() == crate::playback::file_source::FACTORY_NAME
                    })
                })
            else {
                return;
            };
            let location = source.property::<Option<String>>("location");
            if (change.old(), change.current()) != (gst::State::Ready, gst::State::Paused)
                || location.as_deref().map(Path::new) != Some(file.as_path())
                || recorded.lock().unwrap().is_some()
            {
                return;
            }
            let urisourcebin = source
                .parent()
                .and_downcast::<gst::Bin>()
                .expect("urisourcebin");
            let deadline = Instant::now() + RACE_TIMEOUT;
            let race = loop {
                let parsebin = urisourcebin
                    .iterate_elements()
                    .into_iter()
                    .filter_map(Result::ok)
                    .any(|element| {
                        element
                            .factory()
                            .is_some_and(|factory| factory.name() == "parsebin")
                    });
                if parsebin {
                    break ActivationRace::ParsebinSetUp;
                }
                let held = urisourcebin
                    .iterate_elements()
                    .into_iter()
                    .filter_map(Result::ok)
                    .filter_map(|element| element.static_pad("src"))
                    .any(|pad| pad.is_blocking());
                if held {
                    break ActivationRace::TypeFindingHeld;
                }
                if Instant::now() >= deadline {
                    break ActivationRace::Neither;
                }
                std::thread::sleep(Duration::from_millis(1));
            };
            *recorded.lock().unwrap() = Some(race);
        });
        outcome
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{mpsc, Arc};
    use std::thread::JoinHandle;
    use std::time::{Duration, Instant};

    use gst::prelude::*;
    use gstreamer as gst;

    use super::SourceActivation;

    /// How long a test waits for a thread to get somewhere.
    const TIMEOUT: Duration = Duration::from_secs(10);

    /// A `SourceActivation` and a type finder whose setups it orders.
    fn activation() -> (Arc<SourceActivation>, gst::Element) {
        gst::init().expect("gstreamer initialization");
        let bin = gst::Bin::new();
        let typefind = gst::ElementFactory::make("typefind")
            .build()
            .expect("typefind");
        (SourceActivation::connect(bin.upcast_ref()), typefind)
    }

    #[track_caller]
    fn wait_until(what: &str, done: impl Fn() -> bool) {
        let deadline = Instant::now() + TIMEOUT;
        while !done() {
            assert!(Instant::now() < deadline, "{what}");
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    /// Start the setup of `typefind` on another thread, which sends whether
    /// it goes ahead.
    fn set_up_on_thread(
        activation: &Arc<SourceActivation>,
        typefind: &gst::Element,
    ) -> (JoinHandle<()>, mpsc::Receiver<bool>) {
        let (sent, received) = mpsc::channel();
        let activation = Arc::clone(activation);
        let typefind = typefind.clone();
        let thread = std::thread::spawn(move || {
            sent.send(activation.setting_up(&typefind)).unwrap();
        });
        (thread, received)
    }

    /// Start a state change on another thread, which sends once it runs.
    fn change_on_thread(
        activation: &Arc<SourceActivation>,
        ends_run: bool,
    ) -> (JoinHandle<()>, mpsc::Receiver<()>) {
        let (sent, received) = mpsc::channel();
        let activation = Arc::clone(activation);
        let thread = std::thread::spawn(move || {
            activation.change(ends_run, || sent.send(()).unwrap());
        });
        (thread, received)
    }

    #[test]
    fn a_setup_waits_for_a_state_change_to_paused_or_playing() {
        // The state change reaches the parsebin that the setup adds, and
        // would activate it while the type finder's thread holds the lock
        // that the activation needs.
        let (activation, typefind) = activation();
        let (thread, set_up) = activation.change(false, || {
            let (thread, set_up) = set_up_on_thread(&activation, &typefind);
            wait_until("the setup did not wait", || activation.setup_waits());
            assert!(set_up.try_recv().is_err());
            (thread, set_up)
        });
        assert_eq!(set_up.recv_timeout(TIMEOUT), Ok(true));
        thread.join().unwrap();
    }

    #[test]
    fn a_state_change_waits_for_a_setup() {
        for ends_run in [false, true] {
            let (activation, typefind) = activation();
            assert!(activation.setting_up(&typefind));
            let (thread, changed) = change_on_thread(&activation, ends_run);
            wait_until("the state change did not wait", || {
                activation.state_change_waits()
            });
            assert!(changed.try_recv().is_err(), "ends run: {ends_run}");
            activation.set_up(&typefind);
            changed.recv_timeout(TIMEOUT).unwrap();
            thread.join().unwrap();
        }
    }

    #[test]
    fn a_setup_is_skipped_while_the_run_ends() {
        // The state change that ends the run waits for the type finder's
        // streaming thread, which a setup that waited would block.
        let (activation, typefind) = activation();
        activation.change(true, || assert!(!activation.setting_up(&typefind)));
        assert_eq!(activation.skipped_setups(), 1);
        // The next run's sources set up what they find.
        assert!(activation.setting_up(&typefind));
    }

    #[test]
    fn a_setup_that_waits_is_skipped_once_the_run_ends() {
        let (activation, typefind) = activation();
        let setup = activation.change(false, || {
            let (setup, set_up) = set_up_on_thread(&activation, &typefind);
            wait_until("the setup did not wait", || activation.setup_waits());
            // A state change of another thread, such as a test's teardown,
            // which waits for the type finder's streaming thread to stop.
            let (sent, received) = mpsc::channel();
            let ending = Arc::clone(&activation);
            let change = std::thread::spawn(move || {
                ending.change(true, || sent.send(set_up.recv_timeout(TIMEOUT)).unwrap());
            });
            assert_eq!(received.recv_timeout(TIMEOUT), Ok(Ok(false)));
            change.join().unwrap();
            setup
        });
        setup.join().unwrap();
        assert_eq!(activation.skipped_setups(), 1);
    }

    #[test]
    fn a_state_change_of_a_source_on_another_thread_waits_for_its_setup() {
        // GStreamer goes on to Playing on a thread of its own once the
        // pipeline has prerolled, which no state change here waits for. That
        // state change once reached the parsebin that the source of a
        // successor was setting up, and deadlocked with the setup.
        gst::init().expect("gstreamer initialization");
        let dir =
            std::env::temp_dir().join(format!("ferrous-source-activation-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let file = dir.join("a.mp3");
        std::fs::write(&file, include_bytes!("../analysis/fixtures/seek-tone.mp3")).unwrap();
        let uri = url::Url::from_file_path(&file).unwrap().to_string();

        let pipeline = gst::Pipeline::new();
        // As each input of playbin3 is.
        let source = gst::ElementFactory::make("urisourcebin")
            .property("uri", uri.as_str())
            .property("parse-streams", true)
            .build()
            .expect("urisourcebin");
        pipeline.add(&source).unwrap();
        let activation = SourceActivation::connect(pipeline.upcast_ref());

        // Once the source has added its parsebin, on the thread that sets
        // it up, another thread asks whether a state change of the source
        // could start now, without waiting, and then changes its state.
        // Sends the answer, and where the state change sends its result.
        let (sent, during_setup) = mpsc::channel();
        let sent = std::sync::Mutex::new(Some(sent));
        pipeline.connect("deep-element-added", false, move |values| {
            let element = values.get(2)?.get::<gst::Element>().ok()?;
            if element.factory()?.name() != "parsebin" {
                return None;
            }
            let sent = sent.lock().unwrap().take()?;
            let source = element.parent().and_downcast::<gst::Element>()?;
            let (answered, answer) = mpsc::channel();
            let (changed, change) = mpsc::channel();
            std::thread::spawn(move || {
                let _ = answered.send(state_lock_free(&source));
                let _ = changed.send(source.set_state(gst::State::Playing));
            });
            let _ = sent.send((answer.recv_timeout(TIMEOUT), change));
            None
        });

        activation
            .set_state(pipeline.upcast_ref(), gst::State::Paused)
            .unwrap();
        let (free, change) = during_setup
            .recv_timeout(TIMEOUT)
            .expect("the source did not set up a parsebin");
        // Once the setup has ended, the state change goes on.
        let Ok(result) = change.recv_timeout(TIMEOUT) else {
            // Still held: the teardown would wait for it too.
            std::mem::forget(pipeline);
            panic!("the state change of the source did not return after the setup");
        };
        activation
            .set_state(pipeline.upcast_ref(), gst::State::Null)
            .unwrap();
        std::fs::remove_dir_all(dir).unwrap();
        assert_eq!(
            free,
            Ok(false),
            "a state change could start during the setup"
        );
        assert!(result.is_ok(), "{result:?}");
    }

    /// Whether a state change of `element` could start now: no other thread
    /// holds its state lock.
    fn state_lock_free(element: &gst::Element) -> bool {
        // SAFETY: `element` is a live `GstElement`, and its state lock is a
        // `GRecMutex` initialized with it. A lock this takes is unlocked at
        // once, on this thread.
        unsafe {
            let lock = &raw mut (*element.as_ptr()).state_lock;
            let free = gst::glib::ffi::g_rec_mutex_trylock(lock) != 0;
            if free {
                gst::glib::ffi::g_rec_mutex_unlock(lock);
            }
            free
        }
    }

    #[test]
    fn a_type_finder_that_is_gone_holds_no_state_change() {
        let (activation, typefind) = activation();
        assert!(activation.setting_up(&typefind));
        drop(typefind);
        let (thread, changed) = change_on_thread(&activation, false);
        changed.recv_timeout(TIMEOUT).unwrap();
        thread.join().unwrap();
    }
}
