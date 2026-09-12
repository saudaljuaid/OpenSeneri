// SPDX-License-Identifier: GPL-3.0-only
//! Every way a project file is refused.
//!
//! A saved project is the user's work, so every refusal here names exactly
//! what was wrong with the bytes (R-7.3). There is no variant meaning "the
//! file is broken somehow", because a reader that cannot say which byte
//! disagreed with the format cannot be trusted to have checked the others.

use media_editor_abi::seam::SeamStatus;
use media_editor_model::ModelStatus;

/// A refusal from reading, writing, or saving a project.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum IoStatus {
    /// The model refused to hold what the file described.
    Model(ModelStatus),
    /// The storage seam refused.
    Seam(SeamStatus),
    /// An allocation the caller must handle.
    OutOfMemory,
    /// The file does not begin with the Media Editor project magic.
    NotAProjectFile,
    /// The file's format version is not one this build reads.
    UnsupportedVersion(u16),
    /// A field reserved for a later format is not zero.
    ReservedFieldSet,
    /// The file ends inside its own header.
    TruncatedHeader,
    /// The file ends inside its payload.
    TruncatedPayload,
    /// The header's payload length disagrees with the file's size.
    LengthMismatch,
    /// The payload's digest disagrees with the header's.
    DigestMismatch,
    /// The payload ends before the structure it declared does.
    TruncatedField,
    /// The payload declares more than the format allows.
    TooMany,
    /// The payload is longer than the format allows.
    PayloadTooLarge,
    /// Bytes follow the payload that the structure did not account for.
    TrailingBytes,
    /// An item is tagged as something this format does not define.
    UnknownItemTag(u8),
    /// A track is tagged as carrying something this format does not define.
    UnknownTrackKind(u8),
    /// A clip names a media entry the file does not contain.
    MediaIndexOutOfRange,
    /// A nested asset naming a sequence the file does not hold.
    SequenceIndexOutOfRange,
    /// A file giving one sequence two bodies.
    SequenceBodyTwice,
    /// A nest whose digest is not the digest of the sequence it names.
    NestDigestMismatch,
    /// A nest carrying a location, which it has no use for.
    NestHasLocation,
    /// A written file did not read back as what was written.
    WriteNotVerified,
    /// The media types refused what the file described.
    Media(media_editor_media::MediaStatus),
    /// A summary the audio crate refused to hold.
    Sound(media_editor_audio::AudioStatus),
    /// The file does not begin with a summary's magic.
    NotASummary,
    /// A byte naming no interpolation this build knows.
    UnknownInterpolationTag(u8),
    /// A byte naming neither the presence nor the absence of a grade.
    UnknownGradeTag(u8),
    /// A capture asked of a pixel format a PNG has no way to hold.
    PngFormatUnsupported,
    /// A capture asked of premultiplied coverage, which a PNG does not store.
    PngPremultiplied,
    /// The colour pipeline refused what a file described.
    Render(media_editor_render::RenderStatus),
    /// A line past the bound a lookup table's lines are read within.
    CubeLineTooLong,
    /// A lookup table file this reader cannot make sense of.
    CubeMalformed,
    /// A one-dimensional table, which is a per-channel curve rather than a
    /// cube and is a different thing in the same file extension.
    CubeNotThreeDimensional,
    /// A file that states its size more than once.
    CubeSizeRepeated,
    /// A file with no size at all.
    CubeNoSize,
    /// A sample before the size that says how many there should be.
    CubeSampleBeforeSize,
    /// A side outside the range this build carries.
    CubeSizeUnsupported,
    /// An input domain other than nought to one, which this build refuses
    /// rather than applying the wrong look to every pixel.
    CubeDomainUnsupported,
    /// A number in scientific notation, which this build does not read.
    CubeExponentUnsupported,
    /// A number with more decimal places than this build carries.
    CubeTooPrecise,
    /// A number too large to hold.
    CubeOutOfRange,
    /// The file does not begin with the Media Editor reel magic.
    NotAReel,
    /// A reel holds no frames.
    EmptyReel,
    /// A reel's frames are not all described the same way.
    ReelDescriptionMismatch,
    /// The file ends before the digest that closes it.
    ///
    /// What a reel cut short by an interrupted write looks like, and it is
    /// answerable without hashing anything — which is the whole reason the
    /// digest is at the end.
    TruncatedTrailer,
    /// A reel cannot be wound a row at a time in a format with several planes.
    ///
    /// A packed frame is plane nought entire, then plane one entire. A scan
    /// arrives row by row, so writing it forwards would mean holding two
    /// thirds of the picture until the last row — which is the allocation the
    /// row form exists to avoid, spelled differently.
    NotOnePlane,
    /// A row arrived that is not the row the writer is waiting for.
    ///
    /// Refused rather than repaired (R-1.3): a writer that took whatever came
    /// next would turn a caller's off-by-one into a file whose pictures are
    /// wrong and whose digest is right.
    RowOutOfOrder,
    /// A reel was finished before every row of every frame had arrived.
    IncompleteReel,
    /// A reel was wound into somewhere that already held bytes.
    SinkNotEmpty,
    /// A reel's sound is not a length its pictures could cover.
    ///
    /// A frame at 30000/1001 into 48 kHz covers 1601.6 samples, so a run of
    /// frames holds one of *two* counts and which one depends on where the
    /// take was cut from. The bound is exact all the same: three such frames
    /// hold 4804 samples or 4805, and never 4803.
    SoundRunsPastPicture,
    /// A reel with no sound was handed some, or one with sound was not.
    SoundNotDeclared,
    /// A reel's header disagrees with itself about its transcript.
    TranscriptNotDeclared,
    /// A caption arrived that is not the one the writer is waiting for.
    CaptionOutOfOrder,
    /// A block of samples arrived that is not the block the writer wants.
    SoundOutOfOrder,
    /// A cue that begins before the one before it, or before the file does.
    CueOutOfOrder,
    /// A cue whose in and out points round to the same millisecond.
    CueVanishes,
    /// A cue with no words in it.
    EmptyCue,
    /// A cue whose text holds a blank line, which would end the cue.
    CueTextNotOneBlock,
    /// A cue whose text holds `-->`, which would read as a timing line.
    CueTextLooksLikeATiming,
    /// A block of samples is not a length one frame could cover.
    SoundBlockWrongLength,
    /// A pixel format tag this format does not define.
    UnknownPixelFormat(u8),
    /// A colour tag this format does not define.
    UnknownColourTag(u8),
    /// A fader tag this format does not define.
    UnknownFaderTag(u8),
    /// A transition tag this format does not define.
    UnknownTransitionTag(u8),
    /// A mask tag this format does not define.
    UnknownMaskTag(u8),
    /// A file listing one piece of content as two assets.
    DuplicateMedia,
    /// A transform tag this format does not define.
    UnknownTransformTag(u8),
    /// A motion tag this format does not define.
    UnknownMotionTag(u8),
    /// A media source tag this format does not define.
    UnknownMediaSourceTag(u8),
    /// A title whose words are not text.
    TitleNotText,
    /// A marker whose text is not text.
    MarkerNotText,
    /// A caption whose words are not text.
    CaptionNotText,
    /// A title whose digest is not the digest of its own description.
    TitleDigestMismatch,
    /// An alignment tag this format does not define.
    UnknownAlignmentTag(u8),
    /// An ink tag this build does not read.
    UnknownInkTag(u8),
    /// A fade tag this format does not define.
    UnknownFadeTag(u8),
    /// A speed tag this format does not define.
    UnknownSpeedTag(u8),
    /// A ramp whose curve has no keyframes, which is not a curve.
    RampWithoutKeyframes,
    /// A name with nothing in it.
    NameEmpty,
    /// Bytes offered as a bitmap that are not one.
    NotABitmap,
    /// Bytes offered as a vault that are not one.
    NotAVault,
    /// A frame position past the end of the reel that holds it.
    FrameOutOfReel,
    /// A plane a format does not have, or a row past a plane's height.
    PlaneOutOfFrame,
    /// A destination shorter than the run of bytes it was to receive.
    TooSmall,
    /// A vault already holding as much material as it may.
    VaultFull,
    /// Material that would take a vault past what one file holds.
    VaultTooLarge,
    /// A name longer than a vault entry carries.
    VaultNameTooLong,
    /// A name that is not printable text.
    VaultNameNotText,
    /// Material a vault already holds.
    VaultItemTwice,
    /// Material a vault does not hold.
    VaultItemAbsent,
    /// A vault whose spans leave a gap, overlap, or run backwards.
    VaultSpanNotContiguous,
    /// Material that is not what its entry says it is.
    VaultItemDigestMismatch,
    /// A bitmap of a shape this build does not read.
    BitmapUnsupported,
    /// A bitmap past the bounds Phipia's importer accepts.
    BitmapTooLarge,
    /// A name past eight-and-three, or past twelve bytes.
    NameTooLong,
    /// A name holding a byte Phipia's 8.3 subset does not accept.
    NameNotCanonical,
    /// A name with a second dot, a leading one, or a trailing one.
    NameDotMisplaced,
    /// A path with nothing in it, or one naming the root.
    PathEmpty,
    /// A path beginning at the root, which no mount accepts.
    PathAbsolute,
    /// A path past its byte or component bound.
    PathTooLong,
    /// A path with a backslash or a repeated separator.
    PathMalformed,
    /// A path climbing above the mount it is relative to.
    PathAboveRoot,
    /// An exact arithmetic or timecode refusal from the core types.
    Time(media_editor_core::CoreStatus),
    /// An edit decision list line is longer than any real one.
    EdlLineTooLong,
    /// An event line does not have the fields an event has.
    EdlMalformedEvent,
    /// A timecode field is not eight digits and three separators.
    EdlMalformedTimecode,
    /// A channel this build has no meaning for.
    EdlUnknownChannel,
    /// A transition this build has no meaning for.
    EdlUnknownTransition,
    /// An `FCM` line that says neither drop frame nor non-drop.
    EdlUnknownFrameCodeMode,
    /// The `FCM` line and a timecode's own punctuation disagree about
    /// drop-frame, and they are two statements about the same fact.
    EdlFrameCodeModeConflict,
    /// An event whose out point is not after its in point names no frames.
    EdlNegativeDuration,
    /// A reel name longer than the eight characters the format carries.
    EdlReelTooLong,
    /// A comment that names a clip before any event exists to name.
    EdlCommentBeforeEvent,
    /// A file with no events in it is not an edit decision list.
    EdlNoEvents,
    /// A sequence with more than one picture track cannot be one list: the
    /// format has one video channel, so the second track's pictures would be
    /// written as though they replaced the first's.
    ConformManyVideoTracks,
    /// A sequence whose picture track comes after a sound track. A list names
    /// channels and not an order, so importing one always builds picture
    /// first, and a sequence that was not in that order does not come back.
    ConformTracksNotOrdered,
    /// Two sources whose digests agree in the first eight characters, which is
    /// all a reel name holds.
    ConformReelCollision,
    /// An event with no comment naming its source by content digest, so there
    /// is no way to know which media it means.
    ConformNoSourceDigest,
    /// An event naming a source digest the media library does not hold.
    ConformUnknownSource,
    /// An event whose reel name is not the first eight characters of the
    /// source digest its comment names, which are two statements about the
    /// same fact.
    ConformReelDisagreesWithSource,
    /// An event that begins before the one before it ended.
    ConformEventsOverlap,
    /// A dissolve with no event immediately before it to dissolve from.
    ConformDissolveFromNothing,
    /// A dissolve that takes more frames than the event carrying it holds.
    ConformDissolveTooLong,
    /// A wipe, which this build's model has no shape for.
    ConformWipeUnsupported,
    /// A `B` event, which describes picture and sound at once. Which sound
    /// tracks it means is a convention rather than a statement, so it is
    /// refused rather than guessed.
    ConformChannelNotSeparate,
}

