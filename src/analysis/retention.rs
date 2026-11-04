// SPDX-License-Identifier: GPL-3.0-or-later

use super::column_hop::reference_hop;
use super::f64_to_u64_saturating;

// Budget speculative decode across every channel. The visible viewport is a
// separate minimum: evicting it to satisfy a byte cap would produce blank areas.
const LOOKAHEAD_BYTES: u64 = 64 * 1024 * 1024;

pub(crate) struct SpectrogramBufferConfig {
    pub(crate) width: u32,
    pub(crate) sample_rate: u32,
    pub(crate) hop: u32,
    pub(crate) bins: u32,
    pub(crate) channels: u32,
    pub(crate) zoom: f64,
    pub(crate) centered: bool,
}

pub(crate) struct SpectrogramBufferLimits {
    pub(crate) lookahead: u64,
    pub(crate) capacity: u64,
}

pub(crate) fn lookahead_seconds() -> f64 {
    std::env::var("FERROUS_SPECTROGRAM_LOOKAHEAD_SECONDS")
        .ok()
        .and_then(|value| value.parse::<f64>().ok())
        .filter(|seconds| seconds.is_finite() && *seconds >= 0.0)
        .unwrap_or(10.0)
}

impl SpectrogramBufferConfig {
    pub(crate) fn limits(&self, seconds: f64) -> SpectrogramBufferLimits {
        let hop = self.hop.max(1);
        let zoom = if self.zoom.is_finite() {
            self.zoom.max(0.001)
        } else {
            1.0
        };
        // Columns `hop` apart are `zoom` pixels a reference hop apart.
        // Reference hops are at most a few thousand samples.
        #[allow(clippy::cast_precision_loss)]
        let reference = reference_hop(self.sample_rate) as f64;
        let effective_zoom = (zoom * f64::from(hop) / reference).max(0.001);
        let visible =
            f64_to_u64_saturating((f64::from(self.width.max(1920)) / effective_zoom).ceil()).max(1);
        let desired = f64_to_u64_saturating(seconds * f64::from(self.sample_rate) / f64::from(hop))
            .saturating_add(if self.centered {
                visible.saturating_mul(2)
            } else {
                0
            });
        let bytes_per_column = u64::from(self.bins.max(1)) * u64::from(self.channels.max(1));
        // At the start of a centered track the entire viewport lies ahead.
        // Rolling mode needs enough initial data for its 127-column warmup.
        let minimum = if self.centered { visible } else { 128 };
        let lookahead = desired
            .max(minimum)
            .min((LOOKAHEAD_BYTES / bytes_per_column).max(minimum));
        let history = visible.saturating_mul(if self.centered { 1 } else { 2 });
        SpectrogramBufferLimits {
            lookahead,
            capacity: history.saturating_add(lookahead).max(1024),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn high_rate_multichannel_lookahead_is_byte_limited_without_evicting_viewport() {
        for centered in [false, true] {
            let config = SpectrogramBufferConfig {
                width: 1920,
                sample_rate: 384_000,
                // Zoomed in all the way.
                hop: 512,
                bins: 4_097,
                channels: 8,
                zoom: 16.0,
                centered,
            };
            let limits = config.limits(10.0);
            assert!(limits.lookahead * 4_097 * 8 <= LOOKAHEAD_BYTES);
            assert!(limits.capacity >= limits.lookahead + 1920);
            assert!(limits.capacity < 8_000);
            assert_eq!(config.limits(1000.0).capacity, limits.capacity);
        }
    }

    fn capacity(sample_rate: u32, zoom: f64, centered: bool) -> u64 {
        let hop = super::super::column_hop::column_hop(sample_rate, zoom as f32);
        SpectrogramBufferConfig {
            width: 1920,
            sample_rate,
            hop: u32::try_from(hop).unwrap(),
            bins: 4_097,
            channels: 2,
            zoom,
            centered,
        }
        .limits(10.0)
        .capacity
    }

    #[test]
    fn every_rate_of_a_family_gets_the_same_ring() {
        for centered in [false, true] {
            for zoom in [1.0, 4.0, 0.25] {
                for family in [
                    [44_100, 88_200, 176_400, 352_800],
                    [48_000, 96_000, 192_000, 384_000],
                ] {
                    let first = capacity(family[0], zoom, centered);
                    for rate in family {
                        assert_eq!(capacity(rate, zoom, centered), first, "{rate} at {zoom}");
                    }
                }
            }
        }
        // A view and 10 s of lookahead: two views of history when rolling,
        // one when centered, which looks ahead a view more besides.
        assert_eq!(capacity(96_000, 1.0, false), 3_840 + 468);
        assert_eq!(capacity(96_000, 1.0, true), 1_920 + 468 + 3_840);
        assert_eq!(capacity(88_200, 1.0, false), 3_840 + 430);
        // Zoomed in four times, the 10 s are four times the columns.
        assert_eq!(capacity(96_000, 4.0, false), 3_840 + 1_875);
    }

    #[test]
    fn full_viewport_is_preserved_when_its_data_exceeds_speculative_budget() {
        let config = SpectrogramBufferConfig {
            width: 7680,
            sample_rate: 384_000,
            hop: 512,
            bins: 4_097,
            channels: 8,
            zoom: 16.0,
            centered: true,
        };
        let limits = config.limits(10.0);
        assert_eq!(limits.lookahead, 7680);
        assert_eq!(limits.capacity, 15360);
    }
}
