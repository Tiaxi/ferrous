// SPDX-License-Identifier: GPL-3.0-or-later

//! `ReplayGain`: the gain that levels tracks and albums from the gain tags
//! their files carry.
//!
//! Nothing here scans audio. The gains come from the tags that the
//! pipeline's demuxers and decoders send with each stream (see
//! `gain_stage`), so they are the file's own, and a gapless successor brings
//! its tags with its stream.

/// Which gain tag levels the tracks.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum ReplayGainMode {
    /// No gain: the audio plays as the file has it.
    #[default]
    Off,
    /// Each track at the same loudness.
    Track,
    /// Each album at the same loudness, its tracks as loud as each other
    /// as they were mastered. A track without an album gain takes its
    /// track gain.
    Album,
}

impl ReplayGainMode {
    #[must_use]
    pub fn from_u8(value: u8) -> Self {
        match value {
            1 => Self::Track,
            2 => Self::Album,
            _ => Self::Off,
        }
    }

    #[must_use]
    pub fn to_u8(self) -> u8 {
        match self {
            Self::Off => 0,
            Self::Track => 1,
            Self::Album => 2,
        }
    }

    #[must_use]
    pub fn settings_value(self) -> &'static str {
        match self {
            Self::Off => "off",
            Self::Track => "track",
            Self::Album => "album",
        }
    }

    #[must_use]
    pub fn parse_settings_value(raw: &str) -> Option<Self> {
        match raw.trim().to_ascii_lowercase().as_str() {
            "off" | "0" => Some(Self::Off),
            "track" | "1" => Some(Self::Track),
            "album" | "2" => Some(Self::Album),
            _ => None,
        }
    }
}

/// The lowest and highest preamp, and gain for files without gain tags,
/// in dB.
pub const REPLAY_GAIN_DB_RANGE: (f32, f32) = (-15.0, 15.0);

/// How `ReplayGain` levels playback.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct ReplayGainSettings {
    pub mode: ReplayGainMode,
    /// Added to every gain, in dB.
    pub preamp_db: f32,
    /// The gain of a file without a gain tag, in dB, before the preamp.
    pub untagged_db: f32,
}

impl ReplayGainSettings {
    /// These settings with the dB values in [`REPLAY_GAIN_DB_RANGE`], and a
    /// value that is not a number taken as 0 dB.
    #[must_use]
    pub fn clamped(self) -> Self {
        let clamp = |db: f32| {
            if db.is_finite() {
                db.clamp(REPLAY_GAIN_DB_RANGE.0, REPLAY_GAIN_DB_RANGE.1)
            } else {
                0.0
            }
        };
        Self {
            mode: self.mode,
            preamp_db: clamp(self.preamp_db),
            untagged_db: clamp(self.untagged_db),
        }
    }
}

/// The gain tags of a stream, as `ReplayGain` 2 has them: gains in dB to
/// the reference level of -18 LUFS, peaks as the highest sample's
/// amplitude, 1.0 being full scale.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct GainTags {
    pub track_gain: Option<f64>,
    pub track_peak: Option<f64>,
    pub album_gain: Option<f64>,
    pub album_peak: Option<f64>,
}

impl GainTags {
    /// Take the values that `newer` has; keep the others. A stream sends
    /// its tags in several events, such as the container's and the
    /// codec's, and not every one has the gains.
    pub fn merge(&mut self, newer: Self) {
        self.track_gain = newer.track_gain.or(self.track_gain);
        self.track_peak = newer.track_peak.or(self.track_peak);
        self.album_gain = newer.album_gain.or(self.album_gain);
        self.album_peak = newer.album_peak.or(self.album_peak);
    }
}

/// How the applied gain was chosen.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum GainSource {
    /// The track gain, in Track mode.
    Track,
    /// The album gain, in Album mode.
    Album,
    /// The track gain, in Album mode, as the file has no album gain.
    TrackWithoutAlbum,
    /// The gain for files without gain tags.
    Untagged,
}

impl GainSource {
    #[must_use]
    pub fn to_u8(self) -> u8 {
        match self {
            Self::Track => 1,
            Self::Album => 2,
            Self::TrackWithoutAlbum => 3,
            Self::Untagged => 4,
        }
    }
}

