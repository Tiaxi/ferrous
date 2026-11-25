// SPDX-License-Identifier: GPL-3.0-or-later

//! The output's `ReplayGain` stage: a probe in the output bin that applies
//! each stream's gain, from the gain tags that travel with the stream.
//!
//! The output bin follows playsink's queue, so the probe sees each stream's
//! items in the order they play, at render time: the start of a stream,
//! then its tags, then its audio. A gapless successor, and the same file
//! again for Repeat-One, starts a new stream with its own tags, so its gain
//! applies from its first sample and the previous stream's gain to its
//! last. A seek starts no new stream and keeps the gain.
//!
//! The stage multiplies the samples itself rather than through a `volume`
//! element, which takes only some formats: with one in the output, a 24-bit
//! FLAC or a big-endian AIFF would be converted even with `ReplayGain` off.
//! With `ReplayGain` off, or at a gain of 1, buffers pass untouched.

use std::sync::{Arc, Mutex};

use gstreamer as gst;
use gstreamer::prelude::*;
use gstreamer_audio as gst_audio;

use super::replay_gain::{gain_for, AppliedGain, GainTags, ReplayGainSettings};

/// What the stage applies, for the engine to report.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(super) struct StageOutput {
    /// The group of the stream that reached the stage last.
    pub group: Option<gst::GroupId>,
    /// Whether that stream's audio has reached the stage, so that the tags
    /// it sends before its audio are in `applied`.
    pub playing: bool,
    /// The gain applied to it, or `None` when `ReplayGain` is off.
    pub applied: Option<AppliedGain>,
}

#[derive(Debug)]
struct StageState {
    settings: ReplayGainSettings,
    /// The gain tags the current stream has sent.
    tags: GainTags,
    output: StageOutput,
    /// The format of the current stream's samples, from its caps.
    format: Option<gst_audio::AudioFormatInfo>,
    /// The factor the samples are multiplied by, or `None` to leave the
    /// buffers untouched, as when `ReplayGain` is off.
    scale: Option<f64>,
}

/// Shared between the engine, which sets the settings and reads what is
/// applied, and the output's streaming thread, which applies it.
#[derive(Debug)]
pub(super) struct GainStage {
    state: Mutex<StageState>,
}

impl GainStage {
    pub(super) fn new() -> Arc<Self> {
        Arc::new(Self {
            state: Mutex::new(StageState {
                settings: ReplayGainSettings::default(),
                tags: GainTags::default(),
                output: StageOutput {
                    group: None,
                    playing: false,
                    applied: None,
                },
                format: None,
                scale: None,
            }),
        })
    }

    /// Apply `settings` from the next buffer on, to the current stream's
    /// tags.
    pub(super) fn set_settings(&self, settings: ReplayGainSettings) {
        self.update(|state| state.settings = settings.clamped());
    }

    /// What the stage applies now.
    pub(super) fn output(&self) -> StageOutput {
        self.state.lock().map_or(
            StageOutput {
                group: None,
                playing: false,
                applied: None,
            },
            |state| state.output,
        )
    }

    /// Change the state and work out the gain again.
    fn update(&self, change: impl FnOnce(&mut StageState)) {
        let Ok(mut state) = self.state.lock() else {
            return;
        };
        change(&mut state);
        state.output.applied = gain_for(&state.settings, &state.tags);
        state.scale = state
            .output
            .applied
            .map(|applied| applied.linear)
            .filter(|linear| (linear - 1.0).abs() >= f64::EPSILON);
    }

    fn stream_started(&self, group: Option<gst::GroupId>) {
        self.update(|state| {
            state.tags = GainTags::default();
            state.output.group = group;
            state.output.playing = false;
        });
    }

    fn tags_arrived(&self, tags: GainTags) {
        if tags == GainTags::default() {
            return;
        }
        self.update(|state| state.tags.merge(tags));
    }

    fn caps_arrived(&self, caps: &gst::CapsRef) {
        let format = gst_audio::AudioInfo::from_caps(caps)
            .ok()
            .map(|info| info.format_info());
        if let Ok(mut state) = self.state.lock() {
            state.format = format;
        }
    }

    /// The gain to multiply a buffer of the current stream by, and the
    /// format of its samples, or `None` to leave it as it is.
    fn gain_for_buffer(&self) -> Option<(f64, gst_audio::AudioFormatInfo)> {
        let mut state = self.state.lock().ok()?;
        state.output.playing = true;
        Some((state.scale?, state.format?))
    }

