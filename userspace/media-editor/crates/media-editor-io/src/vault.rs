// SPDX-License-Identifier: GPL-3.0-only
//! `SSV1`: a media store that lives inside one of Phipia's files.
//!
//! ## Why a store rather than a directory
//!
//! Three numbers from Phipia's filesystem decide this, and none of them is a
//! criticism — they are what a bounded, provable filesystem costs:
//!
//! - a directory holds **sixty-four** live entries, and the mount validator
//!   accepts **two hundred and fifty-six** directories;
//! - a name is **8.3**, uppercase, from letters, digits and sixteen
//!   punctuation marks, so `sunset_beach_take3` is not a filename;
//! - a file is bounded to **sixteen mebibytes**, and the kernel owns
//!   **sixteen** open handles at once.
//!
//! A hundred photographs cannot be a hundred files — and a file holds two
//! frames of high-definition RGBA and not three. Even if a hundred files were
//! possible, they could not keep their names, and an editor that renamed somebody's material
//! to `IMG~0007.BMP` on import has lost the only thing that told them what it
//! was.
//!
//! So a vault is **one** file, 8.3-named like everything else, whose inside
//! Media Editor owns: an index and a payload, holding as many pieces of material
//! as fit, each keyed by what it *is* and carrying the name it arrived with.
//! Phipia's Files app sees one item; Media Editor sees the library.
//!
//! ## The layout
//!
//! ```text
//! offset  size    field
//! 0       4       magic, "SSV1"
//! 4       2       format version, little-endian
//! 6       2       reserved, must be zero
//! 8       4       item count
//! 12      4       reserved, must be zero
//! 16      8       payload bytes
//! 24      32      SHA-256 of bytes 0..24, then every entry, then the payload
//! 56      112*N   the entries
//! 56+112N P       the payload
//! ```
//!
//! One entry:
//!
//! ```text
//! offset  size  field
//! 0       32    the item's content digest
//! 32      8     where it starts in the payload
//! 40      8     how long it is
//! 48      2     how many bytes of name follow
//! 50      62    the name, the rest zero
//! ```
//!
//! ## What is checked, and what that buys
//!
//! The entries are in **ascending offset, contiguous from nought**, so the
//! payload is exactly the items in entry order with nothing between them. That
//! makes the encoding canonical — there is only one file for a given vault —
//! and it makes a gap or an overlap a refusal rather than an oddity.
//!
//! Every digest is **recomputed** from the bytes rather than believed, which
//! is the decision a title and a nest both make and for the same reason: a
//! file that says a piece of material is something it is not would repoint
//! every clip that referred to it. Phipia's FAT32 is explicitly not journaled
//! and says an interruption may leave inconsistency; this is what turns that
//! from a silent wrong answer into a named refusal.
//!
//! And the whole file is sealed by one digest over the header, the index and
//! the payload, the same shape the project file uses.

use alloc::string::String;
use alloc::vec::Vec;

use media_editor_abi::seam::{Slot, Storage};
use media_editor_core::{Digest, Sha256};

use crate::bytes::{Reader, Writer};
use crate::phipia;
use crate::status::{IoStatus, Result};

/// What a vault file begins with.
pub const MAGIC: [u8; 4] = *b"SSV1";

/// The version of this layout.
pub const FORMAT_VERSION: u16 = 1;

/// How many bytes precede the first entry.
pub const HEADER_BYTES: usize = 56;

/// How many bytes one entry takes.
pub const ENTRY_BYTES: usize = 112;

/// How many bytes of name one item may carry.
///
/// Sixty-two, which is what is left of an entry once the digest, the span and
/// the length are in it — and which is five times what Phipia's 8.3 names can
/// express. That ratio is the point of the whole module.
pub const MAX_NAME_BYTES: usize = 62;

/// How many items one vault may hold.
///
/// Two hundred and fifty-six: four times Phipia's per-directory limit, and
/// bounded so the index is a known size. The index at this count is 28,728
/// bytes, which leaves 16,748,488 of Phipia's sixteen-mebibyte file for
/// material — derived rather than chosen, and asserted below.
pub const MAX_ITEMS: usize = 256;

/// How many bytes of payload fit once a full index is in the file.
pub const MAX_PAYLOAD_BYTES: usize =
    phipia::MAX_FILE_BYTES - HEADER_BYTES - ENTRY_BYTES * MAX_ITEMS;

const _: () = assert!(
    MAX_PAYLOAD_BYTES == 16_748_488,
    "the payload bound is derived from Phipia's file bound and the index"
);

/// One piece of material in a vault.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Item {
    digest: Digest,
    name: String,
    bytes: Vec<u8>,
}

impl Item {
    /// What this material is.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// What it was called when it arrived.
    ///
    /// The one thing a vault carries that Phipia's filesystem cannot. It is a
    /// hint for a person, never an identity: two items with one name are two
    /// items, and the digest is what says whether they are the same material.
    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }

    /// The material itself.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

/// A media store.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Vault {
    items: Vec<Item>,
}

impl Vault {
    /// An empty vault.
    #[must_use]
    pub fn new() -> Self {
        Self { items: Vec::new() }
    }

    /// The material, in the order it was added.
    #[must_use]
    pub fn items(&self) -> &[Item] {
        &self.items
    }

    /// How many pieces of material.
    #[must_use]
    pub fn len(&self) -> usize {
        self.items.len()
    }

