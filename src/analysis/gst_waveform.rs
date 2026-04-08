// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::Path;
use std::time::Duration;

use gst::prelude::*;
use gstreamer as gst;

use super::cache::usize_to_u64;
use super::fft::reduce_waveform_peaks;

pub(super) struct GstWaveformAccumulator {
    observed_span_ns: u64,
    peak_events: Vec<(u64, f32)>,
    fallback_peaks: Vec<f32>,
    level_messages_seen: usize,
    last_preview_emit: std::time::Instant,
    max_points: usize,
}

impl GstWaveformAccumulator {
    pub(super) fn new(max_points: usize, duration_ns: Option<u64>) -> Self {
        Self {
            observed_span_ns: duration_ns.unwrap_or(0),
            peak_events: Vec::with_capacity(max_points.saturating_mul(2)),
            fallback_peaks: Vec::with_capacity(max_points),
            level_messages_seen: 0,
            last_preview_emit: std::time::Instant::now(),
            max_points,
        }
    }

    pub(super) fn record_peak(&mut self, structure: &gst::StructureRef, peak: f32) {
        self.level_messages_seen = self.level_messages_seen.saturating_add(1);
        if let Some((time_ns, end_ns)) = level_message_time_range_ns(structure) {
            self.observed_span_ns = self.observed_span_ns.max(end_ns);
            self.peak_events.push((time_ns, peak));
            return;
        }

        self.fallback_peaks.push(peak);
        if self.fallback_peaks.len() > self.max_points {
            self.fallback_peaks = reduce_waveform_peaks(&self.fallback_peaks, self.max_points);
        }
    }

    pub(super) fn preview_ready(&self) -> bool {
        self.level_messages_seen >= 12
            && self.last_preview_emit.elapsed() >= Duration::from_millis(240)
    }

    pub(super) fn take_preview(&mut self) -> Vec<f32> {
        self.last_preview_emit = std::time::Instant::now();
        if self.observed_span_ns > 0 && !self.peak_events.is_empty() {
            return materialize_waveform_peaks(
                &self.peak_events,
                self.observed_span_ns,
                self.max_points,
            );
        }
        self.fallback_peaks.clone()
    }

    pub(super) fn coverage_seconds(&self) -> f32 {
        Duration::from_nanos(self.observed_span_ns).as_secs_f32()
    }

    pub(super) fn finish(self) -> Vec<f32> {
        if self.observed_span_ns > 0 && !self.peak_events.is_empty() {
            return materialize_waveform_peaks(
                &self.peak_events,
                self.observed_span_ns,
                self.max_points,
            );
        }
        if self.fallback_peaks.len() > self.max_points {
            return reduce_waveform_peaks(&self.fallback_peaks, self.max_points);
        }
        self.fallback_peaks
    }
}

pub(super) fn build_waveform_gst_pipeline(
    path: &Path,
) -> anyhow::Result<(gst::Pipeline, gst::Bus, gst::Element)> {
    let pipeline = gst::Pipeline::new();
    let src = gst::ElementFactory::make("filesrc")
        .build()
        .map_err(|_| anyhow::anyhow!("missing filesrc element"))?;
    src.set_property("location", path.to_string_lossy().to_string());

    let decodebin = gst::ElementFactory::make("decodebin")
        .build()
        .map_err(|_| anyhow::anyhow!("missing decodebin element"))?;
    let conv = gst::ElementFactory::make("audioconvert")
        .build()
        .map_err(|_| anyhow::anyhow!("missing audioconvert element"))?;
    let resample = gst::ElementFactory::make("audioresample")
        .build()
        .map_err(|_| anyhow::anyhow!("missing audioresample element"))?;
    let capsfilter = gst::ElementFactory::make("capsfilter")
        .build()
        .map_err(|_| anyhow::anyhow!("missing capsfilter element"))?;
    let caps = gst::Caps::builder("audio/x-raw")
        .field("format", "F32LE")
        .field("layout", "interleaved")
        .field("rate", 44_100i32)
        .build();
    capsfilter.set_property("caps", &caps);
    let level = gst::ElementFactory::make("level")
        .build()
        .map_err(|_| anyhow::anyhow!("missing level element"))?;
    let fakesink = gst::ElementFactory::make("fakesink")
        .build()
        .map_err(|_| anyhow::anyhow!("missing fakesink element"))?;
    fakesink.set_property("sync", false);

    pipeline.add_many([
        &src,
        &decodebin,
        &conv,
        &resample,
        &capsfilter,
        &level,
        &fakesink,
    ])?;
    src.link(&decodebin)?;
    gst::Element::link_many([&conv, &resample, &capsfilter, &level, &fakesink])?;

    let conv_sink_pad = conv
        .static_pad("sink")
        .ok_or_else(|| anyhow::anyhow!("missing audioconvert sink pad"))?;
    decodebin.connect_pad_added(move |_dbin, src_pad| {
        if conv_sink_pad.is_linked() {
            return;
        }
        let Some(caps) = src_pad
            .current_caps()
            .or_else(|| Some(src_pad.query_caps(None)))
        else {
            return;
        };
        let Some(structure) = caps.structure(0) else {
            return;
        };
        if !structure.name().starts_with("audio/") {
            return;
        }
        let _ = src_pad.link(&conv_sink_pad);
    });

    let bus = pipeline
        .bus()
        .ok_or_else(|| anyhow::anyhow!("waveform pipeline has no bus"))?;
    Ok((pipeline, bus, level))
}

