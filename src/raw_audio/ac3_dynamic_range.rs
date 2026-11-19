// SPDX-License-Identifier: GPL-3.0-or-later

//! Finds the dynamic range words of an AC3 frame, so that it decodes at the
//! full dynamic range it was mixed at.
//!
//! Each of the 6 audio blocks of a frame may carry a dynamic range word
//! (`dynrng`, and `dynrng2` for the second programme of 1+1 dual mono), a
//! gain that a decoder may apply to compress the audio, by 15 dB or more.
//! `FFmpeg`'s decoder applies them until its first flush, which zeroes how
//! much to apply (`drc_scale`) along with the stream's state, and
//! `gst-libav` cannot set that. A word of 0 is a gain of exactly 1, so a
//! frame whose words are all 0 decodes the same either way.
//!
//! The blocks follow each other without byte alignment, and where one ends
//! depends on how many bits its mantissas take, which the decoder works out
//! from the exponents and the bit allocation parameters (A/52 section 7.2).
//! So this module parses each block as `FFmpeg`'s decoder does, up to the
//! word of the last block. A frame it cannot parse is left as it is: see
//! [`Unparsed`].

use super::bitstream::plain_ac3_frame_len;

/// Why the words of a frame were not found. Such a frame is passed on
/// unchanged, as decoding it changed would be no better.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum Unparsed {
    /// Not one plain AC3 frame of the length its header tells: E-AC3, whose
    /// blocks have another syntax, or not a frame at all.
    NotPlainAc3,
    /// A field ran past the end of the audio blocks.
    PastEnd,
    /// A value that the decoder rejects, or a block that has it use what it
    /// kept from the frame before, which this module does not know:
    /// exponents, SNR offsets, a coupling leak, or a delta bit allocation
    /// that no block of the frame has set yet. A/52 requires the first block
    /// to set what the frame uses. The text names what.
    Invalid(&'static str),
}

/// Where a frame's dynamic range flags are: `dynrnge` (and `dynrng2e`) of
/// each block, each followed by its 8-bit word if set.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct RangeFlags {
    at: [usize; 12],
    len: usize,
}

impl RangeFlags {
    fn new() -> Self {
        Self {
            at: [0; 12],
            len: 0,
        }
    }

    fn push(&mut self, at: usize) {
        self.at[self.len] = at;
        self.len += 1;
    }

    /// The bit positions of the flags, in the order of the stream.
    pub(super) fn positions(&self) -> &[usize] {
        &self.at[..self.len]
    }

    /// The words of `frame` that are set and not 0, by bit position.
    fn compressing<'a>(&'a self, frame: &'a [u8]) -> impl Iterator<Item = usize> + 'a {
        self.positions()
            .iter()
            .filter(move |&&flag| bit_at(frame, flag) == 1 && read_bits(frame, flag + 1, 8) != 0)
            .map(|flag| flag + 1)
    }

    /// Whether any word of `frame` changes its gain.
    pub(super) fn compress(&self, frame: &[u8]) -> bool {
        self.compressing(frame).next().is_some()
    }

    /// Set each word of `frame` that changes its gain to 0, a gain of 1.
    /// The caller sets the CRCs again.
    pub(super) fn neutralize(&self, frame: &mut [u8]) {
        let mut words = [0; 12];
        let mut count = 0;
        for word in self.compressing(frame) {
            words[count] = word;
            count += 1;
        }
        for &word in &words[..count] {
            for index in word..word + 8 {
                frame[index / 8] &= !(0x80 >> (index % 8));
            }
        }
    }
}

/// The bit at `index` of `data`, most significant first.
fn bit_at(data: &[u8], index: usize) -> u32 {
    u32::from((data[index / 8] >> (7 - index % 8)) & 1)
}

/// `count` bits of `data` from `index` on, at most 16.
fn read_bits(data: &[u8], index: usize, count: usize) -> usize {
    (index..index + count).fold(0, |value, bit| {
        (value << 1) | usize::from(bit_at(data, bit) == 1)
    })
}

/// Reads the bits of a frame up to its last 2 bytes, `crc2`.
struct Reader<'a> {
    frame: &'a [u8],
    at: usize,
    end: usize,
}

impl Reader<'_> {
    fn read(&mut self, count: usize) -> Result<usize, Unparsed> {
        if self.at + count > self.end {
            return Err(Unparsed::PastEnd);
        }
        let value = read_bits(self.frame, self.at, count);
        self.at += count;
        Ok(value)
    }

    fn flag(&mut self) -> Result<bool, Unparsed> {
        Ok(self.read(1)? == 1)
    }

    fn skip(&mut self, count: usize) -> Result<(), Unparsed> {
        if self.at + count > self.end {
            return Err(Unparsed::PastEnd);
        }
        self.at += count;
        Ok(())
    }

    /// `count` bits, at most 8, as a number to reckon with the tables'.
    fn number(&mut self, count: usize) -> Result<i32, Unparsed> {
        i32::try_from(self.read(count)?).map_err(|_| Unparsed::Invalid("a field too long"))
    }
}

/// Full-bandwidth channels by `acmod` (A/52 Table 5.8).
pub(super) const FULL_BANDWIDTH_CHANNELS: [usize; 8] = [2, 1, 2, 3, 3, 4, 4, 5];

