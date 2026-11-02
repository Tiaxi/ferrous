// SPDX-License-Identifier: GPL-3.0-or-later

//! Which channels of a stream are heard.
//!
//! Mutes and solos are independent, and neither changes the other.
//!
//! - A muted channel is silent, also while it is soloed.
//! - While at least one channel of the stream is soloed, the channels that
//!   are not soloed are silent too.
//! - Mutes and solos of channels a stream does not have do nothing for it,
//!   and apply again to a later stream that has them.
//! - A stream with fewer channels than the last one never starts silenced by
//!   mutes made for the wider one; see [`ChannelMix::fit_narrower_stream`].

/// Channels are bits of a mask, so this many can be muted or soloed.
const MAX_CHANNELS: u32 = u64::BITS;

fn channel_bit(channel: u8) -> u64 {
    1 << u32::from(channel).min(MAX_CHANNELS - 1)
}

/// The channels of a stream of `channels` channels. A stream that is not
/// known yet, with 0 channels, may have them all.
fn stream_channels(channels: u32) -> u64 {
    if channels == 0 || channels >= MAX_CHANNELS {
        u64::MAX
    } else {
        (1 << channels) - 1
    }
}

/// What the user muted and soloed. Masks of a stream of `channels` channels
/// describe that stream; see the module's rules.
#[derive(Debug, Default, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ChannelMix {
    pub(crate) muted: u64,
    pub(crate) soloed: u64,
}

impl ChannelMix {
    /// The channels of the stream that are not heard. The mock backend
    /// plays no channels.
    #[cfg(any(feature = "gst", test))]
    pub(crate) fn silenced(self, channels: u32) -> u64 {
        let present = stream_channels(channels);
        let soloed = self.soloed & present;
        let left_out = if soloed == 0 { 0 } else { !soloed };
        (self.muted | left_out) & present
    }

    /// The channels of the stream that are muted.
    pub(crate) fn muted_in(self, channels: u32) -> u64 {
        self.muted & stream_channels(channels)
    }

    /// The channels of the stream that are soloed.
    pub(crate) fn soloed_in(self, channels: u32) -> u64 {
        self.soloed & stream_channels(channels)
    }

    pub(crate) fn toggle_solo(&mut self, channel: u8) {
        self.soloed ^= channel_bit(channel);
    }

    pub(crate) fn toggle_mute(&mut self, channel: u8) {
        self.muted ^= channel_bit(channel);
    }

