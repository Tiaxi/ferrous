// SPDX-License-Identifier: GPL-3.0-or-later

use std::cell::Cell;
use std::path::Path;

use gst::prelude::*;
use gstreamer as gst;

use super::cache::WaveformSourceStamp;
use super::decoders::{
    raw_surround_duration_ns, start_gstreamer_decode, AudioRead, AudioTimeline, GstDecode,
    RateChanges,
};
use super::fft::{OverviewProgress, WaveformAccumulator};
use super::track_io::stamp_of;
use crate::playback::file_source::FileSource;
use crate::raw_audio::{
    audio_byte_range_of, conceal_damaged_ac3_frames, configure_raw_surround_decoding,
    filter_raw_surround_source_to,
};

/// The frames assumed for a file that tells no duration, which only sizes
/// the overview's bins: 240 s.
const UNKNOWN_DURATION_SECONDS: u64 = 240;

/// Start the overview decode of the file at `path` in `pipeline`, from the
/// playback's `ferrousfilesrc`, which tells the metadata of the file it
/// opened. The decode is that of the analysis, at the stream's own rate;
/// where that rate changes mid-stream, the audio is resampled to the rate
/// the stream started at.
///
/// The file is opened once, here: the audio range of a raw surround file is
/// read from that handle, and the source reads it, so that the range, the
/// audio and the stamp are all of one file, even if another one replaces
/// it at the path meanwhile.
fn start_overview_decode(
    path: &Path,
    pipeline: gst::Pipeline,
) -> anyhow::Result<(GstDecode, FileSource)> {
    let mut file = std::fs::File::open(path)
        .map_err(|err| anyhow::anyhow!("cannot open \"{}\": {err}", path.display()))?;
    let range = audio_byte_range_of(path, &mut file);
    let file_source = gst::glib::Object::builder::<FileSource>()
        .property("location", path.to_string_lossy().to_string())
        .build();
    file_source.read_opened(file);
    let src = file_source.clone().upcast::<gst::Element>();
    filter_raw_surround_source_to(&src, range);
    #[cfg(test)]
    test_hooks::source_built();
    // The peaks of a stream whose rate changes mid-stream stay on one time
    // line, and are exact where the rate is the one it started at.
    let decode = start_gstreamer_decode(path, pipeline, &src, RateChanges::ResampleToFirst)?;
    Ok((decode, file_source))
}

/// Decode the overview of the file at `path` with `GStreamer`. `source`
/// tells, for each update, the stamp of the file the decode reads, taken
/// from the handle its source opened, as another file may be at the path
/// before the source opens it, or by the time the decode ends.
///
/// The last update is [`OverviewProgress::Complete`] only if the decode
/// reached the end of the file. An error that stops it after it decoded
/// audio ends it with [`OverviewProgress::Truncated`] and the peaks of the
/// audio before the error.
pub(super) fn decode_waveform_peaks_stream_gst<F, C>(
    path: &Path,
    max_points: usize,
    source: &Cell<Option<WaveformSourceStamp>>,
    on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, OverviewProgress) -> bool,
    C: FnMut() -> bool,
{
    source.set(None);
    if is_cancelled() {
        return Ok(());
    }

    gst::init()?;
    configure_raw_surround_decoding();
    let pipeline = gst::Pipeline::new();
    conceal_damaged_ac3_frames(pipeline.upcast_ref());
    decode_overview_in(path, pipeline, max_points, source, on_update, is_cancelled)
}

/// [`decode_waveform_peaks_stream_gst`] in `pipeline`, which has what it
/// needs connected.
fn decode_overview_in<F, C>(
    path: &Path,
    pipeline: gst::Pipeline,
    max_points: usize,
    source: &Cell<Option<WaveformSourceStamp>>,
    on_update: F,
    is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, OverviewProgress) -> bool,
    C: FnMut() -> bool,
{
    let (decode, file_source) = start_overview_decode(path, pipeline)?;
    // The source opened the file when the pipeline started.
    source.set(file_source.opened_metadata().as_ref().and_then(stamp_of));
    read_overview(path, decode, max_points, on_update, is_cancelled)
}

