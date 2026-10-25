// SPDX-License-Identifier: GPL-3.0-or-later

//! Activation of the sources of `playbin3` pipelines that cannot deadlock
//! with their own type finding.
//!
//! A `playbin3` pipeline connects a `SourceActivation`, makes each of its
//! state changes in `change_state`, and hands over each successor in
//! about-to-finish in `hand_over`. The playback engine does, and so must
//! every other pipeline whose sources are `urisourcebin`s, tests included.
//!
//! `urisourcebin` starts to find the type of its file while it still changes
//! state. Once its type finder has found the type, its streaming thread adds
//! a `parsebin` and changes its state. That deadlocks when another thread
//! changes the state of the new `parsebin` meanwhile, as a state change of
//! the pipeline does when it reaches the source: each thread waits for a
//! lock that the other holds. Under load the threads are slow enough for
//! that to happen. See
//! <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/4472>, open
//! in 1.28.
//!
//! So a source finds the type of its file only while no state change of the
//! pipeline and no handover is in progress, and a state change starts only
//! once no source is finding the type of its file.

use std::sync::{Arc, Condvar, Mutex, MutexGuard, PoisonError};

use gst::glib;
use gst::prelude::*;
use gstreamer as gst;

/// Orders the type finding of the sources of a `playbin3` pipeline against
/// the activations of the sources: the pipeline's state changes and the
/// successors it is handed.
pub(crate) struct SourceActivation {
    state: Mutex<ActivationState>,
    /// Signalled when a source has found the type of its file, or no longer
    /// finds it.
    found: Condvar,
}

#[derive(Default)]
struct ActivationState {
    /// State changes and handovers in progress.
    activations: usize,
    /// The source pads of type finders held at the start of their stream,
    /// before they find the type, with the probes that hold them, until no
    /// activation is in progress.
    held: Vec<(gst::Pad, gst::PadProbeId)>,
    /// Type finders that were let go and find the type of their file.
    finding: Vec<glib::WeakRef<gst::Element>>,
    /// State changes that wait for the type finding to end.
    #[cfg(test)]
    waiting: usize,
}

impl ActivationState {
    fn finding(&self) -> bool {
        self.finding
            .iter()
            .any(|typefind| typefind.upgrade().is_some())
    }
}

impl SourceActivation {
    /// Order the type finding of every source that `urisourcebin` sets up in
    /// `playbin`. The type finder of a source announces the stream before it
    /// finds the type, so a probe holds it there. That holds both when the
    /// type finder reads the file itself and when the file source pushes the
    /// file to it, as for a raw surround file that `raw_audio`'s byte filter
    /// offers in push mode only.
    pub(crate) fn connect(playbin: &gst::Element) -> Arc<Self> {
        let activation = Arc::new(Self {
            state: Mutex::default(),
            found: Condvar::new(),
        });
        let added = Arc::clone(&activation);
        // Runs on the thread that adds the type finder, before it starts.
        playbin.connect("deep-element-added", false, move |values| {
            added.watch(&source_type_finder(values)?);
            None
        });
        let removed = Arc::clone(&activation);
        playbin.connect("deep-element-removed", false, move |values| {
            removed.done(&source_type_finder(values)?);
            None
        });
        activation
    }

    /// Run `change`, a state change of the pipeline, once no source finds
    /// the type of its file. Sources that reach their type finding
    /// meanwhile find it once `change` has returned.
    ///
    /// The wait takes as long as finding a type does, a moment for a local
    /// file: a source that has started to find its type needs no lock that a
    /// thread outside a state change holds.
    pub(crate) fn change_state<R>(&self, change: impl FnOnce() -> R) -> R {
        let mut state = self.lock();
        state.activations += 1;
        #[cfg(test)]
        {
            state.waiting += 1;
        }
        while state.finding() {
            state = self
                .found
                .wait(state)
                .unwrap_or_else(PoisonError::into_inner);
        }
        #[cfg(test)]
        {
            state.waiting -= 1;
        }
        drop(state);
        self.activated(change)
    }

    /// Run `hand_over`, which hands the pipeline a successor and activates
    /// its source. The source finds the type of its file once `hand_over`
    /// has returned. Unlike a state change, a handover changes the state of
    /// the successor's source only, so it does not wait for other sources.
    pub(crate) fn hand_over<R>(&self, hand_over: impl FnOnce() -> R) -> R {
        self.lock().activations += 1;
        self.activated(hand_over)
    }

    /// Run `activate`, counted as an activation in progress, and let the
    /// held type finders go once no activation is in progress.
    fn activated<R>(&self, activate: impl FnOnce() -> R) -> R {
        let result = activate();
        let released = {
            let mut state = self.lock();
            state.activations -= 1;
            if state.activations == 0 {
                let held = std::mem::take(&mut state.held);
                // A type finder that a state change deactivated meanwhile
                // finds no type.
                let finding = held
                    .iter()
                    .filter(|(pad, _)| pad.is_active())
                    .filter_map(|(pad, _)| pad.parent_element())
                    .map(|typefind| typefind.downgrade())
                    .collect::<Vec<_>>();
                state.finding.extend(finding);
                held
            } else {
                Vec::new()
            }
        };
        for (pad, probe) in released {
            pad.remove_probe(probe);
        }
        result
    }

    /// Hold `typefind` at the start of its stream while an activation is in
    /// progress, and count it as finding the type of its file from then on
    /// until it has found it or given up.
    fn watch(self: &Arc<Self>, typefind: &gst::Element) {
        let Some(src) = typefind.static_pad("src") else {
            return;
        };
        let activation = Arc::clone(self);
        let weak = typefind.downgrade();
        src.add_probe(
            gst::PadProbeType::BLOCK | gst::PadProbeType::DATA_DOWNSTREAM,
            move |pad, info| {
                let mut state = activation.lock();
                if state.activations == 0 {
                    state.finding.push(weak.clone());
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
        // A type finder that finds no type pushes the end of the stream.
        let activation = Arc::clone(self);
        let weak = typefind.downgrade();
        src.add_probe(gst::PadProbeType::EVENT_DOWNSTREAM, move |_, info| {
            let eos = matches!(
                &info.data,
                Some(gst::PadProbeData::Event(event)) if event.type_() == gst::EventType::Eos
            );
            if let Some(typefind) = weak.upgrade().filter(|_| eos) {
                activation.done(&typefind);
            }
            gst::PadProbeReturn::Ok
        });
        // After the handler of `urisourcebin`, which sets up the `parsebin`.
        let activation = Arc::clone(self);
        typefind.connect("have-type", true, move |values| {
            activation.done(&values.first()?.get::<gst::Element>().ok()?);
            None
        });
    }

    /// `typefind` has found the type of its file, or no longer finds it.
    fn done(&self, typefind: &gst::Element) {
        self.lock().finding.retain(|finding| {
            finding
                .upgrade()
                .is_some_and(|finding| finding != *typefind)
        });
        self.found.notify_all();
    }

    fn lock(&self) -> MutexGuard<'_, ActivationState> {
        self.state.lock().unwrap_or_else(PoisonError::into_inner)
    }

    /// Whether a state change waits for a source to find the type of its
    /// file.
    #[cfg(test)]
    pub(crate) fn state_change_waits(&self) -> bool {
        self.lock().waiting > 0
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
                    source
                        .factory()
                        .is_some_and(|factory| factory.name() == "filesrc")
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