/// The coupling channel's index; the full-bandwidth channels follow from 1,
/// then the LFE.
const CPL: usize = 0;
const MAX_CHANNELS: usize = 7;
const CRITICAL_BANDS: usize = 50;
/// A/52 exponent strategies; `D25` and `D45` share exponents in pairs and
/// fours.
const EXP_REUSE: usize = 0;
const EXP_D45: usize = 3;
/// A/52 delta bit allocation strategies.
const DBA_REUSE: usize = 0;
const DBA_NEW: usize = 1;
const DBA_NONE: usize = 2;
const DBA_RESERVED: usize = 3;

/// The first coefficient of each critical band, and the end of the last.
const BAND_START: [usize; CRITICAL_BANDS + 1] = [
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,
    26, 27, 28, 31, 34, 37, 40, 43, 46, 49, 55, 61, 67, 73, 79, 85, 97, 109, 121, 133, 157, 181,
    205, 229, 253,
];

/// The critical band of each coefficient.
const BIN_TO_BAND: [usize; 253] = {
    let mut table = [0; 253];
    let mut band = 0;
    let mut bin = 0;
    while bin < 253 {
        if bin == BAND_START[band + 1] {
            band += 1;
        }
        table[bin] = band;
        bin += 1;
    }
    table
};

/// The log-addition table of the PSD integration (A/52 Table 7.14).
const LOG_ADD: [u8; 256] = [
    0x40, 0x3f, 0x3e, 0x3d, 0x3c, 0x3b, 0x3a, 0x39, 0x38, 0x37, 0x36, 0x35, 0x34, 0x34, 0x33, 0x32,
    0x31, 0x30, 0x2f, 0x2f, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x29, 0x29, 0x28, 0x27, 0x26, 0x26,
    0x25, 0x24, 0x24, 0x23, 0x23, 0x22, 0x21, 0x21, 0x20, 0x20, 0x1f, 0x1e, 0x1e, 0x1d, 0x1d, 0x1c,
    0x1c, 0x1b, 0x1b, 0x1a, 0x1a, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x16, 0x15, 0x15, 0x15,
    0x14, 0x14, 0x13, 0x13, 0x13, 0x12, 0x12, 0x12, 0x11, 0x11, 0x11, 0x10, 0x10, 0x10, 0x0f, 0x0f,
    0x0f, 0x0e, 0x0e, 0x0e, 0x0d, 0x0d, 0x0d, 0x0d, 0x0c, 0x0c, 0x0c, 0x0c, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x09, 0x09, 0x09, 0x09, 0x09, 0x08, 0x08, 0x08, 0x08, 0x08, 0x08,
    0x07, 0x07, 0x07, 0x07, 0x07, 0x07, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x05, 0x05,
    0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04,
    0x04, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x02,
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
    0x02, 0x02, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
];

/// The hearing threshold of each critical band at 48, 44.1, and 32 kHz
/// (A/52 Table 7.15).
const HEARING_THRESHOLD: [[i32; 3]; CRITICAL_BANDS] = [
    [0x04d0, 0x04f0, 0x0580],
    [0x04d0, 0x04f0, 0x0580],
    [0x0440, 0x0460, 0x04b0],
    [0x0400, 0x0410, 0x0450],
    [0x03e0, 0x03e0, 0x0420],
    [0x03c0, 0x03d0, 0x03f0],
    [0x03b0, 0x03c0, 0x03e0],
    [0x03b0, 0x03b0, 0x03d0],
    [0x03a0, 0x03b0, 0x03c0],
    [0x03a0, 0x03a0, 0x03b0],
    [0x03a0, 0x03a0, 0x03b0],
    [0x03a0, 0x03a0, 0x03b0],
    [0x03a0, 0x03a0, 0x03a0],
    [0x0390, 0x03a0, 0x03a0],
    [0x0390, 0x0390, 0x03a0],
    [0x0390, 0x0390, 0x03a0],
    [0x0380, 0x0390, 0x03a0],
    [0x0380, 0x0380, 0x03a0],
    [0x0370, 0x0380, 0x03a0],
    [0x0370, 0x0380, 0x03a0],
    [0x0360, 0x0370, 0x0390],
    [0x0360, 0x0370, 0x0390],
    [0x0350, 0x0360, 0x0390],
    [0x0350, 0x0360, 0x0390],
    [0x0340, 0x0350, 0x0380],
    [0x0340, 0x0350, 0x0380],
    [0x0330, 0x0340, 0x0380],
    [0x0320, 0x0340, 0x0370],
    [0x0310, 0x0320, 0x0360],
    [0x0300, 0x0310, 0x0350],
    [0x02f0, 0x0300, 0x0340],
    [0x02f0, 0x02f0, 0x0330],
    [0x02f0, 0x02f0, 0x0320],
    [0x02f0, 0x02f0, 0x0310],
    [0x0300, 0x02f0, 0x0300],
    [0x0310, 0x0300, 0x02f0],
    [0x0340, 0x0320, 0x02f0],
    [0x0390, 0x0350, 0x02f0],
    [0x03e0, 0x0390, 0x0300],
    [0x0420, 0x03e0, 0x0310],
    [0x0460, 0x0420, 0x0330],
    [0x0490, 0x0450, 0x0350],
    [0x04a0, 0x04a0, 0x03c0],
    [0x0460, 0x0490, 0x0410],
    [0x0440, 0x0460, 0x0470],
    [0x0440, 0x0440, 0x04a0],
    [0x0520, 0x0480, 0x0460],
    [0x0800, 0x0630, 0x0440],
    [0x0840, 0x0840, 0x0450],
    [0x0840, 0x0840, 0x04e0],
];