pub(super) fn configure_waveform_gst_pipeline(
    pipeline: &gst::Pipeline,
    level: &gst::Element,
    max_points: usize,
) -> anyhow::Result<Option<u64>> {
    pipeline.set_state(gst::State::Paused)?;
    let _ = pipeline.state(gst::ClockTime::from_seconds(2));

    let duration_ns = pipeline
        .query_duration::<gst::ClockTime>()
        .map(gst::ClockTime::nseconds);
    level.set_property(
        "interval",
        level_message_interval_ns(max_points, duration_ns),
    );
    level.set_property("post-messages", true);
    pipeline.set_state(gst::State::Playing)?;
    Ok(duration_ns)
}

pub(super) fn decode_waveform_peaks_stream_gst<F, C>(
    path: &Path,
    max_points: usize,
    mut on_update: F,
    mut is_cancelled: C,
) -> anyhow::Result<()>
where
    F: FnMut(Vec<f32>, f32, bool) -> bool,
    C: FnMut() -> bool,
{
    if is_cancelled() {
        return Ok(());
    }

    gst::init()?;
    let (pipeline, bus, level) = build_waveform_gst_pipeline(path)?;
    let duration_ns = configure_waveform_gst_pipeline(&pipeline, &level, max_points)?;
    let mut waveform = GstWaveformAccumulator::new(max_points, duration_ns);
    loop {
        if is_cancelled() {
            let _ = pipeline.set_state(gst::State::Null);
            return Ok(());
        }

        if let Some(msg) = bus.timed_pop_filtered(
            gst::ClockTime::from_mseconds(50),
            &[
                gst::MessageType::Element,
                gst::MessageType::Eos,
                gst::MessageType::Error,
            ],
        ) {
            match msg.view() {
                gst::MessageView::Element(element) => {
                    if let Some(structure) = element.message().structure() {
                        if let Some(peak) = level_message_peak(structure) {
                            waveform.record_peak(structure, peak);
                        }
                        if waveform.preview_ready()
                            && !on_update(
                                waveform.take_preview(),
                                waveform.coverage_seconds(),
                                false,
                            )
                        {
                            let _ = pipeline.set_state(gst::State::Null);
                            return Ok(());
                        }
                    }
                }
                gst::MessageView::Eos(..) => break,
                gst::MessageView::Error(err) => {
                    let _ = pipeline.set_state(gst::State::Null);
                    return Err(anyhow::anyhow!(
                        "gstreamer waveform decode failed: {} ({:?})",
                        err.error(),
                        err.debug()
                    ));
                }
                _ => {}
            }
        }
    }

    let coverage_seconds = waveform.coverage_seconds();
    let peaks = waveform.finish();

    let _ = pipeline.set_state(gst::State::Null);

    if !is_cancelled() {
        let _ = on_update(peaks, coverage_seconds, true);
    }
    Ok(())
}

fn level_message_interval_ns(max_points: usize, duration_ns: Option<u64>) -> u64 {
    let fallback_duration_ns = 240u64 * 1_000_000_000;
    (duration_ns.unwrap_or(fallback_duration_ns) / usize_to_u64(max_points.max(1)))
        .clamp(20_000_000, 500_000_000)
}

fn level_message_bin_index(time_ns: u64, duration_ns: u64, max_points: usize) -> Option<usize> {
    if duration_ns == 0 || max_points == 0 {
        return None;
    }

    let max_points_u64 = usize_to_u64(max_points);
    let raw_index = (time_ns.saturating_mul(max_points_u64) / duration_ns)
        .min(max_points_u64.saturating_sub(1));
    usize::try_from(raw_index).ok()
}

fn level_message_time_range_ns(structure: &gst::StructureRef) -> Option<(u64, u64)> {
    let start = structure
        .get::<u64>("running-time")
        .ok()
        .or_else(|| structure.get::<u64>("stream-time").ok())
        .or_else(|| structure.get::<u64>("timestamp").ok())
        .or_else(|| {
            let end = structure.get::<u64>("endtime").ok()?;
            let duration = structure.get::<u64>("duration").ok().unwrap_or(0);
            Some(end.saturating_sub(duration))
        })?;
    let duration = structure.get::<u64>("duration").ok().unwrap_or(0);
    let end = structure
        .get::<u64>("endtime")
        .ok()
        .unwrap_or_else(|| start.saturating_add(duration));
    let center = start.saturating_add(duration / 2);
    Some((center, end.max(center)))
}

