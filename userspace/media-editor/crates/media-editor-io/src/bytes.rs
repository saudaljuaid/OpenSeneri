// SPDX-License-Identifier: GPL-3.0-only
//! Reading and writing fixed-width fields.
//!
//! Every read is checked against what is left (R-11.2), and every write grows
//! its buffer fallibly and refuses past a bound (R-5.1, R-5.2). These two
//! types are the only way this crate touches bytes, so "the parser forgot a
//! bounds check" is not a thing that can be true of it.

use alloc::vec::Vec;

use crate::status::{IoStatus, Result};

/// A cursor over bytes that refuses to run off the end.
pub struct Reader<'a> {
    bytes: &'a [u8],
    position: usize,
}

impl<'a> Reader<'a> {
    /// Read from the start of a slice.
    #[must_use]
    pub const fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, position: 0 }
    }

    /// How many bytes have not been read.
    #[must_use]
    pub const fn remaining(&self) -> usize {
        self.bytes.len() - self.position
    }

    /// Whether every byte has been read.
    #[must_use]
    pub const fn is_finished(&self) -> bool {
        self.position == self.bytes.len()
    }

    /// Take a fixed number of bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`] if that many are not left.
    pub fn take(&mut self, count: usize) -> Result<&'a [u8]> {
        let end = self
            .position
            .checked_add(count)
            .ok_or(IoStatus::TruncatedField)?;
        let slice = self
            .bytes
            .get(self.position..end)
            .ok_or(IoStatus::TruncatedField)?;
        self.position = end;
        Ok(slice)
    }

    /// Take one byte.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u8(&mut self) -> Result<u8> {
        Ok(self.take(1)?[0])
    }

    /// Take a little-endian `u16`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u16(&mut self) -> Result<u16> {
        let mut bytes = [0; 2];
        bytes.copy_from_slice(self.take(2)?);
        Ok(u16::from_le_bytes(bytes))
    }

    /// Take a little-endian `u32`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u32(&mut self) -> Result<u32> {
        let mut bytes = [0; 4];
        bytes.copy_from_slice(self.take(4)?);
        Ok(u32::from_le_bytes(bytes))
    }

    /// Take a little-endian `u64`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn u64(&mut self) -> Result<u64> {
        let mut bytes = [0; 8];
        bytes.copy_from_slice(self.take(8)?);
        Ok(u64::from_le_bytes(bytes))
    }

    /// Take a little-endian `i64`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn i64(&mut self) -> Result<i64> {
        let mut bytes = [0; 8];
        bytes.copy_from_slice(self.take(8)?);
        Ok(i64::from_le_bytes(bytes))
    }

    /// Take a little-endian `i32`.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn i32(&mut self) -> Result<i32> {
        let mut bytes = [0; 4];
        bytes.copy_from_slice(self.take(4)?);
        Ok(i32::from_le_bytes(bytes))
    }

    /// Take thirty-two bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::TruncatedField`].
    pub fn digest_bytes(&mut self) -> Result<[u8; 32]> {
        let mut bytes = [0; 32];
        bytes.copy_from_slice(self.take(32)?);
        Ok(bytes)
    }
}

/// A growing buffer that refuses past a bound.
pub struct Writer {
    bytes: Vec<u8>,
    limit: usize,
}

impl Writer {
    /// A buffer that will refuse to exceed `limit` bytes.
    #[must_use]
    pub const fn new(limit: usize) -> Self {
        Self {
            bytes: Vec::new(),
            limit,
        }
    }

    /// How many bytes have been written.
    #[must_use]
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// Whether nothing has been written.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// The bytes.
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        &self.bytes
    }

    /// Take the bytes.
    #[must_use]
    pub fn finish(self) -> Vec<u8> {
        self.bytes
    }

    /// Empty it, keeping the allocation.
    ///
    /// For a writer that assembles one bounded record at a time and hands each
    /// to a sink: the buffer is filled, written and emptied, and the
    /// allocation is made once rather than once a record.
    pub fn clear(&mut self) {
        self.bytes.clear();
    }

    /// Append a number in decimal, zero-padded to at least `width` digits.
    ///
    /// Written a digit at a time rather than through a formatter, because a
    /// formatter that grows a `String` is an allocation this type exists to
    /// bound. Nineteen digits is every `u64` there is, so the scratch array
    /// cannot overrun whatever it is handed.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn decimal(&mut self, value: u64, width: usize) -> Result<()> {
        let mut digits = [b'0'; 20];
        let mut written = 0_usize;
        let mut left = value;
        loop {
            let digit = u8::try_from(left % 10).unwrap_or(0);
            digits[digits.len() - 1 - written] = b'0' + digit;
            written += 1;
            left /= 10;
            if left == 0 {
                break;
            }
        }
        let from = digits.len() - written.max(width.min(digits.len()));
        self.bytes(&digits[from..])
    }

    /// Append bytes.
    ///
    /// # Errors
    ///
    /// [`IoStatus::PayloadTooLarge`] past the bound, or
    /// [`IoStatus::OutOfMemory`].
    pub fn bytes(&mut self, bytes: &[u8]) -> Result<()> {
        let end = self
            .bytes
            .len()
            .checked_add(bytes.len())
            .ok_or(IoStatus::PayloadTooLarge)?;
        if end > self.limit {
            return Err(IoStatus::PayloadTooLarge);
        }
        self.bytes
            .try_reserve(bytes.len())
            .map_err(|_| IoStatus::OutOfMemory)?;
        self.bytes.extend_from_slice(bytes);
        Ok(())
    }

    /// Append one byte.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u8(&mut self, value: u8) -> Result<()> {
        self.bytes(&[value])
    }

    /// Append a little-endian `u16`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u16(&mut self, value: u16) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `u32`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u32(&mut self, value: u32) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `u64`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn u64(&mut self, value: u64) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `i32`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn i32(&mut self, value: i32) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }

    /// Append a little-endian `i64`.
    ///
    /// # Errors
    ///
    /// As [`Writer::bytes`].
    pub fn i64(&mut self, value: i64) -> Result<()> {
        self.bytes(&value.to_le_bytes())
    }
}