/// The bit allocation pointer of each masked PSD address (A/52 Table 7.16).
const BAP: [u8; 64] = [
    0, 1, 1, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
    10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14, 14,
    15, 15, 15, 15, 15, 15, 15, 15, 15,
];

/// Bits of an ungrouped mantissa by bit allocation pointer (A/52 Table
/// 7.18); pointers 1, 2, and 4 group their mantissas.
const MANTISSA_BITS: [usize; 16] = [0, 0, 0, 3, 0, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16];

const SLOW_DECAY: [i32; 4] = [0x0f, 0x11, 0x13, 0x15];
const FAST_DECAY: [i32; 4] = [0x3f, 0x53, 0x67, 0x7b];
const SLOW_GAIN: [i32; 4] = [0x540, 0x4d8, 0x478, 0x410];
const DB_PER_BIT: [i32; 4] = [0x000, 0x700, 0x900, 0xb00];
const FLOOR: [i32; 8] = [0x2f0, 0x2b0, 0x270, 0x230, 0x1f0, 0x170, 0x0f0, -0x800];
const FAST_GAIN: [i32; 8] = [0x080, 0x100, 0x180, 0x200, 0x280, 0x300, 0x380, 0x400];

/// What the blocks of a frame say about a channel and keep for the blocks
/// after, until a block says otherwise. The decoder keeps them from frame to
/// frame too, so a block that reuses one no block of this frame has set
/// leaves the frame unparsed.
#[derive(Clone, Copy)]
struct Channel {
    start: usize,
    end: usize,
    exponents: [u8; 256],
    /// The coefficients whose exponents a block of this frame set.
    exponents_set: Option<(usize, usize)>,
    offsets: Option<Offsets>,
    in_coupling: bool,
    /// The delta bit allocation strategy a block of this frame set.
    dba_mode: Option<usize>,
    /// How many delta bit allocation segments a block of this frame set.
    dba_segments: Option<usize>,
    dba_offsets: [usize; 8],
    dba_lengths: [usize; 8],
    dba_values: [i32; 8],
}

impl Channel {
    const NEW: Self = Self {
        start: 0,
        end: 0,
        exponents: [0; 256],
        exponents_set: None,
        offsets: None,
        in_coupling: false,
        dba_mode: None,
        dba_segments: None,
        dba_offsets: [0; 8],
        dba_lengths: [0; 8],
        dba_values: [0; 8],
    };
}

/// A channel's SNR offset and fast gain.
#[derive(Clone, Copy)]
struct Offsets {
    snr: i32,
    fast_gain: i32,
}

/// The parameters of the bit allocation that the frame sets for all
/// channels.
#[derive(Default)]
struct BitAllocation {
    slow_decay: i32,
    fast_decay: i32,
    slow_gain: i32,
    db_per_bit: i32,
    floor: i32,
    cpl_fast_leak: i32,
    cpl_slow_leak: i32,
    cpl_leak_set: bool,
    /// `fscod`, and the shift of a half- or quarter-rate stream.
    sample_rate_code: usize,
    rate_shift: usize,
}

/// One frame as its blocks are parsed: what [`Channel`] and
/// [`BitAllocation`] hold, for the channels of [`CPL`], the full-bandwidth
/// channels, and the LFE.
struct Frame<'a> {
    reader: Reader<'a>,
    acmod: usize,
    full_bandwidth: usize,
    /// The index of the last channel: the LFE if there is one.
    last_channel: usize,
    lfe: Option<usize>,
    channels: [Channel; MAX_CHANNELS],
    allocation: BitAllocation,
    cpl_in_use: bool,
    phase_flags_in_use: bool,
    cpl_bands: usize,
}

/// Find the dynamic range flags of `frame`, one whole plain AC3 frame.
pub(super) fn find_range_flags(frame: &[u8]) -> Result<RangeFlags, Unparsed> {
    if plain_ac3_frame_len(frame) != Some(frame.len()) {
        return Err(Unparsed::NotPlainAc3);
    }
    let mut parsed = Frame::new(frame)?;
    let mut flags = RangeFlags::new();
    for block in 0..6 {
        parsed.block(block, &mut flags)?;
    }
    Ok(flags)
}

impl<'a> Frame<'a> {
    /// Parse the header of `frame` (A/52 section 5.4.2).
    fn new(frame: &'a [u8]) -> Result<Self, Unparsed> {
        let mut reader = Reader {
            frame,
            // The sync word and `crc1`.
            at: 32,
            end: (frame.len() - 2) * 8,
        };
        let sample_rate_code = reader.read(2)?;
        // `frmsizecod`, which `plain_ac3_frame_len` checked.
        reader.skip(6)?;
        let bsid = reader.read(5)?;
        // `bsmod`.
        reader.skip(3)?;
        let acmod = reader.read(3)?;
        if acmod == 2 {
            // `dsurmod`.
            reader.skip(2)?;
        } else {
            if acmod & 1 == 1 && acmod != 1 {
                // `cmixlev`.
                reader.skip(2)?;
            }
            if acmod & 4 != 0 {
                // `surmixlev`.
                reader.skip(2)?;
            }
        }
        let lfe_on = reader.flag()?;
        let programmes = if acmod == 0 { 2 } else { 1 };
        for _ in 0..programmes {
            // `dialnorm`, then `compr`, `langcod`, and `mixlevel` with
            // `roomtyp`, each if flagged.
            reader.skip(5)?;
            for len in [8, 8, 7] {
                if reader.flag()? {
                    reader.skip(len)?;
                }
            }
        }
        // `copyrightb` and `origbs`, then `timecod1` and `timecod2`, or
        // with `bsid` 6 the extended fields of the same lengths, each if
        // flagged.
        reader.skip(2)?;
        for _ in 0..2 {
            if reader.flag()? {
                reader.skip(14)?;
            }
        }
        if reader.flag()? {
            let addbsil = reader.read(6)?;
            reader.skip(8 * (addbsil + 1))?;
        }

        let full_bandwidth = FULL_BANDWIDTH_CHANNELS[acmod];
        let lfe = lfe_on.then_some(full_bandwidth + 1);
        let mut channels = [Channel::NEW; MAX_CHANNELS];
        if let Some(lfe) = lfe {
            channels[lfe].end = 7;
        }
        Ok(Self {
            reader,
            acmod,
            full_bandwidth,
            last_channel: full_bandwidth + usize::from(lfe_on),
            lfe,
            channels,
            allocation: BitAllocation {
                sample_rate_code,
                rate_shift: bsid.max(8) - 8,
                ..BitAllocation::default()
            },
            cpl_in_use: false,
            phase_flags_in_use: false,
            cpl_bands: 0,
        })
    }