    /// Whether it holds nothing.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// How many bytes of material, which is what the file will hold.
    #[must_use]
    pub fn payload_bytes(&self) -> usize {
        self.items.iter().map(|item| item.bytes.len()).sum()
    }

    /// The material with this digest.
    #[must_use]
    pub fn get(&self, digest: Digest) -> Option<&Item> {
        self.items.iter().find(|item| item.digest == digest)
    }

    /// Whether this vault holds that material.
    #[must_use]
    pub fn holds(&self, digest: Digest) -> bool {
        self.get(digest).is_some()
    }

    /// Put material in, and say what it is.
    ///
    /// The digest is computed here rather than accepted, which is what makes
    /// "paste this in" and "is this already in" one question.
    ///
    /// # Errors
    ///
    /// [`IoStatus::VaultFull`] past [`MAX_ITEMS`],
    /// [`IoStatus::VaultTooLarge`] past [`MAX_PAYLOAD_BYTES`],
    /// [`IoStatus::VaultNameTooLong`] past [`MAX_NAME_BYTES`],
    /// [`IoStatus::VaultNameNotText`] for a name that is not printable ASCII,
    /// [`IoStatus::VaultItemTwice`] for material already here, and
    /// [`IoStatus::OutOfMemory`].
    pub fn insert(&mut self, name: &str, bytes: &[u8]) -> Result<Digest> {
        if self.items.len() == MAX_ITEMS {
            return Err(IoStatus::VaultFull);
        }
        if name.len() > MAX_NAME_BYTES {
            return Err(IoStatus::VaultNameTooLong);
        }
        if name.is_empty() || !name.bytes().all(|byte| (0x20..=0x7E).contains(&byte)) {
            // Printable ASCII, and something rather than nothing. Not because
            // a name has to be ASCII in principle, but because this build has
            // no text stack to decide what a name in another script *is* — and
            // a name nobody can render is a name nobody can pick from a list.
            return Err(IoStatus::VaultNameNotText);
        }
        let digest = Digest::of(bytes);
        if self.holds(digest) {
            // The same material twice is one piece of material. Refusing
            // rather than folding them together is deliberate: the caller
            // asked to add something, and quietly doing nothing would leave
            // them believing a second copy exists.
            return Err(IoStatus::VaultItemTwice);
        }
        let payload = self
            .payload_bytes()
            .checked_add(bytes.len())
            .ok_or(IoStatus::TooMany)?;
        if payload > MAX_PAYLOAD_BYTES {
            return Err(IoStatus::VaultTooLarge);
        }
        let mut held = Vec::new();
        held.try_reserve(bytes.len())
            .map_err(|_| IoStatus::OutOfMemory)?;
        held.extend_from_slice(bytes);
        let mut label = String::new();
        label
            .try_reserve(name.len())
            .map_err(|_| IoStatus::OutOfMemory)?;
        label.push_str(name);
        self.items
            .try_reserve(1)
            .map_err(|_| IoStatus::OutOfMemory)?;
        self.items.push(Item {
            digest,
            name: label,
            bytes: held,
        });
        Ok(digest)
    }

    /// Take material out, and hand it back.
    ///
    /// # Errors
    ///
    /// [`IoStatus::VaultItemAbsent`] if it is not there.
    pub fn remove(&mut self, digest: Digest) -> Result<Item> {
        let at = self
            .items
            .iter()
            .position(|item| item.digest == digest)
            .ok_or(IoStatus::VaultItemAbsent)?;
        Ok(self.items.remove(at))
    }
}

/// Write a vault out.
///
/// # Errors
///
/// [`IoStatus::TooMany`] for a vault past its own bounds, or
/// [`IoStatus::OutOfMemory`].
pub fn encode(vault: &Vault) -> Result<Vec<u8>> {
    let payload = vault.payload_bytes();
    let total = HEADER_BYTES
        .checked_add(
            ENTRY_BYTES
                .checked_mul(vault.len())
                .ok_or(IoStatus::TooMany)?,
        )
        .and_then(|index| index.checked_add(payload))
        .ok_or(IoStatus::TooMany)?;
    let mut writer = Writer::new(phipia::MAX_FILE_BYTES);
    writer.bytes(&MAGIC)?;
    writer.u16(FORMAT_VERSION)?;
    writer.u16(0)?;
    writer.u32(u32::try_from(vault.len()).map_err(|_| IoStatus::TooMany)?)?;
    writer.u32(0)?;
    writer.u64(u64::try_from(payload).map_err(|_| IoStatus::TooMany)?)?;
    // The digest goes in after the rest is written, like the project file's:
    // it covers what follows it as well as what precedes it, so the space is
    // held with zeroes and filled once there is something to seal.
    writer.bytes(&[0_u8; 32])?;

    let mut at = 0_u64;
    for item in vault.items() {
        writer.bytes(item.digest.bytes())?;
        writer.u64(at)?;
        let length = u64::try_from(item.bytes.len()).map_err(|_| IoStatus::TooMany)?;
        writer.u64(length)?;
        at = at.checked_add(length).ok_or(IoStatus::TooMany)?;
        let name = item.name.as_bytes();
        writer.u16(u16::try_from(name.len()).map_err(|_| IoStatus::TooMany)?)?;
        writer.bytes(name)?;
        for _ in name.len()..MAX_NAME_BYTES {
            writer.u8(0)?;
        }
    }
    for item in vault.items() {
        writer.bytes(&item.bytes)?;
    }
    let mut file = writer.finish();
    if file.len() != total {
        // The one place this could go wrong silently: a writer that produced a
        // different number of bytes from the header's own arithmetic would
        // make every offset in the index a lie.
        return Err(IoStatus::TooMany);
    }
    let sealed = seal(&file);
    file[24..HEADER_BYTES].copy_from_slice(sealed.bytes());
    Ok(file)
}