    /// Apply the gain to the audio that passes `pad`, an output pad of an
    /// element whose output is raw audio.
    pub(super) fn install(self: &Arc<Self>, pad: &gst::Pad) {
        let stage = Arc::downgrade(self);
        pad.add_probe(
            gst::PadProbeType::BUFFER
                | gst::PadProbeType::BUFFER_LIST
                | gst::PadProbeType::EVENT_DOWNSTREAM,
            move |_, info| {
                let Some(stage) = stage.upgrade() else {
                    return gst::PadProbeReturn::Ok;
                };
                match &mut info.data {
                    Some(gst::PadProbeData::Event(event)) => match event.view() {
                        gst::EventView::StreamStart(start) => {
                            stage.stream_started(start.group_id());
                        }
                        gst::EventView::Tag(tag) => {
                            stage.tags_arrived(GainTags::from_tag_list(tag.tag()));
                        }
                        gst::EventView::Caps(caps) => stage.caps_arrived(caps.caps()),
                        _ => {}
                    },
                    Some(gst::PadProbeData::Buffer(buffer)) => {
                        if let Some((gain, format)) = stage.gain_for_buffer() {
                            scale_buffer(buffer.make_mut(), format, gain);
                        }
                    }
                    Some(gst::PadProbeData::BufferList(list)) => {
                        if let Some((gain, format)) = stage.gain_for_buffer() {
                            list.make_mut().foreach_mut(|mut buffer, _| {
                                scale_buffer(buffer.make_mut(), format, gain);
                                std::ops::ControlFlow::Continue(Some(buffer))
                            });
                        }
                    }
                    _ => {}
                }
                gst::PadProbeReturn::Ok
            },
        );
    }
}

fn scale_buffer(buffer: &mut gst::BufferRef, format: gst_audio::AudioFormatInfo, gain: f64) {
    if let Ok(mut map) = buffer.map_writable() {
        scale_samples(map.as_mut_slice(), format, gain);
    }
}

