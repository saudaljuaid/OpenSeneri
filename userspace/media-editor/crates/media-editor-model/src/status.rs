// SPDX-License-Identifier: GPL-3.0-only
//! Every way the model refuses.

use media_editor_core::CoreStatus;

/// A refusal from the project model.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ModelStatus {
    /// An arithmetic or timebase refusal from the core types.
    Time(CoreStatus),
    /// An allocation that the caller must handle rather than abort on.
    OutOfMemory,
    /// A fixed policy capacity is reached.
    CapacityExhausted,
    /// A track index names no track.
    UnknownTrack,
    /// An item index names no item.
    UnknownItem,
    /// A media identifier names nothing in the library.
    UnknownMedia,
    /// A sequence identifier names nothing in the project.
    UnknownSequence,
    /// A media asset is counted in a timebase the sequence does not share.
    MediaTimebaseMismatch,
    /// An item's length would become zero or negative.
    EmptyItem,
    /// A split was asked for at the very start or end of an item, which would
    /// produce an empty piece.
    SplitOutsideItem,
    /// Two items cannot be joined: different kinds, different media, or a gap
    /// in the source between them.
    ItemsNotContiguous,
    /// A clip's source range would run off the start of its media.
    SourceBeforeStart,
    /// A clip's source range would run past the end of its media.
    SourceAfterEnd,
    /// A track that still holds items cannot be removed.
    TrackNotEmpty,
    /// There is nothing left to undo, or nothing left to redo.
    NothingToDo,
    /// A wipe with no direction, which would leave its edge nowhere.
    DegenerateWipe,
    /// A wipe softness outside nought to one.
    SoftnessOutOfRange,
    /// A mask with too few corners to enclose anything.
    MaskTooSimple,
    /// A mask whose corners turn both ways.
    MaskNotConvex,
    /// Two records of one digest that describe it differently.
    MediaContradiction,
    /// A location hint with no bytes in it.
    EmptyLocation,
    /// A transform whose linear part flattens the picture onto a line.
    TransformNotInvertible,
    /// A scale at nought or below, which is not a size.
    ScaleNotPositive,
    /// A title with no words in it.
    EmptyTitle,
    /// A title longer than this describes.
    TitleTooLong,
    /// Type at or below no size.
    TypeNotPositive,
    /// An ink channel outside nought to one, which is not a colour.
    InkOutOfRange,
    /// An asset that is generated, where one that was recorded was wanted.
    NotRecordedMedia,
    /// An asset that was recorded, where a generated one was wanted.
    NotGeneratedMedia,
    /// A clip whose fades together outlast it.
    FadesLongerThanClip,
    /// A speed of nought, which shows one frame forever.
    SpeedNotUsable,
    /// A ramp whose speed changes sign part way along it.
    SpeedRampChangesDirection,
    /// A Bézier ease asked for the exact area beneath it.
    EaseHasNoExactArea,
    /// Sound asked to play at a speed other than one.
    SoundCannotBeRetimed,
    /// A motion on a clip that has no transform to animate.
    NoTransformToAnimate,
    /// An animation of a mask on a clip that has none.
    NoMaskToAnimate,
    /// An animation of a grade's strength on a clip that has no grade.
    NoGradeToAnimate,
    /// A pair of numbers offered as a rotation that is not on the unit circle.
    NotATurn,
    /// An item was expected to be a gap of a stated length, and was not.
    NotTheGapThatWasLifted,
    /// A marker carrying more text than the bound allows.
    MarkerTextTooLong,
    /// A caption whose range ends at or before it begins.
    EmptyCaption,
    /// A caption carrying more text than the bound allows.
    CaptionTextTooLong,
    /// An asset carrying more captions than the bound allows.
    TooManyCaptions,
    /// Two captions of one voice covering the same tick of the recording.
    ///
    /// A person is not saying two things at once, and a reader given both has
    /// no way to choose. Two *voices* overlapping is a conversation and is
    /// allowed.
    CaptionsOverlap,
    /// A voice past the number an asset distinguishes.
    UnknownVoice,
    /// A marker was asked for at an instant that has none.
    NoSuchMarker,
    /// A marker already sits at that instant.
    MarkerExists,
    /// A marker was placed before the programme starts.
    MarkerBeforeStart,
    /// A sequence that would contain itself, directly or through others.
    SequenceWouldContainItself,
    /// Nesting deeper than the bound allows.
    NestingTooDeep,
    /// An edit that would leave a transition dissolving from a gap.
    TransitionWouldLoseItsClip,
    /// An operation that only a clip supports was asked of a gap.
    NotAClip,
    /// A fader set past the ends of its own travel.
    FaderOutOfRange,
    /// A dissolve was asked for at a cut that already has one.
    TransitionExists,
    /// A dissolve longer than one of the clips it dissolves between.
    TransitionTooLong,
    /// A cut named no dissolve.
    UnknownTransition,
    /// An edit would renumber a cut that a dissolve sits on. Take the dissolve
    /// off first.
    TransitionInTheWay,
    /// Undoing an edit did not reproduce the edit that was applied, so the
    /// model and its history describe different projects.
    HistoryInconsistent,
    /// Media cannot be removed while a sequence still cuts from it.
    MediaInUse,
    /// A curve with no keyframes, which could not say what the parameter is.
    EmptyCurve,
    /// Keyframes that do not run strictly forward in time — which covers two
    /// at one instant, because a parameter with two values at one moment has
    /// none.
    KeyframesOutOfOrder,
    /// Keyframes counted in more than one timebase.
    MixedTimebases,
    /// An instant counted differently from the thing it was asked about.
    WrongTimebase,
    /// An ease handle outside the span between its keyframes, which makes a
    /// curve that goes back in time.
    HandleOutOfSpan,
    /// A position, or a stretch, that a caption reading does not cover.
    OutsideTheReading,
    /// An opacity was set on a sound track, whose level is its fader.
    OpacityOnSound,
    /// A fader level was set on a picture track, whose level is its opacity.
    LevelOnPicture,
    /// No keyframe sits at that instant.
    NoSuchKeyframe,
    /// A keyframe already sits at that instant, and a parameter with two
    /// values at one moment has none.
    KeyframeExists,
    /// The last keyframe cannot be taken out of a curve; turning the
    /// automation off is a different operation.
    LastKeyframe,
    /// That lane has no automation to change.
    NoAutomation,
}

