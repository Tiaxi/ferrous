// SPDX-License-Identifier: GPL-3.0-or-later

//! Reports a stream that `parsebin` holds back after exposing it, which the
//! pipeline then never plays.
//!
//! `parsebin` blocks the output of the last element of each chain until it
//! has exposed the chain's stream, and lets the output go when it has. A
//! parser sets its caps only once it reads, on its own thread. The thread
//! that plugged the parser can expose the stream first, with the caps still
//! incomplete (`gst_parse_chain_expose`), and it disconnects `parsebin`'s
//! handler for the parser's caps (`caps_notify_cb`). When the parser's
//! thread already runs that handler, the handler goes on: it analyses the
//! stream again and blocks the output once more, after the exposing thread
//! has let it go. The exposure that follows finds every stream exposed and
//! lets nothing go, so the parser's first buffer waits there until the run
//! ends. `GStreamer` 1.24 and 1.28.8 have that race. Upstream commit
//! 51cb58de (on `main`, and on the 1.28 branch after 1.28.8) closes it: it
//! exposes such a stream only once the parser has pushed, from the block on
//! the parser's thread.
//!
//! So every source pad of an element of a `parsebin` is watched. After
//! `parsebin`'s own handler for new caps, which is where the block comes
//! back, a probe looks at the next item the pad pushes once `parsebin`'s
//! probes have seen it. `parsebin` decides there whether to expose, under
//! the lock that its exposures take. If the pad still blocks the item and
//! its stream is exposed, nothing will let it go, and the pipeline's bus
//! gets a `STALL_REPORT` from the `parsebin`. A version without the race
//! never leaves the block, so it is never reported. The probe holds no item
//! itself.

use std::sync::{Arc, Mutex, MutexGuard, PoisonError};

use gst::glib;
use gst::prelude::*;
use gstreamer as gst;

/// Name of the element message that reports a stalled stream. It comes
/// from the `parsebin` that holds the stream.
pub(crate) const STALL_REPORT: &str = "ferrous-parsebin-stall";

/// Report each stream that a `parsebin` of `playbin` holds after exposing
/// it, on the bus of `playbin`.
pub(crate) fn connect(playbin: &gst::Element) {
    let Some(bus) = playbin.bus() else {
        return;
    };
    // Runs when `parsebin` adds an element, before the element has pushed
    // anything and before `parsebin` connects to its caps.
    playbin.connect("deep-element-added", false, move |values| {
        let parsebin = values.get(1)?.get::<gst::Element>().ok()?;
        if parsebin.factory()?.name() != "parsebin" {
            return None;
        }
        let element = values.get(2)?.get::<gst::Element>().ok()?;
        let reporter = Reporter {
            parsebin: parsebin.downgrade(),
            bus: bus.clone(),
        };
        for pad in element.src_pads() {
            watch(&pad, reporter.clone());
        }
        element.connect_pad_added(move |_, pad| {
            if pad.direction() == gst::PadDirection::Src {
                watch(pad, reporter.clone());
            }
        });
        None
    });
}

/// Whether `message` reports a stalled stream.
pub(crate) fn is_report(message: &gst::message::Element) -> bool {
    message
        .structure()
        .is_some_and(|structure| structure.name() == STALL_REPORT)
}

/// Look at the item that follows each change of the caps of `pad`.
fn watch(pad: &gst::Pad, reporter: Reporter) {
    let check = Arc::new(Check {
        pad: pad.downgrade(),
        reporter,
        stage: Mutex::default(),
    });
    // Connected after: `parsebin`'s handler, which can block the pad again,
    // has run by then.
    pad.connect("notify::caps", true, move |_| {
        check.look();
        None
    });
}

/// Where a `parsebin` posts its reports.
#[derive(Clone)]
struct Reporter {
    parsebin: glib::WeakRef<gst::Element>,
    bus: gst::Bus,
}

