// SPDX-License-Identifier: GPL-3.0-or-later

//! Which channels of a stream are heard.
//!
//! - While at least one channel of the stream is soloed, exactly the soloed
//!   channels are heard, also those muted by hand.
//! - Clearing the last solo brings back the mutes from before the first one.
//! - Toggling the mute of a soloed channel clears its solo, as its solo
//!   button does.
//! - Toggling the mute of any other channel while solos are active turns
//!   them into plain mutes: the solos go out, what is heard stays, and the
//!   toggled channel is heard too. The mutes from before the first solo are
//!   gone.
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
    /// Muted by hand. Solos leave this as it is.
    pub(crate) muted: u64,
    pub(crate) soloed: u64,
}

impl ChannelMix {
    /// The channels of the stream that are not heard.
    pub(crate) fn silenced(self, channels: u32) -> u64 {
        let present = stream_channels(channels);
        let soloed = self.soloed & present;
        if soloed == 0 {
            self.muted & present
        } else {
            !soloed & present
        }
    }

    /// The channels of the stream that are soloed.
    pub(crate) fn soloed_in(self, channels: u32) -> u64 {
        self.soloed & stream_channels(channels)
    }

    pub(crate) fn toggle_solo(&mut self, channel: u8) {
        self.soloed ^= channel_bit(channel);
    }

    pub(crate) fn toggle_mute(&mut self, channel: u8, channels: u32) {
        let bit = channel_bit(channel);
        let soloed = self.soloed_in(channels);
        if soloed & bit != 0 {
            self.soloed ^= bit;
            return;
        }
        if soloed != 0 {
            self.muted = self.silenced(channels);
            self.soloed = 0;
        }
        self.muted ^= bit;
    }

    /// Apply to a stream with fewer channels than the one before it. Mutes
    /// that silence all of it were made for other channels: a solo of a
    /// channel it lacks, turned into mutes, or a mute of its left channel on
    /// a stereo track before a mono one. Those of its channels are dropped,
    /// as nothing else tells the user why the track is silent; mutes of
    /// channels it lacks stay for a later stream. The mock backend's streams
    /// have no channel count, so only `GStreamer` streams narrow.
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
    fn a_solo_wins_over_a_mute_of_its_channel() {
        let mut mix = ChannelMix::default();
        mix.toggle_mute(1, 4);
        mix.toggle_solo(1);
        assert_eq!(mix.silenced(4), 0b1101);
    }

    #[test]
    fn clearing_the_last_solo_restores_the_mutes() {
        let mut mix = ChannelMix::default();
        mix.toggle_mute(3, 4);
        mix.toggle_solo(0);
        mix.toggle_solo(1);
        mix.toggle_solo(0);
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
    fn muting_a_soloed_channel_clears_only_its_solo() {
        let mut mix = soloed(&[0, 2]);
        mix.toggle_mute(0, 4);
        assert_eq!(mix.soloed_in(4), 0b0100);
        assert_eq!(mix.silenced(4), 0b1011);
        // The last one brings the mutes back, here none.
        mix.toggle_mute(2, 4);
        assert_eq!(mix, ChannelMix::default());
    }

    #[test]
    fn muting_another_channel_turns_the_solos_into_mutes() {
        let mut mix = ChannelMix::default();
        mix.toggle_mute(0, 4);
        mix.toggle_solo(1);
        mix.toggle_solo(2);
        // Channel 3 was silent, and is heard with what was heard.
        mix.toggle_mute(3, 4);
        assert_eq!(
            mix,
            ChannelMix {
                muted: 0b0001,
                soloed: 0
            }
        );
    }

    #[test]
    fn a_narrower_stream_keeps_the_mutes_of_channels_it_lacks() {
        let mut mix = ChannelMix::default();
        for channel in [0, 1, 5] {
            mix.toggle_mute(channel, 6);
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
        // And a mute there does not end them: they are not active.
        mix.toggle_mute(1, 2);
        assert_eq!(
            mix,
            ChannelMix {
                muted: 0b11,
                soloed: 0b1000
            }
        );
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
    fn solos_keep_a_narrower_stream_audible_with_every_channel_muted() {
        let mut mix = ChannelMix {
            muted: 0b11,
            soloed: 0b01,
        };
        mix.fit_narrower_stream(2);
        assert_eq!(
            mix,
            ChannelMix {
                muted: 0b11,
                soloed: 0b01
            }
        );
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
