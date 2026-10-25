// SPDX-License-Identifier: GPL-3.0-or-later

//! Activation of the sources of `playbin3` pipelines that cannot deadlock
//! with their own type finding.
//!
//! A `playbin3` pipeline connects `connect_type_finding_hold` and runs each
//! of its state changes, and each successor it hands over in
//! about-to-finish, in `activate_sources`. The playback engine does, and so
//! must every other pipeline whose sources are `urisourcebin`s, tests
//! included.

use std::cell::RefCell;

use gst::prelude::*;
use gstreamer as gst;

thread_local! {
    /// While the thread runs `activate_sources`, the probes that hold back
    /// the type finding of the sources it activates.
    static TYPE_FINDING_HOLDS: RefCell<Option<Vec<(gst::Pad, gst::PadProbeId)>>> =
        const { RefCell::new(None) };
}

/// Run `activate`, which can activate sources of a pipeline that
/// `connect_type_finding_hold` was connected to: a state change of the
/// pipeline, or a successor handed over. The sources find the type
/// of their files only once it has returned.
///
/// `urisourcebin` starts to find the type of its file while it still changes
/// state. Its streaming thread then adds a `parsebin` and changes its state,
/// which deadlocks when the activating thread changes the state of the new
/// `parsebin` too: each waits for a lock that the other holds. Under load
/// the activating thread is slow enough for that to happen. See
/// <https://gitlab.freedesktop.org/gstreamer/gstreamer/-/issues/4472>, open
/// in 1.28.
pub(crate) fn activate_sources<R>(activate: impl FnOnce() -> R) -> R {
    let outer = TYPE_FINDING_HOLDS.with(|holds| holds.replace(Some(Vec::new())));
    let result = activate();
    let holds = TYPE_FINDING_HOLDS.with(|holds| holds.replace(outer));
    for (pad, probe) in holds.into_iter().flatten() {
        pad.remove_probe(probe);
    }
    result
}

/// Hold back the type finding of every source that `urisourcebin` sets up
/// in `activate_sources`, until that returns. The type finder of a source
/// announces the stream before it finds the type, so the probe holds it
/// there. That holds both when the type finder reads the file itself and
/// when the file source pushes the file to it, as for a raw surround file
/// that `raw_audio`'s byte filter offers in push mode only.
pub(crate) fn connect_type_finding_hold(playbin: &gst::Element) {
    // Runs on the thread that adds the type finder, before it starts.
    playbin.connect("deep-element-added", false, |values| {
        let bin = values.get(1)?.get::<gst::Bin>().ok()?;
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        let factory_is = |element: &gst::Element, name: &str| {
            element
                .factory()
                .is_some_and(|factory| factory.name() == name)
        };
        if !factory_is(&element, "typefind") || !factory_is(bin.upcast_ref(), "urisourcebin") {
            return None;
        }
        TYPE_FINDING_HOLDS.with(|holds| {
            let mut holds = holds.borrow_mut();
            let holds = holds.as_mut()?;
            let pad = element.static_pad("src")?;
            let probe = pad.add_probe(
                gst::PadProbeType::BLOCK | gst::PadProbeType::DATA_DOWNSTREAM,
                |_, _| gst::PadProbeReturn::Ok,
            )?;
            holds.push((pad, probe));
            Some(())
        });
        None
    });
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