impl Reporter {
    /// Whether the `parsebin` has exposed a stream that `pad` pushes.
    fn exposes(&self, pad: &gst::Pad) -> bool {
        let Some(parsebin) = self.parsebin.upgrade() else {
            return false;
        };
        // The peer of the pad is the internal pad of the stream's ghost
        // pad, which belongs to the `parsebin` once it is exposed. The peer
        // of a pad that feeds another element of the `parsebin` is no
        // internal pad.
        pad.peer()
            .and_then(|internal| internal.parent())
            .and_then(|output| output.downcast::<gst::GhostPad>().ok())
            .and_then(|output| output.parent())
            .is_some_and(|parent| parent == *parsebin.upcast_ref::<gst::Object>())
    }

    fn report(&self) {
        let Some(parsebin) = self.parsebin.upgrade() else {
            return;
        };
        let message = gst::message::Element::builder(gst::Structure::new_empty(STALL_REPORT))
            .src(&parsebin)
            .build();
        // Posted on the pipeline's bus directly, because the bins in
        // between handle element messages of their own. A bus that flushes
        // drops it, because the run ends.
        let _ = self.bus.post(message);
    }
}

/// Looks at the items that a pad of a `parsebin` pushes.
struct Check {
    pad: glib::WeakRef<gst::Pad>,
    reporter: Reporter,
    stage: Mutex<Stage>,
}

#[derive(Default)]
enum Stage {
    /// Nothing to look at until the caps of the pad change.
    #[default]
    Idle,
    /// The probe looks at the next item that the pad pushes.
    Looking(gst::PadProbeId),
    /// The pad held the item when the probe looked. A probe added after
    /// looks again, once the probes that were added since have seen it.
    Confirming(gst::PadProbeId),
    /// The stream was reported.
    Reported,
}

impl Check {
    /// Look at the next item that the pad pushes.
    fn look(self: &Arc<Self>) {
        let Some(pad) = self.pad.upgrade() else {
            return;
        };
        let mut stage = self.lock();
        if matches!(*stage, Stage::Reported) {
            return;
        }
        // Added again, the probe goes after every probe that `parsebin` has
        // added.
        stage.stop(&pad);
        if let Some(probe) = self.probe(&pad) {
            *stage = Stage::Looking(probe);
        }
    }

    /// Add a probe that sees each item pushed on `pad` after the blocks
    /// that `pad` has.
    fn probe(self: &Arc<Self>, pad: &gst::Pad) -> Option<gst::PadProbeId> {
        let check = Arc::clone(self);
        // Called for every item that a block of `parsebin` holds, which
        // takes the same types.
        pad.add_probe(
            gst::PadProbeType::BLOCK_DOWNSTREAM
                | gst::PadProbeType::QUERY_DOWNSTREAM
                | gst::PadProbeType::PUSH,
            move |pad, info| check.see(pad, info),
        )
    }

    fn see(self: &Arc<Self>, pad: &gst::Pad, info: &gst::PadProbeInfo) -> gst::PadProbeReturn {
        let mut stage = self.lock();
        let confirming = match &*stage {
            Stage::Looking(probe) if info.id.as_ref() == Some(probe) => false,
            Stage::Confirming(probe) if info.id.as_ref() == Some(probe) => true,
            _ => return gst::PadProbeReturn::Remove,
        };
        // Removed before looking, so that the probe is no block of the pad:
        // the pad holds the item only for the blocks that took it.
        stage.stop(pad);
        if !pad.is_blocked() || !self.reporter.exposes(pad) {
            return gst::PadProbeReturn::Remove;
        }
        if !held_by_block(pad, info) {
            // The item passes the block; look at the next one.
            drop(stage);
            self.look_after(pad);
            return gst::PadProbeReturn::Remove;
        }
        if !confirming {
            // A block that another thread added after this probe has not
            // seen the item yet. `parsebin` adds one while it exposes the
            // streams of a group, and lets it go before it is done.
            if let Some(probe) = self.probe(pad) {
                *stage = Stage::Confirming(probe);
            }
            return gst::PadProbeReturn::Remove;
        }
        *stage = Stage::Reported;
        drop(stage);
        self.reporter.report();
        gst::PadProbeReturn::Remove
    }

    /// Look at the item that `pad` pushes after the one it pushes now.
    fn look_after(self: &Arc<Self>, pad: &gst::Pad) {
        let check = Arc::clone(self);
        // Sees the item once it has passed the blocks.
        pad.add_probe(
            gst::PadProbeType::DATA_DOWNSTREAM
                | gst::PadProbeType::QUERY_DOWNSTREAM
                | gst::PadProbeType::PUSH,
            move |_, _| {
                check.look();
                gst::PadProbeReturn::Remove
            },
        );
    }

