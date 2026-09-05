// SPDX-License-Identifier: GPL-3.0-or-later

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
        let effective_zoom = (zoom * f64::from(hop) / 1024.0).max(0.001);
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
                hop: 64,
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

    #[test]
    fn full_viewport_is_preserved_when_its_data_exceeds_speculative_budget() {
        let config = SpectrogramBufferConfig {
            width: 7680,
            sample_rate: 384_000,
            hop: 64,
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