/// Read a vault back.
///
/// # Errors
///
/// [`IoStatus::NotAVault`], [`IoStatus::UnsupportedVersion`],
/// [`IoStatus::ReservedFieldSet`], [`IoStatus::DigestMismatch`],
/// [`IoStatus::TruncatedPayload`], [`IoStatus::TrailingBytes`],
/// [`IoStatus::VaultSpanNotContiguous`] for a gap, an overlap or an
/// out-of-order entry, [`IoStatus::VaultItemDigestMismatch`] for material that
/// is not what its entry says, and whatever [`Vault::insert`] refuses.
pub fn decode(file: &[u8]) -> Result<Vault> {
    if file.len() < HEADER_BYTES {
        return Err(IoStatus::NotAVault);
    }
    let mut reader = Reader::new(file);
    if reader.take(MAGIC.len())? != MAGIC {
        return Err(IoStatus::NotAVault);
    }
    let version = reader.u16()?;
    if version != FORMAT_VERSION {
        return Err(IoStatus::UnsupportedVersion(version));
    }
    if reader.u16()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let count = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
    if count > MAX_ITEMS {
        return Err(IoStatus::TooMany);
    }
    if reader.u32()? != 0 {
        return Err(IoStatus::ReservedFieldSet);
    }
    let payload_bytes = usize::try_from(reader.u64()?).map_err(|_| IoStatus::TooMany)?;
    if payload_bytes > MAX_PAYLOAD_BYTES {
        return Err(IoStatus::TooMany);
    }
    let stated = Digest::new(reader.digest_bytes()?);

    // The seal, before a single field below it is believed. Phipia's FAT32 is
    // explicitly not journaled and says an interruption may leave a file it
    // will refuse at the next mount; this is the same answer one level up.
    if seal(file) != stated {
        return Err(IoStatus::DigestMismatch);
    }

    let index = HEADER_BYTES
        .checked_add(ENTRY_BYTES.checked_mul(count).ok_or(IoStatus::TooMany)?)
        .ok_or(IoStatus::TooMany)?;
    let total = index.checked_add(payload_bytes).ok_or(IoStatus::TooMany)?;
    if file.len() < total {
        return Err(IoStatus::TruncatedPayload);
    }
    if file.len() > total {
        return Err(IoStatus::TrailingBytes);
    }
    let payload = file.get(index..).ok_or(IoStatus::TruncatedPayload)?;

    let mut vault = Vault::new();
    let mut expected = 0_u64;
    for _ in 0..count {
        let digest = Digest::new(reader.digest_bytes()?);
        let at = reader.u64()?;
        let length = reader.u64()?;
        if at != expected {
            // Ascending, contiguous, from nought. A file whose spans leave a
            // gap holds bytes nothing accounts for; one whose spans overlap
            // holds two items claiming one run of bytes. Neither is a file
            // this encoder could have written.
            return Err(IoStatus::VaultSpanNotContiguous);
        }
        expected = expected.checked_add(length).ok_or(IoStatus::TooMany)?;
        let words = usize::from(reader.u16()?);
        if words > MAX_NAME_BYTES {
            return Err(IoStatus::VaultNameTooLong);
        }
        let held = reader.take(MAX_NAME_BYTES)?;
        for byte in held.get(words..).ok_or(IoStatus::TruncatedField)? {
            if *byte != 0 {
                // The unused tail of a name field is zero, so there is exactly
                // one encoding of a given name and two files holding one vault
                // are one file.
                return Err(IoStatus::ReservedFieldSet);
            }
        }
        let name = core::str::from_utf8(held.get(..words).ok_or(IoStatus::TruncatedField)?)
            .map_err(|_| IoStatus::VaultNameNotText)?;
        let start = usize::try_from(at).map_err(|_| IoStatus::TooMany)?;
        let stop = usize::try_from(expected).map_err(|_| IoStatus::TooMany)?;
        let bytes = payload.get(start..stop).ok_or(IoStatus::TruncatedPayload)?;
        // Through the vault's own door, so a file cannot hold a vault no
        // sequence of imports could produce -- and `insert` recomputes the
        // digest, which is what turns a bad cluster into a named refusal.
        let computed = vault.insert(name, bytes)?;
        if computed != digest {
            return Err(IoStatus::VaultItemDigestMismatch);
        }
    }
    if expected != u64::try_from(payload_bytes).unwrap_or(u64::MAX) {
        return Err(IoStatus::VaultSpanNotContiguous);
    }
    Ok(vault)
}

/// The digest a vault file carries: everything but the field holding it.
///
/// The thirty-two bytes at offset 24 are skipped rather than zeroed in a copy,
/// which matters at sixteen mebibytes: a copy to seal a file would double the
/// largest allocation this program makes.
fn seal(file: &[u8]) -> Digest {
    let mut hasher = Sha256::new();
    hasher.update(&file[..24]);
    if let Some(rest) = file.get(HEADER_BYTES..) {
        hasher.update(rest);
    }
    hasher.finish()
}

