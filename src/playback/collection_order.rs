// SPDX-License-Identifier: GPL-3.0-or-later

//! Keeps the data of a stream that a source parses from reaching
//! `decodebin3` before the stream's collection.
//!
//! A `urisourcebin` parses the streams of a local file in a `parsebin`.
//! `decodebin3` sets up an input whose caps are parsed only once the
//! collection of the input's stream has arrived. Data that arrives before it
//! finds the input unlinked, and the parser that pushed the data stops with
//! `not-linked`, an "Internal data stream error".
//!
//! In 1.24, `parsebin` sends that collection only once it has exposed the
//! pad of a stream and let the pad's data through, and from the thread that
//! exposes the pad. A parser that reads its file in pull mode pushes on its
//! own thread meanwhile, so its first buffer can reach the pad between the
//! two and go first. From 1.25.1 (upstream commit f130382b), `parsebin`
//! stores the collection on the pad before it exposes the pad, and the pad
//! sends it ahead of any data.
//!
//! So the data of a `parsebin` output that is exposed without its collection
//! waits at the output until the collection has reached the output's peer.
//! The collection reaches the peer under the peer's stream lock, so data let
//! go then reaches the peer after it. The wait ends with the exposure, which
//! sends the collection right after letting the data through; a state change
//! that ends the run flushes the data that waits.

use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use gst::glib;
use gst::prelude::*;
use gstreamer as gst;

/// Order the collection of each stream that a `urisourcebin` of `playbin`
/// parses ahead of the stream's data.
pub(crate) fn connect(playbin: &gst::Element) {
    // Runs when `urisourcebin` adds the `parsebin`, before `parsebin`
    // exposes any pad.
    playbin.connect("deep-element-added", false, |values| {
        let parsebin = source_parsebin(values)?;
        parsebin.connect_pad_added(|_, pad| {
            if pad.direction() == gst::PadDirection::Src {
                hold_until_collection(pad);
            }
        });
        None
    });
}

/// The `parsebin` of a `urisourcebin` that a `deep-element-added` signal
/// with `values` reports, if it reports one.
fn source_parsebin(values: &[glib::Value]) -> Option<gst::Element> {
    let bin = values.get(1)?.get::<gst::Bin>().ok()?;
    let element = values.get(2)?.get::<gst::Element>().ok()?;
    let factory_is = |element: &gst::Element, name: &str| {
        element
            .factory()
            .is_some_and(|factory| factory.name() == name)
    };
    (factory_is(&element, "parsebin") && factory_is(bin.upcast_ref(), "urisourcebin"))
        .then_some(element)
}

/// Hold the data of `pad`, an output that `parsebin` exposes, until the
/// stream collection has reached the pad's peer, unless the pad has its
/// collection already. Events other than gaps pass.
fn hold_until_collection(pad: &gst::Pad) {
    if pad
        .sticky_event::<gst::event::StreamCollection>(0)
        .is_some()
    {
        return;
    }
    let probe = pad.add_probe(
        gst::PadProbeType::BLOCK | gst::PadProbeType::DATA_DOWNSTREAM,
        |_, info| match &info.data {
            Some(gst::PadProbeData::Event(event)) if event.type_() != gst::EventType::Gap => {
                gst::PadProbeReturn::Pass
            }
            _ => gst::PadProbeReturn::Ok,
        },
    );
    let hold = Hold {
        pad: pad.downgrade(),
        probe: Arc::new(Mutex::new(probe)),
    };
    // `urisourcebin` links the pad once `parsebin` has exposed it, before
    // `parsebin` lets its data through.
    let linked = hold.clone();
    pad.connect_linked(move |_, peer| linked.watch_peer(peer));
    if let Some(peer) = pad.peer() {
        hold.watch_peer(&peer);
    }
    // A collection that the pad sends while it has no peer goes ahead of
    // the data once the pad is linked.
    pad.add_probe(gst::PadProbeType::EVENT_DOWNSTREAM, move |pad, info| {
        if !is_collection(info) {
            return gst::PadProbeReturn::Ok;
        }
        if pad.peer().is_none() {
            hold.release();
        }
        gst::PadProbeReturn::Remove
    });
}

/// Whether the probe `info` carries a stream collection.
fn is_collection(info: &gst::PadProbeInfo) -> bool {
    matches!(
        &info.data,
        Some(gst::PadProbeData::Event(event)) if event.type_() == gst::EventType::StreamCollection
    )
}

/// The block that holds the data of a `parsebin` output until its stream
/// collection has reached the output's peer.
#[derive(Clone)]
struct Hold {
    pad: glib::WeakRef<gst::Pad>,
    /// Taken once the data may pass.
    probe: Arc<Mutex<Option<gst::PadProbeId>>>,
}

impl Hold {
    /// Let the data pass once the collection reaches `peer`, on the thread
    /// that sends it, which holds `peer`'s stream lock meanwhile.
    fn watch_peer(&self, peer: &gst::Pad) {
        if self.lock().is_none() {
            return;
        }
        let hold = self.clone();
        peer.add_probe(gst::PadProbeType::EVENT_DOWNSTREAM, move |_, info| {
            if !is_collection(info) {
                return gst::PadProbeReturn::Ok;
            }
            hold.release();
            gst::PadProbeReturn::Remove
        });
    }

    fn release(&self) {
        let probe = self.lock().take();
        if let (Some(pad), Some(probe)) = (self.pad.upgrade(), probe) {
            pad.remove_probe(probe);
        }
    }