    fn lock(&self) -> MutexGuard<'_, Stage> {
        self.stage.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

impl Stage {
    /// Remove the probe of a stage that looks, which leaves it idle.
    fn stop(&mut self, pad: &gst::Pad) {
        match std::mem::take(self) {
            Self::Looking(probe) | Self::Confirming(probe) => pad.remove_probe(probe),
            Self::Reported => *self = Self::Reported,
            Self::Idle => {}
        }
    }
}

/// Whether a block of `parsebin` on `pad` holds the item of `info` once its
/// probe has seen it, as `source_pad_blocked_cb` does in 1.24 and 1.28: data,
/// serialized events except the sticky ones that it sends on (all but EOS),
/// and serialized queries once the pad has caps. It decides about those
/// under the lock of its exposures.
fn held_by_block(pad: &gst::Pad, info: &gst::PadProbeInfo) -> bool {
    match &info.data {
        Some(gst::PadProbeData::Buffer(_) | gst::PadProbeData::BufferList(_)) => true,
        Some(gst::PadProbeData::Event(event)) => {
            event.is_serialized() && (!event.is_sticky() || event.type_() == gst::EventType::Eos)
        }
        Some(gst::PadProbeData::Query(query)) => query.is_serialized() && pad.has_current_caps(),
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex};
    use std::thread::JoinHandle;
    use std::time::{Duration, Instant};

    use gst::prelude::*;
    use gstreamer as gst;

    use super::{is_report, watch, Reporter};

    /// How long a test waits for a thread to get somewhere.
    const TIMEOUT: Duration = Duration::from_secs(10);

    /// What reached the stream's peer, in order.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum Item {
        Caps,
        Segment,
        Buffer,
    }

    /// The output of a parser in a stand-in for a `parsebin`, which reports
    /// on the bus of its pipeline.
    struct Output {
        pipeline: gst::Pipeline,
        parsebin: gst::Bin,
        /// The parser's output, which `parsebin` blocks.
        pad: gst::Pad,
        /// The ghost pad that exposes the stream once it is added to
        /// `parsebin`.
        stream: gst::GhostPad,
        /// Records what reaches it in `items`.
        _peer: gst::Pad,
        items: Arc<Mutex<Vec<Item>>>,
    }

    impl Output {
        /// A watched output whose stream has started, exposed if `exposed`.
        fn new(exposed: bool) -> Self {
            gst::init().expect("gstreamer initialization");
            let pipeline = gst::Pipeline::new();
            let parsebin = gst::Bin::new();
            pipeline.add(&parsebin).unwrap();
            let items = Arc::new(Mutex::new(Vec::new()));
            let chained = Arc::clone(&items);
            let received = Arc::clone(&items);
            let peer = gst::Pad::builder(gst::PadDirection::Sink)
                .chain_function(move |_, _, _| {
                    chained.lock().unwrap().push(Item::Buffer);
                    Ok(gst::FlowSuccess::Ok)
                })
                .event_function(move |_, _, event| {
                    let item = match event.type_() {
                        gst::EventType::Caps => Some(Item::Caps),
                        gst::EventType::Segment => Some(Item::Segment),
                        _ => None,
                    };
                    received.lock().unwrap().extend(item);
                    true
                })
                // Takes caps that change.
                .query_function(|pad, parent, query| match query.view_mut() {
                    gst::QueryViewMut::AcceptCaps(accept) => {
                        accept.set_result(true);
                        true
                    }
                    _ => gst::Pad::query_default(pad, parent, query),
                })
                .build();
            let pad = gst::Pad::builder(gst::PadDirection::Src).build();
            let stream = gst::GhostPad::builder_with_target(&pad).unwrap().build();
            if exposed {
                parsebin.add_pad(&stream).unwrap();
            }
            stream.link(&peer).unwrap();
            for pad in [&peer, stream.upcast_ref(), &pad] {
                pad.set_active(true).unwrap();
            }
            watch(
                &pad,
                Reporter {
                    parsebin: parsebin.upcast_ref::<gst::Element>().downgrade(),
                    bus: pipeline.bus().unwrap(),
                },
            );
            assert!(pad.push_event(gst::event::StreamStart::new("stream")));
            Self {
                pipeline,
                parsebin,
                pad,
                stream,
                _peer: peer,
                items,
            }
        }

