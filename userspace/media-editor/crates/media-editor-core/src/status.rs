// SPDX-License-Identifier: GPL-3.0-only
//! Every way this crate refuses.
//!
//! A refusal names its condition (R-7.3). There is no `Other`, no `Unknown`,
//! and no variant carrying a message: a caller that cannot distinguish two
//! failures cannot react to them differently, and a failure a caller cannot
//! react to should not have been representable.

/// A refusal from the core time and identity types.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum CoreStatus {
    /// A rational was built with a zero denominator.
    ZeroDenominator,
    /// A division by a zero-valued rational.
    DivideByZero,
    /// An exact result does not fit in the representation.
    Overflow,
    /// A rate that is zero or negative was offered where a rate is required.
    NonPositiveRate,
    /// A duration was asked to be negative.
    NegativeDuration,
    /// Two quantities in different timebases were combined without conversion.
    TimebaseMismatch,
    /// A conversion between timebases has no exact answer.
    InexactConversion,
    /// A timecode was requested for a rate this crate does not encode.
    UnsupportedTimecodeRate,
    /// Drop-frame counting was requested for a rate that does not define it.
    DropFrameUnavailable,
    /// A timecode's fields do not name a real instant at its rate.
    TimecodeMalformed,
    /// A timecode is outside the twenty-four hour counting range.
    TimecodeOutOfRange,
    /// A range's parts do not describe a half-open interval.
    RangeMalformed,
    /// A fixed-capacity structure is full.
    CapacityExhausted,
    /// An identifier names a slot whose generation has moved on.
    StaleIdentifier,
    /// An identifier names a slot that was never occupied.
    UnknownIdentifier,
    /// An operation with no real answer for this value: the logarithm of
    /// something that is not positive, the square root of something negative,
    /// or zero raised to the zero.
    NoRealAnswer,
}

impl CoreStatus {
    /// One line naming the condition, for transcripts and diagnostics.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::ZeroDenominator => "a rational cannot have a zero denominator",
            Self::DivideByZero => "division by a zero-valued rational",
            Self::Overflow => "the exact result does not fit",
            Self::NonPositiveRate => "a rate must be greater than zero",
            Self::NegativeDuration => "a duration cannot be negative",
            Self::TimebaseMismatch => "the two quantities are in different timebases",
            Self::InexactConversion => "the conversion has no exact answer",
            Self::UnsupportedTimecodeRate => "timecode is not defined for this rate",
            Self::DropFrameUnavailable => "drop-frame counting is not defined for this rate",
            Self::TimecodeMalformed => "the timecode fields do not name a real instant",
            Self::TimecodeOutOfRange => "the timecode is outside twenty-four hours",
            Self::RangeMalformed => "the range does not describe a half-open interval",
            Self::CapacityExhausted => "the structure is at its capacity",
            Self::StaleIdentifier => "the identifier's generation has moved on",
            Self::UnknownIdentifier => "the identifier names an empty slot",
            Self::NoRealAnswer => "that operation has no real answer for this value",
        }
    }
}

impl core::fmt::Display for CoreStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, CoreStatus>;
