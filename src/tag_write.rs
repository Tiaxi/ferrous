// SPDX-License-Identifier: GPL-3.0-or-later

//! Saving edited tags without losing what was not edited.
//!
//! Callers read files through lofty's generic [`Tag`], which loses details
//! that [`save_tag`] restores before writing.

use std::fs::File;
use std::path::Path;

use lofty::ape::ApeTag;
use lofty::config::{ParseOptions, WriteOptions};
use lofty::error::LoftyError;
use lofty::file::{AudioFile, FileType};
use lofty::id3::v2::{Id3v2Tag, Id3v2Version};
use lofty::ogg::VorbisComments;
use lofty::prelude::{MergeTag, SplitTag, TagExt};
use lofty::tag::{ItemKey, Tag, TagItem, TagType};

use crate::safe_save::rewrite_via_temp_copy;

/// Save `tag`, as read from `path` and then edited, back into the file.
///
/// The file is replaced through [`rewrite_via_temp_copy`], so a failure
/// leaves it unchanged. Two details of the original file are kept:
///
/// * The `ID3v2` version. lofty writes `ID3v2.4` by default, which some
///   older players cannot read; files read as v2.3 (or v2.2, which lofty
///   cannot write) are written as v2.3.
/// * Items lofty has no generic mapping for. The generic tag keeps them for
///   `ID3v2` and MP4 but drops them for Vorbis comments and APE tags, so
///   those are merged back from the file's own tag.
///
/// # Errors
///
/// Returns a message when the file cannot be read, the tag cannot be
/// written, or the file cannot be replaced.
pub(crate) fn save_tag(path: &Path, file_type: FileType, tag: &Tag) -> Result<(), String> {
    let describe = |err: LoftyError| format!("failed to save tags for {}: {err}", path.display());
    let mut tag = tag.clone();
    let write_options = if tag.tag_type() == TagType::Id3v2
        && matches!(
            id3v2_version(path, file_type).map_err(describe)?,
            Some(Id3v2Version::V2 | Id3v2Version::V3)
        ) {
        describe_pictures(&mut tag);
        WriteOptions::new().use_id3v23(true)
    } else {
        WriteOptions::new()
    };
    let original = read_native_tag(path, file_type, tag.tag_type()).map_err(describe)?;
    rewrite_via_temp_copy(path, |file| {
        match original {
            Some(NativeTag::Vorbis(original)) => {
                let (remainder, _) = original.split_tag();
                remainder
                    .merge_tag(vendor_items_first(&tag))
                    .save_to(file, write_options)
            }
            Some(NativeTag::Ape(original)) => {
                let (remainder, _) = original.split_tag();
                remainder.merge_tag(tag).save_to(file, write_options)
            }
            None => tag.save_to(file, write_options),
        }
        .map_err(describe)
    })
}

/// lofty 0.24 ends a missing picture description with one zero byte, but
/// `ID3v2.3` stores it as UTF-16, which needs two, and the picture frame can no
/// longer be read. An empty description is written correctly and reads back
/// as missing.
fn describe_pictures(tag: &mut Tag) {
    for index in 0..tag.pictures().len() {
        if tag.pictures()[index].description().is_none() {
            let mut picture = tag.pictures()[index].clone();
            picture.set_description(Some(String::new()));
            tag.set_picture(index, picture);
        }
    }
}

/// lofty's Vorbis comment merge takes out the vendor item with swaps that
/// reorder the other items, which would reverse values such as two artists.
/// With the vendor items first, those swaps leave the order unchanged.
fn vendor_items_first(tag: &Tag) -> Tag {
    let (vendor, other): (Vec<TagItem>, Vec<TagItem>) = tag
        .items()
        .cloned()
        .partition(|item| item.key() == ItemKey::EncoderSoftware);
    let mut ordered = tag.clone();
    ordered.retain(|_| false);
    for item in vendor.into_iter().chain(other) {
        ordered.push_unchecked(item);
    }
    ordered
}

fn id3v2_version(path: &Path, file_type: FileType) -> Result<Option<Id3v2Version>, LoftyError> {
    fn read_version<T: AudioFile>(
        path: &Path,
        id3v2: impl FnOnce(&T) -> Option<&Id3v2Tag>,
    ) -> Result<Option<Id3v2Version>, LoftyError> {
        let options = ParseOptions::new()
            .read_properties(false)
            .read_cover_art(false);
        read(path, options, |file: T| {
            id3v2(&file).map(Id3v2Tag::original_version)
        })
    }

    // These are the formats where ID3v2 is a writable tag.
    match file_type {
        FileType::Mpeg => read_version(path, lofty::mpeg::MpegFile::id3v2),
        FileType::Aac => read_version(path, lofty::aac::AacFile::id3v2),
        FileType::Aiff => read_version(path, lofty::iff::aiff::AiffFile::id3v2),
        FileType::Wav => read_version(path, lofty::iff::wav::WavFile::id3v2),
        _ => Ok(None),
    }
}

/// A file's own tag of a type whose generic conversion drops items.
enum NativeTag {
    Vorbis(VorbisComments),
    Ape(ApeTag),
}