    /// The channels with exponents and bit allocation in this block: the
    /// coupling channel if coupling is in use, then the others.
    fn coded_channels(&self) -> std::ops::RangeInclusive<usize> {
        usize::from(!self.cpl_in_use)..=self.last_channel
    }

    /// Parse block `block` (A/52 section 5.4.3), adding its flags to
    /// `flags`, as far as the start of the next block, or for the last
    /// block as far as its flags.
    fn block(&mut self, block: usize, flags: &mut RangeFlags) -> Result<(), Unparsed> {
        // `blksw` and `dithflag` of each full-bandwidth channel.
        self.reader.skip(2 * self.full_bandwidth)?;
        let programmes = if self.acmod == 0 { 2 } else { 1 };
        for _ in 0..programmes {
            flags.push(self.reader.at);
            if self.reader.flag()? {
                self.reader.skip(8)?;
            }
        }
        if block == 5 {
            return Ok(());
        }

        self.coupling_strategy(block)?;
        if self.cpl_in_use {
            self.coupling_coordinates(block)?;
        }
        self.rematrixing()?;
        let strategies = self.exponent_strategies()?;
        self.exponents(&strategies)?;
        self.bit_allocation_parameters(block)?;
        let bits = self.mantissa_bits()?;
        // `skipl` bytes of unused data, if `skiple`.
        if self.reader.flag()? {
            let skipped = self.reader.read(9)?;
            self.reader.skip(8 * skipped)?;
        }
        self.reader.skip(bits)
    }

    fn coupling_strategy(&mut self, block: usize) -> Result<(), Unparsed> {
        if !self.reader.flag()? {
            return if block == 0 {
                Err(Unparsed::Invalid("no coupling strategy in block 0"))
            } else {
                Ok(())
            };
        }
        self.cpl_in_use = self.reader.flag()?;
        if !self.cpl_in_use {
            for channel in 1..=self.full_bandwidth {
                self.channels[channel].in_coupling = false;
            }
            self.phase_flags_in_use = false;
            return Ok(());
        }
        if self.acmod < 2 {
            return Err(Unparsed::Invalid("coupling in mono or dual mono"));
        }
        for channel in 1..=self.full_bandwidth {
            self.channels[channel].in_coupling = self.reader.flag()?;
        }
        if self.acmod == 2 {
            self.phase_flags_in_use = self.reader.flag()?;
        }
        let start_subband = self.reader.read(4)?;
        let end_subband = self.reader.read(4)? + 3;
        if start_subband >= end_subband {
            return Err(Unparsed::Invalid("an empty coupling range"));
        }
        self.channels[CPL].start = start_subband * 12 + 37;
        self.channels[CPL].end = end_subband * 12 + 37;
        // `cplbndstrc`: each set bit merges a subband into the band before.
        let subbands = end_subband - start_subband;
        self.cpl_bands = subbands;
        for _ in 1..subbands {
            if self.reader.flag()? {
                self.cpl_bands -= 1;
            }
        }
        Ok(())
    }

    fn coupling_coordinates(&mut self, block: usize) -> Result<(), Unparsed> {
        let mut coordinates = false;
        for channel in 1..=self.full_bandwidth {
            if !self.channels[channel].in_coupling {
                continue;
            }
            if self.reader.flag()? {
                coordinates = true;
                // `mstrcplco`, then `cplcoexp` and `cplcomant` of each band.
                self.reader.skip(2 + 8 * self.cpl_bands)?;
            } else if block == 0 {
                return Err(Unparsed::Invalid("no coupling coordinates in block 0"));
            }
        }
        if self.acmod == 2 && coordinates && self.phase_flags_in_use {
            self.reader.skip(self.cpl_bands)?;
        }
        Ok(())
    }

    /// `rematstr` and `rematflg` of a stereo stream (A/52 section 7.5).
    fn rematrixing(&mut self) -> Result<(), Unparsed> {
        if self.acmod != 2 || !self.reader.flag()? {
            return Ok(());
        }
        let cpl_start = self.channels[CPL].start;
        let bands = if self.cpl_in_use && cpl_start <= 61 {
            3 - usize::from(cpl_start == 37)
        } else {
            4
        };
        self.reader.skip(bands)
    }

