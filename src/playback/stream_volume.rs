// SPDX-License-Identifier: GPL-3.0-or-later

//! Following the volume of the output's stream, which other programs can
//! change too.
//!
//! An output that controls the volume of its stream, as pulsesink does,
//! gets the engine's volume from playsink, and the sound server applies it
//! as the stream plays. Plasma's volume applet and the task manager's
//! per-app slider set that same stream volume and mute. The sink tells of
//! a change with `notify::volume` and `notify::mute`: pulsesink from its
//! streaming thread, as it plays, once the server has told it of any change
//! of the stream, the engine's own included, and every sink also from the
//! thread that sets the property.
//!
//! The engine writes the stream volume itself: the volume the user picked,
//! its fades, and the silence of a new run. To tell those writes from a
//! change made elsewhere, it numbers them. The server applies them in
//! order, but some time after it has taken them: pipewire-pulse reads back
//! a stream's earlier volume for a while. So a value the stream shows is
//! the engine's own if it is one of its writes from the latest the stream
//! has shown on; a value that none of them explains was set by someone
//! else. A value read before the engine's latest write began tells nothing:
//! that write is newer on the server, and a change made after it is
//! notified again.

use std::cell::Cell;
use std::collections::VecDeque;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, PoisonError};

use crossbeam_channel::{Receiver, Sender};
use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_audio::{StreamVolume, StreamVolumeFormat};

thread_local! {
    /// Whether this thread is writing the stream volume for the engine.
    /// A sink notifies a property that is set from the thread that sets
    /// it, so a notification on such a thread is the engine's own write.
    static WRITING: Cell<bool> = const { Cell::new(false) };
    /// Whether this thread is reading a sink's stream volume to report it.
    static READING: Cell<bool> = const { Cell::new(false) };
}

/// What a sink that controls the volume of its stream read when it told of
/// a change.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct StreamVolumeReport {
    /// The linear gain of the stream.
    pub(super) gain: f64,
    pub(super) muted: bool,
    /// The engine's writes that had begun when the read had returned: the
    /// value shows at most these.
    pub(super) writes_begun: u64,
}

/// What the engine makes of a [`StreamVolumeReport`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) enum StreamVolumeChange {
    /// The stream is as the engine set it, now or a little earlier.
    Unchanged,
    /// Something other than the engine set the stream to this gain and
    /// mute.
    Outside { gain: f64, muted: bool },
}

/// The stream as the engine left it after one of its writes.
#[derive(Debug, Clone, Copy)]
struct WrittenState {
    /// The number of the write; 0 is the state the engine started from.
    write: u64,
    gain: f64,
    muted: bool,
}

/// The most writes the stream has not shown yet that are kept. The stream
/// shows them as it plays, so they pile up only while it does not, such as
/// for volume changes while paused, and then only the latest ones matter.
const WRITES_KEPT: usize = 256;

/// How close, on the cubic scale, a gain read back from the stream must be
/// to a gain the engine wrote to be that write. The sound server keeps the
/// volume as a 16-bit fraction on that scale, so a write reads back within
/// half of its 1/65536 step; a step of a volume slider is 100 times more.
const SAME_LEVEL: f64 = 1e-4;

/// The engine's side of following the stream volume, shared with the
/// notification handlers of the output's sinks.
pub(super) struct StreamVolumeWatch {
    /// The engine's writes that have begun.
    begun: AtomicU64,
    /// The latest write the stream has shown, then the writes after it,
    /// newest last. Only the engine's thread uses it; the lock lets the
    /// writes stay `&self`.
    written: Mutex<VecDeque<WrittenState>>,
    /// Held while a sink's stream is read and reported, so that the
    /// reports arrive in the order of their reads, also when sinks notify
    /// from more than one thread: a value read before another is never
    /// taken in after it.
    reading: Mutex<()>,
    reports: Sender<StreamVolumeReport>,
}

impl StreamVolumeWatch {
    /// A watch of a stream that the engine has set to `gain`, unmuted, and
    /// the receiver of its reports.
    pub(super) fn new(gain: f64) -> (Arc<Self>, Receiver<StreamVolumeReport>) {
        let (reports, received) = crossbeam_channel::unbounded();
        let watch = Arc::new(Self {
            begun: AtomicU64::new(0),
            written: Mutex::new(VecDeque::from([WrittenState {
                write: 0,
                gain,
                muted: false,
            }])),
            reading: Mutex::new(()),
            reports,
        });
        (watch, received)
    }

    /// Follow the sinks inside `output` that control the volume of their
    /// stream, those it has and those it makes later, as autoaudiosink
    /// does for each run. These are the sinks playsink gives the volume to.
    pub(super) fn watch_output(self: &Arc<Self>, output: &gst::Element) {
        if let Some(bin) = output.downcast_ref::<gst::Bin>() {
            for element in bin.iterate_recurse().into_iter().flatten() {
                self.watch_sink(&element);
            }
            let watch = Arc::downgrade(self);
            bin.connect_deep_element_added(move |_, _, element| {
                if let Some(watch) = watch.upgrade() {
                    watch.watch_sink(element);
                }
            });
        } else {
            self.watch_sink(output);
        }
    }