fn level_message_peak(structure: &gst::StructureRef) -> Option<f32> {
    if structure.name() != "level" {
        return None;
    }

    let peaks = structure.value("peak").ok()?;
    collapse_level_peak_value(peaks)
}

fn collapse_level_db_peaks(values: &[gst::glib::SendValue]) -> Option<f32> {
    let mut peak = 0.0f32;
    let mut seen_any = false;

    for value in values {
        let db = level_db_value(value)?;
        let linear = dbfs_peak_to_linear(db);
        if linear > peak {
            peak = linear;
        }
        seen_any = true;
    }

    seen_any.then_some(peak)
}

fn collapse_level_peak_value(value: &gst::glib::SendValue) -> Option<f32> {
    if let Ok(peaks) = value.get::<gst::Array>() {
        return collapse_level_db_peaks(peaks.as_slice());
    }
    if let Ok(peaks) = value.get::<gst::List>() {
        return collapse_level_db_peaks(peaks.as_slice());
    }
    if let Ok(peaks) = value.get::<gst::glib::ValueArray>() {
        return collapse_level_db_values(peaks.as_slice());
    }
    level_db_value(value).map(dbfs_peak_to_linear)
}

fn collapse_level_db_values(values: &[gst::glib::Value]) -> Option<f32> {
    let mut peak = 0.0f32;
    let mut seen_any = false;

    for value in values {
        let db = value
            .get::<f64>()
            .ok()
            .or_else(|| value.get::<f32>().ok().map(f64::from))?;
        let linear = dbfs_peak_to_linear(db);
        if linear > peak {
            peak = linear;
        }
        seen_any = true;
    }

    seen_any.then_some(peak)
}

fn level_db_value(value: &gst::glib::SendValue) -> Option<f64> {
    value
        .get::<f64>()
        .ok()
        .or_else(|| value.get::<f32>().ok().map(f64::from))
}

fn dbfs_peak_to_linear(db: f64) -> f32 {
    if !db.is_finite() || db <= -120.0 {
        return 0.0;
    }
    let linear = 10f64.powf(db / 20.0).clamp(0.0, 1.0);
    linear.to_string().parse::<f32>().unwrap_or(1.0)
}

fn materialize_waveform_peaks(events: &[(u64, f32)], span_ns: u64, max_points: usize) -> Vec<f32> {
    if span_ns == 0 || max_points == 0 || events.is_empty() {
        return Vec::new();
    }

    let mut peaks = vec![0.0f32; max_points];
    for &(time_ns, peak) in events {
        if let Some(bin_index) = level_message_bin_index(time_ns, span_ns, max_points) {
            if peak > peaks[bin_index] {
                peaks[bin_index] = peak;
            }
        }
    }
    peaks
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn collapse_level_message_peaks_uses_loudest_channel() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("peak", gst::Array::new([-18.0f64, -6.0, -12.0]))
            .build();

        let peak = level_message_peak(structure.as_ref()).expect("peak");

        assert!((peak - 10f32.powf(-6.0 / 20.0)).abs() < 0.0001);
    }

    #[test]
    fn collapse_level_message_peaks_treats_floor_as_silence() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("peak", gst::Array::new([-150.0f64, f64::NEG_INFINITY]))
            .build();

        assert_eq!(level_message_peak(structure.as_ref()), Some(0.0));
    }

    #[test]
    fn collapse_level_message_peaks_accepts_list_values_too() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("peak", gst::List::new([-9.0f64, -3.0]))
            .build();

        let peak = level_message_peak(structure.as_ref()).expect("peak");

        assert!((peak - 10f32.powf(-3.0 / 20.0)).abs() < 0.0001);
    }

    #[test]
    fn collapse_level_message_peaks_accepts_value_array_too() {
        let _ = gst::init();
        let peaks = gst::glib::ValueArray::new([-15.0f64, -4.0]);
        let peak = collapse_level_db_values(peaks.as_slice()).expect("peak");

        assert!((peak - 10f32.powf(-4.0 / 20.0)).abs() < 0.0001);
    }

    #[test]
    fn level_message_bin_index_uses_running_time_when_present() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("running-time", 5_000_000_000u64)
            .field("duration", 1_000_000_000u64)
            .field("peak", gst::Array::new([-9.0f64]))
            .build();

        let (time_ns, _) = level_message_time_range_ns(structure.as_ref()).expect("time");
        assert_eq!(
            level_message_bin_index(time_ns, 10_000_000_000, 100),
            Some(55)
        );
    }

    #[test]
    fn level_message_bin_index_falls_back_to_end_minus_duration() {
        let _ = gst::init();
        let structure = gst::Structure::builder("level")
            .field("endtime", 8_000_000_000u64)
            .field("duration", 2_000_000_000u64)
            .field("peak", gst::Array::new([-9.0f64]))
            .build();

        let (time_ns, end_ns) = level_message_time_range_ns(structure.as_ref()).expect("time");
        assert_eq!(end_ns, 8_000_000_000);
        assert_eq!(
            level_message_bin_index(time_ns, 10_000_000_000, 100),
            Some(70)
        );
    }
}
