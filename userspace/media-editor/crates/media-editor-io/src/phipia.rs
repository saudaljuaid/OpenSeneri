// SPDX-License-Identifier: GPL-3.0-only
//! Phipia's filesystem contract, in types Media Editor can hold.
//!
//! Every constant and every rule in this module is read out of Phipia rather
//! than guessed at, and the source is named beside each one. It is written
//! against **Phipia 2.1.0** at commit `8fe1817`; when Phipia moves, this is
//! the file that has to be re-read, and the version above is how the next
//! person knows whether it has been.
//!
//! ## Why this exists
//!
//! Nothing above this module should know what FAT32 is. What it *does* need
//! to know is that a name somebody types may be impossible, and this is where
//! that is decided — once, against the real rule, with a refusal for each way
//! it can fail rather than one for all of them.
//!
//! ## The rule, in full
//!
//! Phipia deliberately accepts a case-insensitive ASCII 8.3 subset:
//! `A`–`Z`, `0`–`9`, and sixteen punctuation marks. Lowercase is accepted on
//! input and uppercased on disk, so `clip.bmp` and `CLIP.BMP` are one file.
//! Long-filename entries are validated enough to reject malformed ordinals and
//! non-ASCII UTF-16, and are then refused as unsupported — which is a better
//! answer than presenting partial VFAT semantics as complete support.
//!
//! Read from `src/rust/fat32.rs`, `canonical_character` and
//! `parse_component`, and from `docs/FAT32.md`.

use alloc::vec::Vec;

use crate::status::{IoStatus, Result};

/// How many bytes a whole path may be. `FAT32_PATH_BYTES`.
pub const MAX_PATH_BYTES: usize = 127;

/// How many components a path may have. `PHIPFS_MAX_DEPTH`.
pub const MAX_PATH_COMPONENTS: usize = 16;

/// How many bytes one component may be: eight, a dot, and three.
pub const MAX_COMPONENT_BYTES: usize = 12;

/// How many bytes a name's base may be.
pub const MAX_BASE_BYTES: usize = 8;

/// How many bytes a name's extension may be.
pub const MAX_EXTENSION_BYTES: usize = 3;

/// How large one file may be. `PHIPFS_MAX_FILE_BYTES`, sixteen mebibytes.
///
/// This is the number that shapes everything downstream. One 1920×1080 frame
/// in eight-bit RGBA is 8,294,400 bytes, so a file holds **two** of them with
/// 188,416 bytes to spare and cannot hold three — and a directory holds
/// sixty-four entries, so a hundred photographs cannot be a hundred files.
/// That pair of facts is the whole argument for [`crate::vault`].
pub const MAX_FILE_BYTES: usize = 16 * 1024 * 1024;

/// How many live entries one directory may hold. `PHIPFS_MAX_LIST_ENTRIES`.
pub const MAX_DIRECTORY_ENTRIES: usize = 64;

/// How many directories the mount-time validator accepts, from `FAT32.md`.
pub const MAX_DIRECTORIES: usize = 256;

/// How many open handles the kernel owns at once. `PHIPFS_MAX_HANDLES`.
///
/// Worth knowing above the seam, because an editor that wanted one handle per
/// piece of media would run out at sixteen — which is another way of arriving
/// at one file holding many.
pub const MAX_HANDLES: usize = 16;

/// A cluster, which is also a sector. `FAT32_CLUSTER_BYTES`.
pub const CLUSTER_BYTES: usize = 512;

/// How many data clusters a release volume has, from `FAT32.md`'s geometry.
pub const DATA_CLUSTERS: usize = 129_022;

/// The punctuation Phipia's 8.3 subset accepts, beside letters and digits.
///
/// Sixteen marks, in the order `src/rust/fat32.rs` lists them. Notably absent:
/// space, dot beyond the one separator, comma, semicolon, equals, plus,
/// bracket, and every non-ASCII byte.
pub const PUNCTUATION: [u8; 16] = *b"$%'-_@~`!(){}^#&";

/// Whether a byte may appear in a name once it has been uppercased.
#[must_use]
pub fn is_canonical(byte: u8) -> bool {
    byte.is_ascii_uppercase() || byte.is_ascii_digit() || PUNCTUATION.contains(&byte)
}

/// One 8.3 name, uppercased, as Phipia stores it.
///
/// Held as the bytes somebody may write into a directory entry rather than as
/// two fields, because that is the form every check below is about.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Name {
    bytes: Vec<u8>,
}