impl ModelStatus {
    /// One line naming the condition.
    #[must_use]
    pub const fn describe(self) -> &'static str {
        match self {
            Self::Time(status) => status.describe(),
            Self::OutOfMemory => "the allocation could not be satisfied",
            Self::CapacityExhausted => "a policy capacity is reached",
            Self::UnknownTrack => "there is no track at that index",
            Self::UnknownItem => "there is no item at that index",
            Self::UnknownMedia => "the media identifier names nothing",
            Self::UnknownSequence => "the sequence identifier names nothing",
            Self::MediaTimebaseMismatch => "the media is counted in another timebase",
            Self::EmptyItem => "an item cannot have zero length",
            Self::SplitOutsideItem => "the split point is not inside the item",
            Self::ItemsNotContiguous => "the two items are not contiguous in their source",
            Self::SourceBeforeStart => "the source range starts before the media does",
            Self::SourceAfterEnd => "the source range runs past the end of the media",
            Self::TrackNotEmpty => "the track still holds items",
            Self::NothingToDo => "there is nothing to undo or redo",
            Self::DegenerateWipe => "a wipe with no direction has nowhere to put its edge",
            Self::SoftnessOutOfRange => "a wipe fades over none to all of its travel",
            Self::MaskTooSimple => "those corners enclose no area",
            Self::MaskNotConvex => {
                "a concave mask is a union of convex ones, which this does not build"
            }
            Self::MediaContradiction => "the same content cannot be two different lengths",
            Self::EmptyLocation => "a hint that says nothing is worse than none",
            Self::TransformNotInvertible => "that transform flattens the picture onto a line",
            Self::ScaleNotPositive => "a scale of nought or less is not a size",
            Self::EmptyTitle => "a title with no words in it is a gap",
            Self::TitleTooLong => "that title is longer than this describes",
            Self::TypeNotPositive => "type has to be some size",
            Self::InkOutOfRange => "an ink channel runs from no light to full light",
            Self::NotRecordedMedia => "that asset is generated, not recorded",
            Self::NotGeneratedMedia => "that asset was recorded, not generated",
            Self::FadesLongerThanClip => "those fades together outlast the clip",
            Self::SpeedNotUsable => "a speed of nought is a freeze, not a speed",
            Self::SpeedRampChangesDirection => "a ramp that turns around is two shots, not one",
            Self::EaseHasNoExactArea => {
                "the area under an ease is not a number this can write down"
            }
            Self::SoundCannotBeRetimed => "sound cannot yet be played at another speed",
            Self::NoTransformToAnimate => "there is no transform on that clip to animate",
            Self::NoMaskToAnimate => "there is no mask on that clip to animate",
            Self::NoGradeToAnimate => "there is no grade on that clip to bring on",
            Self::NotATurn => "a turn is a point on the unit circle, and that is not one",
            Self::NotTheGapThatWasLifted => {
                "that slot is not the gap of that length a lift left behind"
            }
            Self::MarkerTextTooLong => "a marker carries more text than one may",
            Self::EmptyCaption => "a caption ends at or before it begins",
            Self::CaptionTextTooLong => "a caption carries more text than one may",
            Self::TooManyCaptions => "this asset carries more captions than one may",
            Self::CaptionsOverlap => "one voice cannot say two things at once",
            Self::UnknownVoice => "that voice is past the number an asset distinguishes",
            Self::NoSuchMarker => "no marker sits at that instant",
            Self::MarkerExists => "a marker already sits at that instant",
            Self::MarkerBeforeStart => "a marker before the programme starts is a note on nothing",
            Self::SequenceWouldContainItself => "a sequence cannot contain itself",
            Self::NestingTooDeep => "sequences nested deeper than this program renders",
            Self::TransitionWouldLoseItsClip => {
                "a transition at that cut needs a clip on both sides of it"
            }
            Self::NotAClip => "that operation applies only to a clip",
            Self::FaderOutOfRange => "that level is past the ends of the fader",
            Self::TransitionExists => "that cut already has a dissolve on it",
            Self::TransitionTooLong => "a dissolve may not outlast the clips it is between",
            Self::UnknownTransition => "that cut has no dissolve on it",
            Self::TransitionInTheWay => "this edit would move a dissolve; take it off first",
            Self::HistoryInconsistent => "the model and its history have diverged",
            Self::MediaInUse => "a sequence still cuts from that media",
            Self::EmptyCurve => "a curve with no keyframes has no value to give",
            Self::KeyframesOutOfOrder => "keyframes must run strictly forward in time",
            Self::MixedTimebases => "these are not all counted the same way",
            Self::WrongTimebase => "that instant is counted another way",
            Self::HandleOutOfSpan => "an ease handle sits outside the span it eases across",
            Self::OutsideTheReading => "that is outside the span this reading was projected over",
            Self::OpacityOnSound => "a sound track's level is its fader, not an opacity",
            Self::LevelOnPicture => "a picture track's level is its opacity, not a fader",
            Self::NoSuchKeyframe => "no keyframe sits at that instant",
            Self::KeyframeExists => "a keyframe already sits at that instant",
            Self::LastKeyframe => "a curve cannot lose its last keyframe",
            Self::NoAutomation => "that lane has no automation to change",
        }
    }
}

impl From<CoreStatus> for ModelStatus {
    fn from(status: CoreStatus) -> Self {
        Self::Time(status)
    }
}

impl core::fmt::Display for ModelStatus {
    fn fmt(&self, formatter: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        formatter.write_str(self.describe())
    }
}

/// The result of any fallible operation in this crate.
pub type Result<T> = core::result::Result<T, ModelStatus>;