fn read_native_tag(
    path: &Path,
    file_type: FileType,
    tag_type: TagType,
) -> Result<Option<NativeTag>, LoftyError> {
    // Same options as the generic read, so the split matches the edited tag.
    let options = ParseOptions::new().read_properties(false);
    let vorbis = |tag: Option<VorbisComments>| tag.map(NativeTag::Vorbis);
    let ape = |tag: Option<ApeTag>| tag.map(NativeTag::Ape);
    Ok(match (file_type, tag_type) {
        (FileType::Flac, TagType::VorbisComments) => {
            vorbis(read(path, options, |mut file: lofty::flac::FlacFile| {
                file.remove_vorbis_comments()
            })?)
        }
        (FileType::Vorbis, TagType::VorbisComments) => {
            vorbis(read(path, options, |mut file: lofty::ogg::VorbisFile| {
                Some(file.remove_vorbis_comments())
            })?)
        }
        (FileType::Opus, TagType::VorbisComments) => {
            vorbis(read(path, options, |mut file: lofty::ogg::OpusFile| {
                Some(file.remove_vorbis_comments())
            })?)
        }
        (FileType::Speex, TagType::VorbisComments) => {
            vorbis(read(path, options, |mut file: lofty::ogg::SpeexFile| {
                Some(file.remove_vorbis_comments())
            })?)
        }
        (FileType::Ape, TagType::Ape) => {
            ape(read(path, options, |mut file: lofty::ape::ApeFile| {
                file.remove_ape()
            })?)
        }
        (FileType::Mpc, TagType::Ape) => ape(read(
            path,
            options,
            |mut file: lofty::musepack::MpcFile| file.remove_ape(),
        )?),
        (FileType::WavPack, TagType::Ape) => ape(read(
            path,
            options,
            |mut file: lofty::wavpack::WavPackFile| file.remove_ape(),
        )?),
        _ => None,
    })
}

/// Read `path` as the concrete file type `T` and take what `take` needs from it.
fn read<T: AudioFile, R>(
    path: &Path,
    options: ParseOptions,
    take: impl FnOnce(T) -> R,
) -> Result<R, LoftyError> {
    let mut file = File::open(path)?;
    Ok(take(T::read_from(&mut file, options)?))
}

/// Audio frames for MP3 fixtures.
#[cfg(test)]
pub(crate) const TEST_MP3: &[u8] = include_bytes!("analysis/fixtures/seek-tone.mp3");

/// Write a FLAC stream with the given Vorbis comments and no audio frames.
#[cfg(test)]
pub(crate) fn write_test_flac(path: &Path, comments: &[(&str, &str)]) {
    let le_u32 = |len: usize| u32::try_from(len).expect("fixture length").to_le_bytes();
    let mut bytes = b"fLaC".to_vec();
    // STREAMINFO: 4096-sample blocks, no frame sizes, then the packed fields.
    bytes.extend_from_slice(&[0x00, 0, 0, 34]);
    bytes.extend_from_slice(&4096_u16.to_be_bytes());
    bytes.extend_from_slice(&4096_u16.to_be_bytes());
    bytes.extend_from_slice(&[0; 6]);
    let (sample_rate, channels, bits_per_sample, total_samples) =
        (44_100_u64, 2_u64, 16_u64, 44_100_u64);
    let packed = (sample_rate << 44)
        | ((channels - 1) << 41)
        | ((bits_per_sample - 1) << 36)
        | total_samples;
    bytes.extend_from_slice(&packed.to_be_bytes());
    bytes.extend_from_slice(&[0; 16]);

    let vendor = b"ferrous-test";
    let mut block = le_u32(vendor.len()).to_vec();
    block.extend_from_slice(vendor);
    block.extend_from_slice(&le_u32(comments.len()));
    for (key, value) in comments {
        let entry = format!("{key}={value}");
        block.extend_from_slice(&le_u32(entry.len()));
        block.extend_from_slice(entry.as_bytes());
    }
    // The last metadata block: VORBIS_COMMENT.
    bytes.push(0x84);
    bytes.extend_from_slice(&u32::try_from(block.len()).expect("block").to_be_bytes()[1..]);
    bytes.extend_from_slice(&block);
    std::fs::write(path, bytes).expect("write FLAC fixture");
}

/// Write an MP3 with an ID3v2.3 tag holding a full recording date and a cover.
#[cfg(test)]
pub(crate) fn write_test_mp3_with_id3v23(path: &Path) {
    std::fs::write(path, TEST_MP3).expect("write MP3 fixture");
    let mut tag = Tag::new(TagType::Id3v2);
    tag.insert_text(ItemKey::TrackTitle, "Old".to_string());
    tag.insert_text(ItemKey::TrackArtist, "Artist".to_string());
    tag.insert_text(ItemKey::AlbumTitle, "Album".to_string());
    tag.insert_text(ItemKey::RecordingDate, "1997-05-12".to_string());
    tag.insert_text(ItemKey::OriginalReleaseDate, "1995".to_string());
    // Most tagged files have covers without a description.
    tag.push_picture(
        lofty::picture::Picture::unchecked(b"\xff\xd8\xff\xd9".to_vec())
            .mime_type(lofty::picture::MimeType::Jpeg)
            .pic_type(lofty::picture::PictureType::CoverFront)
            .description("")
            .build(),
    );
    tag.save_to_path(path, WriteOptions::new().use_id3v23(true))
        .expect("write ID3v2.3 tag");
    assert_eq!(&std::fs::read(path).expect("read fixture")[..4], b"ID3\x03");
}