    /// Report the volume and mute of `element` whenever it tells of a
    /// change, if it controls the volume of its stream.
    fn watch_sink(self: &Arc<Self>, element: &gst::Element) {
        if !element.is::<StreamVolume>() {
            return;
        }
        for property in ["notify::volume", "notify::mute"] {
            let watch = Arc::downgrade(self);
            element.connect(property, false, move |values| {
                let sink = values.first()?.get::<gst::Element>().ok()?;
                if let Some(watch) = watch.upgrade() {
                    watch.report(&sink);
                }
                None
            });
        }
    }

    /// Read the volume and mute of `sink` and report them, unless the
    /// engine is setting them on this thread, or this thread reads them
    /// already. A sink below Paused has not opened its stream, so nothing
    /// else can have changed it: it is notified as playsink sets it up for
    /// a run, giving it the volume, then the mute, the engine set. Runs on
    /// the thread that notifies, such as pulsesink's streaming thread,
    /// whose read asks the sound server; it waits only for another such
    /// read, never for the engine.
    fn report(&self, sink: &gst::Element) {
        if WRITING.with(Cell::get)
            || READING.with(Cell::get)
            || sink.current_state() < gst::State::Paused
        {
            return;
        }
        let _reading = self.reading.lock().unwrap_or_else(PoisonError::into_inner);
        READING.with(|reading| reading.set(true));
        let gain = sink.property::<f64>("volume");
        let muted = sink.property::<bool>("mute");
        let writes_begun = self.begun.load(Ordering::SeqCst);
        READING.with(|reading| reading.set(false));
        let _ = self.reports.send(StreamVolumeReport {
            gain,
            muted,
            writes_begun,
        });
    }

    /// Write the stream volume with `write`, which leaves the stream at
    /// `gain` and `muted`. The sink's notifications of the write on this
    /// thread are not reported.
    pub(super) fn write(&self, gain: f64, muted: bool, write: impl FnOnce()) {
        let number = self.begun.fetch_add(1, Ordering::SeqCst) + 1;
        WRITING.with(|writing| writing.set(true));
        write();
        WRITING.with(|writing| writing.set(false));
        if let Ok(mut written) = self.written.lock() {
            if written.len() == WRITES_KEPT {
                written.pop_front();
            }
            written.push_back(WrittenState {
                write: number,
                gain,
                muted,
            });
        }
    }

    /// What `report` tells of the stream. A write the stream shows, and the
    /// writes after it, are all it can show of the engine's from then on.
    /// Called on the engine's thread, which writes, so no write is under
    /// way.
    pub(super) fn classify(&self, report: StreamVolumeReport) -> StreamVolumeChange {
        let Ok(mut written) = self.written.lock() else {
            return StreamVolumeChange::Unchanged;
        };
        // Read before the engine's latest write began, which is newer on
        // the server.
        if written
            .back()
            .is_none_or(|latest| report.writes_begun != latest.write)
        {
            return StreamVolumeChange::Unchanged;
        }
        let level = gain_to_level(report.gain);
        let shown = written.iter().position(|state| {
            state.muted == report.muted && (gain_to_level(state.gain) - level).abs() < SAME_LEVEL
        });
        if let Some(shown) = shown {
            written.drain(..shown);
            StreamVolumeChange::Unchanged
        } else {
            StreamVolumeChange::Outside {
                gain: report.gain,
                muted: report.muted,
            }
        }
    }

    /// Take the stream's `gain` and `muted`, set elsewhere, as the state
    /// the engine left it in: the latest the stream shows, so that later
    /// reports of it are no change.
    pub(super) fn adopt(&self, gain: f64, muted: bool) {
        if let Ok(mut written) = self.written.lock() {
            let latest = written.len().saturating_sub(1);
            written.drain(..latest);
            if let Some(latest) = written.back_mut() {
                latest.gain = gain;
                latest.muted = muted;
            }
        }
    }
}