/// Read the overview of the file at `path` from `decode`, its started
/// decode, and send its updates.
fn read_overview<F, C>(
    path: &Path,
    decode: GstDecode,
    max_points: usize,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, OverviewProgress) -> bool,
    C: FnMut() -> bool,
{
    let GstDecode {
        source: mut frames,
        sample_rate,
        duration_ns,
        ..
    } = decode;
    // A failure ends the overview with the audio decoded before it, also
    // the audio still queued when the pipeline reported it.
    frames.read_queued_audio_on_failure();
    // The duration only sizes the bins; the peaks and the seconds they
    // cover are those of the audio decoded.
    let estimated_frames = duration_ns
        .or_else(|| raw_surround_duration_ns(path))
        .map_or(sample_rate * UNKNOWN_DURATION_SECONDS, |ns| {
            ns.saturating_mul(sample_rate) / 1_000_000_000
        });
    let mut waveform = WaveformAccumulator::new(max_points, estimated_frames, sample_rate);
    // Missing audio counts as silence and overlaps once, so the peaks stay
    // at the times they play.
    let mut timeline = AudioTimeline::new(0);
    let mut decoded_any = false;
    let reached_end = loop {
        if is_cancelled() {
            return Ok(());
        }
        match timeline.read(|| frames.next_frames()) {
            Ok(AudioRead::Frames(audio)) => {
                decoded_any = true;
                let samples = &audio.samples[..audio.analysis_frames() * audio.channels];
                if !waveform.push_frames(samples, audio.channels, &mut on_update, &mut is_cancelled)
                {
                    return Ok(());
                }
            }
            Ok(AudioRead::Pending) => {}
            Ok(AudioRead::Eof) => break true,
            Err(_) if decoded_any => break false,
            Err(err) => return Err(err.context("gstreamer waveform decode failed")),
        }
    };
    drop(frames);

    if !decoded_any {
        anyhow::bail!("no audio could be decoded");
    }
    if !is_cancelled() {
        let progress = if reached_end {
            OverviewProgress::Complete
        } else {
            OverviewProgress::Truncated
        };
        let coverage_seconds = waveform.coverage_seconds();
        let _ = on_update(waveform.finish(), coverage_seconds, progress);
    }
    Ok(())
}

/// An overview update: its peaks, the seconds they cover and its progress.
#[cfg(test)]
pub(super) type OverviewUpdate = (Vec<f32>, f32, OverviewProgress);

/// The updates of the overview decode of `path` into 64 peaks, whose
/// pipeline fails once `buffers` buffers of audio have reached the decode,
/// and the seconds those buffers end at. With `before_first_read`, the
/// decode reads nothing before the failure: the audio waits in its queue.
#[cfg(test)]
pub(super) fn overview_failing_after(
    path: &Path,
    buffers: usize,
    before_first_read: bool,
) -> (Vec<OverviewUpdate>, f32) {
    use std::sync::atomic::{AtomicU64, AtomicUsize, Ordering};
    use std::sync::Arc;

    gst::init().expect("gstreamer initialization");
    configure_raw_surround_decoding();
    let pipeline = gst::Pipeline::new();
    conceal_damaged_ac3_frames(pipeline.upcast_ref());
    let end_ns = Arc::new(AtomicU64::new(0));
    let (failed_tx, failed_rx) = crossbeam_channel::bounded(1);
    let passed_end_ns = Arc::clone(&end_ns);
    pipeline.connect_element_added(move |_, element| {
        // The decode's sink, which its builder makes without a factory.
        if !element.is::<gstreamer_app::AppSink>() {
            return;
        }
        let Some(pad) = element.static_pad("sink") else {
            return;
        };
        let reached = AtomicUsize::new(0);
        let end_ns = Arc::clone(&passed_end_ns);
        let failed_tx = failed_tx.clone();
        pad.add_probe(gst::PadProbeType::BUFFER, move |pad, info| {
            let count = reached.fetch_add(1, Ordering::SeqCst) + 1;
            if count <= buffers {
                if let Some(buffer) = info.buffer() {
                    let end = buffer
                        .pts()
                        .zip(buffer.duration())
                        .map(|(pts, duration)| pts + duration);
                    end_ns.fetch_max(end.map_or(0, gst::ClockTime::nseconds), Ordering::SeqCst);
                }
                return gst::PadProbeReturn::Ok;
            }
            if count == buffers + 1 {
                // The buffers before this one are in the sink's queue.
                if let Some(element) = pad.parent_element() {
                    let error =
                        gst::message::Error::builder(gst::CoreError::Failed, "injected failure")
                            .src(&element)
                            .build();
                    let _ = element.post_message(error);
                }
                let _ = failed_tx.try_send(());
            }
            gst::PadProbeReturn::Drop
        });
    });

    let (decode, _) = start_overview_decode(path, pipeline).expect("start the decode");
    if before_first_read {
        failed_rx
            .recv_timeout(std::time::Duration::from_secs(10))
            .expect("the pipeline fails");
    }
    let mut updates = Vec::new();
    read_overview(
        path,
        decode,
        64,
        |peaks, coverage, progress| {
            updates.push((peaks, coverage, progress));
            true
        },
        || false,
    )
    .expect("an overview of the audio before the failure");
    let seconds = std::time::Duration::from_nanos(end_ns.load(Ordering::SeqCst)).as_secs_f32();
    (updates, seconds)
}