/// The gain that `ReplayGain` applies to a stream.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AppliedGain {
    pub source: GainSource,
    /// The gain of the tag, or the gain for files without one, in dB.
    pub gain_db: f64,
    /// The preamp added to it, in dB.
    pub preamp_db: f64,
    /// What the stream plays at, in dB: the gain and the preamp, less what
    /// the peak takes off.
    pub applied_db: f64,
    /// Whether the peak lowered the gain, so that the loudest sample does
    /// not clip.
    pub peak_limited: bool,
    /// The factor that each sample is multiplied by.
    pub linear: f64,
}

/// The most that `ReplayGain` turns a stream up or down, in dB, whatever its
/// tags say.
const MAX_GAIN_DB: f64 = 60.0;

/// The gain that `settings` give a stream with `tags`, or `None` when
/// `ReplayGain` is off and the stream plays as it is.
///
/// Album mode takes the album gain and Track mode the track gain; Album
/// mode takes the track gain of a file without an album gain, and a file
/// without the gain it needs takes the gain for untagged files. The preamp
/// is added to every gain. Where a peak tag tells the stream's loudest
/// sample, the gain stays low enough for that sample not to clip: there is
/// no limiter.
#[must_use]
pub fn gain_for(settings: &ReplayGainSettings, tags: &GainTags) -> Option<AppliedGain> {
    let settings = settings.clamped();
    let (source, gain_db, peak) = match settings.mode {
        ReplayGainMode::Off => return None,
        ReplayGainMode::Album => match (tags.album_gain, tags.track_gain) {
            // The album's peak keeps its tracks as loud as each other; a
            // file without one keeps its own track from clipping.
            (Some(gain), _) => (GainSource::Album, gain, tags.album_peak.or(tags.track_peak)),
            (None, Some(gain)) => (GainSource::TrackWithoutAlbum, gain, tags.track_peak),
            (None, None) => (
                GainSource::Untagged,
                f64::from(settings.untagged_db),
                tags.track_peak.or(tags.album_peak),
            ),
        },
        ReplayGainMode::Track => match tags.track_gain {
            Some(gain) => (GainSource::Track, gain, tags.track_peak),
            None => (
                GainSource::Untagged,
                f64::from(settings.untagged_db),
                tags.track_peak.or(tags.album_peak),
            ),
        },
    };
    let preamp_db = f64::from(settings.preamp_db);
    let wanted_db = (gain_db + preamp_db).clamp(-MAX_GAIN_DB, MAX_GAIN_DB);
    let mut linear = db_to_linear(wanted_db);
    let mut peak_limited = false;
    if let Some(peak) = peak.filter(|peak| *peak > 0.0) {
        let highest = 1.0 / peak;
        if linear > highest {
            linear = highest;
            peak_limited = true;
        }
    }
    Some(AppliedGain {
        source,
        gain_db,
        preamp_db,
        applied_db: if peak_limited {
            linear_to_db(linear)
        } else {
            wanted_db
        },
        peak_limited,
        linear,
    })
}

#[must_use]
pub fn db_to_linear(db: f64) -> f64 {
    10f64.powf(db / 20.0)
}

#[must_use]
pub fn linear_to_db(linear: f64) -> f64 {
    20.0 * linear.log10()
}

/// The `ReplayGain` 2 gain for an R128 gain, which the Opus tags tell to the
/// EBU R128 reference level of -23 LUFS instead of `ReplayGain`'s -18 LUFS.
#[must_use]
pub fn r128_to_replay_gain_db(r128_db: f64) -> f64 {
    r128_db + 5.0
}

/// The gain of an Opus `R128_TRACK_GAIN` or `R128_ALBUM_GAIN` comment: a
/// signed Q7.8 number of dB, to the R128 reference level.
#[must_use]
pub fn parse_r128_comment(value: &str) -> Option<f64> {
    let q78 = value.trim().parse::<i16>().ok()?;
    Some(f64::from(q78) / 256.0)
}

#[cfg(feature = "gst")]
mod tag_list {
    use gstreamer as gst;

    use super::{parse_r128_comment, r128_to_replay_gain_db, GainTags};

    /// The tags under which `GStreamer` 1.28 tells an Opus file's R128
    /// gains, in dB to the R128 reference level. Older versions leave them
    /// in the extended comments.
    const R128_TRACK_GAIN: &str = "r128-track-gain";
    const R128_ALBUM_GAIN: &str = "r128-album-gain";