/// A vault, answering the render's questions about media.
///
/// This is where the whole thing pays off: material pasted into a vault is
/// keyed by its digest, a clip in the model refers to media by digest, and a
/// source node in the graph asks for a digest and a tick. So a vault *is* a
/// [`media_editor_render::Library`], with nothing in between translating — and
/// "the file is in the vault" and "the picture renders" become one fact rather
/// than two that have to be kept in step.
///
/// Every item is an [`crate::sprw::Reel`], which is what makes one answer do
/// for both kinds of material a person pastes in: a photograph is a reel of
/// one frame, and a piece of footage is a reel of many. Nothing above here has
/// to ask which it got.
pub struct Shelf<'a> {
    vault: &'a Vault,
    looks: &'a [(Digest, media_editor_render::Look)],
}

impl<'a> Shelf<'a> {
    /// Read from a vault, with looks supplied separately.
    ///
    /// Looks are not vault material and deliberately do not become it: a cube
    /// is a *table*, not a picture, and folding the two into one store would
    /// mean a media library that could hand back something no clip can show.
    #[must_use]
    pub const fn new(vault: &'a Vault, looks: &'a [(Digest, media_editor_render::Look)]) -> Self {
        Self { vault, looks }
    }
}

impl media_editor_render::Library for Shelf<'_> {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: media_editor_media::FrameDescription,
    ) -> media_editor_render::Result<media_editor_media::Frame> {
        let item = self
            .vault
            .get(media)
            .ok_or(media_editor_render::RenderStatus::MediaAbsent)?;
        let reel = crate::sprw::decode(item.bytes())
            .map_err(|_| media_editor_render::RenderStatus::MediaUnreadable)?;
        // A tick outside the reel is a clip reading past its material, which
        // the model refuses at the edit. Reaching it here means something
        // upstream did not ask, so it is a refusal rather than a clamp: a
        // clamp would show the last frame forever and call it the picture.
        let at =
            usize::try_from(tick).map_err(|_| media_editor_render::RenderStatus::FrameAbsent)?;
        let frame = reel
            .frames()
            .get(at)
            .ok_or(media_editor_render::RenderStatus::FrameAbsent)?;
        if frame.description() != &description {
            // The library hands back what it has, described as it is. A
            // library that converted to whatever was asked for would put a
            // conversion inside the cache key of the *source*, which is the
            // staleness a content-addressed graph exists to prevent.
            return Err(media_editor_render::RenderStatus::SourceDescriptionMismatch);
        }
        Ok(frame.clone())
    }

    fn available(&mut self, media: Digest) -> bool {
        self.vault.holds(media)
    }

    fn look(&mut self, look: Digest) -> media_editor_render::Result<media_editor_render::Look> {
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(media_editor_render::RenderStatus::LookAbsent)
    }
}

/// One entry of a vault's index, read out of storage on its own.
///
/// A hundred and twelve bytes, which is what a catalogue holds at a time and
/// the largest single thing it ever asks for besides material a caller has
/// already made room for.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    digest: Digest,
    at: u64,
    length: u64,
    name: String,
}

impl Entry {
    /// What the material is.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// Where the material starts, in the payload.
    #[must_use]
    pub const fn at(&self) -> u64 {
        self.at
    }

    /// How many bytes of material.
    #[must_use]
    pub const fn length(&self) -> u64 {
        self.length
    }

    /// What it was called when it arrived.
    #[must_use]
    pub fn name(&self) -> &str {
        &self.name
    }
}

/// A vault read through the seam, an entry at a time.
///
/// **This is the shape the target requires.** One of Phipia's files holds
/// sixteen mebibytes and a Phipia program is mapped seventy-six kilobytes, so
/// [`decode`] — which builds the whole vault in memory — cannot run against a
/// full one on the machine this is for, by three orders of magnitude. A
/// catalogue never holds more than its header and one entry, and asks storage
/// for material only when somebody wants that material.
///
/// It is also what Phipia does. Its own bitmap importer issues random row
/// reads through the filesystem rather than holding a picture, which is the
/// same decision one layer down.
///
/// ## What `open` does not do
///
/// **It does not check the seal**, and that has to be said out loud rather
/// than discovered. Verifying the seal means reading every byte of the file,
/// which is the thing a catalogue exists to avoid; [`Catalogue::verify`] is
/// there for when a caller can afford it — at import, at a fitness check, on a
/// machine with memory.
///
/// What `open` *does* check is everything a fixed-size header can be checked
/// against on its own: the magic, the version, both reserved words, the count
/// against its bound, and the payload length against what the slot actually
/// holds. What it cannot check without reading is caught later and named
/// where it happens — a span that does not line up, or material that is not
/// what its entry says.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Catalogue {
    slot: Slot,
    count: usize,
    payload_bytes: u64,
}