        /// New caps at `rate`, which the watch looks after.
        fn push_caps(&self, rate: i32) {
            assert!(self.pad.push_event(gst::event::Caps::new(
                &gst::Caps::builder("audio/mpeg").field("rate", rate).build()
            )));
        }

        fn push_segment(&self) {
            assert!(self
                .pad
                .push_event(gst::event::Segment::new(&gst::FormattedSegment::<
                    gst::ClockTime,
                >::new())));
        }

        /// Push a buffer on another thread, which returns the result.
        fn push_on_thread(&self) -> JoinHandle<Result<gst::FlowSuccess, gst::FlowError>> {
            let pad = self.pad.clone();
            std::thread::spawn(move || pad.push(gst::Buffer::new()))
        }

        /// Block the data of the output, as `parsebin` does, and let it go
        /// in the probe if `lets_go`.
        fn block(&self, lets_go: bool) -> Option<gst::PadProbeId> {
            self.pad.add_probe(
                gst::PadProbeType::BLOCK | gst::PadProbeType::BUFFER,
                move |_, _| {
                    if lets_go {
                        gst::PadProbeReturn::Remove
                    } else {
                        gst::PadProbeReturn::Ok
                    }
                },
            )
        }

        /// Block the output as `parsebin` does until it can expose the
        /// stream: send sticky events on, and expose the stream and let the
        /// block go when data arrives.
        fn block_until_exposed(&self) {
            self.pad
                .add_probe(
                    gst::PadProbeType::BLOCK_DOWNSTREAM,
                    |pad, info| match &info.data {
                        Some(gst::PadProbeData::Event(event)) if event.is_sticky() => {
                            pad.peer().unwrap().send_event(event.clone());
                            gst::PadProbeReturn::Handled
                        }
                        _ => gst::PadProbeReturn::Remove,
                    },
                )
                .unwrap();
        }

        /// The report on the bus, if one is there.
        fn report(&self) -> Option<gst::Message> {
            // `timed_pop_filtered` with no time to wait pops nothing.
            self.pipeline
                .bus()
                .unwrap()
                .pop_filtered(&[gst::MessageType::Element])
                .filter(is_stall_report)
        }

        /// Wait for the report on the bus.
        fn wait_for_report(&self) -> gst::Message {
            self.pipeline
                .bus()
                .unwrap()
                .timed_pop_filtered(
                    gst::ClockTime::from_seconds(10),
                    &[gst::MessageType::Element],
                )
                .filter(is_stall_report)
                .expect("a report")
        }

        fn items(&self) -> Vec<Item> {
            self.items.lock().unwrap().clone()
        }
    }

