// SPDX-License-Identifier: GPL-3.0-only
//! Generated title media.
//!
//! Titles are media assets, so normal clip edits, transitions, masks, grades,
//! and transforms apply without a separate item type. Their digest covers the
//! complete description, which makes identical titles shareable and changed
//! text a new asset. [`Ink`] stores colour as fractions of full light; encoding
//! happens when the title is rendered into a frame.

use alloc::string::String;
use alloc::vec::Vec;

use media_editor_core::{Digest, Rational, Sha256};

use crate::status::{ModelStatus, Result};

/// How long one line of a title may be.
///
/// A title card, not a screenplay. The same bound the renderer sets on a run,
/// stated here too because the model refuses what it cannot describe rather
/// than passing it on to find out (R-11.2).
pub const MAX_TITLE_TEXT: usize = 128;

/// How many lines a title may have.
///
/// An end card is a handful of lines and a lower third is two. A title with
/// more than this is a document, and a document wants a different tool.
pub const MAX_TITLE_LINES: usize = 16;

/// How the lines of a title sit against one another.
///
/// Only how they sit *against each other* — where the block as a whole goes is
/// [`Title::across`] and [`Title::down`], and keeping those two questions
/// apart is what lets a left-aligned block be moved without re-aligning it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum Alignment {
    /// Every line starts at the block's left edge.
    Left,
    /// Every line is centred on the block's middle.
    Centre,
    /// Every line ends at the block's right edge.
    Right,
}

/// The colour of a title's letters, as fractions of full light.
///
/// Three rationals, not three bytes. A byte is a number in an encoding, and
/// the same byte is a different colour in sRGB than it is in a linear working
/// space — so a model that stored bytes would be storing an encoding it had
/// never named (R-8.3). A fraction of light is the same colour everywhere, and
/// the renderer turns it into whatever code value *that frame* spells it with.
///
/// The pleasant consequence is that the colours people actually pick are
/// exact: white, half light, a third.
///
/// ## Why nought to one, refused rather than clamped
///
/// A channel above one is brighter than white, and a premultiplied frame
/// holding one is a frame [`crate::media`]'s compositor refuses on the way
/// past. Clamping here would turn a number somebody typed into a different
/// number without telling them; refusing it says which of the three was wrong,
/// at the moment they set it, which is the only moment anybody can fix it.
///
/// Below nought is not darker than black. It is a colour that subtracts, and
/// there is no such ink.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ink {
    red: Rational,
    green: Rational,
    blue: Rational,
}

impl Ink {
    /// Full light in every channel, which is what a title was before it could
    /// be anything else.
    pub const WHITE: Self = Self {
        red: Rational::ONE,
        green: Rational::ONE,
        blue: Rational::ONE,
    };

    /// An ink from three fractions of full light.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::InkOutOfRange`] for any channel below nought or above
    /// one.
    pub fn new(red: Rational, green: Rational, blue: Rational) -> Result<Self> {
        for channel in [red, green, blue] {
            if channel < Rational::ZERO || channel > Rational::ONE {
                return Err(ModelStatus::InkOutOfRange);
            }
        }
        Ok(Self { red, green, blue })
    }

    /// The three channels, in order.
    #[must_use]
    pub const fn channels(&self) -> [Rational; 3] {
        [self.red, self.green, self.blue]
    }

    /// Whether every channel is at full intensity.
    #[must_use]
    pub fn is_white(&self) -> bool {
        *self == Self::WHITE
    }
}

/// A picture made of words.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Title {
    lines: Vec<String>,
    size: Rational,
    across: Rational,
    down: Rational,
    alignment: Alignment,
    ink: Ink,
}