impl Catalogue {
    /// Open a vault in a slot, reading its header and nothing else.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Seam`] for anything storage refuses,
    /// [`IoStatus::NotAVault`], [`IoStatus::UnsupportedVersion`],
    /// [`IoStatus::ReservedFieldSet`], [`IoStatus::TooMany`] past the item
    /// bound, and [`IoStatus::TruncatedPayload`] or
    /// [`IoStatus::TrailingBytes`] if the slot is not the length its own
    /// header describes.
    pub fn open(storage: &dyn Storage, slot: Slot) -> Result<Self> {
        let mut header = [0_u8; HEADER_BYTES];
        let read = storage.read_at(slot, 0, &mut header)?;
        if read != HEADER_BYTES {
            return Err(IoStatus::NotAVault);
        }
        let mut reader = Reader::new(&header);
        if reader.take(MAGIC.len())? != MAGIC {
            return Err(IoStatus::NotAVault);
        }
        let version = reader.u16()?;
        if version != FORMAT_VERSION {
            return Err(IoStatus::UnsupportedVersion(version));
        }
        if reader.u16()? != 0 {
            return Err(IoStatus::ReservedFieldSet);
        }
        let count = usize::try_from(reader.u32()?).map_err(|_| IoStatus::TooMany)?;
        if count > MAX_ITEMS {
            return Err(IoStatus::TooMany);
        }
        if reader.u32()? != 0 {
            return Err(IoStatus::ReservedFieldSet);
        }
        let payload_bytes = reader.u64()?;
        if payload_bytes > u64::try_from(MAX_PAYLOAD_BYTES).unwrap_or(u64::MAX) {
            return Err(IoStatus::TooMany);
        }
        let held = storage.len(slot)?;
        let stated = payload_at(count)?
            .checked_add(usize::try_from(payload_bytes).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        if held < stated {
            return Err(IoStatus::TruncatedPayload);
        }
        if held > stated {
            return Err(IoStatus::TrailingBytes);
        }
        Ok(Self {
            slot,
            count,
            payload_bytes,
        })
    }

    /// Which slot this reads from.
    #[must_use]
    pub const fn slot(&self) -> Slot {
        self.slot
    }

    /// How many pieces of material.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.count
    }