    /// Apply to a stream with fewer channels than the one before it. Mutes
    /// that silence all of it were made for other channels, like a mute of
    /// its left channel on a stereo track before a mono one. Those of its
    /// channels are dropped, as nothing else tells the user why the track is
    /// silent; mutes of channels it lacks stay for a later stream. The mock
    /// backend's streams have no channel count, so only `GStreamer` streams
    /// narrow.
    #[cfg(any(feature = "gst", test))]
    pub(crate) fn fit_narrower_stream(&mut self, channels: u32) {
        let present = stream_channels(channels);
        if self.silenced(channels) == present {
            self.muted &= !present;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn soloed(channels: &[u8]) -> ChannelMix {
        let mut mix = ChannelMix::default();
        for &channel in channels {
            mix.toggle_solo(channel);
        }
        mix
    }

    #[test]
    fn only_soloed_channels_are_heard() {
        let mix = soloed(&[1, 3]);
        assert_eq!(mix.silenced(6), 0b11_0101);
        assert_eq!(mix.soloed_in(6), 0b1010);
    }

    #[test]
    fn a_stream_of_unknown_width_counts_every_channel() {
        let mix = soloed(&[1]);
        assert_eq!(mix.silenced(0), u64::MAX ^ 0b10);
        assert_eq!(mix.soloed_in(0), 0b10);
    }

    #[test]
    fn a_channel_can_be_muted_and_soloed_and_is_silent() {
        let mut mix = soloed(&[0, 1]);
        mix.toggle_mute(1);
        assert_eq!(mix.muted_in(4), 0b0010);
        assert_eq!(mix.soloed_in(4), 0b0011);
        assert_eq!(mix.silenced(4), 0b1110);
    }

    #[test]
    fn solos_leave_the_mutes_as_they_are() {
        let mut mix = ChannelMix::default();
        mix.toggle_mute(3);
        mix.toggle_solo(0);
        mix.toggle_solo(1);
        mix.toggle_solo(0);
        assert_eq!(mix.muted_in(4), 0b1000);
        assert_eq!(mix.silenced(4), 0b1101);
        mix.toggle_solo(1);
        assert_eq!(
            mix,
            ChannelMix {
                muted: 0b1000,
                soloed: 0
            }
        );
    }

    #[test]
    fn muting_and_unmuting_a_soloed_channel_keeps_the_solos() {
        let before = soloed(&[0, 2]);
        let mut mix = before;
        mix.toggle_mute(0);
        assert_eq!(mix.soloed_in(4), 0b0101);
        assert_eq!(mix.silenced(4), 0b1011);
        mix.toggle_mute(0);
        assert_eq!(mix, before);
    }

    #[test]
    fn muting_and_unmuting_another_channel_keeps_the_solos() {
        let before = soloed(&[1, 2]);
        let mut mix = before;
        mix.toggle_mute(3);
        assert_eq!(mix.soloed_in(4), 0b0110);
        assert_eq!(mix.muted_in(4), 0b1000);
        assert_eq!(mix.silenced(4), 0b1001);
        mix.toggle_mute(3);
        assert_eq!(mix, before);
    }

    #[test]
    fn a_mute_made_during_solos_outlasts_them() {
        let mut mix = soloed(&[1]);
        mix.toggle_mute(3);
        mix.toggle_solo(1);
        assert_eq!(mix.silenced(4), 0b1000);
    }

    #[test]
    fn muting_every_soloed_channel_silences_the_stream() {
        let mut mix = soloed(&[1]);
        mix.toggle_mute(1);
        assert_eq!(mix.silenced(4), 0b1111);
    }

    #[test]
    fn a_narrower_stream_keeps_the_mutes_of_channels_it_lacks() {
        let mut mix = ChannelMix::default();
        for channel in [0, 1, 5] {
            mix.toggle_mute(channel);
        }
        mix.fit_narrower_stream(2);
        assert_eq!(mix.silenced(2), 0);
        assert_eq!(mix.silenced(6), 0b10_0000);
    }

    #[test]
    fn solos_past_the_stream_do_not_silence_it() {
        let mix = soloed(&[3, 5]);
        assert_eq!(mix.silenced(2), 0);
        assert_eq!(mix.soloed_in(2), 0);
    }

    #[test]
    fn solos_past_the_stream_leave_its_mutes_in_force() {
        let mut mix = soloed(&[3]);
        mix.muted = 0b01;
        assert_eq!(mix.silenced(2), 0b01);
        // They apply again to a stream that has their channel.
        assert_eq!(mix.silenced(6), 0b11_0111);
    }

    #[test]
    fn mutes_past_the_stream_do_nothing() {
        let mix = ChannelMix {
            muted: 0b11_0000,
            soloed: 0,
        };
        assert_eq!(mix.silenced(2), 0);
        assert_eq!(mix.silenced(6), 0b11_0000);
    }

    #[test]
    fn the_last_channel_of_a_mask_works() {
        let mut mix = ChannelMix::default();
        mix.toggle_solo(63);
        mix.toggle_solo(0);
        assert_eq!(mix.soloed_in(64), (1 << 63) | 1);
        assert_eq!(mix.silenced(64), !((1 << 63) | 1));
        // More channels than a mask has are out of its reach.
        assert_eq!(mix.silenced(200), !((1 << 63) | 1));
    }

    #[test]
    fn a_narrower_stream_does_not_start_silenced_by_mutes() {
        let mut mix = ChannelMix {
            muted: 0b0011,
            soloed: 0,
        };
        mix.fit_narrower_stream(2);
        assert_eq!(mix, ChannelMix::default());
    }

    #[test]
    fn a_narrower_stream_keeps_mutes_that_leave_it_audible() {
        let mut mix = ChannelMix {
            muted: 0b1001,
            soloed: 0,
        };
        mix.fit_narrower_stream(2);
        assert_eq!(mix.muted, 0b1001);
        assert_eq!(mix.silenced(2), 0b01);
    }

    #[test]
    fn a_narrower_stream_does_not_start_silenced_by_a_mute_of_its_soloed_channel() {
        let mut mix = ChannelMix {
            muted: 0b01,
            soloed: 0b01,
        };
        mix.fit_narrower_stream(2);
        assert_eq!(
            mix,
            ChannelMix {
                muted: 0,
                soloed: 0b01
            }
        );
        assert_eq!(mix.silenced(2), 0b10);
    }

    #[test]
    fn a_narrower_stream_past_every_solo_drops_mutes_that_silence_it() {
        let mut mix = ChannelMix {
            muted: 0b11,
            soloed: 0b1000,
        };
        mix.fit_narrower_stream(2);
        assert_eq!(
            mix,
            ChannelMix {
                muted: 0,
                soloed: 0b1000
            }
        );
    }
}