    /// The exponent strategies of the coded channels, and the bandwidth of
    /// the full-bandwidth channels with new exponents.
    fn exponent_strategies(&mut self) -> Result<[usize; MAX_CHANNELS], Unparsed> {
        let mut strategies = [EXP_REUSE; MAX_CHANNELS];
        for channel in self.coded_channels() {
            let bits = if Some(channel) == self.lfe { 1 } else { 2 };
            strategies[channel] = self.reader.read(bits)?;
        }
        let cpl_start = self.channels[CPL].start;
        let full_bandwidth = 1..=self.full_bandwidth;
        for (state, &strategy) in self.channels[full_bandwidth.clone()]
            .iter_mut()
            .zip(&strategies[full_bandwidth])
        {
            if strategy == EXP_REUSE {
                continue;
            }
            state.start = 0;
            state.end = if state.in_coupling {
                cpl_start
            } else {
                let bandwidth = self.reader.read(6)?;
                if bandwidth > 60 {
                    return Err(Unparsed::Invalid("a bandwidth code above 60"));
                }
                bandwidth * 3 + 73
            };
        }
        Ok(strategies)
    }

    fn exponents(&mut self, strategies: &[usize; MAX_CHANNELS]) -> Result<(), Unparsed> {
        for channel in self.coded_channels() {
            let strategy = strategies[channel];
            if strategy == EXP_REUSE {
                continue;
            }
            let group_size = 3 << (strategy - 1);
            let state = &self.channels[channel];
            let groups = if channel == CPL {
                (state.end - state.start) / group_size
            } else if Some(channel) == self.lfe {
                2
            } else {
                (state.end + group_size - 4) / group_size
            };
            let absolute = self.reader.read(4)?;
            // The coupling channel's first exponent is in steps of 2 and
            // only a reference; the others' is that of coefficient 0.
            let (mut exponent, first) = if channel == CPL {
                (absolute << 1, state.start)
            } else {
                (absolute, 0)
            };
            let repeat = if strategy == EXP_D45 { 4 } else { strategy };
            self.channels[channel].exponents_set = Some((state.start, state.end));
            if first == 0 {
                self.store_exponents(channel, 0, 1, exponent)?;
            }
            let first = first.max(1);
            let mut bin = first;
            for _ in 0..groups {
                let grouped = self.reader.read(7)?;
                if grouped >= 125 {
                    return Err(Unparsed::Invalid("an exponent group above 124"));
                }
                for delta in [grouped / 25, (grouped / 5) % 5, grouped % 5] {
                    exponent = (exponent + delta)
                        .checked_sub(2)
                        .filter(|exponent| *exponent <= 24)
                        .ok_or(Unparsed::Invalid("an exponent out of range"))?;
                    self.store_exponents(channel, bin, repeat, exponent)?;
                    bin += repeat;
                }
            }
            if channel != CPL && Some(channel) != self.lfe {
                // `gainrng`.
                self.reader.skip(2)?;
            }
        }
        Ok(())
    }

    /// Set `count` exponents of `channel` from coefficient `bin` on.
    fn store_exponents(
        &mut self,
        channel: usize,
        bin: usize,
        count: usize,
        exponent: usize,
    ) -> Result<(), Unparsed> {
        let exponent = u8::try_from(exponent).map_err(|_| Unparsed::Invalid("an exponent"))?;
        self.channels[channel]
            .exponents
            .get_mut(bin..bin + count)
            .ok_or(Unparsed::Invalid("exponents past the last coefficient"))?
            .fill(exponent);
        Ok(())
    }

    /// `baie`, `snroffste`, `cplleake`, and `deltbaie`, with what they set.
    fn bit_allocation_parameters(&mut self, block: usize) -> Result<(), Unparsed> {
        if self.reader.flag()? {
            let allocation = &mut self.allocation;
            allocation.slow_decay = SLOW_DECAY[self.reader.read(2)?] >> allocation.rate_shift;
            allocation.fast_decay = FAST_DECAY[self.reader.read(2)?] >> allocation.rate_shift;
            allocation.slow_gain = SLOW_GAIN[self.reader.read(2)?];
            allocation.db_per_bit = DB_PER_BIT[self.reader.read(2)?];
            allocation.floor = FLOOR[self.reader.read(3)?];
        } else if block == 0 {
            return Err(Unparsed::Invalid("no bit allocation parameters in block 0"));
        }

        if self.reader.flag()? {
            let coarse = (self.reader.number(6)? - 15) << 4;
            for channel in self.coded_channels() {
                let fine = self.reader.number(4)?;
                self.channels[channel].offsets = Some(Offsets {
                    snr: (coarse + fine) << 2,
                    fast_gain: FAST_GAIN[self.reader.read(3)?],
                });
            }
        } else if block == 0 {
            return Err(Unparsed::Invalid("no SNR offsets in block 0"));
        }

        if self.cpl_in_use {
            if self.reader.flag()? {
                self.allocation.cpl_fast_leak = self.reader.number(3)?;
                self.allocation.cpl_slow_leak = self.reader.number(3)?;
                self.allocation.cpl_leak_set = true;
            } else if block == 0 {
                return Err(Unparsed::Invalid("no coupling leak in block 0"));
            }
        }

        let delta_channels = usize::from(!self.cpl_in_use)..=self.full_bandwidth;
        if self.reader.flag()? {
            for channel in delta_channels.clone() {
                let mode = self.reader.read(2)?;
                if mode == DBA_RESERVED {
                    return Err(Unparsed::Invalid("a reserved delta bit allocation"));
                }
                if mode == DBA_REUSE && self.channels[channel].dba_segments.is_none() {
                    return Err(Unparsed::Invalid(
                        "a delta bit allocation from the frame before",
                    ));
                }
                self.channels[channel].dba_mode = Some(mode);
            }
            for channel in delta_channels {
                if self.channels[channel].dba_mode != Some(DBA_NEW) {
                    continue;
                }
                let segments = self.reader.read(3)? + 1;
                let state = &mut self.channels[channel];
                state.dba_segments = Some(segments);
                for segment in 0..segments {
                    state.dba_offsets[segment] = self.reader.read(5)?;
                    state.dba_lengths[segment] = self.reader.read(4)?;
                    state.dba_values[segment] = self.reader.number(3)?;
                }
            }
        } else if block == 0 {
            for channel in &mut self.channels {
                channel.dba_mode = Some(DBA_NONE);
            }
        }
        Ok(())
    }