    /// Whether the vault holds nothing.
    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.count == 0
    }

    /// How many bytes of material the vault holds altogether.
    #[must_use]
    pub const fn payload_bytes(&self) -> u64 {
        self.payload_bytes
    }

    /// One entry, by position.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Seam`], [`IoStatus::VaultItemAbsent`] for a position past
    /// the count, [`IoStatus::TruncatedField`] for an entry the slot does not
    /// hold in full, [`IoStatus::VaultNameTooLong`], and
    /// [`IoStatus::ReservedFieldSet`] for a name field with a tail.
    pub fn entry(&self, storage: &dyn Storage, index: usize) -> Result<Entry> {
        if index >= self.count {
            return Err(IoStatus::VaultItemAbsent);
        }
        let at = HEADER_BYTES
            .checked_add(ENTRY_BYTES.checked_mul(index).ok_or(IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        let mut bytes = [0_u8; ENTRY_BYTES];
        if storage.read_at(self.slot, at, &mut bytes)? != ENTRY_BYTES {
            return Err(IoStatus::TruncatedField);
        }
        read_entry(&bytes)
    }

    /// The entry for material with this digest, if the vault holds it.
    ///
    /// A scan, and deliberately so: the index is in ascending *offset* order
    /// rather than digest order, because that is what makes the payload
    /// exactly the items end to end and the encoding canonical. Two hundred
    /// and fifty-six reads of a hundred and twelve bytes is the price of that,
    /// and it is bounded — which on this platform is the property that matters
    /// more than the constant.
    ///
    /// # Errors
    ///
    /// Whatever [`Catalogue::entry`] refuses.
    pub fn find(&self, storage: &dyn Storage, digest: Digest) -> Result<Option<Entry>> {
        for index in 0..self.count {
            let entry = self.entry(storage, index)?;
            if entry.digest == digest {
                return Ok(Some(entry));
            }
        }
        Ok(None)
    }

    /// Copy an entry's material into `into`, and say how many bytes that was.
    ///
    /// Short only at the end of the material, never in the middle: a caller
    /// with a buffer smaller than the item gets what fits, and a caller with a
    /// larger one gets the item. Which is how a caller with seventy-six
    /// kilobytes reads a photograph it cannot hold.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Seam`], or [`IoStatus::TruncatedPayload`] if the slot ends
    /// inside the span the entry describes.
    pub fn material(
        &self,
        storage: &dyn Storage,
        entry: &Entry,
        offset: u64,
        into: &mut [u8],
    ) -> Result<usize> {
        let past = entry.length.saturating_sub(offset);
        if past == 0 {
            return Ok(0);
        }
        let wanted = usize::try_from(past).unwrap_or(usize::MAX).min(into.len());
        let at = payload_at(self.count)?
            .checked_add(usize::try_from(entry.at).map_err(|_| IoStatus::TooMany)?)
            .and_then(|start| {
                start.checked_add(
                    usize::try_from(offset)
                        .map_err(|_| IoStatus::TooMany)
                        .ok()?,
                )
            })
            .ok_or(IoStatus::TooMany)?;
        let read = storage.read_at(self.slot, at, &mut into[..wanted])?;
        if read != wanted {
            return Err(IoStatus::TruncatedPayload);
        }
        Ok(read)
    }

    /// Walk the whole vault and check every claim it makes.
    ///
    /// The expensive answer, for when a caller can afford it: every span
    /// checked to run end to end, every digest recomputed from the material,
    /// and the seal recomputed over the whole file. This is what [`decode`]
    /// does implicitly, done without building a vault — so it can be run on a
    /// file far larger than the memory available, `chunk` bytes at a time.
    ///
    /// # Errors
    ///
    /// [`IoStatus::Seam`], [`IoStatus::VaultSpanNotContiguous`],
    /// [`IoStatus::VaultItemDigestMismatch`], [`IoStatus::DigestMismatch`] for
    /// a broken seal, or [`IoStatus::OutOfMemory`] for a chunk it cannot hold.
    pub fn verify(&self, storage: &dyn Storage, chunk: usize) -> Result<()> {
        if chunk == 0 {
            return Err(IoStatus::TooMany);
        }
        let mut window = Vec::new();
        window
            .try_reserve(chunk)
            .map_err(|_| IoStatus::OutOfMemory)?;
        window.resize(chunk, 0);

        let mut sealed = Sha256::new();
        let mut stated = [0_u8; 32];
        let mut expected = 0_u64;
        // The header's first twenty-four bytes, then the field holding the
        // seal, which is skipped for the reason `seal` skips it.
        let mut header = [0_u8; HEADER_BYTES];
        if storage.read_at(self.slot, 0, &mut header)? != HEADER_BYTES {
            return Err(IoStatus::NotAVault);
        }
        sealed.update(&header[..24]);
        stated.copy_from_slice(&header[24..HEADER_BYTES]);

        for index in 0..self.count {
            let entry = self.entry(storage, index)?;
            if entry.at != expected {
                return Err(IoStatus::VaultSpanNotContiguous);
            }
            expected = expected
                .checked_add(entry.length)
                .ok_or(IoStatus::TooMany)?;
        }
        if expected != self.payload_bytes {
            return Err(IoStatus::VaultSpanNotContiguous);
        }
        // Everything past the header, in windows, sealing as it goes and
        // re-digesting each item's material where it falls. Two passes would
        // be simpler and would read the file twice.
        let start = HEADER_BYTES;
        let end = payload_at(self.count)?
            .checked_add(usize::try_from(self.payload_bytes).map_err(|_| IoStatus::TooMany)?)
            .ok_or(IoStatus::TooMany)?;
        let mut at = start;
        while at < end {
            let wanted = chunk.min(end - at);
            let read = storage.read_at(self.slot, at, &mut window[..wanted])?;
            if read != wanted {
                return Err(IoStatus::TruncatedPayload);
            }
            sealed.update(&window[..wanted]);
            at += wanted;
        }
        if sealed.finish() != Digest::new(stated) {
            return Err(IoStatus::DigestMismatch);
        }
        for index in 0..self.count {
            let entry = self.entry(storage, index)?;
            let mut item = Sha256::new();
            let mut done = 0_u64;
            while done < entry.length {
                let read = self.material(storage, &entry, done, &mut window)?;
                if read == 0 {
                    return Err(IoStatus::TruncatedPayload);
                }
                item.update(&window[..read]);
                done = done
                    .checked_add(u64::try_from(read).unwrap_or(u64::MAX))
                    .ok_or(IoStatus::TooMany)?;
            }
            if item.finish() != entry.digest {
                return Err(IoStatus::VaultItemDigestMismatch);
            }
        }
        Ok(())
    }
}

/// Where the payload begins, given how many entries precede it.
fn payload_at(count: usize) -> Result<usize> {
    HEADER_BYTES
        .checked_add(ENTRY_BYTES.checked_mul(count).ok_or(IoStatus::TooMany)?)
        .ok_or(IoStatus::TooMany)
}

/// One entry, out of its hundred and twelve bytes.
fn read_entry(bytes: &[u8; ENTRY_BYTES]) -> Result<Entry> {
    let mut reader = Reader::new(bytes);
    let digest = Digest::new(reader.digest_bytes()?);
    let at = reader.u64()?;
    let length = reader.u64()?;
    let words = usize::from(reader.u16()?);
    if words > MAX_NAME_BYTES {
        return Err(IoStatus::VaultNameTooLong);
    }
    let held = reader.take(MAX_NAME_BYTES)?;
    for byte in held.get(words..).ok_or(IoStatus::TruncatedField)? {
        if *byte != 0 {
            return Err(IoStatus::ReservedFieldSet);
        }
    }
    let text = core::str::from_utf8(held.get(..words).ok_or(IoStatus::TruncatedField)?)
        .map_err(|_| IoStatus::VaultNameNotText)?;
    let mut name = String::new();
    name.try_reserve(text.len())
        .map_err(|_| IoStatus::OutOfMemory)?;
    name.push_str(text);
    Ok(Entry {
        digest,
        at,
        length,
        name,
    })
}

/// Write a vault, and return the digest of the file that was committed.
///
/// The same four steps [`crate::save::save`] performs and for the same reason
/// (R-9.4): encode, write to the scratch slot, read it back and compare, and
/// only then commit. Steps one to three cannot touch the vault slot at all, so
/// an interrupted save leaves the last good vault exactly where it was.
///
/// # Errors
///
/// [`IoStatus::Seam`] for anything storage refuses,
/// [`IoStatus::WriteNotVerified`] if the bytes did not read back as
/// themselves, or an encoding refusal. In every case the vault slot is
/// unchanged.
pub fn store(vault: &Vault, storage: &mut dyn Storage) -> Result<Digest> {
    let file = encode(vault)?;
    storage.write(Slot::Scratch, &file)?;
    let mut echoed = Vec::new();
    echoed
        .try_reserve(file.len())
        .map_err(|_| IoStatus::OutOfMemory)?;
    echoed.resize(file.len(), 0);
    let read = storage.read(Slot::Scratch, &mut echoed)?;
    if read != file.len() || echoed != file {
        return Err(IoStatus::WriteNotVerified);
    }
    storage.commit(Slot::Vault)?;
    Ok(Digest::of(&file))
}

/// Read the committed vault, whole.
///
/// For a caller with room for it. On Phipia there is not: use
/// [`Catalogue::open`], which reads a header and asks for material only when
/// somebody wants material.
///
/// # Errors
///
/// [`IoStatus::Seam`] if there is nothing to read, or any decoding refusal.
pub fn fetch(storage: &dyn Storage) -> Result<Vault> {
    let length = storage.len(Slot::Vault)?;
    let mut file = Vec::new();
    file.try_reserve(length)
        .map_err(|_| IoStatus::OutOfMemory)?;
    file.resize(length, 0);
    let read = storage.read(Slot::Vault, &mut file)?;
    if read != length {
        return Err(IoStatus::TruncatedPayload);
    }
    decode(&file)
}

/// One piece of a vault's material, as a run of bytes on its own.
///
/// What turns a catalogue entry into something a reel reader can be pointed
/// at. The offsets it hides are the ones nothing above it should have to know:
/// a caller asks for byte nought of *this photograph* and gets it, wherever in
/// the file the photograph happens to sit.
pub struct Material<'a> {
    catalogue: &'a Catalogue,
    entry: &'a Entry,
    storage: &'a dyn Storage,
}