/// Multiply the samples in `data`, of `format`, by `gain`. Every sample
/// takes the same gain, so the channels' layout does not matter. Integer
/// samples are rounded and saturate at full scale; float samples are not
/// limited.
pub(super) fn scale_samples(data: &mut [u8], format: gst_audio::AudioFormatInfo, gain: f64) {
    let big_endian = format.endianness() == gst_audio::AudioEndianness::BigEndian;
    let width = usize::try_from(format.width() / 8).unwrap_or(0);
    if format.is_float() {
        match width {
            4 => {
                #[allow(clippy::cast_possible_truncation)]
                let gain = gain as f32;
                for sample in data.chunks_exact_mut(4) {
                    let bytes: [u8; 4] = sample.try_into().unwrap_or_default();
                    let value = if big_endian {
                        f32::from_be_bytes(bytes)
                    } else {
                        f32::from_le_bytes(bytes)
                    } * gain;
                    sample.copy_from_slice(&if big_endian {
                        value.to_be_bytes()
                    } else {
                        value.to_le_bytes()
                    });
                }
            }
            8 => {
                for sample in data.chunks_exact_mut(8) {
                    let bytes: [u8; 8] = sample.try_into().unwrap_or_default();
                    let value = if big_endian {
                        f64::from_be_bytes(bytes)
                    } else {
                        f64::from_le_bytes(bytes)
                    } * gain;
                    sample.copy_from_slice(&if big_endian {
                        value.to_be_bytes()
                    } else {
                        value.to_le_bytes()
                    });
                }
            }
            _ => {}
        }
        return;
    }
    let depth = format.depth();
    if !(1..=4).contains(&width) || depth == 0 || depth > format.width() {
        return;
    }
    let signed = format.is_signed();
    let half = 1i64 << (depth - 1);
    let mask = if depth == 32 {
        u32::MAX
    } else {
        (1u32 << depth) - 1
    };
    // Integer samples of up to 32 bits are exact in an `f64`.
    #[allow(clippy::cast_precision_loss)]
    let (lowest, highest) = (-half as f64, (half - 1) as f64);
    for sample in data.chunks_exact_mut(width) {
        let mut word = [0u8; 4];
        let raw = if big_endian {
            word[4 - width..].copy_from_slice(sample);
            u32::from_be_bytes(word)
        } else {
            word[..width].copy_from_slice(sample);
            u32::from_le_bytes(word)
        };
        let raw = i64::from(raw & mask);
        let value = if !signed {
            raw - half
        } else if raw >= half {
            // Sign-extend from the sample's depth.
            raw - (half << 1)
        } else {
            raw
        };
        #[allow(clippy::cast_precision_loss)]
        let scaled = (value as f64 * gain).round().clamp(lowest, highest);
        // In the sample's range, which the clamp keeps it in.
        #[allow(clippy::cast_possible_truncation)]
        let scaled = scaled as i64;
        // Signed samples are written sign-extended over their width, as
        // `audioconvert` writes them; unsigned ones with the upper bits 0.
        let out = if signed {
            i32::try_from(scaled).map_or(0, i32::cast_unsigned)
        } else {
            u32::try_from(scaled + half).unwrap_or(0)
        };
        if big_endian {
            sample.copy_from_slice(&out.to_be_bytes()[4 - width..]);
        } else {
            sample.copy_from_slice(&out.to_le_bytes()[..width]);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gst_audio::AudioFormat;

    fn info(format: AudioFormat) -> gst_audio::AudioFormatInfo {
        gst::init().unwrap();
        gst_audio::AudioFormatInfo::from_format(format)
    }

    #[test]
    fn integer_samples_are_scaled_rounded_and_saturated() {
        let format = info(AudioFormat::S16le);
        let mut data: Vec<u8> = [1000i16, -1000, 32_000, -32_768, 3]
            .iter()
            .flat_map(|s| s.to_le_bytes())
            .collect();
        scale_samples(&mut data, format, 0.5);
        let out: Vec<i16> = data
            .chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]))
            .collect();
        // 1.5 rounds away from zero.
        assert_eq!(out, [500, -500, 16_000, -16_384, 2]);

        let mut data: Vec<u8> = [20_000i16, -20_000]
            .iter()
            .flat_map(|s| s.to_le_bytes())
            .collect();
        scale_samples(&mut data, format, 2.0);
        let out: Vec<i16> = data
            .chunks_exact(2)
            .map(|b| i16::from_le_bytes([b[0], b[1]]))
            .collect();
        assert_eq!(out, [i16::MAX, i16::MIN]);
    }

    #[test]
    fn samples_of_24_bits_in_32_keep_their_sign_extension() {
        let format = info(AudioFormat::S2432le);
        let mut data: Vec<u8> = [-4_000_000i32, 4_000_000]
            .iter()
            .flat_map(|s| s.to_le_bytes())
            .collect();
        scale_samples(&mut data, format, 0.5);
        let out: Vec<i32> = data
            .chunks_exact(4)
            .map(|b| i32::from_le_bytes(b.try_into().unwrap()))
            .collect();
        assert_eq!(out, [-2_000_000, 2_000_000]);
        // Full scale for 24 bits, not 32.
        let mut data = 8_000_000i32.to_le_bytes().to_vec();
        scale_samples(&mut data, format, 2.0);
        assert_eq!(i32::from_le_bytes(data.try_into().unwrap()), (1 << 23) - 1);
    }

    #[test]
    fn packed_big_endian_and_unsigned_samples_are_scaled() {
        // S24BE: -2 as three bytes.
        let format = info(AudioFormat::S24be);
        let mut data = vec![0xFF, 0xFF, 0xFE, 0x00, 0x00, 0x64];
        scale_samples(&mut data, format, 0.5);
        assert_eq!(data, [0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x32]);

        // U8: 128 is silence.
        let format = info(AudioFormat::U8);
        let mut data = vec![128, 228, 28];
        scale_samples(&mut data, format, 0.5);
        assert_eq!(data, [128, 178, 78]);

        let format = info(AudioFormat::S16be);
        let mut data = 1000i16.to_be_bytes().to_vec();
        scale_samples(&mut data, format, 0.25);
        assert_eq!(i16::from_be_bytes([data[0], data[1]]), 250);

        let format = info(AudioFormat::S32le);
        let mut data = i32::MIN.to_le_bytes().to_vec();
        scale_samples(&mut data, format, 0.5);
        assert_eq!(i32::from_le_bytes(data.try_into().unwrap()), i32::MIN / 2);
    }

    #[test]
    fn float_samples_are_multiplied() {
        let format = info(AudioFormat::F32le);
        let mut data: Vec<u8> = [0.5f32, -1.0]
            .iter()
            .flat_map(|s| s.to_le_bytes())
            .collect();
        scale_samples(&mut data, format, 0.5);
        let out: Vec<f32> = data
            .chunks_exact(4)
            .map(|b| f32::from_le_bytes(b.try_into().unwrap()))
            .collect();
        assert_eq!(out, [0.25, -0.5]);

        let format = info(AudioFormat::F64be);
        let mut data = 0.5f64.to_be_bytes().to_vec();
        scale_samples(&mut data, format, 3.0);
        assert!((f64::from_be_bytes(data.try_into().unwrap()) - 1.5).abs() < f64::EPSILON);
    }
}