    /// The bits that the mantissas of this block take. Pointers 1 and 2
    /// group 3 mantissas, and 4 groups 2, across the channels of a block.
    fn mantissa_bits(&self) -> Result<usize, Unparsed> {
        let mut counts = [0_usize; 16];
        let mut coupling_counted = false;
        for channel in 1..=self.last_channel {
            self.count_pointers(channel, &mut counts)?;
            if self.channels[channel].in_coupling && !coupling_counted {
                self.count_pointers(CPL, &mut counts)?;
                coupling_counted = true;
            }
        }
        let grouped =
            counts[1].div_ceil(3) * 5 + counts[2].div_ceil(3) * 7 + counts[4].div_ceil(2) * 7;
        let ungrouped: usize = counts
            .iter()
            .zip(MANTISSA_BITS)
            .map(|(count, bits)| count * bits)
            .sum();
        Ok(grouped + ungrouped)
    }

    /// Add the bit allocation pointers of `channel`'s coefficients to
    /// `counts` (A/52 section 7.2.2).
    fn count_pointers(&self, channel: usize, counts: &mut [usize; 16]) -> Result<(), Unparsed> {
        let state = &self.channels[channel];
        let (start, end) = (state.start, state.end);
        // A coupling range can grow in a block that reuses exponents.
        let (Some((first, last)), Some(offsets)) = (state.exponents_set, state.offsets) else {
            return Err(Unparsed::Invalid(
                "exponents or SNR offsets from the frame before",
            ));
        };
        if start < first || last < end {
            return Err(Unparsed::Invalid("exponents from the frame before"));
        }
        if channel == CPL && !self.allocation.cpl_leak_set {
            return Err(Unparsed::Invalid("a coupling leak from the frame before"));
        }
        if end <= start || end > 253 {
            return Err(Unparsed::Invalid("an empty or too wide channel"));
        }
        if offsets.snr == -960 {
            // All SNR offsets 0: no bits for any coefficient.
            counts[0] += end - start;
            return Ok(());
        }

        // Exponents to power spectral density, integrated per band.
        let mut psd = [0_i32; 256];
        let mut band_psd = [0_i32; CRITICAL_BANDS + 1];
        for (psd, &exponent) in psd[start..end].iter_mut().zip(&state.exponents[start..end]) {
            *psd = 3072 - (i32::from(exponent) << 7);
        }
        let mut bin = start;
        let mut band = BIN_TO_BAND[start];
        loop {
            let mut value = psd[bin];
            bin += 1;
            let band_end = BAND_START[band + 1].min(end);
            while bin < band_end {
                let max = value.max(psd[bin]);
                // At least 0: `max` is at least the mean.
                let address = usize::try_from(max - ((value + psd[bin] + 1) >> 1)).unwrap_or(0);
                value = max + i32::from(LOG_ADD[address.min(255)]);
                bin += 1;
            }
            band_psd[band] = value;
            band += 1;
            if end <= BAND_START[band] {
                break;
            }
        }

        let mask = self.masking_curve(channel, offsets.fast_gain, &band_psd)?;

        let allocation = &self.allocation;
        let mut bin = start;
        let mut band = BIN_TO_BAND[start];
        loop {
            let masked =
                ((mask[band] - offsets.snr - allocation.floor).max(0) & 0x1FE0) + allocation.floor;
            band += 1;
            let band_end = BAND_START[band].min(end);
            while bin < band_end {
                let address = usize::try_from((psd[bin] - masked) >> 5)
                    .unwrap_or(0)
                    .min(63);
                counts[usize::from(BAP[address])] += 1;
                bin += 1;
            }
            if end <= band_end {
                break;
            }
        }
        Ok(())
    }