impl<'a> Material<'a> {
    /// Point at one entry's material.
    #[must_use]
    pub const fn new(catalogue: &'a Catalogue, entry: &'a Entry, storage: &'a dyn Storage) -> Self {
        Self {
            catalogue,
            entry,
            storage,
        }
    }
}

impl crate::bytes::Extent for Material<'_> {
    fn length(&self) -> u64 {
        self.entry.length()
    }

    fn read_at(&self, offset: u64, into: &mut [u8]) -> Result<usize> {
        self.catalogue
            .material(self.storage, self.entry, offset, into)
    }
}

/// A vault answering the render's questions **through storage**, never loaded.
///
/// [`Shelf`] holds a whole vault in memory, which is what a host test wants
/// and what the target cannot afford. This is the other one: a catalogue, a
/// storage, and a look table, and the largest thing it ever holds is one
/// frame.
///
/// The chain is worth reading once, because every link in it was built to
/// avoid holding the one above:
///
/// 1. a [`Catalogue`] holds a count and a payload length, not a vault;
/// 2. a [`Material`] is one entry's bytes, not a vault's;
/// 3. a [`crate::sprw::Spool`] holds a description and a count, not a reel;
/// 4. [`crate::sprw::Spool::frame`] builds one frame, not a reel of them.
///
/// The ceiling that remains is a frame, which is a fact about the picture
/// rather than about the material — and
/// [`crate::sprw::Spool::plane_row`] is there for a caller that cannot afford
/// even that.
pub struct Stacks<'a> {
    catalogue: &'a Catalogue,
    storage: &'a dyn Storage,
    looks: &'a [(Digest, media_editor_render::Look)],
}

impl<'a> Stacks<'a> {
    /// Read from a vault in storage, with looks supplied separately.
    #[must_use]
    pub const fn new(
        catalogue: &'a Catalogue,
        storage: &'a dyn Storage,
        looks: &'a [(Digest, media_editor_render::Look)],
    ) -> Self {
        Self {
            catalogue,
            storage,
            looks,
        }
    }
}

impl media_editor_model::caption::Transcript for Stacks<'_> {
    /// The words of a recording, out of a reel inside a vault inside a slot.
    ///
    /// The third thing this type serves, and the last of the three a
    /// projection needs. A reel that has no transcript is not an error here —
    /// it is a recording nobody has transcribed — so it answers with nothing
    /// rather than refusing, which is what lets a programme of captioned and
    /// uncaptioned shots be asked about at all.
    fn captions(
        &mut self,
        media: Digest,
        from: i64,
        to: i64,
    ) -> media_editor_model::Result<alloc::vec::Vec<media_editor_model::caption::Caption>> {
        let Ok(Some(entry)) = self.catalogue.find(self.storage, media) else {
            return Ok(alloc::vec::Vec::new());
        };
        let material = Material::new(self.catalogue, &entry, self.storage);
        let Ok(spool) = crate::sprw::Spool::open(&material) else {
            return Ok(alloc::vec::Vec::new());
        };
        if spool.spoken().count() == 0 {
            return Ok(alloc::vec::Vec::new());
        }
        spool
            .captions(&material, from, to)
            .map_err(|_| media_editor_model::ModelStatus::OutOfMemory)
    }
}