impl Title {
    /// Create title text with frame-relative size and position.
    ///
    /// `size` is an em relative to frame height. `across` and `down` locate the
    /// line center relative to frame width and height.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyTitle`] if every line is empty. Blank lines are
    /// allowed between non-empty lines.
    /// [`ModelStatus::TitleTooLong`] past [`MAX_TITLE_TEXT`] on any line or
    /// [`MAX_TITLE_LINES`] lines.
    /// [`ModelStatus::TypeNotPositive`] for type at or below no size.
    pub fn new(
        lines: Vec<String>,
        size: Rational,
        across: Rational,
        down: Rational,
        alignment: Alignment,
    ) -> Result<Self> {
        if lines.iter().all(String::is_empty) {
            return Err(ModelStatus::EmptyTitle);
        }
        if lines.len() > MAX_TITLE_LINES {
            return Err(ModelStatus::TitleTooLong);
        }
        for line in &lines {
            if line.chars().count() > MAX_TITLE_TEXT {
                return Err(ModelStatus::TitleTooLong);
            }
        }
        if !size.is_positive() {
            return Err(ModelStatus::TypeNotPositive);
        }
        Ok(Self {
            lines,
            size,
            across,
            down,
            alignment,
            ink: Ink::WHITE,
        })
    }

    /// Return this title in another color. New titles default to white.
    #[must_use]
    pub fn with_ink(&self, ink: Ink) -> Self {
        Self {
            ink,
            ..self.clone()
        }
    }

    /// What colour its letters are.
    #[must_use]
    pub const fn ink(&self) -> Ink {
        self.ink
    }

    /// A card of one line, which is what most cards are.
    ///
    /// # Errors
    ///
    /// Whatever [`Title::new`] refuses.
    pub fn line(text: String, size: Rational, across: Rational, down: Rational) -> Result<Self> {
        Self::new(alloc::vec![text], size, across, down, Alignment::Centre)
    }

    /// What it says, a line at a time.
    #[must_use]
    pub fn lines(&self) -> &[String] {
        &self.lines
    }

    /// How the lines sit against one another.
    #[must_use]
    pub const fn alignment(&self) -> Alignment {
        self.alignment
    }

    /// The em, as a fraction of the frame's height.
    #[must_use]
    pub const fn size(&self) -> Rational {
        self.size
    }

    /// Where the middle of the line sits across the frame.
    #[must_use]
    pub const fn across(&self) -> Rational {
        self.across
    }

    /// And down it.
    #[must_use]
    pub const fn down(&self) -> Rational {
        self.down
    }

    /// What this title *is*: the digest of its own description.
    ///
    /// Every field, length-prefixed where it is not fixed. A title that hashed
    /// only its words would make two cards at two sizes one asset, and every
    /// clip of either would show whichever was drawn first. The ink is in
    /// there for exactly that reason: the same words in two colours are two
    /// pictures, so they are two assets and two cache entries.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow, which the lengths here
    /// cannot actually produce.
    pub fn digest(&self) -> Result<Digest> {
        let mut hasher = Sha256::new();
        hasher.update(b"media-editor-title-v3");
        hasher.update(
            &u64::try_from(self.lines.len())
                .map_err(|_| ModelStatus::TitleTooLong)?
                .to_le_bytes(),
        );
        for line in &self.lines {
            // Each line's length before its bytes, so that two cards whose
            // lines concatenate the same are two cards. "AB" then "C" and "A"
            // then "BC" are a different card in every way that matters.
            hasher.update(
                &u64::try_from(line.len())
                    .map_err(|_| ModelStatus::TitleTooLong)?
                    .to_le_bytes(),
            );
            hasher.update(line.as_bytes());
        }
        hasher.update(&[match self.alignment {
            Alignment::Left => 0,
            Alignment::Centre => 1,
            Alignment::Right => 2,
        }]);
        let [red, green, blue] = self.ink.channels();
        for held in [self.size, self.across, self.down, red, green, blue] {
            hasher.update(&held.numerator().to_le_bytes());
            hasher.update(&held.denominator().to_le_bytes());
        }
        Ok(hasher.finish())
    }
}
