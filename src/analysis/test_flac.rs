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
