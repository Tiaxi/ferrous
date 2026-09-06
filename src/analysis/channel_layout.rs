// SPDX-License-Identifier: GPL-3.0-or-later

use std::path::Path;

use super::decoders::{open_audio_file, AudioFrameSource};
use symphonia::core::audio::Channels;

/// Read positions from the same decoder selection and native channel order used
/// by waveform and spectrogram analysis. Called only on the metadata worker.
pub(crate) fn probe_source_channel_labels(path: &Path) -> Vec<String> {
    if !path.is_file() {
        return Vec::new();
    }
    let Some((source, _, count, _)) = open_audio_file(path) else {
        return Vec::new();
    };
    let labels = match &source {
        AudioFrameSource::Symphonia {
            format, track_id, ..
        } => format
            .tracks()
            .iter()
            .find(|track| track.id == *track_id)
            .and_then(|track| track.codec_params.channels)
            .map(symphonia_channel_labels)
            .unwrap_or_default(),
        #[cfg(feature = "gst")]
        AudioFrameSource::Gst { appsink, .. } => {
            use gstreamer::prelude::*;
            appsink
                .static_pad("sink")
                .and_then(|pad| pad.current_caps())
                .and_then(|caps| gstreamer_audio::AudioInfo::from_caps(&caps).ok())
                .and_then(|info| {
                    info.positions().map(|positions| {
                        positions
                            .iter()
                            .map(|position| gst_channel_label(*position).to_owned())
                            .collect()
                    })
                })
                .unwrap_or_default()
        }
    };
    if labels.len() == count && count <= 64 {
        labels
    } else {
        Vec::new()
    }
}

fn symphonia_channel_labels(channels: Channels) -> Vec<String> {
    // Channels::iter follows the ascending bit order of Symphonia's PCM planes.
    const LABELS: [&str; 26] = [
        "L", "R", "C", "LFE", "RL", "RR", "FLC", "FRC", "RC", "SL", "SR", "TC", "TFL", "TFC",
        "TFR", "TRL", "TRC", "TRR", "RLC", "RRC", "WL", "WR", "FHL", "FHC", "FHR", "LFE2",
    ];
    if channels.count() == 1 {
        return vec!["M".to_owned()];
    }
    channels
        .iter()
        .map(|channel| {
            usize::try_from(channel.bits().trailing_zeros())
                .ok()
                .and_then(|index| LABELS.get(index))
                .copied()
                .unwrap_or_default()
                .to_owned()
        })
        .collect()
}

#[cfg(feature = "gst")]
fn gst_channel_label(position: gstreamer_audio::AudioChannelPosition) -> &'static str {
    use gstreamer_audio::AudioChannelPosition as P;
    match position {
        P::Mono => "M",
        P::FrontLeft => "L",
        P::FrontRight => "R",
        P::FrontCenter => "C",
        P::Lfe1 => "LFE",
        P::Lfe2 => "LFE2",
        P::RearLeft => "RL",
        P::RearRight => "RR",
        P::FrontLeftOfCenter => "FLC",
        P::FrontRightOfCenter => "FRC",
        P::RearCenter => "RC",
        P::SideLeft => "SL",
        P::SideRight => "SR",
        P::TopCenter => "TC",
        P::TopFrontLeft => "TFL",
        P::TopFrontCenter => "TFC",
        P::TopFrontRight => "TFR",
        P::TopRearLeft => "TRL",
        P::TopRearCenter => "TRC",
        P::TopRearRight => "TRR",
        P::TopSideLeft => "TSL",
        P::TopSideRight => "TSR",
        P::BottomFrontLeft => "BFL",
        P::BottomFrontCenter => "BFC",
        P::BottomFrontRight => "BFR",
        P::WideLeft => "WL",
        P::WideRight => "WR",
        P::SurroundLeft => "SurL",
        P::SurroundRight => "SurR",
        _ => "",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn same_channel_count_preserves_different_speaker_positions() {
        let front =
            Channels::FRONT_LEFT | Channels::FRONT_RIGHT | Channels::FRONT_CENTRE | Channels::LFE1;
        assert_eq!(
            symphonia_channel_labels(front | Channels::REAR_LEFT | Channels::REAR_RIGHT),
            ["L", "R", "C", "LFE", "RL", "RR"]
        );
        assert_eq!(
            symphonia_channel_labels(front | Channels::SIDE_LEFT | Channels::SIDE_RIGHT),
            ["L", "R", "C", "LFE", "SL", "SR"]
        );
        assert_eq!(
            symphonia_channel_labels(
                front | Channels::FRONT_LEFT_CENTRE | Channels::FRONT_RIGHT_CENTRE
            ),
            ["L", "R", "C", "LFE", "FLC", "FRC"]
        );
    }

    #[test]
    fn extensible_wave_masks_reach_metadata_in_decoder_order() {
        for (mask, expected) in [
            (0x3fu32, ["L", "R", "C", "LFE", "RL", "RR"]),
            (0x60f, ["L", "R", "C", "LFE", "SL", "SR"]),
            (0xcf, ["L", "R", "C", "LFE", "FLC", "FRC"]),
        ] {
            let mut bytes = Vec::new();
            bytes.extend_from_slice(b"RIFF");
            bytes.extend_from_slice(&(60u32 + 1536).to_le_bytes());
            bytes.extend_from_slice(b"WAVEfmt ");
            bytes.extend_from_slice(&40u32.to_le_bytes());
            bytes.extend_from_slice(&0xfffeu16.to_le_bytes());
            bytes.extend_from_slice(&6u16.to_le_bytes());
            bytes.extend_from_slice(&48_000u32.to_le_bytes());
            bytes.extend_from_slice(&576_000u32.to_le_bytes());
            bytes.extend_from_slice(&12u16.to_le_bytes());
            bytes.extend_from_slice(&16u16.to_le_bytes());
            bytes.extend_from_slice(&22u16.to_le_bytes());
            bytes.extend_from_slice(&16u16.to_le_bytes());
            bytes.extend_from_slice(&mask.to_le_bytes());
            bytes.extend_from_slice(&[
                1, 0, 0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71,
            ]);
            bytes.extend_from_slice(b"data");
            bytes.extend_from_slice(&1536u32.to_le_bytes());
            bytes.resize(bytes.len() + 1536, 0);
            let path = std::env::temp_dir()
                .join(format!("ferrous-layout-{}-{mask}.wav", std::process::id()));
            std::fs::write(&path, bytes).expect("write generated WAV fixture");
            let actual = probe_source_channel_labels(&path);
            std::fs::remove_file(path).expect("remove generated WAV fixture");
            assert_eq!(actual, expected);
        }
    }

    #[cfg(feature = "gst")]
    #[test]
    fn gstreamer_positions_keep_negotiated_order_and_unknowns() {
        use gstreamer_audio::AudioChannelPosition as P;
        let positions = [
            P::FrontCenter,
            P::FrontLeft,
            P::FrontRight,
            P::SideLeft,
            P::SideRight,
            P::Lfe1,
            P::None,
        ];
        assert_eq!(
            positions.map(gst_channel_label),
            ["C", "L", "R", "SL", "SR", "LFE", ""]
        );
    }
}