    /// The masking curve of `channel`, with its delta bit allocation.
    fn masking_curve(
        &self,
        channel: usize,
        fast_gain: i32,
        band_psd: &[i32; CRITICAL_BANDS + 1],
    ) -> Result<[i32; CRITICAL_BANDS], Unparsed> {
        let state = &self.channels[channel];
        let allocation = &self.allocation;
        let is_lfe = Some(channel) == self.lfe;
        let band_start = BIN_TO_BAND[state.start];
        let band_end = BIN_TO_BAND[state.end - 1] + 1;
        let mut excite = [0_i32; CRITICAL_BANDS];
        let (mut fast_leak, mut slow_leak);
        let begin;
        if band_start == 0 {
            let mut low_comp = low_comp_near(0, band_psd[0], band_psd[1], 384);
            excite[0] = band_psd[0] - fast_gain - low_comp;
            low_comp = low_comp_near(low_comp, band_psd[1], band_psd[2], 384);
            excite[1] = band_psd[1] - fast_gain - low_comp;
            let mut next = 7;
            fast_leak = 0;
            slow_leak = 0;
            for band in 2..7 {
                if !(is_lfe && band == 6) {
                    low_comp = low_comp_near(low_comp, band_psd[band], band_psd[band + 1], 384);
                }
                fast_leak = band_psd[band] - fast_gain;
                slow_leak = band_psd[band] - allocation.slow_gain;
                excite[band] = fast_leak - low_comp;
                if !(is_lfe && band == 6) && band_psd[band] <= band_psd[band + 1] {
                    next = band + 1;
                    break;
                }
            }
            for band in next..band_end.min(22) {
                if !(is_lfe && band == 6) {
                    low_comp = low_comp_at(low_comp, band_psd[band], band_psd[band + 1], band);
                }
                fast_leak = (fast_leak - allocation.fast_decay).max(band_psd[band] - fast_gain);
                slow_leak =
                    (slow_leak - allocation.slow_decay).max(band_psd[band] - allocation.slow_gain);
                excite[band] = (fast_leak - low_comp).max(slow_leak);
            }
            begin = 22;
        } else {
            // The coupling channel.
            begin = band_start;
            fast_leak = (allocation.cpl_fast_leak << 8) + 768;
            slow_leak = (allocation.cpl_slow_leak << 8) + 768;
        }
        for band in begin..band_end {
            fast_leak = (fast_leak - allocation.fast_decay).max(band_psd[band] - fast_gain);
            slow_leak =
                (slow_leak - allocation.slow_decay).max(band_psd[band] - allocation.slow_gain);
            excite[band] = fast_leak.max(slow_leak);
        }

        let mut mask = [0_i32; CRITICAL_BANDS];
        for band in band_start..band_end {
            let below = allocation.db_per_bit - band_psd[band];
            if below > 0 {
                excite[band] += below >> 2;
            }
            let threshold =
                HEARING_THRESHOLD[band >> allocation.rate_shift][allocation.sample_rate_code];
            mask[band] = threshold.max(excite[band]);
        }

        let segments = match (state.dba_mode, state.dba_segments) {
            (Some(DBA_NONE), _) => 0,
            (Some(_), Some(segments)) => segments,
            // The coupling channel's, when the first block is uncoupled.
            _ => {
                return Err(Unparsed::Invalid(
                    "a delta bit allocation from the frame before",
                ))
            }
        };
        let mut band = band_start;
        for segment in 0..segments {
            band += state.dba_offsets[segment];
            let length = state.dba_lengths[segment];
            if band >= CRITICAL_BANDS || length > CRITICAL_BANDS - band {
                return Err(Unparsed::Invalid("delta bit allocation past the last band"));
            }
            let value = state.dba_values[segment];
            let delta = 128 * if value >= 4 { value - 3 } else { value - 4 };
            for masked in &mut mask[band..band + length] {
                *masked += delta;
            }
            band += length;
        }
        Ok(mask)
    }
}

/// The low-frequency compensation of a band below 7, or one up to 19 with
/// `step` 320 (A/52 section 7.2.2.4).
fn low_comp_near(low_comp: i32, psd: i32, next_psd: i32, step: i32) -> i32 {
    if psd + 256 == next_psd {
        step
    } else if psd > next_psd {
        (low_comp - 64).max(0)
    } else {
        low_comp
    }
}

fn low_comp_at(low_comp: i32, psd: i32, next_psd: i32, band: usize) -> i32 {
    match band {
        0..7 => low_comp_near(low_comp, psd, next_psd, 384),
        7..20 => low_comp_near(low_comp, psd, next_psd, 320),
        _ => (low_comp - 128).max(0),
    }
}

#[cfg(test)]
mod tests {
    use super::{find_range_flags, Unparsed};
    use crate::raw_audio::ac3_conceal::{
        silent_frame_of, silent_frame_with, with_dynamic_range, AC3_LAYOUTS, COMPRESSING_DYNRNG,
    };
    use crate::raw_audio::bitstream::plain_ac3_frame_len;

    /// The frames of `stream`.
    fn frames(stream: &[u8]) -> Vec<&[u8]> {
        let mut frames = Vec::new();
        let mut rest = stream;
        while !rest.is_empty() {
            let len = plain_ac3_frame_len(rest).expect("a frame");
            let (frame, next) = rest.split_at(len);
            frames.push(frame);
            rest = next;
        }
        frames
    }

    /// The words of `frame` at its flags: `None` for a clear flag.
    fn words(frame: &[u8]) -> Vec<Option<usize>> {
        let flags = find_range_flags(frame).expect("a frame that parses");
        flags
            .positions()
            .iter()
            .map(|&flag| {
                (super::bit_at(frame, flag) == 1).then(|| super::read_bits(frame, flag + 1, 8))
            })
            .collect()
    }

    #[test]
    fn every_block_of_every_fixture_is_found_without_words() {
        for (layout, stream) in AC3_LAYOUTS {
            for (index, frame) in frames(stream).into_iter().enumerate() {
                assert_eq!(words(frame), [None; 6], "{layout} frame {index}");
            }
        }
    }

    #[test]
    fn words_written_in_every_block_are_found_where_they_were_written() {
        for (layout, stream) in AC3_LAYOUTS {
            let compressed = with_dynamic_range(stream, COMPRESSING_DYNRNG);
            for (index, frame) in frames(&compressed).into_iter().enumerate() {
                let flags = find_range_flags(frame).expect("a frame that parses");
                assert!(flags.compress(frame), "{layout} frame {index}");
                assert_eq!(
                    words(frame),
                    [Some(usize::from(COMPRESSING_DYNRNG)); 6],
                    "{layout} frame {index}"
                );
            }
        }
    }

