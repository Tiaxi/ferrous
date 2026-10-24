// SPDX-License-Identifier: GPL-3.0-or-later

//! AC3 (A/52) and DTS frame headers: sync words and bitrates.

/// AC3 bitrate lookup indexed by `frmsizecod / 2` (kbps) — A/52 Table 5.18.
const AC3_BITRATES_KBPS: [u32; 19] = [
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 448, 512, 576, 640,
];

/// DTS sample rate table indexed by SFREQ code.
const DTS_SAMPLE_RATES: [u32; 16] = [
    0, 8000, 16000, 32000, 0, 0, 11025, 22050, 44100, 0, 0, 12000, 24000, 48000, 0, 0,
];

/// Find the byte offset of the first AC3 sync word (`0x0B77`) in `data`.
pub(super) fn find_ac3_sync(data: &[u8]) -> Option<usize> {
    data.windows(2).position(|w| w == [0x0B, 0x77])
}

/// Parse the bitrate from an AC3 (A/52) sync frame header.
///
/// Layout of the first 5 bytes:
///   `[0..2]` sync word `0x0B77`
///   `[2..4]` CRC1
///   `[4]`    fscod (2 bits) | frmsizecod (6 bits)
pub(super) fn parse_ac3_bitrate(header: &[u8]) -> Option<u32> {
    if header.len() < 5 || header[0] != 0x0B || header[1] != 0x77 {
        return None;
    }
    let frmsizcod = usize::from(header[4] & 0x3F);
    let kbps = *AC3_BITRATES_KBPS.get(frmsizcod / 2)?;
    Some(kbps * 1000)
}

/// Find the byte offset of the first DTS sync word (`0x7FFE8001`) in `data`.
pub(super) fn find_dts_sync(data: &[u8]) -> Option<usize> {
    data.windows(4).position(|w| w == [0x7F, 0xFE, 0x80, 0x01])
}

/// Parse the bitrate from a DTS frame header.
///
/// Sync word: `0x7FFE8001` (bytes `[0..4]`).
/// Frame size and sample rate are extracted from the header to compute
/// the bitrate as `frame_bytes * 8 * sample_rate / 512` (512 PCM samples
/// per DTS frame).
pub(super) fn parse_dts_bitrate(header: &[u8]) -> Option<u32> {
    if header.len() < 12 {
        return None;
    }
    if header[0] != 0x7F || header[1] != 0xFE || header[2] != 0x80 || header[3] != 0x01 {
        return None;
    }

    // FSIZE is in bits 47..61 (14 bits), big-endian packed.
    let fsize_raw = (u16::from(header[5] & 0x03) << 12)
        | (u16::from(header[6]) << 4)
        | (u16::from(header[7]) >> 4);
    let frame_bytes = u32::from(fsize_raw) + 1;

    // SFREQ is at bits 67..71.
    let sfreq_code = usize::from((header[8] >> 2) & 0x0F);
    let sample_rate = *DTS_SAMPLE_RATES.get(sfreq_code)?;
    if sample_rate == 0 || frame_bytes == 0 {
        return None;
    }

    let bitrate = u64::from(frame_bytes) * 8 * u64::from(sample_rate) / 512;
    u32::try_from(bitrate).ok()
}

#[cfg(test)]
mod tests {
    #[test]
    fn ac3_bitrate_parses_standard_header() {
        // AC3 sync word 0x0B77, CRC1 placeholder, fscod=00 (48kHz), frmsizecod=12 → index 6 → 96 kbps
        let header = [0x0B, 0x77, 0x00, 0x00, 0b00_001100, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(super::parse_ac3_bitrate(&header), Some(96_000));
    }

    #[test]
    fn ac3_bitrate_parses_640kbps() {
        // frmsizecod=36 → index 18 → 640 kbps
        let header = [0x0B, 0x77, 0x00, 0x00, 0b00_100100, 0, 0, 0, 0, 0, 0, 0];
        assert_eq!(super::parse_ac3_bitrate(&header), Some(640_000));
    }

    #[test]
    fn ac3_bitrate_rejects_bad_sync() {
        let header = [0x00, 0x00, 0x00, 0x00, 0x00];
        assert_eq!(super::parse_ac3_bitrate(&header), None);
    }

    #[test]
    fn dts_bitrate_parses_standard_header() {
        // DTS sync: 7F FE 80 01
        // Build a header with known FSIZE and SFREQ.
        // FSIZE = 2047 (frame_bytes = 2048) at bits 47..61
        // SFREQ = 13 (48000 Hz) at bits 67..71
        //
        // Byte layout:
        //   [0..4] = 7F FE 80 01  (sync)
        //   [4]    = don't care (FTYPE, SHORT, CPF, NBLKS upper)
        //   [5]    = NBLKS lower 2 bits | FSIZE upper 2 bits
        //           FSIZE=2047 → 0b_0111_1111_1111 (11 bits in 14-bit field → upper 2 = 0b01)
        //           → byte5 lower 2 bits = 0b11 (FSIZE bits 12..13)
        //           Wait, FSIZE is 14 bits. 2047 = 0x7FF = 0b0000_0111_1111_1111
        //           byte5[1:0] = bits 13..12 = 0b00
        //   Actually let me just compute the exact bytes.
        //
        // FSIZE = 2047 → 14 bits → 0b00_0111_1111_1111
        // Packed into bytes 5..7:
        //   byte5 & 0x03 = upper 2 bits of 14 = 0b00
        //   byte6 = next 8 bits = 0b0111_1111 = 0x7F
        //   byte7 upper 4 bits = lower 4 bits = 0b1111 → 0xF0
        //
        // SFREQ = 13 → 4 bits → 0b1101
        // Packed into byte 8: bits [5..2] = (byte8 >> 2) & 0x0F
        //   byte8 = 0b00_1101_00 = 0x34
        let mut header = [0u8; 12];
        header[0] = 0x7F;
        header[1] = 0xFE;
        header[2] = 0x80;
        header[3] = 0x01;
        header[5] = 0x00; // FSIZE upper 2 = 0
        header[6] = 0x7F; // FSIZE mid 8
        header[7] = 0xF0; // FSIZE lower 4 in upper nibble
        header[8] = 0x34; // SFREQ=13 at bits 5..2

        // frame_bytes = 2047 + 1 = 2048
        // sample_rate = 48000
        // bitrate = 2048 * 8 * 48000 / 512 = 1_536_000
        assert_eq!(super::parse_dts_bitrate(&header), Some(1_536_000));
    }

    #[test]
    fn dts_bitrate_rejects_bad_sync() {
        let header = [0u8; 12];
        assert_eq!(super::parse_dts_bitrate(&header), None);
    }
}
