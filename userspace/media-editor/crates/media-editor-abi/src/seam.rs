// SPDX-License-Identifier: GPL-3.0-only
//! The five seams.
//!
//! Everything the platform provides reaches Media Editor through exactly five
//! interfaces, and no more: presentation, input, storage, time, and audio.
//! Five is a rule, not a count — a sixth would be the beginning of a general
//! "system" interface, and a general system interface is how an application
//! stops being native to anything.
//!
//! Two are defined here because two are all that `PHIP-01` supports. The rest
//! arrive with the capabilities they need, each one in this module, each one
//! with a Phipia implementation and a deterministic test implementation.

use core::fmt::Write;

/// A refusal from a seam.
///
/// Deliberately coarse: a seam's job is to be a boundary, and an application
/// that wants to distinguish twelve kinds of console failure has put logic in
/// the wrong place.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum SeamStatus {
    /// The kernel refused the request.
    Refused,
    /// The request was larger than the seam accepts.
    TooLarge,
    /// The capability this seam needs does not exist on this kernel.
    Unavailable,
    /// The slot holds nothing.
    Empty,
}

/// The result of a seam operation.
pub type Result<T> = core::result::Result<T, SeamStatus>;

/// Somewhere to write diagnostics.
///
/// On Phipia this is the kernel's console and serial transcript. In the host
/// suite it is a buffer a test compares against, which is what makes an
/// application's whole output checkable without an emulator.
pub trait Console {
    /// Write bytes, all of them or none.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::TooLarge`] past whatever the implementation accepts in
    /// one call, or [`SeamStatus::Refused`].
    fn write(&mut self, bytes: &[u8]) -> Result<()>;

    /// Write a line, followed by a newline.
    ///
    /// # Errors
    ///
    /// As [`Console::write`].
    fn write_line(&mut self, text: &str) -> Result<()> {
        self.write(text.as_bytes())?;
        self.write(b"\n")
    }
}

/// An adapter that lets `write!` target a [`Console`].
///
/// Formatting cannot report a seam refusal through [`core::fmt`], so this
/// records the first one and hands it back at the end rather than losing it
/// (R-7.4).
pub struct ConsoleWriter<'a, C: Console + ?Sized> {
    console: &'a mut C,
    failure: Option<SeamStatus>,
}

impl<'a, C: Console + ?Sized> ConsoleWriter<'a, C> {
    /// Wrap a console for formatted output.
    pub fn new(console: &'a mut C) -> Self {
        Self {
            console,
            failure: None,
        }
    }

    /// The first refusal that occurred, if any.
    ///
    /// # Errors
    ///
    /// Whatever the console refused.
    pub fn finish(self) -> Result<()> {
        self.failure.map_or(Ok(()), Err)
    }
}

impl<C: Console + ?Sized> Write for ConsoleWriter<'_, C> {
    fn write_str(&mut self, text: &str) -> core::fmt::Result {
        match self.console.write(text.as_bytes()) {
            Ok(()) => Ok(()),
            Err(status) => {
                self.failure.get_or_insert(status);
                Err(core::fmt::Error)
            }
        }
    }
}

/// Which fixed storage slot an operation names.
///
/// This enum's first version said "Phipia has no paths, no directories, and no
/// rename (`PHIP-08`)", which was true at v1.1.0 and is not true now: Phipia
/// 2.1.0 has a read-write FAT32 volume with nested directories, rename and
/// sync. The shape below survived the correction anyway, and it is worth
/// saying why rather than leaving it looking like an accident.
///
/// A named slot is **less** than a path on purpose. An application that could
/// name any file could lose any file, and the property R-9.4 asks for — that
/// an interrupted save leaves the previous file exactly where it was — is a
/// property of a *protocol*, not of a filesystem. Phipia's own Media Editor
/// workspace performs that protocol by hand in five steps with three names and
/// four syncs; [`Storage::commit`] is the name for the one step in it that
/// must be indivisible.
///
/// Three slots rather than two, because a project and the material it refers
/// to are two files. They are committed separately and neither can damage the
/// other, which is what makes "the vault is being written" survivable.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Slot {
    /// The project as it was last committed. Never written directly.
    Project,
    /// The media vault as it was last committed. Never written directly.
    ///
    /// Separate from the project deliberately. Material is large and changes
    /// rarely; a project is small and changes constantly, and writing sixteen
    /// mebibytes of photographs every time somebody trims a clip would be a
    /// save protocol nobody could afford to run.
    Vault,
    /// Where a save is assembled and verified before it is committed.
    ///
    /// One scratch for both, because a save is a sequence and there is never
    /// more than one in flight — which is also why Phipia's workspace uses one
    /// `STUTEMP.PHI` for its own.
    Scratch,
}