    fn is_stall_report(message: &gst::Message) -> bool {
        matches!(message.view(), gst::MessageView::Element(element) if is_report(element))
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
    fn a_stream_blocked_after_it_was_exposed_is_reported() {
        // The parser's thread blocks the output again after `parsebin` has
        // exposed its stream and let it go.
        let output = Output::new(true);
        output.block(false);
        output.push_caps(48_000);
        output.push_segment();
        let buffer = output.push_on_thread();

        let report = output.wait_for_report();
        assert_eq!(
            report.src(),
            Some(output.parsebin.upcast_ref::<gst::Object>())
        );
        wait_until("the buffer did not wait", || output.pad.is_blocking());
        // The watch holds nothing: the events that the block lets pass have
        // passed.
        assert_eq!(output.items(), [Item::Caps, Item::Segment]);
        // Once.
        assert!(output.report().is_none());

        // The state change that ends the run flushes the buffer.
        output.pad.set_active(false).unwrap();
        assert_eq!(buffer.join().unwrap(), Err(gst::FlowError::Flushing));
    }

    #[test]
    fn a_stream_that_its_block_lets_go_is_not_reported() {
        // `parsebin` exposes the stream from its block and lets it go.
        let output = Output::new(true);
        output.block(true);
        output.push_caps(48_000);
        output.push_segment();
        assert_eq!(
            output.pad.push(gst::Buffer::new()),
            Ok(gst::FlowSuccess::Ok)
        );
        assert_eq!(output.items(), [Item::Caps, Item::Segment, Item::Buffer]);
        assert!(output.report().is_none());
        assert!(!output.pad.is_blocked());
    }

    #[test]
    fn a_stream_held_before_it_is_exposed_is_not_reported() {
        // `parsebin` holds a stream until it can expose it, for example
        // until the other streams of its group are there.
        let output = Output::new(false);
        let block = output.block(false).unwrap();
        output.push_caps(48_000);
        output.push_segment();
        let buffer = output.push_on_thread();
        // The watch has looked at the buffer before it waits.
        wait_until("the buffer did not wait", || output.pad.is_blocking());
        assert!(output.report().is_none());

        output.parsebin.add_pad(&output.stream).unwrap();
        output.pad.remove_probe(block);
        assert_eq!(buffer.join().unwrap(), Ok(gst::FlowSuccess::Ok));
        assert_eq!(output.items(), [Item::Caps, Item::Segment, Item::Buffer]);
        assert!(output.report().is_none());
    }

    #[test]
    fn a_pad_that_feeds_another_element_of_parsebin_is_not_reported() {
        // Such as a demuxer's pad that feeds a parser.
        let output = Output::new(true);
        let parser = gst::ElementFactory::make("identity").build().unwrap();
        output.parsebin.add(&parser).unwrap();
        parser.set_state(gst::State::Playing).unwrap();
        let pad = gst::Pad::builder(gst::PadDirection::Src).build();
        pad.link(&parser.static_pad("sink").unwrap()).unwrap();
        pad.set_active(true).unwrap();
        watch(
            &pad,
            Reporter {
                parsebin: output.parsebin.upcast_ref::<gst::Element>().downgrade(),
                bus: output.pipeline.bus().unwrap(),
            },
        );
        pad.add_probe(
            gst::PadProbeType::BLOCK | gst::PadProbeType::BUFFER,
            |_, _| gst::PadProbeReturn::Ok,
        )
        .unwrap();
        assert!(pad.push_event(gst::event::StreamStart::new("stream")));
        assert!(pad.push_event(gst::event::Caps::new(
            &gst::Caps::builder("audio/mpeg").build()
        )));
        let buffer = {
            let pad = pad.clone();
            std::thread::spawn(move || pad.push(gst::Buffer::new()))
        };
        wait_until("the buffer did not wait", || pad.is_blocking());
        assert!(output.report().is_none());
        pad.set_active(false).unwrap();
        assert_eq!(buffer.join().unwrap(), Err(gst::FlowError::Flushing));
        parser.set_state(gst::State::Null).unwrap();
    }

    #[test]
    fn a_block_added_after_the_watch_and_let_go_is_not_reported() {
        // Another thread that exposes a stream of the same group blocks the
        // exposed ones while it does, and lets them go before it is done.
        // Its block goes after the watch's probe, which looks at the first
        // buffer because the block before it sends the events on.
        let output = Output::new(true);
        output.block_until_exposed();
        output.push_caps(48_000);
        output.push_segment();
        output.block(true);
        assert_eq!(
            output.pad.push(gst::Buffer::new()),
            Ok(gst::FlowSuccess::Ok)
        );
        assert_eq!(output.items(), [Item::Caps, Item::Segment, Item::Buffer]);
        assert!(output.report().is_none());
    }

    #[test]
    fn the_watch_looks_again_after_new_caps() {
        let output = Output::new(true);
        output.push_caps(48_000);
        output.push_segment();
        assert_eq!(
            output.pad.push(gst::Buffer::new()),
            Ok(gst::FlowSuccess::Ok)
        );
        // A later block that new caps bring back is reported.
        output.block(false);
        output.push_caps(44_100);
        let buffer = output.push_on_thread();
        output.wait_for_report();
        output.pad.set_active(false).unwrap();
        assert_eq!(buffer.join().unwrap(), Err(gst::FlowError::Flushing));
    }
}
