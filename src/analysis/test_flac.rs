// SPDX-License-Identifier: GPL-3.0-or-later

//! FLAC files for tests, in verbatim blocks, written bit by bit.

/// Bits written most significant first.
#[derive(Default)]
struct Bits {
    bytes: Vec<u8>,
    pending: u64,
    count: u32,
}

impl Bits {
    fn put(&mut self, value: u64, bits: u32) {
        for bit in (0..bits).rev() {
            self.pending = (self.pending << 1) | ((value >> bit) & 1);
            self.count += 1;
            if self.count == 8 {
                self.bytes.push(u8::try_from(self.pending).unwrap());
                (self.pending, self.count) = (0, 0);
            }
        }
    }

    fn align(&mut self) {
        while self.count != 0 {
            self.put(0, 1);
        }
    }
}

fn crc(bytes: &[u8], polynomial: u16, width: u32) -> u16 {
    let top = 1_u32 << (width - 1);
    let mask = (1_u32 << width) - 1;
    let mut crc = 0_u32;
    for &byte in bytes {
        crc ^= u32::from(byte) << (width - 8);
        for _ in 0..8 {
            crc = if crc & top == 0 {
                crc << 1
            } else {
                (crc << 1) ^ u32::from(polynomial)
            } & mask;
        }
    }
    u16::try_from(crc).unwrap()
}

/// A FLAC file of `samples`, interleaved in `channels` channels of `bits`
/// bits at `rate` Hz, in verbatim blocks of 4,096 frames. With `damaged`,
/// that block's subframes have a reserved type, under valid checksums: the
/// file reads, and a decoder refuses that block alone.
pub(super) fn flac_bytes(
    samples: &[i32],
    rate: u32,
    channels: usize,
    bits: u32,
    damaged: Option<usize>,
) -> Vec<u8> {
    let frames = samples.len() / channels;
    let channel_bits = u64::try_from(channels - 1).unwrap();
    let mut out = Bits::default();
    out.put(u64::from(u32::from_be_bytes(*b"fLaC")), 32);
    // The last metadata block: STREAMINFO, 34 bytes.
    out.put(0x80, 8);
    out.put(34, 24);
    out.put(4_096, 16);
    out.put(4_096, 16);
    out.put(0, 48);
    out.put(u64::from(rate), 20);
    out.put(channel_bits, 3);
    out.put(u64::from(bits - 1), 5);
    out.put(u64::try_from(frames).unwrap(), 36);
    out.put(0, 64);
    out.put(0, 64);
    for (number, block) in samples.chunks(4_096 * channels).enumerate() {
        let block_frames = block.len() / channels;
        let mut frame = Bits::default();
        // Sync code, fixed block size, block size after the header,
        // rate of STREAMINFO, independent channels, sample size.
        frame.put(0b1111_1111_1111_1000, 16);
        frame.put(0b0111, 4);
        frame.put(0, 4);
        frame.put(channel_bits, 4);
        frame.put(if bits == 16 { 0b100 } else { 0b110 }, 3);
        frame.put(0, 1);
        // The frame number, coded as UTF-8 is.
        let index = number;
        let number = u64::try_from(number).unwrap();
        if number < 0x80 {
            frame.put(number, 8);
        } else {
            frame.put(0xc0 | (number >> 6), 8);
            frame.put(0x80 | (number & 0x3f), 8);
        }
        frame.put(u64::try_from(block_frames - 1).unwrap(), 16);
        let header_crc = crc(&frame.bytes, 0x07, 8);
        frame.put(u64::from(header_crc), 8);
        for channel in 0..channels {
            // A verbatim subframe without wasted bits; a damaged one has
            // the reserved type 0b000010.
            frame.put(
                if damaged == Some(index) {
                    0b0000_0100
                } else {
                    0b0000_0010
                },
                8,
            );
            for samples in block.chunks_exact(channels) {
                let value = u64::from(samples[channel].cast_unsigned());
                frame.put(value & ((1 << bits) - 1), bits);
            }
        }
        frame.align();
        let frame_crc = crc(&frame.bytes, 0x8005, 16);
        frame.put(u64::from(frame_crc), 16);
        out.bytes.extend(frame.bytes);
    }
    out.bytes
}

/// An ID3v1 tag, as appended to a file.
pub(super) fn id3v1_tag() -> Vec<u8> {
    let mut tag = b"TAG".to_vec();
    tag.resize(128, b' ');
    tag
}

/// An APEv2 tag of one item, as appended to a file, with a header before
/// the item if `header`.
pub(super) fn apev2_tag(header: bool) -> Vec<u8> {
    apev2_tag_with(header, b"Tone!")
}

/// An APEv2 tag of one item of `value`, as appended to a file, with a
/// header before the item if `header`.
pub(super) fn apev2_tag_with(header: bool, value: &[u8]) -> Vec<u8> {
    const HAS_HEADER: u32 = 1 << 31;
    const IS_HEADER: u32 = 1 << 29;
    let mut item = u32::try_from(value.len()).unwrap().to_le_bytes().to_vec();
    item.extend(0_u32.to_le_bytes());
    item.extend(b"Title\0");
    item.extend(value);
    // The size counts the item and the footer.
    let size = u32::try_from(item.len()).unwrap() + 32;
    let block = |flags: u32| {
        let mut block = b"APETAGEX".to_vec();
        block.extend(2_000_u32.to_le_bytes());
        block.extend(size.to_le_bytes());
        block.extend(1_u32.to_le_bytes());
        block.extend(flags.to_le_bytes());
        block.extend([0_u8; 8]);
        block
    };
    let has_header = if header { HAS_HEADER } else { 0 };
    let mut tag = Vec::new();
    if header {
        tag.extend(block(has_header | IS_HEADER));
    }
    tag.extend(item);
    tag.extend(block(has_header));
    tag
}

/// Interleaved 16-bit stereo `samples`, changed so that their FLAC file
/// from [`flac_bytes`] has `TAG` 128 bytes before its end, where an ID3v1
/// tag would start, in the audio of its last frame. The frame ends with
/// the samples of the second channel and the CRC-16 of the frame.
pub(super) fn with_tag_marker_at_the_end(samples: &mut [i32]) {
    let frames = samples.len() / 2;
    // 126 bytes before the CRC: 63 samples before the end of the channel.
    samples[2 * (frames - 63) + 1] = 0x5441;
    samples[2 * (frames - 62) + 1] = 0x4741;
}