impl media_editor_audio::SampleSource for Stacks<'_> {
    /// Samples out of a reel inside a vault inside a slot.
    ///
    /// The sound half of what this type already did for pictures, and the last
    /// link the chain was missing: nothing on this path holds a vault, a reel,
    /// or a take's sound. What comes off the storage is one block — one
    /// frame's worth — which at 48 kHz stereo is 12,816 bytes.
    ///
    /// It is asked for a **digest**, which is why it can exist at all: a vault
    /// is shared between projects and keyed by what material *is*, and the
    /// sound source trait used to be asked for a position in one project's
    /// table. There was no fourth asset to look up here.
    fn samples(
        &mut self,
        media: Digest,
        start: i64,
        count: usize,
    ) -> media_editor_audio::Result<media_editor_audio::AudioBuffer> {
        let entry = self
            .catalogue
            .find(self.storage, media)
            .map_err(|_| media_editor_audio::AudioStatus::NotMixable)?
            .ok_or(media_editor_audio::AudioStatus::NotMixable)?;
        let material = Material::new(self.catalogue, &entry, self.storage);
        let spool = crate::sprw::Spool::open(&material)
            .map_err(|_| media_editor_audio::AudioStatus::NotMixable)?;
        spool
            .samples(&material, start, count)
            .map_err(|_| media_editor_audio::AudioStatus::NotMixable)
    }
}

impl media_editor_render::Library for Stacks<'_> {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: media_editor_media::FrameDescription,
    ) -> media_editor_render::Result<media_editor_media::Frame> {
        let entry = self
            .catalogue
            .find(self.storage, media)
            .map_err(|_| media_editor_render::RenderStatus::MediaUnreadable)?
            .ok_or(media_editor_render::RenderStatus::MediaAbsent)?;
        let material = Material::new(self.catalogue, &entry, self.storage);
        let spool = crate::sprw::Spool::open(&material)
            .map_err(|_| media_editor_render::RenderStatus::MediaUnreadable)?;
        if spool.description() != &description {
            // The library hands back what it has, described as it is, for the
            // reason `Shelf` does: a conversion here would sit inside the
            // cache key of the source rather than beside it.
            return Err(media_editor_render::RenderStatus::SourceDescriptionMismatch);
        }
        let at =
            usize::try_from(tick).map_err(|_| media_editor_render::RenderStatus::FrameAbsent)?;
        spool
            .frame(&material, at)
            .map_err(|_| media_editor_render::RenderStatus::FrameAbsent)
    }

    fn available(&mut self, media: Digest) -> bool {
        self.catalogue
            .find(self.storage, media)
            .is_ok_and(|found| found.is_some())
    }

    fn look(&mut self, look: Digest) -> media_editor_render::Result<media_editor_render::Look> {
        self.looks
            .iter()
            .find(|(digest, _)| *digest == look)
            .map(|(_, held)| held.clone())
            .ok_or(media_editor_render::RenderStatus::LookAbsent)
    }

    fn row(
        &mut self,
        media: Digest,
        tick: i64,
        description: media_editor_media::FrameDescription,
        row: usize,
    ) -> media_editor_render::Result<media_editor_media::Frame> {
        // The end of the chain, and the reason every link of it was built.
        // Nothing on this path holds a vault, a reel, or a frame: what comes
        // off the storage is one row of one plane of one frame, which for a
        // 1920-wide picture is 5,760 bytes.
        let entry = self
            .catalogue
            .find(self.storage, media)
            .map_err(|_| media_editor_render::RenderStatus::MediaUnreadable)?
            .ok_or(media_editor_render::RenderStatus::MediaAbsent)?;
        let material = Material::new(self.catalogue, &entry, self.storage);
        let spool = crate::sprw::Spool::open(&material)
            .map_err(|_| media_editor_render::RenderStatus::MediaUnreadable)?;
        if spool.description() != &description {
            return Err(media_editor_render::RenderStatus::SourceDescriptionMismatch);
        }
        if description.format().plane_count() != 1 {
            // A planar format's row is not one read, and a subsampled one's is
            // not one row -- which the graph refuses first, so reaching this
            // means a planar format that is not subsampled. It is a shape this
            // build has not written rather than one it cannot.
            return Err(media_editor_render::RenderStatus::NoRowForm);
        }
        let at =
            usize::try_from(tick).map_err(|_| media_editor_render::RenderStatus::FrameAbsent)?;
        let stride = description
            .format()
            .plane_row_bytes(description.geometry(), 0)
            .map_err(media_editor_render::RenderStatus::Media)?;
        let mut samples = alloc::vec::Vec::new();
        samples
            .try_reserve(stride)
            .map_err(|_| media_editor_render::RenderStatus::OutOfMemory)?;
        samples.resize(stride, 0);
        spool
            .plane_row(&material, at, 0, row, &mut samples)
            .map_err(|_| media_editor_render::RenderStatus::FrameAbsent)?;
        let one = media_editor_media::FrameDescription::new(
            media_editor_media::Geometry::new(description.geometry().width(), 1)
                .map_err(media_editor_render::RenderStatus::Media)?,
            description.format(),
            description.colour(),
            description.siting(),
            description.alpha(),
            description.pixel_aspect(),
        )
        .map_err(media_editor_render::RenderStatus::Media)?;
        let plane = media_editor_media::Plane::new(samples, stride)
            .map_err(media_editor_render::RenderStatus::Media)?;
        let mut planes = alloc::vec::Vec::new();
        planes
            .try_reserve(1)
            .map_err(|_| media_editor_render::RenderStatus::OutOfMemory)?;
        planes.push(plane);
        media_editor_media::Frame::new(one, planes)
            .map_err(media_editor_render::RenderStatus::Media)
    }
}