    #[test]
    fn neutralized_words_are_0_and_leave_the_rest_of_the_frame() {
        for (layout, stream) in AC3_LAYOUTS {
            let compressed = with_dynamic_range(stream, COMPRESSING_DYNRNG);
            let plain = with_dynamic_range(stream, 0);
            for (index, (frame, plain)) in frames(&compressed)
                .into_iter()
                .zip(frames(&plain))
                .enumerate()
            {
                let flags = find_range_flags(frame).expect("a frame that parses");
                let mut neutralized = frame.to_vec();
                flags.neutralize(&mut neutralized);
                assert!(!flags.compress(&neutralized), "{layout} frame {index}");
                assert_eq!(words(&neutralized), [Some(0); 6], "{layout} frame {index}");
                // But for the CRCs, which the probe sets again.
                let end = plain.len() - 2;
                assert_eq!(neutralized[4..end], plain[4..end], "{layout} frame {index}");
            }
        }
    }

    #[test]
    fn a_word_of_0_does_not_compress() {
        let plain = with_dynamic_range(AC3_LAYOUTS[0].1, 0);
        for frame in frames(&plain) {
            let flags = find_range_flags(frame).expect("a frame that parses");
            assert!(!flags.compress(frame));
        }
    }

    #[test]
    fn both_programmes_of_dual_mono_and_every_layout_are_found() {
        // 32 kbit/s, the lowest bitrate, and 192 kbit/s.
        for frmsizecod in [0, 20] {
            for acmod in 0..8 {
                for lfe in [false, true] {
                    let name = format!("bitrate {frmsizecod}, acmod {acmod}, LFE {lfe}");
                    let stream = silent_frame_of(frmsizecod, acmod, lfe).repeat(2);
                    let programmes = if acmod == 0 { 2 } else { 1 };
                    for frame in frames(&stream) {
                        assert_eq!(words(frame), vec![None; 6 * programmes], "{name}");
                    }
                    let compressed = with_dynamic_range(&stream, COMPRESSING_DYNRNG);
                    for frame in frames(&compressed) {
                        assert_eq!(
                            words(frame),
                            vec![Some(usize::from(COMPRESSING_DYNRNG)); 6 * programmes],
                            "{name}"
                        );
                    }
                }
            }
        }
    }

    #[test]
    fn a_delta_bit_allocation_from_the_frame_before_leaves_the_frame_unparsed() {
        let header = frames(AC3_LAYOUTS[0].1)[0];
        let frame = silent_frame_with(header, None, Some(4));
        assert_eq!(
            find_range_flags(&frame),
            Err(Unparsed::Invalid(
                "a delta bit allocation from the frame before"
            ))
        );
    }

    #[test]
    fn coupling_exponents_or_strategies_from_the_frame_before_leave_the_frame_unparsed() {
        let frame = frames(AC3_LAYOUTS[1].1)[0];
        let mut parsed = super::Frame::new(frame).expect("a header");
        parsed
            .block(0, &mut super::RangeFlags::new())
            .expect("the first block");
        assert!(parsed.cpl_in_use, "a coupled fixture");
        let mut counts = [0; 16];
        let coupling = |parsed: &super::Frame, counts: &mut [usize; 16]| {
            parsed.count_pointers(super::CPL, counts)
        };
        assert_eq!(coupling(&parsed, &mut counts), Ok(()));
        // A later block that starts coupling lower but reuses exponents.
        parsed.channels[super::CPL].start -= 12;
        assert_eq!(
            coupling(&parsed, &mut counts),
            Err(Unparsed::Invalid("exponents from the frame before"))
        );
        parsed.channels[super::CPL].start += 12;
        // A first block without coupling that sets the delta bit allocation
        // of the other channels only.
        parsed.channels[super::CPL].dba_mode = None;
        assert_eq!(
            coupling(&parsed, &mut counts),
            Err(Unparsed::Invalid(
                "a delta bit allocation from the frame before"
            ))
        );
    }

    #[test]
    fn frames_that_do_not_parse_say_why() {
        let frame = frames(AC3_LAYOUTS[0].1)[0];
        // E-AC3, and a frame shorter than its header says.
        let mut e_ac3 = frame.to_vec();
        e_ac3[5] = 16 << 3;
        assert_eq!(find_range_flags(&e_ac3), Err(Unparsed::NotPlainAc3));
        assert_eq!(find_range_flags(&frame[..100]), Err(Unparsed::NotPlainAc3));
        // Blocks of zeros, as the concealment's stand-in for a damaged
        // frame has: the first block sets no coupling strategy.
        let mut zeros = frame.to_vec();
        zeros[7..].fill(0);
        assert_eq!(
            find_range_flags(&zeros),
            Err(Unparsed::Invalid("no coupling strategy in block 0"))
        );
        // All bits set: coupling in a mono stream.
        let mut ones = frame.to_vec();
        ones[8..].fill(0xFF);
        assert_eq!(
            find_range_flags(&ones),
            Err(Unparsed::Invalid("coupling in mono or dual mono"))
        );
        // A frame whose blocks need more bits than it has: the first, mono,
        // with its exponents taken by a 5.1 stream's six channels.
        let mut short = frames(AC3_LAYOUTS[5].1)[0].to_vec();
        // 32 kbit/s.
        short[4] &= 0xC0;
        short.truncate(128);
        assert_eq!(find_range_flags(&short), Err(Unparsed::PastEnd));
    }
}