/// The volume level, as the slider shows it, of a stream's linear gain:
/// `GStreamer`'s cubic stream volume, which is also the percentage the
/// sound server and Plasma show. Not clamped.
pub(super) fn gain_to_level(gain: f64) -> f64 {
    StreamVolume::convert_volume(
        StreamVolumeFormat::Linear,
        StreamVolumeFormat::Cubic,
        gain.max(0.0),
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn report(gain: f64, muted: bool, writes_begun: u64) -> StreamVolumeReport {
        StreamVolumeReport {
            gain,
            muted,
            writes_begun,
        }
    }

    #[test]
    fn a_cubic_level_survives_the_linear_stream_volume_exactly() {
        gst::init().unwrap();
        // 66 % on Ferrous's slider, as the engine sets it on the stream.
        let gain = f64::from(crate::playback::volume_level_to_gain(0.66));
        #[allow(clippy::cast_possible_truncation)]
        let level = gain_to_level(gain) as f32;
        assert_eq!(level, 0.66_f32);
        // 66 % set by Plasma: the sound server's cubic volume, which
        // pulsesink reads as linear.
        let plasma = StreamVolume::convert_volume(
            StreamVolumeFormat::Cubic,
            StreamVolumeFormat::Linear,
            0.66,
        );
        #[allow(clippy::cast_possible_truncation)]
        let level = gain_to_level(plasma) as f32;
        assert_eq!(level, 0.66_f32);
        assert_eq!((gain_to_level(plasma) * 100.0).round(), 66.0);
    }

    #[test]
    fn the_engines_own_writes_are_no_change() {
        gst::init().unwrap();
        let (watch, _reports) = StreamVolumeWatch::new(1.0);
        assert_eq!(
            watch.classify(report(1.0, false, 0)),
            StreamVolumeChange::Unchanged
        );
        // A fade: the stream shows each step, a little off as the server's
        // volume steps are, and late, as the server applies the steps after
        // it has taken them.
        for gain in [0.5_f64, 0.25, 0.125] {
            watch.write(gain, false, || {});
        }
        for gain in [1.0, 0.5 * 1.000_01, 0.25, 0.125] {
            assert_eq!(
                watch.classify(report(gain, false, 3)),
                StreamVolumeChange::Unchanged,
                "{gain}"
            );
        }
        // A read before the latest write began tells nothing, whatever it
        // read.
        assert_eq!(
            watch.classify(report(0.9, false, 2)),
            StreamVolumeChange::Unchanged
        );
    }

    #[test]
    fn a_change_made_elsewhere_is_told_once() {
        gst::init().unwrap();
        let (watch, _reports) = StreamVolumeWatch::new(1.0);
        watch.write(0.5, false, || {});
        let change = watch.classify(report(0.2, false, 1));
        assert_eq!(
            change,
            StreamVolumeChange::Outside {
                gain: 0.2,
                muted: false
            }
        );
        watch.adopt(0.2, false);
        assert_eq!(
            watch.classify(report(0.2, false, 1)),
            StreamVolumeChange::Unchanged
        );
        // A mute set elsewhere is a change too, at the same volume.
        assert_eq!(
            watch.classify(report(0.2, true, 1)),
            StreamVolumeChange::Outside {
                gain: 0.2,
                muted: true
            }
        );
        watch.adopt(0.2, true);
        // So is a write of the engine's that the stream has gone past: the
        // server applies the writes in order, so someone set it back.
        watch.write(0.3, true, || {});
        watch.write(0.4, true, || {});
        assert_eq!(
            watch.classify(report(0.4, true, 3)),
            StreamVolumeChange::Unchanged
        );
        assert_eq!(
            watch.classify(report(0.3, true, 3)),
            StreamVolumeChange::Outside {
                gain: 0.3,
                muted: true
            }
        );
    }

    #[test]
    fn notifications_of_the_engines_own_writes_on_its_thread_are_not_read() {
        gst::init().unwrap();
        let (watch, reports) = StreamVolumeWatch::new(1.0);
        let source = gst::ElementFactory::make("audiotestsrc").build().unwrap();
        let sink = gst::ElementFactory::make("fakeaudiosink")
            .build()
            .expect("fakeaudiosink, from gst-plugins-bad");
        let output = gst::Bin::new();
        watch.watch_output(output.upcast_ref());
        let pipeline = gst::Pipeline::new();
        pipeline.add_many([&source, output.upcast_ref()]).unwrap();
        output.add(&sink).unwrap();
        let pad = sink.static_pad("sink").unwrap();
        output
            .add_pad(&gst::GhostPad::with_target(&pad).unwrap())
            .unwrap();
        source.link(&output).unwrap();
        // Set up for a run before it has a stream: nothing else can have
        // changed it.
        std::thread::scope(|scope| {
            scope.spawn(|| sink.set_property("volume", 0.25_f64));
        });
        assert!(reports.try_recv().is_err());
        pipeline.set_state(gst::State::Paused).unwrap();
        let (changed, _, _) = pipeline.state(gst::ClockTime::from_seconds(10));
        changed.unwrap();
        watch.write(0.5, false, || sink.set_property("volume", 0.5_f64));
        assert!(reports.try_recv().is_err());
        // Set elsewhere: reported with the writes it may show.
        std::thread::scope(|scope| {
            scope.spawn(|| sink.set_property("mute", true));
        });
        assert_eq!(reports.try_recv().unwrap(), report(0.5, true, 1));
        pipeline.set_state(gst::State::Null).unwrap();
    }
}