/// Fixed extents, ranged reads, and an atomic swap between them.
///
/// The contract that matters is [`Storage::commit`]: until it returns, the
/// committed slot holds exactly what it held before, whatever happened to the
/// scratch slot. That is what makes R-9.4 provable rather than hoped for.
pub trait Storage {
    /// The most bytes a slot can hold.
    fn capacity(&self, slot: Slot) -> usize;

    /// How many bytes the slot currently holds.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`] if nothing has been written to it.
    fn len(&self, slot: Slot) -> Result<usize>;

    /// Copy the whole slot into `into`, and say how many bytes that was.
    ///
    /// All or nothing: a destination too small for the stored bytes is
    /// refused rather than partly filled (R-1.4).
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`], or [`SeamStatus::TooLarge`] if `into` is
    /// smaller than the stored length.
    fn read(&self, slot: Slot, into: &mut [u8]) -> Result<usize>;

    /// Copy a run of bytes beginning at `offset`, and say how many that was.
    ///
    /// **This is the method a vault needs and a project does not.** One of
    /// Phipia's files holds sixteen mebibytes and a Phipia program is mapped
    /// seventy-six kilobytes, so [`Storage::read`] — which fills a buffer the
    /// size of the whole slot — cannot read a full vault on the target by
    /// three orders of magnitude. A store that is read an entry at a time can
    /// be, and this is what makes that possible.
    ///
    /// It is also what Phipia itself does. Its bitmap importer "issues random
    /// row reads through the normal filesystem and NVMe paths" rather than
    /// holding a picture, which is `phipfs_seek` followed by `phipfs_read` —
    /// exactly the shape below.
    ///
    /// Short at the end, like every read of a file: a run that begins inside
    /// the slot and reaches past it fills what there is and says how much.
    /// A run beginning **at or past** the end fills nothing and says nought,
    /// which is what Phipia's `phipfs_read` does at end of file and is a
    /// different thing from a refusal.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`] if the slot holds nothing, or
    /// [`SeamStatus::Refused`].
    fn read_at(&self, slot: Slot, offset: usize, into: &mut [u8]) -> Result<usize>;

    /// Replace a slot's contents.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::TooLarge`] past the slot's capacity, or
    /// [`SeamStatus::Refused`].
    fn write(&mut self, slot: Slot, bytes: &[u8]) -> Result<()>;

    /// Extend the scratch slot by these bytes, starting it if it is empty.
    ///
    /// **This is the method a recorder needs and a project does not**, and it
    /// is the mirror of [`Storage::read_at`] one milestone later. A reel this
    /// build writes is bounded at five hundred and twelve mebibytes against
    /// the seventy-six kilobytes a Phipia program is mapped, so
    /// [`Storage::write`] — which takes the whole file in one slice — cannot
    /// write a full reel on the target by four orders of magnitude. A file
    /// that is extended a row at a time can be.
    ///
    /// ## Why it takes no slot
    ///
    /// Because there is exactly one place a save is assembled, and an
    /// operation that could name the committed project or the committed vault
    /// would be an operation that could write a live file in place. That is
    /// the one thing this whole protocol exists to prevent, and refusing it at
    /// run time would be strictly worse than not being able to ask: the rest
    /// of this program makes overlapping items unrepresentable rather than
    /// merely rejected, and this is the same choice.
    ///
    /// ## Why appending rather than writing at an offset
    ///
    /// Phipia's FAT32 offers both — it has random access as well as file
    /// growth — so this is a choice rather than a limitation. Appending is
    /// **strictly weaker**, and a writer that cannot seek backwards is a
    /// writer that cannot damage what it has already written. It is why
    /// `SPRW` version three moved its digest to a trailer: a format that has
    /// to patch its own header is a format that needs the stronger
    /// capability, and it needed it for no reason.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::TooLarge`] if the slot would grow past its capacity, or
    /// [`SeamStatus::Refused`].
    fn append(&mut self, bytes: &[u8]) -> Result<()>;

    /// Make the scratch slot the named one, in one step that either happens
    /// or does not.
    ///
    /// The destination is named because there are two committed slots now.
    /// Committing *into* the scratch slot is refused: it is where a save is
    /// assembled, and an operation that made it its own destination would be
    /// an operation with nothing to say about what happened.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Empty`] if nothing has been written to the scratch slot,
    /// or [`SeamStatus::Refused`] — including for a commit into
    /// [`Slot::Scratch`].
    fn commit(&mut self, into: Slot) -> Result<()>;
}

/// The kernel's one monotonic clock.
pub trait Time {
    /// Nanoseconds since an unspecified origin, never going backwards.
    ///
    /// # Errors
    ///
    /// [`SeamStatus::Unavailable`] where `PHIP-05` does not exist.
    fn monotonic_nanoseconds(&self) -> Result<u64>;
}