/// Swap files where a decode has read the audio range of its file but its
/// source has not started.
#[cfg(test)]
pub(super) mod test_hooks {
    use std::cell::RefCell;

    thread_local! {
        static SOURCE_BUILT: RefCell<Option<Box<dyn FnMut()>>> = const { RefCell::new(None) };
    }

    /// Run `hook` each time this thread has built an overview pipeline,
    /// before it starts.
    pub(in super::super) fn after_each_source_built(hook: impl FnMut() + 'static) {
        SOURCE_BUILT.with(|cell| *cell.borrow_mut() = Some(Box::new(hook)));
    }

    pub(in super::super) fn clear() {
        SOURCE_BUILT.with(|cell| *cell.borrow_mut() = None);
    }

    pub(super) fn source_built() {
        SOURCE_BUILT.with(|cell| {
            if let Some(hook) = cell.borrow_mut().as_mut() {
                hook();
            }
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The updates of the overview decode of `path` into `max_points`
    /// peaks: their peaks, the seconds they cover and their progress.
    fn overview_updates(path: &Path, max_points: usize) -> Vec<(Vec<f32>, f32, OverviewProgress)> {
        let mut updates = Vec::new();
        decode_waveform_peaks_stream_gst(
            path,
            max_points,
            &Cell::new(None),
            |peaks, coverage, progress| {
                updates.push((peaks, coverage, progress));
                true
            },
            || false,
        )
        .unwrap_or_else(|err| panic!("{}: {err}", path.display()));
        updates
    }

    /// The complete overview of `path` in `updates`, after checking that
    /// every preview before it covers only audio it decoded, more of it each
    /// time.
    fn complete_overview(updates: Vec<(Vec<f32>, f32, OverviewProgress)>) -> (Vec<f32>, f32) {
        let ((peaks, coverage, progress), previews) =
            updates.split_last().expect("an overview update");
        assert_eq!(*progress, OverviewProgress::Complete);
        let mut covered = 0.0;
        for (_, preview_coverage, preview_progress) in previews {
            assert_eq!(*preview_progress, OverviewProgress::Partial);
            assert!(
                covered < *preview_coverage && *preview_coverage <= *coverage,
                "a preview covers {preview_coverage} s after {covered} s, of {coverage} s"
            );
            covered = *preview_coverage;
        }
        (peaks.clone(), *coverage)
    }

    /// A WAV file of `samples`, interleaved in `channels` channels at
    /// `rate`, named `name` in the test folder.
    fn test_wave(name: &str, samples: &[i16], rate: u32, channels: u16) -> std::path::PathBuf {
        let written =
            super::super::waveform_window::tests::write_test_wave(samples, rate, channels);
        let path = crate::paths::test_root().join(name);
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        std::fs::rename(&written, &path).expect("move fixture");
        path
    }

    #[test]
    fn a_failed_overview_start_releases_the_started_pipeline() {
        gst::init().expect("gstreamer initialization");
        // A directory opens, but its source cannot read it.
        let folder = crate::paths::test_root().join("waveform-startup");
        std::fs::create_dir_all(&folder).expect("folder");
        let pipeline = gst::Pipeline::new();
        assert!(start_overview_decode(&folder, pipeline.clone()).is_err());
        assert_eq!(pipeline.current_state(), gst::State::Null);
    }

    #[test]
    fn ripped_surround_files_produce_complete_waveforms() {
        use crate::raw_audio::{ripped_surround_bytes, TEST_AC3, TEST_DTS};

        // A leading partial frame and an appended APEv2 tag.
        for (stream, ext, length) in [(TEST_AC3, "AC3", 1.024), (TEST_DTS, "dts", 0.501)] {
            let path = std::env::temp_dir().join(format!(
                "ferrous-waveform-ripped-{}.{ext}",
                std::process::id()
            ));
            std::fs::write(&path, ripped_surround_bytes(stream).0).expect("write fixture");
            let updates = overview_updates(&path, 64);
            std::fs::remove_file(&path).expect("remove fixture");

            let (peaks, coverage) = complete_overview(updates);
            assert!(
                (coverage - length).abs() < 0.05,
                "{ext}: covers {coverage} s"
            );
            // The fixture tone peaks at 1/8 of full scale.
            let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
            assert!((0.1..0.2).contains(&loudest), "{ext}: peak {loudest}");
        }
    }

    #[test]
    fn a_dts_cd_rip_gets_the_overview_of_its_dts() {
        use crate::raw_audio::{ripped_dts_cd_bytes, TEST_DTS_44K};

        let path = crate::paths::test_root().join("waveform-dts-cd.dts");
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        std::fs::write(&path, ripped_dts_cd_bytes(TEST_DTS_44K).0).expect("write fixture");
        let updates = overview_updates(&path, 64);
        std::fs::remove_file(&path).expect("remove fixture");

        let (peaks, coverage) = complete_overview(updates);
        assert!((coverage - 0.511).abs() < 0.05, "covers {coverage} s");
        // The tone peaks at 1/8 of full scale; the 14-bit words, read as
        // PCM, at 1/4.
        let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
        assert!((0.1..0.2).contains(&loudest), "peak {loudest}");
    }

    #[test]
    fn the_overview_decodes_no_damaged_ac3_frame() {
        use crate::raw_audio::{
            ac3_frame_is_damaged, record_decoder_input, with_damaged_frames, SEAMED_FRAME,
            TEST_AC3, TEST_AC3_FRAMES,
        };

        gst::init().expect("gst init");
        configure_raw_surround_decoding();
        let path = std::env::temp_dir().join(format!(
            "ferrous-waveform-damaged-{}.ac3",
            std::process::id()
        ));
        let stream = with_damaged_frames(TEST_AC3, &[SEAMED_FRAME]);
        std::fs::write(&path, stream).expect("write fixture");
        let pipeline = gst::Pipeline::new();
        conceal_damaged_ac3_frames(pipeline.upcast_ref());
        let frames = record_decoder_input(pipeline.upcast_ref());
        let mut complete = false;
        let result = decode_overview_in(
            &path,
            pipeline,
            64,
            &Cell::new(None),
            |_, _, progress| {
                complete |= progress == OverviewProgress::Complete;
                true
            },
            || false,
        );
        std::fs::remove_file(&path).expect("remove fixture");
        result.expect("decode");
        assert!(complete, "the overview reaches the end of the stream");

        let frames = frames.lock().expect("frames");
        assert_eq!(frames.len(), TEST_AC3_FRAMES);
        for frame in frames.iter() {
            assert!(!ac3_frame_is_damaged(&frame.bytes));
        }
    }

    /// The overview has the peak of every sample the decoder gave, at the
    /// stream's own rate: no resampling rounds a transient off or overshoots
    /// it, and the peaks are those of the Symphonia overview of the same
    /// audio.
    #[test]
    fn the_overview_has_the_exact_peaks_of_the_decoded_samples() {
        // 1.5 s of 48 kHz 5.1 silence, with a one-sample transient in a
        // different channel at each of a few offsets, and a quiet tone in one.
        let rate = 48_000_u32;
        let channels = 6_usize;
        let frames = 1_024 * 70 + 512;
        let mut samples = vec![0_i16; frames * channels];
        for frame in 0..frames {
            let t = f64::from(u32::try_from(frame).expect("frame")) / f64::from(rate);
            samples[frame * channels + 1] =
                ((t * 997.0 * std::f64::consts::TAU).sin() * 3_000.0) as i16;
        }
        for (index, frame) in [1_usize, 4_099, 30_001, 71_999].into_iter().enumerate() {
            samples[frame * channels + index % channels] =
                30_000 - i16::try_from(index).expect("index") * 1_000;
        }
        let path = test_wave("waveform-exact-peaks.wav", &samples, rate, 6);

        let (peaks, coverage) = complete_overview(overview_updates(&path, 1_024));
        let mut symphonia = None;
        super::super::decode_waveform_peaks_stream(
            &path,
            1_024,
            &Cell::new(None),
            |peaks, _, progress| {
                if progress == OverviewProgress::Complete {
                    symphonia = Some(peaks);
                }
                true
            },
            || false,
        )
        .expect("Symphonia overview");
        std::fs::remove_file(&path).expect("remove fixture");

        let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
        assert_eq!(loudest, 30_000.0 / 32_768.0);
        let expected_seconds = frames as f32 / rate as f32;
        assert!(
            (coverage - expected_seconds).abs() < 1e-4,
            "covers {coverage} s"
        );
        assert_eq!(Some(peaks), symphonia);
    }

    #[test]
    fn a_cancelled_overview_stops_without_a_final_update() {
        let path = test_wave(
            "waveform-cancelled.wav",
            &vec![8_000_i16; 48_000 * 4],
            48_000,
            2,
        );
        let mut updates = Vec::new();
        let cancelled = Cell::new(false);
        let result = decode_waveform_peaks_stream_gst(
            &path,
            1_024,
            &Cell::new(None),
            |_, _, progress| {
                assert!(!cancelled.get(), "an update after the cancel");
                updates.push(progress);
                // The first preview cancels the decode.
                cancelled.set(true);
                true
            },
            || cancelled.get(),
        );
        std::fs::remove_file(&path).expect("remove fixture");
        result.expect("a cancelled decode is no error");
        assert_eq!(updates, [OverviewProgress::Partial]);
    }

    /// A stream whose sample rate changes mid-stream, as a DTS 96/24
    /// stream's does where its decoder falls back to the 48 kHz core of a
    /// damaged frame, gets a complete overview of all of it, at the times
    /// its audio plays.
    #[test]
    fn a_stream_whose_rate_changes_mid_stream_gets_a_complete_overview() {
        use crate::raw_audio::{TEST_DTS, TEST_DTS_44K};

        let path = crate::paths::test_root().join("waveform-rate-change.dts");
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        // 0.50 s at 48 kHz, then 0.51 s at 44.1 kHz, of which the parser
        // reads only part, and the other way round.
        for (name, stream, first_part) in [
            ("48 kHz first", [TEST_DTS, TEST_DTS_44K].concat(), 0.501),
            ("44.1 kHz first", [TEST_DTS_44K, TEST_DTS].concat(), 0.511),
        ] {
            std::fs::write(&path, stream).expect("write fixture");
            let (peaks, coverage) = complete_overview(overview_updates(&path, 64));
            assert!(
                coverage > first_part + 0.1 && coverage < 1.05,
                "{name}: covers {coverage} s"
            );
            // The tone peaks at 1/8 of full scale in both parts.
            let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
            assert!((0.1..0.2).contains(&loudest), "{name}: peak {loudest}");
        }
        std::fs::remove_file(&path).expect("remove fixture");
    }

    /// A failing pipeline still gives the overview of the audio it decoded
    /// before: also what its decoder queued, unread, when it reported the
    /// failure, whether the decode read none of its audio yet or much.
    #[test]
    fn a_failed_overview_keeps_the_audio_queued_before_the_failure() {
        use crate::raw_audio::TEST_DTS;

        let path = crate::paths::test_root().join("waveform-failing.dts");
        std::fs::create_dir_all(path.parent().expect("folder")).expect("folder");
        std::fs::write(&path, TEST_DTS).expect("write fixture");
        // More buffers than the sink queues: the decode has read some.
        for (buffers, before_first_read) in [(5, true), (20, false)] {
            let (updates, decoded) = overview_failing_after(&path, buffers, before_first_read);
            let (peaks, coverage, progress) = updates.last().expect("a last update");
            assert_eq!(*progress, OverviewProgress::Truncated, "{buffers} buffers");
            assert!(decoded > 0.0);
            assert!(
                (coverage - decoded).abs() < 1e-3,
                "{buffers} buffers: covers {coverage} s of {decoded} s decoded"
            );
            // The fixture tone peaks at 1/8 of full scale.
            let loudest = peaks.iter().copied().fold(0.0_f32, f32::max);
            assert!((0.1..0.2).contains(&loudest), "peak {loudest}");
        }
        std::fs::remove_file(&path).expect("remove fixture");
    }
}