    fn finite(value: f64) -> Option<f64> {
        value.is_finite().then_some(value)
    }

    fn r128_tag(tags: &gst::TagListRef, name: &str) -> Option<f64> {
        tags.generic(name)?.get::<f64>().ok().and_then(finite)
    }

    fn r128_comment(tags: &gst::TagListRef, key: &str) -> Option<f64> {
        tags.iter_tag::<gst::tags::ExtendedComment>()
            .find_map(|comment| {
                let (name, value) = comment.get().split_once('=')?;
                name.trim()
                    .eq_ignore_ascii_case(key)
                    .then(|| parse_r128_comment(value))
                    .flatten()
            })
    }

    impl GainTags {
        /// The gain tags in `tags`, an event's tags. An Opus file's R128
        /// gains take the place of any `ReplayGain` tags it has, as the Opus
        /// format defines them.
        #[must_use]
        pub fn from_tag_list(tags: &gst::TagListRef) -> Self {
            let value = |found: Option<gst::tags::TagValue<f64>>| found.map(|v| v.get());
            let r128_track = r128_tag(tags, R128_TRACK_GAIN)
                .or_else(|| r128_comment(tags, "R128_TRACK_GAIN"))
                .map(r128_to_replay_gain_db);
            let r128_album = r128_tag(tags, R128_ALBUM_GAIN)
                .or_else(|| r128_comment(tags, "R128_ALBUM_GAIN"))
                .map(r128_to_replay_gain_db);
            Self {
                track_gain: r128_track
                    .or_else(|| value(tags.get::<gst::tags::TrackGain>()).and_then(finite)),
                track_peak: value(tags.get::<gst::tags::TrackPeak>()).and_then(finite),
                album_gain: r128_album
                    .or_else(|| value(tags.get::<gst::tags::AlbumGain>()).and_then(finite)),
                album_peak: value(tags.get::<gst::tags::AlbumPeak>()).and_then(finite),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn settings(mode: ReplayGainMode, preamp_db: f32, untagged_db: f32) -> ReplayGainSettings {
        ReplayGainSettings {
            mode,
            preamp_db,
            untagged_db,
        }
    }

    fn tagged(track: f64, album: Option<f64>) -> GainTags {
        GainTags {
            track_gain: Some(track),
            album_gain: album,
            ..GainTags::default()
        }
    }

    fn assert_db(applied: &AppliedGain, db: f64) {
        assert!(
            (applied.applied_db - db).abs() < 1e-9,
            "{applied:?} is not {db} dB"
        );
        assert!(
            (applied.linear - db_to_linear(db)).abs() < 1e-12,
            "{applied:?}"
        );
    }

    #[test]
    fn off_applies_no_gain_whatever_the_tags() {
        let off = settings(ReplayGainMode::Off, 6.0, -3.0);
        assert_eq!(gain_for(&off, &tagged(-6.4, Some(-3.0))), None);
        assert_eq!(gain_for(&off, &GainTags::default()), None);
    }

    #[test]
    fn the_default_is_off_without_preamp_or_untagged_gain() {
        assert_eq!(
            ReplayGainSettings::default(),
            settings(ReplayGainMode::Off, 0.0, 0.0)
        );
    }

    #[test]
    fn track_mode_takes_the_track_gain() {
        let applied = gain_for(
            &settings(ReplayGainMode::Track, 0.0, 0.0),
            &tagged(-6.4, Some(-3.0)),
        )
        .unwrap();
        assert_eq!(applied.source, GainSource::Track);
        assert!((applied.gain_db + 6.4).abs() < 1e-9);
        assert_db(&applied, -6.4);
        assert!(!applied.peak_limited);
    }

    #[test]
    fn album_mode_takes_the_album_gain_and_falls_back_to_the_track_gain() {
        let album = settings(ReplayGainMode::Album, 0.0, 0.0);
        let applied = gain_for(&album, &tagged(-6.4, Some(-3.0))).unwrap();
        assert_eq!(applied.source, GainSource::Album);
        assert_db(&applied, -3.0);

        let applied = gain_for(&album, &tagged(-6.4, None)).unwrap();
        assert_eq!(applied.source, GainSource::TrackWithoutAlbum);
        assert_db(&applied, -6.4);
    }

    #[test]
    fn a_file_without_the_gain_takes_the_untagged_gain() {
        for mode in [ReplayGainMode::Track, ReplayGainMode::Album] {
            let applied = gain_for(&settings(mode, 0.0, -4.5), &GainTags::default()).unwrap();
            assert_eq!(applied.source, GainSource::Untagged);
            assert!((applied.gain_db + 4.5).abs() < 1e-9);
            assert_db(&applied, -4.5);
        }
        // Track mode does not take an album gain for a missing track gain.
        let album_only = GainTags {
            album_gain: Some(-3.0),
            ..GainTags::default()
        };
        let applied = gain_for(&settings(ReplayGainMode::Track, 0.0, 0.0), &album_only).unwrap();
        assert_eq!(applied.source, GainSource::Untagged);
        assert_db(&applied, 0.0);
        assert!(
            (applied.linear - 1.0).abs() < f64::EPSILON,
            "0 dB untagged is unity gain"
        );
    }

    #[test]
    fn the_preamp_is_added_to_every_gain() {
        let applied = gain_for(
            &settings(ReplayGainMode::Track, 2.5, -1.0),
            &tagged(-6.4, None),
        )
        .unwrap();
        assert!((applied.preamp_db - 2.5).abs() < 1e-9);
        assert_db(&applied, -3.9);
        let applied = gain_for(
            &settings(ReplayGainMode::Album, 2.5, -1.0),
            &GainTags::default(),
        )
        .unwrap();
        assert_db(&applied, 1.5);
    }

    #[test]
    fn the_peak_keeps_the_loudest_sample_from_clipping() {
        // +6 dB on a peak of 0.8 would reach 1.6.
        let tags = GainTags {
            track_gain: Some(6.0),
            track_peak: Some(0.8),
            ..GainTags::default()
        };
        let applied = gain_for(&settings(ReplayGainMode::Track, 0.0, 0.0), &tags).unwrap();
        assert!(applied.peak_limited);
        assert!((applied.linear * 0.8 - 1.0).abs() < 1e-12);
        assert!((applied.applied_db - linear_to_db(1.0 / 0.8)).abs() < 1e-9);
        assert!((applied.gain_db - 6.0).abs() < 1e-9);

        // A gain that keeps the peak below full scale is not limited.
        let quiet = GainTags {
            track_peak: Some(0.4),
            ..tags
        };
        let applied = gain_for(&settings(ReplayGainMode::Track, 0.0, 0.0), &quiet).unwrap();
        assert!(!applied.peak_limited);
        assert_db(&applied, 6.0);

        // The preamp counts towards the peak's limit too.
        let applied = gain_for(
            &settings(ReplayGainMode::Track, 6.0, 0.0),
            &GainTags {
                track_gain: Some(0.0),
                track_peak: Some(0.8),
                ..GainTags::default()
            },
        )
        .unwrap();
        assert!(applied.peak_limited);
    }

    #[test]
    fn album_mode_limits_by_the_album_peak_or_the_track_peak() {
        let album = settings(ReplayGainMode::Album, 0.0, 0.0);
        let tags = GainTags {
            track_gain: Some(0.0),
            track_peak: Some(0.5),
            album_gain: Some(6.0),
            album_peak: Some(0.9),
        };
        let applied = gain_for(&album, &tags).unwrap();
        assert!(applied.peak_limited);
        assert!((applied.linear - 1.0 / 0.9).abs() < 1e-12);
        let applied = gain_for(
            &album,
            &GainTags {
                album_peak: None,
                ..tags
            },
        )
        .unwrap();
        assert!(!applied.peak_limited, "{applied:?}");
        assert_db(&applied, 6.0);
    }

    #[test]
    fn a_peak_of_zero_limits_nothing() {
        let tags = GainTags {
            track_gain: Some(3.0),
            track_peak: Some(0.0),
            ..GainTags::default()
        };
        let applied = gain_for(&settings(ReplayGainMode::Track, 0.0, 0.0), &tags).unwrap();
        assert!(!applied.peak_limited);
        assert_db(&applied, 3.0);
    }

    #[test]
    fn settings_outside_the_range_are_clamped() {
        let applied = gain_for(
            &settings(ReplayGainMode::Track, 40.0, f32::NAN),
            &GainTags::default(),
        )
        .unwrap();
        assert!((applied.preamp_db - 15.0).abs() < 1e-9);
        assert!(applied.gain_db.abs() < 1e-9);
        assert_db(&applied, 15.0);
    }

    #[test]
    fn r128_gains_are_converted_to_the_replay_gain_reference() {
        // -1792 / 256 = -7 dB to -23 LUFS, which is -2 dB to -18 LUFS.
        let r128 = parse_r128_comment("-1792").unwrap();
        assert!((r128 + 7.0).abs() < 1e-12);
        assert!((r128_to_replay_gain_db(r128) + 2.0).abs() < 1e-12);
        assert!((parse_r128_comment(" 256 ").unwrap() - 1.0).abs() < 1e-12);
        assert_eq!(parse_r128_comment("-6.4 dB"), None);
        assert_eq!(parse_r128_comment("40000"), None);
    }

    #[test]
    fn merged_tags_keep_the_values_a_later_event_lacks() {
        let mut tags = tagged(-6.4, Some(-3.0));
        tags.merge(GainTags {
            track_peak: Some(0.7),
            ..GainTags::default()
        });
        assert_eq!(
            tags,
            GainTags {
                track_gain: Some(-6.4),
                track_peak: Some(0.7),
                album_gain: Some(-3.0),
                album_peak: None,
            }
        );
        tags.merge(tagged(-1.0, None));
        assert_eq!(tags.track_gain, Some(-1.0));
        assert_eq!(tags.album_gain, Some(-3.0));
    }

    #[test]
    fn modes_round_trip_through_their_settings_values_and_codes() {
        for mode in [
            ReplayGainMode::Off,
            ReplayGainMode::Track,
            ReplayGainMode::Album,
        ] {
            assert_eq!(
                ReplayGainMode::parse_settings_value(mode.settings_value()),
                Some(mode)
            );
            assert_eq!(ReplayGainMode::from_u8(mode.to_u8()), mode);
        }
        assert_eq!(ReplayGainMode::parse_settings_value("loud"), None);
        assert_eq!(ReplayGainMode::from_u8(9), ReplayGainMode::Off);
    }

    #[cfg(feature = "gst")]
    mod tag_lists {
        use gstreamer as gst;

        use super::super::GainTags;

        #[test]
        fn gains_and_peaks_are_read_from_a_tag_list() {
            gst::init().unwrap();
            let mut tags = gst::TagList::new();
            {
                let tags = tags.get_mut().unwrap();
                tags.add::<gst::tags::TrackGain>(&-6.4, gst::TagMergeMode::Append);
                tags.add::<gst::tags::TrackPeak>(&0.5, gst::TagMergeMode::Append);
                tags.add::<gst::tags::AlbumGain>(&-3.0, gst::TagMergeMode::Append);
                tags.add::<gst::tags::AlbumPeak>(&0.6, gst::TagMergeMode::Append);
            }
            assert_eq!(
                GainTags::from_tag_list(&tags),
                GainTags {
                    track_gain: Some(-6.4),
                    track_peak: Some(0.5),
                    album_gain: Some(-3.0),
                    album_peak: Some(0.6),
                }
            );
            assert_eq!(
                GainTags::from_tag_list(&gst::TagList::new()),
                GainTags::default()
            );
        }

        #[test]
        fn r128_comments_take_the_place_of_replay_gain_tags() {
            gst::init().unwrap();
            let mut tags = gst::TagList::new();
            {
                let tags = tags.get_mut().unwrap();
                tags.add::<gst::tags::TrackGain>(&-6.4, gst::TagMergeMode::Append);
                tags.add::<gst::tags::ExtendedComment>(
                    &"R128_TRACK_GAIN=-1792",
                    gst::TagMergeMode::Append,
                );
                tags.add::<gst::tags::ExtendedComment>(
                    &"r128_album_gain=256",
                    gst::TagMergeMode::Append,
                );
            }
            let gains = GainTags::from_tag_list(&tags);
            assert_eq!(gains.track_gain, Some(-2.0));
            assert_eq!(gains.album_gain, Some(6.0));
        }
    }
}