impl Name {
    /// The canonical form of a name somebody typed.
    ///
    /// Lowercase is accepted and uppercased, exactly as Phipia does, so the
    /// two spellings of one file do not become two files.
    ///
    /// # Errors
    ///
    /// [`IoStatus::NameEmpty`] for nothing at all,
    /// [`IoStatus::NameTooLong`] past twelve bytes or eight-and-three,
    /// [`IoStatus::NameNotCanonical`] for a byte outside the subset, and
    /// [`IoStatus::NameDotMisplaced`] for a second dot, a leading one, or a
    /// trailing one.
    pub fn new(text: &str) -> Result<Self> {
        let raw = text.as_bytes();
        if raw.is_empty() {
            return Err(IoStatus::NameEmpty);
        }
        if raw.len() > MAX_COMPONENT_BYTES {
            return Err(IoStatus::NameTooLong);
        }
        let mut dot = None;
        for (index, byte) in raw.iter().copied().enumerate() {
            if byte == b'.' {
                // One dot, and neither at the front nor at the back. Phipia
                // refuses all three, and each is a different mistake: `.hidden`
                // is a Unix habit, `name.` is a slip, and `a.b.c` is a name
                // from a filesystem that has more room than this one.
                if dot.is_some() || index == 0 || index + 1 == raw.len() {
                    return Err(IoStatus::NameDotMisplaced);
                }
                dot = Some(index);
            } else if !is_canonical(uppercase(byte)) {
                return Err(IoStatus::NameNotCanonical);
            }
        }
        let base = dot.unwrap_or(raw.len());
        let extension = raw.len() - dot.map_or(raw.len(), |index| index + 1);
        if base > MAX_BASE_BYTES || extension > MAX_EXTENSION_BYTES {
            return Err(IoStatus::NameTooLong);
        }
        let mut bytes = Vec::new();
        bytes
            .try_reserve(raw.len())
            .map_err(|_| IoStatus::OutOfMemory)?;
        bytes.extend(raw.iter().copied().map(uppercase));
        Ok(Self { bytes })
    }

    /// The name as Phipia would store it.
    #[must_use]
    pub fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }

    /// The name as text. Always ASCII, so always valid.
    #[must_use]
    pub fn as_str(&self) -> &str {
        core::str::from_utf8(&self.bytes).unwrap_or("")
    }

    /// Whether this name ends in the given extension, which is already
    /// uppercase because a name is.
    #[must_use]
    pub fn has_extension(&self, extension: &str) -> bool {
        let wanted = extension.as_bytes();
        match self.bytes.len().checked_sub(wanted.len() + 1) {
            None => false,
            Some(at) => self.bytes[at] == b'.' && &self.bytes[at + 1..] == wanted,
        }
    }
}

/// A path relative to one mount.
///
/// Never absolute, never containing a backslash, never climbing above the
/// root, and never more than sixteen components or a hundred and twenty-seven
/// bytes — each of which Phipia refuses separately, so each is refused
/// separately here.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Path {
    components: Vec<Name>,
}

impl Path {
    /// Parse a path.
    ///
    /// # Errors
    ///
    /// [`IoStatus::PathEmpty`], [`IoStatus::PathAbsolute`] for a leading
    /// separator, [`IoStatus::PathTooLong`] past the byte or component bound,
    /// [`IoStatus::PathMalformed`] for a backslash or a repeated separator,
    /// [`IoStatus::PathAboveRoot`] for a `..` that climbs out, and whatever
    /// [`Name::new`] refuses for any component.
    pub fn new(text: &str) -> Result<Self> {
        if text.is_empty() {
            return Err(IoStatus::PathEmpty);
        }
        if text.len() > MAX_PATH_BYTES {
            return Err(IoStatus::PathTooLong);
        }
        if text.starts_with('/') {
            return Err(IoStatus::PathAbsolute);
        }
        if text.contains('\\') {
            // A backslash is not a separator here and is not a legal name
            // byte either, so accepting it would mean guessing which of the
            // two somebody meant.
            return Err(IoStatus::PathMalformed);
        }
        let mut components: Vec<Name> = Vec::new();
        for piece in text.split('/') {
            if piece.is_empty() {
                // A repeated separator, or a trailing one. Both are a path
                // that names the same place twice over.
                return Err(IoStatus::PathMalformed);
            }
            if piece == "." {
                continue;
            }
            if piece == ".." {
                if components.pop().is_none() {
                    return Err(IoStatus::PathAboveRoot);
                }
                continue;
            }
            if components.len() == MAX_PATH_COMPONENTS {
                return Err(IoStatus::PathTooLong);
            }
            components
                .try_reserve(1)
                .map_err(|_| IoStatus::OutOfMemory)?;
            components.push(Name::new(piece)?);
        }
        if components.is_empty() {
            // `.` alone, or `a/..`. It names the root, which is a directory
            // rather than a file, and every caller here wants a file.
            return Err(IoStatus::PathEmpty);
        }
        Ok(Self { components })
    }

    /// The components, in order.
    #[must_use]
    pub fn components(&self) -> &[Name] {
        &self.components
    }

    /// The last component, which is the file this path names.
    #[must_use]
    pub fn name(&self) -> &Name {
        // A path with no components cannot be built.
        &self.components[self.components.len() - 1]
    }
}

/// The uppercase of an ASCII byte, and every other byte unchanged.
const fn uppercase(byte: u8) -> u8 {
    if byte.is_ascii_lowercase() {
        byte - b'a' + b'A'
    } else {
        byte
    }
}