    fn lock(&self) -> MutexGuard<'_, Option<gst::PadProbeId>> {
        self.probe.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};
    use std::thread::JoinHandle;
    use std::time::{Duration, Instant};

    use gst::prelude::*;
    use gstreamer as gst;

    use super::hold_until_collection;

    /// How long a test waits for a thread to get somewhere.
    const TIMEOUT: Duration = Duration::from_secs(10);

    /// What reached the peer of an output, in order.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Item {
        Collection,
        Gap,
        Buffer,
    }

    type Items = Arc<Mutex<Vec<Item>>>;

    /// An output whose stream has started, and its peer, which records the
    /// collections, gaps and buffers that reach it. The output is linked if
    /// `linked`.
    fn output(linked: bool) -> (gst::Pad, gst::Pad, Items) {
        gst::init().expect("gstreamer initialization");
        let items = Items::default();
        let chained = Arc::clone(&items);
        let received = Arc::clone(&items);
        let peer = gst::Pad::builder(gst::PadDirection::Sink)
            .chain_function(move |_, _, _| {
                chained.lock().unwrap().push(Item::Buffer);
                Ok(gst::FlowSuccess::Ok)
            })
            .event_function(move |_, _, event| {
                let item = match event.type_() {
                    gst::EventType::StreamCollection => Some(Item::Collection),
                    gst::EventType::Gap => Some(Item::Gap),
                    _ => None,
                };
                received.lock().unwrap().extend(item);
                true
            })
            .build();
        let pad = gst::Pad::builder(gst::PadDirection::Src).build();
        peer.set_active(true).unwrap();
        pad.set_active(true).unwrap();
        if linked {
            pad.link(&peer).unwrap();
        }
        pad.push_event(gst::event::StreamStart::new("stream"));
        pad.push_event(gst::event::Segment::new(&gst::FormattedSegment::<
            gst::ClockTime,
        >::new()));
        (pad, peer, items)
    }

    fn collection() -> gst::Event {
        gst::event::StreamCollection::new(&gst::StreamCollection::builder(None).build())
    }

    /// Push a buffer to `pad` on another thread, which returns the result.
    fn push_on_thread(pad: &gst::Pad) -> JoinHandle<Result<gst::FlowSuccess, gst::FlowError>> {
        let pad = pad.clone();
        std::thread::spawn(move || pad.push(gst::Buffer::new()))
    }

    #[track_caller]
    fn wait_until(what: &str, done: impl Fn() -> bool) {
        let deadline = Instant::now() + TIMEOUT;
        while !done() {
            assert!(Instant::now() < deadline, "{what}");
            std::thread::sleep(Duration::from_millis(1));
        }
    }

    #[test]
    fn data_waits_until_the_collection_of_its_output_reaches_the_peer() {
        // GStreamer 1.24's parsebin: the parser's first buffer reaches the
        // output after parsebin has let it through and before parsebin has
        // sent the collection, from the thread that exposed the output.
        let (pad, _peer, items) = output(true);
        hold_until_collection(&pad);
        let buffer = push_on_thread(&pad);
        wait_until("the buffer did not wait", || pad.is_blocking());
        // Events other than gaps pass.
        assert!(pad.push_event(gst::event::Tag::new(gst::TagList::new())));
        assert!(items.lock().unwrap().is_empty());

        assert!(pad.push_event(collection()));
        assert_eq!(buffer.join().unwrap(), Ok(gst::FlowSuccess::Ok));
        assert_eq!(*items.lock().unwrap(), [Item::Collection, Item::Buffer]);
        assert!(!pad.is_blocked());
    }

    #[test]
    fn a_gap_waits_like_data() {
        let (pad, _peer, items) = output(true);
        hold_until_collection(&pad);
        let gap = {
            let pad = pad.clone();
            std::thread::spawn(move || {
                pad.push_event(gst::event::Gap::builder(gst::ClockTime::ZERO).build())
            })
        };
        wait_until("the gap did not wait", || pad.is_blocking());
        assert!(items.lock().unwrap().is_empty());
        assert!(pad.push_event(collection()));
        assert!(gap.join().unwrap());
        assert_eq!(*items.lock().unwrap(), [Item::Collection, Item::Gap]);
    }

    #[test]
    fn an_output_exposed_with_its_collection_passes_its_data() {
        // From GStreamer 1.25.1, parsebin stores the collection on an
        // output before it exposes it.
        let (pad, _peer, items) = output(true);
        assert!(pad.push_event(collection()));
        hold_until_collection(&pad);
        assert!(!pad.is_blocked());
        assert_eq!(pad.push(gst::Buffer::new()), Ok(gst::FlowSuccess::Ok));
        assert_eq!(*items.lock().unwrap(), [Item::Collection, Item::Buffer]);
    }

    #[test]
    fn a_collection_sent_without_a_peer_goes_ahead_of_the_data_once_linked() {
        let (pad, peer, items) = output(false);
        hold_until_collection(&pad);
        pad.push_event(collection());
        assert!(!pad.is_blocked());
        pad.link(&peer).unwrap();
        assert_eq!(pad.push(gst::Buffer::new()), Ok(gst::FlowSuccess::Ok));
        assert_eq!(*items.lock().unwrap(), [Item::Collection, Item::Buffer]);
    }

    #[test]
    fn data_that_waits_is_flushed_when_the_run_ends() {
        let (pad, peer, items) = output(true);
        hold_until_collection(&pad);
        let buffer = push_on_thread(&pad);
        wait_until("the buffer did not wait", || pad.is_blocking());
        // A state change to Ready or Null deactivates the output.
        pad.set_active(false).unwrap();
        assert_eq!(buffer.join().unwrap(), Err(gst::FlowError::Flushing));
        assert!(items.lock().unwrap().is_empty());
        drop(peer);
    }
}