/// A run of bytes somewhere, readable at an offset.
///
/// The smallest thing a streaming reader needs, and deliberately smaller than
/// [`media_editor_abi::seam::Storage`]: no slots, no capacity, no writing. A file
/// in memory is one of these, and so is one item inside a vault inside a slot,
/// which is what lets the same reel reader serve both without knowing which it
/// has.
///
/// It exists because of a number. One of Phipia's files holds sixteen
/// mebibytes and a Phipia program is mapped seventy-six kilobytes, so every
/// reader in this crate that takes a `&[u8]` is a reader that cannot run on
/// the target against a real file. A reader that takes one of these can.
pub trait Extent {
    /// How many bytes there are altogether.
    fn length(&self) -> u64;

    /// Copy a run beginning at `offset`, and say how many bytes that was.
    ///
    /// **Short at the end rather than refused**, matching
    /// [`media_editor_abi::seam::Storage::read_at`] and, underneath it, Phipia's
    /// `phipfs_read`: a run beginning at or past the end fills nothing and says
    /// nought, which is not an error.
    ///
    /// # Errors
    ///
    /// Whatever the underlying store refuses.
    fn read_at(&self, offset: u64, into: &mut [u8]) -> Result<usize>;
}

impl Extent for &[u8] {
    fn length(&self) -> u64 {
        u64::try_from(self.len()).unwrap_or(u64::MAX)
    }

    fn read_at(&self, offset: u64, into: &mut [u8]) -> Result<usize> {
        let at = usize::try_from(offset).unwrap_or(usize::MAX);
        let from = self.get(at.min(self.len())..).unwrap_or(&[]);
        let count = from.len().min(into.len());
        into[..count].copy_from_slice(&from[..count]);
        Ok(count)
    }
}

/// Somewhere bytes can be added to the end of, and nowhere else.
///
/// The write-side mirror of [`Extent`], and deliberately the *weaker* of the
/// two: an extent can be read anywhere, and a sink can only be extended. That
/// asymmetry is the whole design. A writer that cannot seek backwards cannot
/// damage what it has already written, so "the recorder overwrote frame two
/// hundred while writing frame four hundred" is not a failure this program can
/// have — not because it is careful, but because it cannot express the idea.
///
/// It exists because of the same number [`Extent`] exists for, in the other
/// direction. A reel this build writes is bounded at five hundred and twelve
/// mebibytes and a Phipia program is mapped seventy-six kilobytes, so every
/// writer in this crate that returns a `Vec<u8>` is a writer that cannot run
/// on the target against a real reel. A writer that takes one of these can.
pub trait Sink {
    /// How many bytes have been written so far.
    ///
    /// Not a cursor: there is nowhere else to be. It is the length of what
    /// exists, and a caller uses it to check that it is starting from nothing.
    fn written(&self) -> u64;

    /// Add these bytes to the end, all of them or none.
    ///
    /// # Errors
    ///
    /// Whatever the underlying store refuses.
    fn append(&mut self, bytes: &[u8]) -> Result<()>;
}

impl Sink for Vec<u8> {
    fn written(&self) -> u64 {
        u64::try_from(self.len()).unwrap_or(u64::MAX)
    }

    fn append(&mut self, bytes: &[u8]) -> Result<()> {
        // Fallibly, like every other allocation in this crate (R-5.2). A
        // recorder that aborted the process because a reel got long would be
        // a recorder nobody could use on the machine this is for.
        self.try_reserve(bytes.len())
            .map_err(|_| IoStatus::OutOfMemory)?;
        self.extend_from_slice(bytes);
        Ok(())
    }
}
