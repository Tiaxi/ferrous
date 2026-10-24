// SPDX-License-Identifier: GPL-3.0-or-later

//! Saving edited tags without losing what was not edited.
//!
//! Callers read files through lofty's generic [`Tag`], which loses details
//! that [`save_tag`] restores before writing.

use std::collections::HashSet;
use std::fs::File;
use std::path::Path;

use lofty::ape::{ApeItem, ApeTag};
use lofty::config::{ParseOptions, WriteOptions};
use lofty::error::LoftyError;
use lofty::file::{AudioFile, FileType};
use lofty::id3::v2::{Id3v2Tag, Id3v2Version};
use lofty::mp4::Ilst;
use lofty::ogg::{OggPictureStorage, VorbisComments};
use lofty::picture::PictureInformation;
use lofty::prelude::{SplitTag, TagExt};
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
/// * Everything in Vorbis comments, APE tags, and MP4 `ilst` atoms that the
///   edit did not change. The generic tag drops unknown items of the first
///   two, and keeps only the first value of each MP4 atom and the first MP4
///   picture, so the edit is applied to the file's own tag instead. See
///   [`Changes`] for how an edit is told apart from what the generic tag lost.
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
                edit_vorbis_comments(original, &tag).save_to(file, write_options)
            }
            Some(NativeTag::Ape(original)) => {
                edit_ape_tag(original, &tag).save_to(file, write_options)
            }
            Some(NativeTag::Ilst(original)) => {
                edit_ilst(original, &tag).save_to(file, write_options)
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

/// What one item of a file's own tag becomes in the generic tag.
#[derive(Debug, Default)]
struct Mapping {
    keys: Vec<ItemKey>,
    pictures: bool,
}

impl Mapping {
    /// The mapping of a native tag holding a single item, given as the
    /// generic tag lofty converts it to.
    fn of(tag: &Tag) -> Self {
        Self {
            keys: tag.items().map(TagItem::key).collect(),
            pictures: !tag.pictures().is_empty(),
        }
    }
}

/// The parts of a generic tag an edit changed.
///
/// The edited tag is compared with the generic tag the file's own tag
/// converts to now, which is what the caller read and edited. Keys whose
/// items are the same in both were not edited, even where the generic tag
/// shows less than the file holds, so the file's own items for them are
/// kept byte for byte. Items for the changed keys are rewritten from the
/// edited tag. An item that holds several keys, such as an MP4 track number
/// and total, is rewritten whole when any of them changed.
#[derive(Debug)]
struct Changes {
    keys: HashSet<ItemKey>,
    pictures: bool,
}

impl Changes {
    fn between(original: &Tag, edited: &Tag) -> Self {
        let keys = original
            .items()
            .chain(edited.items())
            .map(TagItem::key)
            .filter(|&key| !original.get_items(key).eq(edited.get_items(key)))
            .collect();
        Self {
            keys,
            pictures: original.pictures() != edited.pictures(),
        }
    }

    fn touches(&self, mapping: &Mapping) -> bool {
        (self.pictures && mapping.pictures)
            || mapping.keys.iter().any(|key| self.keys.contains(key))
    }

    /// Which native items the edit replaces. Every key of a replaced item is
    /// rewritten, so the other items with those keys are replaced too.
    fn replaced(&mut self, mappings: &[Mapping]) -> Vec<bool> {
        let mut replaced = vec![false; mappings.len()];
        loop {
            let mut grew = false;
            for (mapping, replaced) in mappings.iter().zip(&mut replaced) {
                if !*replaced && self.touches(mapping) {
                    *replaced = true;
                    for key in &mapping.keys {
                        grew |= self.keys.insert(*key);
                    }
                }
            }
            if !grew {
                return replaced;
            }
        }
    }

    /// The items and pictures of `edited` that replace the native ones.
    fn edited_part(&self, edited: &Tag) -> Tag {
        let mut part = Tag::new(edited.tag_type());
        for item in edited
            .items()
            .filter(|item| self.keys.contains(&item.key()))
        {
            part.push_unchecked(item.clone());
        }
        if self.pictures {
            for picture in edited.pictures() {
                part.push_picture(picture.clone());
            }
        }
        part
    }
}

/// Merge native items: kept items stay in place, and the new items for a key
/// take the place of the first replaced item that held it, through `adopt`.
/// New items for keys the tag did not hold go at the end.
fn place_items<T>(
    original: impl IntoIterator<Item = ((T, Mapping), bool)>,
    new: impl IntoIterator<Item = (T, Mapping)>,
    adopt: impl Fn(&T, T) -> T,
) -> Vec<T> {
    let mut new = new.into_iter().map(Some).collect::<Vec<_>>();
    let mut merged = Vec::new();
    for ((item, mapping), replaced) in original {
        if !replaced {
            merged.push(item);
            continue;
        }
        for slot in &mut new {
            let belongs = slot.as_ref().is_some_and(|(_, new_mapping)| {
                new_mapping
                    .keys
                    .first()
                    .is_some_and(|key| mapping.keys.contains(key))
            });
            if let Some((new_item, _)) = slot.take_if(|_| belongs) {
                merged.push(adopt(&item, new_item));
            }
        }
    }
    merged.extend(new.into_iter().flatten().map(|(item, _)| item));
    merged
}

/// Vorbis comments have no generic key for the vendor string, so lofty shows
/// it as the encoder when no `ENCODER` comment exists, and writes the first
/// encoder value back as the vendor. The vendor is never changed here: an
/// edited encoder is written as `ENCODER` comments.
const VORBIS_ENCODER_KEY: &str = "ENCODER";

fn edit_vorbis_comments(mut original: VorbisComments, edited: &Tag) -> VorbisComments {
    let mut changes = Changes::between(&original.clone().into(), edited);
    let items = original.take_items().collect::<Vec<_>>();
    let mappings = items
        .iter()
        .map(|(key, value)| vorbis_mapping(key, value))
        .collect::<Vec<_>>();
    let replaced = changes.replaced(&mappings);
    let mut part = changes.edited_part(edited);
    let encoders = part
        .get_strings(ItemKey::EncoderSoftware)
        .map(str::to_string)
        .collect::<Vec<_>>();
    part.remove_key(ItemKey::EncoderSoftware);
    let mut converted = VorbisComments::from(part);
    let new = converted
        .take_items()
        .chain(
            encoders
                .into_iter()
                .map(|value| (VORBIS_ENCODER_KEY.to_string(), value)),
        )
        .map(|(key, value)| {
            let mapping = vorbis_mapping(&key, &value);
            ((key, value), mapping)
        });
    // Keys are case-insensitive, so an edited comment keeps its spelling.
    let merged = place_items(
        items.into_iter().zip(mappings).zip(replaced),
        new,
        |(key, _), (_, value)| (key.clone(), value),
    );
    for (key, value) in merged {
        original.push(key, value);
    }
    if changes.pictures {
        let previous = original.remove_pictures();
        for picture in edited.pictures() {
            // Kept pictures keep their information. lofty's conversion drops
            // a picture whose information it cannot read from the image.
            let info = previous
                .iter()
                .find(|(kept, _)| kept == picture)
                .map_or_else(
                    || PictureInformation::from_picture(picture).unwrap_or_default(),
                    |(_, info)| *info,
                );
            // Cannot fail: the information is given, not read from the picture.
            let _ = original.insert_picture(picture.clone(), Some(info));
        }
    }
    original
}

fn vorbis_mapping(key: &str, value: &str) -> Mapping {
    let mut single = VorbisComments::new();
    single.push(key.to_string(), value.to_string());
    let (remainder, tag) = single.split_tag();
    // The split adds the vendor string after the comment's own item, which
    // stays in the remainder when lofty has no generic key for it.
    let key = tag.items().next().map(TagItem::key);
    Mapping {
        keys: key
            .filter(|_| remainder.items().len() == 0)
            .into_iter()
            .collect(),
        pictures: false,
    }
}

/// Edit the APE tag. An edited item keeps the key spelling and read-only flag
/// of the item it replaces, since the flag describes the item rather than its
/// value; unedited items keep their flags too.
fn edit_ape_tag(original: ApeTag, edited: &Tag) -> ApeTag {
    let mut changes = Changes::between(&original.clone().into(), edited);
    let read_only = original.read_only;
    let items = original.into_iter().collect::<Vec<_>>();
    let mappings = items.iter().map(ape_mapping).collect::<Vec<_>>();
    let mut replaced = changes.replaced(&mappings);
    let new = ApeTag::from(changes.edited_part(edited))
        .into_iter()
        .collect::<Vec<_>>();
    // An APE tag holds one item per key, so a new item, such as a cover,
    // also replaces an item lofty has no generic key for.
    for (item, replaced) in items.iter().zip(&mut replaced) {
        *replaced |= new
            .iter()
            .any(|new_item| new_item.key().eq_ignore_ascii_case(item.key()));
    }
    let new = new.into_iter().map(|item| {
        let mapping = ape_mapping(&item);
        (item, mapping)
    });
    let mut merged = ApeTag::new();
    merged.read_only = read_only;
    let items = items.into_iter().zip(mappings).zip(replaced);
    for item in place_items(items, new, adopt_ape_item) {
        merged.insert(item);
    }
    merged
}

fn adopt_ape_item(old: &ApeItem, new: ApeItem) -> ApeItem {
    let mut item = ApeItem::new(old.key().to_string(), new.value().clone()).unwrap_or(new);
    item.read_only = old.read_only;
    item
}

fn ape_mapping(item: &ApeItem) -> Mapping {
    let mut single = ApeTag::new();
    single.insert(item.clone());
    Mapping::of(&single.into())
}

/// Edit the MP4 `ilst`. Atoms the edit did not change keep all their values.
fn edit_ilst(mut original: Ilst, edited: &Tag) -> Ilst {
    let mut changes = Changes::between(&original.clone().into(), edited);
    let mappings = (&original)
        .into_iter()
        .map(|atom| {
            let mut single = Ilst::new();
            single.replace_atom(atom.clone());
            Mapping::of(&single.into())
        })
        .collect::<Vec<_>>();
    let mut replaced = changes.replaced(&mappings);
    let new = Ilst::from(changes.edited_part(edited));
    // A new atom also replaces one with its identifier that lofty has no
    // generic key for, so the two are not merged into one atom.
    for (atom, replaced) in (&original).into_iter().zip(&mut replaced) {
        *replaced |= (&new)
            .into_iter()
            .any(|new_atom| new_atom.ident() == atom.ident());
    }
    let mut replaced = replaced.into_iter();
    original.retain(|_| !replaced.next().unwrap_or_default());
    // Inserting merges the values of atoms with the same identifier, as the
    // edited tag holds one item per value.
    for atom in new {
        original.insert(atom);
    }
    original
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

/// A file's own tag of a type whose generic conversion loses items or values.
enum NativeTag {
    Vorbis(VorbisComments),
    Ape(ApeTag),
    Ilst(Ilst),
}

fn read_native_tag(
    path: &Path,
    file_type: FileType,
    tag_type: TagType,
) -> Result<Option<NativeTag>, LoftyError> {
    // Same options as the generic read, so the conversions match the edited tag.
    let options = ParseOptions::new().read_properties(false);
    let vorbis = |tag: Option<VorbisComments>| tag.map(NativeTag::Vorbis);
    let ape = |tag: Option<ApeTag>| tag.map(NativeTag::Ape);
    Ok(match (file_type, tag_type) {
        (FileType::Flac, TagType::VorbisComments) => {
            vorbis(read(path, options, |mut file: lofty::flac::FlacFile| {
                // FLAC keeps pictures in their own blocks. The generic tag
                // shows them after the comments' own pictures, and writing
                // the comments writes all of them as blocks.
                let pictures = file.remove_pictures();
                file.remove_vorbis_comments().map(|mut comments| {
                    for (picture, info) in pictures {
                        // Cannot fail: the information is given, not read
                        // from the picture.
                        let _ = comments.insert_picture(picture, Some(info));
                    }
                    comments
                })
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
        (FileType::Mp4, TagType::Mp4Ilst) => {
            read(path, options, |mut file: lofty::mp4::Mp4File| {
                file.remove_ilst()
            })?
            .map(NativeTag::Ilst)
        }
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

/// Write an MP4 (ALAC) file holding `ilst`.
#[cfg(test)]
pub(crate) fn write_test_mp4(path: &Path, ilst: &Ilst) {
    std::fs::write(path, include_bytes!("analysis/fixtures/tag-tone.m4a")).expect("write MP4");
    ilst.save_to_path(path, WriteOptions::new())
        .expect("write ilst");
}

/// Write an Opus file holding `comments`.
#[cfg(test)]
pub(crate) fn write_test_opus(path: &Path, comments: &VorbisComments) {
    std::fs::write(path, include_bytes!("analysis/fixtures/tag-tone.opus")).expect("write Opus");
    comments
        .save_to_path(path, WriteOptions::new())
        .expect("write Vorbis comments");
}

/// Write a WavPack file holding `tag`.
#[cfg(test)]
pub(crate) fn write_test_wavpack(path: &Path, tag: &ApeTag) {
    std::fs::write(path, include_bytes!("analysis/fixtures/tag-tone.wv")).expect("write WavPack");
    tag.save_to_path(path, WriteOptions::new())
        .expect("write APE tag");
}

/// Write a Monkey's Audio file holding `tag`: a version 3.99 header for one
/// frame of 8 kHz mono audio, and no audio data.
#[cfg(test)]
pub(crate) fn write_test_monkeys_audio(path: &Path, tag: &ApeTag) {
    let mut bytes = b"MAC ".to_vec();
    bytes.extend_from_slice(&3990_u16.to_le_bytes());
    // The descriptor, which ends 52 bytes into the file.
    let mut descriptor = [0_u8; 46];
    descriptor[2..6].copy_from_slice(&52_u32.to_le_bytes());
    bytes.extend_from_slice(&descriptor);
    // The header: compression level, flags, blocks per frame, blocks in
    // the final frame, frames, bits per sample, channels, and sample rate.
    bytes.extend_from_slice(&2000_u16.to_le_bytes());
    bytes.extend_from_slice(&0_u16.to_le_bytes());
    bytes.extend_from_slice(&73_728_u32.to_le_bytes());
    bytes.extend_from_slice(&800_u32.to_le_bytes());
    bytes.extend_from_slice(&1_u32.to_le_bytes());
    bytes.extend_from_slice(&16_u16.to_le_bytes());
    bytes.extend_from_slice(&1_u16.to_le_bytes());
    bytes.extend_from_slice(&8000_u32.to_le_bytes());
    std::fs::write(path, bytes).expect("write Monkey's Audio");
    tag.save_to_path(path, WriteOptions::new())
        .expect("write APE tag");
}

#[cfg(test)]
mod tests {
    use super::*;
    use lofty::file::TaggedFileExt;
    use lofty::mp4::{Atom, AtomData, AtomIdent, Mp4File};
    use lofty::ogg::OpusFile;
    use lofty::picture::{MimeType, Picture, PictureType};
    use lofty::prelude::Accessor;
    use lofty::tag::ItemValue;
    use lofty::wavpack::WavPackFile;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_dir(name: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_or(0, |duration| duration.as_nanos());
        let dir = std::env::temp_dir().join(format!(
            "ferrous-tag-write-{name}-{}-{nanos}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).expect("create test dir");
        dir
    }

    fn picture(data: &[u8]) -> Picture {
        Picture::unchecked(data.to_vec())
            .mime_type(MimeType::Jpeg)
            .pic_type(PictureType::CoverFront)
            .build()
    }

    /// Replace the pictures of the generic tag read from `path`, as the
    /// artwork action does, and save it.
    fn save_with_picture(path: &Path, data: &[u8]) {
        let tagged = lofty::read_from_path(path).expect("read tags");
        let mut tag = tagged.primary_tag().cloned().expect("primary tag");
        while !tag.pictures().is_empty() {
            tag.remove_picture(0);
        }
        tag.push_picture(picture(data));
        save_tag(path, tagged.file_type(), &tag).expect("save tags");
    }

    #[test]
    fn picture_edits_keep_every_mp4_value() {
        let dir = test_dir("mp4-picture");
        let path = dir.join("song.m4a");
        let artist = AtomIdent::Fourcc(*b"\xa9ART");
        let mut ilst = Ilst::new();
        ilst.insert(
            Atom::from_collection(
                artist.clone(),
                vec![AtomData::UTF8("A".into()), AtomData::UTF8("B".into())],
            )
            .expect("artists"),
        );
        ilst.insert_picture(picture(b"old"));
        write_test_mp4(&path, &ilst);

        save_with_picture(&path, b"new");

        let mut file = File::open(&path).expect("open MP4");
        let ilst = Mp4File::read_from(&mut file, ParseOptions::new())
            .expect("parse MP4")
            .ilst()
            .cloned()
            .expect("ilst");
        let artists = ilst.get(&artist).expect("artists").data().count();
        assert_eq!(artists, 2);
        let pictures = ilst.pictures().expect("pictures").collect::<Vec<_>>();
        assert_eq!(pictures.len(), 1);
        assert_eq!(pictures[0].data(), b"new");
        std::fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn picture_edits_keep_every_vorbis_comment() {
        let dir = test_dir("opus-picture");
        let path = dir.join("song.opus");
        let mut comments = VorbisComments::new();
        for (key, value) in [("ARTIST", "A"), ("ENCODER", "Encoder X"), ("ARTIST", "B")] {
            comments.push(key.to_string(), value.to_string());
        }
        comments
            .insert_picture(picture(b"old"), Some(PictureInformation::default()))
            .expect("add picture");
        write_test_opus(&path, &comments);

        save_with_picture(&path, b"new");

        let mut file = File::open(&path).expect("open Opus");
        let after = OpusFile::read_from(&mut file, ParseOptions::new())
            .expect("parse Opus")
            .vorbis_comments()
            .clone();
        let items = after.items().collect::<Vec<_>>();
        assert_eq!(
            items,
            [("ARTIST", "A"), ("ENCODER", "Encoder X"), ("ARTIST", "B")]
        );
        assert_eq!(after.pictures().len(), 1);
        assert_eq!(after.pictures()[0].0.data(), b"new");
        std::fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn picture_edits_replace_the_ape_cover_and_keep_flags() {
        let dir = test_dir("wavpack-picture");
        let path = dir.join("song.wv");
        let mut artist =
            ApeItem::new("Artist".into(), ItemValue::Text("A\0B".into())).expect("artist");
        artist.read_only = true;
        let mut tag = ApeTag::new();
        tag.insert(artist.clone());
        tag.insert(
            ApeItem::new(
                "Cover Art (Front)".into(),
                ItemValue::Binary(picture(b"old").as_ape_bytes()),
            )
            .expect("cover"),
        );
        write_test_wavpack(&path, &tag);

        save_with_picture(&path, b"new");

        let mut file = File::open(&path).expect("open WavPack");
        let after = WavPackFile::read_from(&mut file, ParseOptions::new())
            .expect("parse WavPack")
            .ape()
            .cloned()
            .expect("APE tag");
        assert_eq!(after.get("Artist"), Some(&artist));
        let cover = after.get("Cover Art (Front)").expect("cover").value();
        assert_eq!(cover, &ItemValue::Binary(picture(b"new").as_ape_bytes()));
        assert_eq!(after.into_iter().count(), 2);
        std::fs::remove_dir_all(dir).expect("remove test dir");
    }

    #[test]
    fn edited_vorbis_encoder_becomes_a_comment_and_keeps_the_vendor() {
        let mut original = VorbisComments::new();
        original.set_vendor("vendor".to_string());
        original.push("TITLE".to_string(), "Title".to_string());
        let mut edited = Tag::from(original.clone());
        assert_eq!(edited.get_string(ItemKey::EncoderSoftware), Some("vendor"));
        edited.insert_text(ItemKey::EncoderSoftware, "Encoder X".to_string());

        let merged = edit_vorbis_comments(original, &edited);

        assert_eq!(merged.vendor(), "vendor");
        assert_eq!(
            merged.items().collect::<Vec<_>>(),
            [("TITLE", "Title"), ("ENCODER", "Encoder X")]
        );
        assert_eq!(Tag::from(merged).title().as_deref(), Some("Title"));
    }
}