impl IoStatus {
    /// One line naming the condition.
    #[must_use]
    #[expect(
        clippy::too_many_lines,
        reason = "a table is as long as the number of things it names"
    )]
    pub const fn describe(self) -> &'static str {
        // One arm per status, one line each, and the length of it is the
        // number of ways this crate can refuse -- which is the property worth
        // having rather than a thing to be shortened. Splitting the match
        // would need a subset arm for every status the subset does not name,
        // which is a branch nothing reaches and no test can cover;
        // `Edit::apply` in the model says the same thing at more length and
        // for the same reason.
        //
        // `expect` rather than `allow`, so the day this drops back under a
        // hundred lines the compiler says so instead of leaving a stale
        // waiver behind.
        match self {
            Self::Model(status) => status.describe(),
            Self::Seam(_) => "the storage seam refused",
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::NotAProjectFile => "the file is not a Media Editor project",
            Self::UnsupportedVersion(_) => "the file's format version is not one this build reads",
            Self::ReservedFieldSet => "a reserved field is not zero",
            Self::TruncatedHeader => "the file ends inside its header",
            Self::TruncatedPayload => "the file ends inside its payload",
            Self::LengthMismatch => "the declared length disagrees with the file",
            Self::DigestMismatch => "the payload's digest disagrees with the header",
            Self::TruncatedField => "the payload ends inside a structure it declared",
            Self::TooMany => "the payload declares more than the format allows",
            Self::PayloadTooLarge => "the payload is longer than the format allows",
            Self::TrailingBytes => "bytes follow the payload that nothing accounts for",
            Self::UnknownItemTag(_) => "an item is tagged as something this format does not define",
            Self::UnknownTrackKind(_) => "a track carries something this format does not define",
            Self::MediaIndexOutOfRange => "a clip names media the file does not contain",
            Self::SequenceIndexOutOfRange => "a nest names a sequence this file does not hold",
            Self::SequenceBodyTwice => "a file gives one sequence two bodies",
            Self::NestDigestMismatch => {
                "a nest is named by which sequence it is, and that is not it"
            }
            Self::NestHasLocation => "a nest has nowhere to be, so it cannot have been found there",
            Self::WriteNotVerified => "the written file did not read back as what was written",
            Self::Media(status) => status.describe(),
            Self::Sound(status) => status.describe(),
            Self::NotASummary => "these bytes do not begin with a summary",
            Self::UnknownInterpolationTag(_) => "that byte names no interpolation",
            Self::UnknownGradeTag(_) => {
                "that byte says neither that there is a grade nor that there is not"
            }
            Self::PngFormatUnsupported => "a capture holds grey, colour, or colour and coverage",
            Self::PngPremultiplied => "a capture stores straight coverage, not premultiplied",
            Self::Render(status) => status.describe(),
            Self::CubeLineTooLong => "a lookup table line is past the bound",
            Self::CubeMalformed => "that is not a line a lookup table file holds",
            Self::CubeNotThreeDimensional => "that is a per-channel curve rather than a cube",
            Self::CubeSizeRepeated => "the file states its size more than once",
            Self::CubeNoSize => "the file never says how big the cube is",
            Self::CubeSampleBeforeSize => "a sample arrived before the size",
            Self::CubeSizeUnsupported => "that side is not one this build carries",
            Self::CubeDomainUnsupported => "that input domain is not nought to one",
            Self::CubeExponentUnsupported => "this build does not read scientific notation",
            Self::CubeTooPrecise => "that number has more decimal places than this build carries",
            Self::CubeOutOfRange => "that number is too large to hold",
            Self::NotAReel => "the file is not a Media Editor reel",
            Self::EmptyReel => "a reel must hold at least one frame",
            Self::ReelDescriptionMismatch => "the reel's frames are not all described the same way",
            Self::TruncatedTrailer => "the file ends before the digest that closes it",
            Self::NotOnePlane => "a reel in a planar format cannot be wound a row at a time",
            Self::RowOutOfOrder => "that is not the row this reel is waiting for",
            Self::IncompleteReel => "the reel was finished before all of its rows arrived",
            Self::SinkNotEmpty => "a file is written into something empty",
            Self::SoundRunsPastPicture => "the reel's sound is not a length its pictures cover",
            Self::SoundNotDeclared => "this reel's header does not describe sound",
            Self::TranscriptNotDeclared => "this reel's header disagrees about its transcript",
            Self::CaptionOutOfOrder => "that is not the caption this reel is waiting for",
            Self::SoundOutOfOrder => "that is not the block of sound this reel is waiting for",
            Self::CueOutOfOrder => "that cue begins before the one before it",
            Self::CueVanishes => "that cue is shorter than the millisecond it would be written in",
            Self::EmptyCue => "a cue with no words is a cue a reader cannot see",
            Self::CueTextNotOneBlock => "a blank line inside a cue would end the cue",
            Self::CueTextLooksLikeATiming => "an arrow inside a cue would read as a timing line",
            Self::SoundBlockWrongLength => "that block is not a length one frame covers",
            Self::UnknownPixelFormat(_) => "a pixel format this format does not define",
            Self::UnknownColourTag(_) => "a colour tag this format does not define",
            Self::UnknownFaderTag(_) => "a fader tag this format does not define",
            Self::UnknownTransitionTag(_) => "that transition tag is not one this build reads",
            Self::UnknownMaskTag(_) => "that mask tag is not one this build reads",
            Self::DuplicateMedia => "this file lists the same content twice",
            Self::UnknownTransformTag(_) => "that transform tag is not one this build reads",
            Self::UnknownMotionTag(_) => "that motion tag is not one this build reads",
            Self::UnknownMediaSourceTag(_) => "that media source tag is not one this build reads",
            Self::TitleNotText => "this title's words are not text",
            Self::MarkerNotText => "this marker's text is not text",
            Self::CaptionNotText => "this caption's words are not text",
            Self::TitleDigestMismatch => "this title is not named by what it says",
            Self::UnknownAlignmentTag(_) => "that alignment tag is not one this build reads",
            Self::UnknownInkTag(_) => "that ink tag is not one this build reads",
            Self::UnknownFadeTag(_) => "that fade tag is not one this build reads",
            Self::UnknownSpeedTag(_) => "that speed tag is not one this build reads",
            Self::RampWithoutKeyframes => "a ramp is a curve, and that curve has no keyframes",
            Self::NameEmpty => "a name with nothing in it names nothing",
            Self::NotABitmap => "those bytes are not a bitmap",
            Self::NotAVault => "those bytes are not a vault",
            Self::FrameOutOfReel => "that reel has no frame there",
            Self::PlaneOutOfFrame => "that frame has no such plane or row",
            Self::TooSmall => "that destination is shorter than what it was to receive",
            Self::VaultFull => "this vault holds as much material as one may",
            Self::VaultTooLarge => "that material would take the vault past one file",
            Self::VaultNameTooLong => "that name is longer than a vault entry carries",
            Self::VaultNameNotText => "a name has to be printable text",
            Self::VaultItemTwice => "this vault already holds that material",
            Self::VaultItemAbsent => "this vault does not hold that material",
            Self::VaultSpanNotContiguous => "this vault's spans do not run end to end",
            Self::VaultItemDigestMismatch => "that material is not what its entry says",
            Self::BitmapUnsupported => "that is a bitmap of a shape this build does not read",
            Self::BitmapTooLarge => "that bitmap is larger than the importer accepts",
            Self::NameTooLong => "that name is longer than eight and three",
            Self::NameNotCanonical => "that byte is not one this filesystem's names accept",
            Self::NameDotMisplaced => "a name has one dot, and not at either end",
            Self::PathEmpty => "that path names no file",
            Self::PathAbsolute => "a path is relative to one mount",
            Self::PathTooLong => "that path is longer than the mount accepts",
            Self::PathMalformed => "that path is not a path this filesystem can follow",
            Self::PathAboveRoot => "that path climbs above the mount it is relative to",
            Self::Time(status) => status.describe(),
            Self::EdlLineTooLong => "this edit decision list line is longer than any real one",
            Self::EdlMalformedEvent => "this event line does not have an event's fields",
            Self::EdlMalformedTimecode => "this is not a timecode",
            Self::EdlUnknownChannel => "this build has no meaning for that channel",
            Self::EdlUnknownTransition => "this build has no meaning for that transition",
            Self::EdlUnknownFrameCodeMode => "an FCM line must say drop frame or non-drop frame",
            Self::EdlFrameCodeModeConflict => {
                "the FCM line and the timecode disagree about drop-frame"
            }
            Self::EdlNegativeDuration => "an out point must be after its in point",
            Self::EdlReelTooLong => "a reel name is eight characters",
            Self::EdlCommentBeforeEvent => "this comment names a clip before any event exists",
            Self::EdlNoEvents => "a file with no events is not an edit decision list",
            Self::ConformManyVideoTracks => "an edit decision list has one picture channel",
            Self::ConformTracksNotOrdered => {
                "picture has to come before sound for the order to survive"
            }
            Self::ConformReelCollision => {
                "two sources share the eight characters a reel name holds"
            }
            Self::ConformNoSourceDigest => "this event does not name its source by content",
            Self::ConformUnknownSource => "the media library does not hold that source",
            Self::ConformReelDisagreesWithSource => {
                "this event's reel name is not its source digest's first eight characters"
            }
            Self::ConformEventsOverlap => "this event begins before the one before it ended",
            Self::ConformDissolveFromNothing => {
                "there is nothing before this dissolve to dissolve from"
            }
            Self::ConformDissolveTooLong => "this dissolve is longer than the event carrying it",
            Self::ConformWipeUnsupported => "this build has no shape to wipe with",
            Self::ConformChannelNotSeparate => {
                "which tracks a B event means is a convention, not a statement"
            }
        }
    }
}

impl From<ModelStatus> for IoStatus {
    fn from(status: ModelStatus) -> Self {
        Self::Model(status)
    }
}

impl From<media_editor_core::CoreStatus> for IoStatus {
    fn from(status: media_editor_core::CoreStatus) -> Self {
        Self::Model(ModelStatus::Time(status))
    }
}

impl From<media_editor_media::MediaStatus> for IoStatus {
    fn from(status: media_editor_media::MediaStatus) -> Self {
        Self::Media(status)
    }
}

impl From<media_editor_audio::AudioStatus> for IoStatus {
    fn from(status: media_editor_audio::AudioStatus) -> Self {
        Self::Sound(status)
    }
}

impl From<SeamStatus> for IoStatus {
    fn from(status: SeamStatus) -> Self {
        Self::Seam(status)
    }
}

impl core::fmt::Display for IoStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, IoStatus>;
