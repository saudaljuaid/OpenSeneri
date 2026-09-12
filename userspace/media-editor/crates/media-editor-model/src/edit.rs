// SPDX-License-Identifier: GPL-3.0-only
//! Edits, and the exact inverse of each one.
//!
//! An edit is a value: typed, comparable, and invertible. Applying one returns
//! the edit that undoes it, so history is a list of pairs and undo is not a
//! special mode — it is applying the other half of a pair (R-9.2).
//!
//! Every variant here is total in the sense that matters: it either performs
//! its whole change and returns its exact inverse, or it changes nothing and
//! returns a named refusal. There is no half-applied edit (R-1.4).

use media_editor_core::Duration;

use crate::item::Item;
use crate::sequence::Sequence;
use crate::status::{ModelStatus, Result};
use crate::track::TrackKind;

/// One change to a sequence.
///
/// ## Why some variants are so much bigger than the rest
///
/// `InsertItem` carries an [`Item`], and an item is the largest value in this
/// crate. That is not an accident of this variant: it is what an insert *is*,
/// and it is also what makes undo work. A `RemoveItem` returns an
/// `InsertItem` holding the item that came out, because nothing else in the
/// journal remembers it — the sequence no longer has it and the index alone
/// cannot conjure it back. [`Edit::DropItem`] carries one for the same reason,
/// as the inverse of a lift.
///
/// This enum used to carry a `clippy::large_enum_variant` exemption, and it
/// does not any more — **the lint stopped firing and the `expect` said so**.
/// The lint compares the largest variant against the next largest, and until
/// a lift existed there was exactly one variant holding an item. Now there are
/// two, they are the same size, and there is no outlier to complain about.
///
/// The exemption was written as an `expect` rather than an `allow` precisely
/// so that the day its premise stopped being true the build would fail rather
/// than carry a paragraph explaining a lint that no longer applies. It did.
///
/// What is left is the part that was doing the work anyway: `tests/size.rs`
/// holds a ceiling on this enum, so a variant that grew the *whole* history's
/// cost fails a test. And the reason boxing is not the remedy has not changed
/// — R-5.2 forbids an allocation that is not both bounded by a named policy
/// constant and fallible, and `Box::new` is neither.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Edit {
    /// Place an item, moving everything after it later.
    InsertItem {
        /// Which track.
        track: usize,
        /// Where in the track's item list.
        index: usize,
        /// What to place.
        item: Item,
    },
    /// Take an item out, moving everything after it earlier.
    RemoveItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
    },
    /// Change an item's length, moving everything after it. This is a ripple
    /// trim of the item's tail.
    SetItemDuration {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The new length.
        duration: Duration,
    },
    /// Change which part of its media a clip uses, without moving it or
    /// changing its length. This is a slip.
    SetClipSource {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The new first tick of the media.
        source_start: i64,
    },
    /// Put a look on a clip, or take one off.
    ///
    /// The look is named by the digest of its table, for the same reason media
    /// is: the same grade in two projects is the same grade, and a
    /// project-local handle would cache it twice and could not tell that the
    /// file behind it had been swapped.
    SetClipGrade {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The look, or nothing to take it off.
        grade: Option<crate::media::Digest>,
    },
    /// Move a clip in the frame, or put it back.
    SetClipTransform {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// Where it sits, or nothing to leave it where it was.
        transform: Option<crate::transform::Transform>,
    },
    /// Animate a clip's framing over its own length, or hold it still.
    ///
    /// The animation goes on the clip rather than into a track lane, so there
    /// is no keyframe name to survive a renumbering: a ripple that moves every
    /// item after this one moves its push-in with it and renumbers nothing.
    SetClipMotion {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The animation, or nothing to hold it still.
        motion: Option<crate::transform::Motion>,
    },
    /// Play a clip at a different speed, or hold it on one frame.
    ///
    /// One is real time, a half is slow motion, two is fast, and a negative
    /// speed runs the media backwards from the clip's in point. The clip keeps
    /// its length on the timeline — what changes is how much media it consumes
    /// to fill it, and a **freeze** is the case where that is one frame.
    ///
    /// One edit rather than two, because its inverse has to be able to say
    /// either: undoing a freeze on a clip that was at double speed must put it
    /// back at double speed, and undoing a retime of a still must put the
    /// still back.
    SetClipPlayback {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// At a speed, or held.
        playback: crate::item::Playback,
    },
    /// Animate a clip's mask over its own length, or hold it still.
    ///
    /// A uniform scale about the mask's **own centroid** and a move, which is
    /// what an iris opening, a vignette breathing and a shape sweeping across
    /// a card are all made of. Not the corners individually: a corner that
    /// moves on its own can turn a convex outline concave part way through,
    /// and this build computes an exact area only for a convex one — so
    /// per-corner animation would mean a refusal arriving at a *frame* rather
    /// than at the edit that caused it.
    ///
    /// Separate from [`Edit::SetClipMotion`], which animates the framing. A
    /// mask glued to the picture is what tracking wants and exactly wrong for
    /// a vignette, which should stay put while the shot pushes in.
    SetClipMaskMotion {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The animation, or nothing to hold the shape still.
        motion: Option<crate::transform::Motion>,
    },
    /// Animate a clip's opacity over its own length, or hold it still.
    ///
    /// The general answer where [`Edit::SetClipFades`] is the quick one: a
    /// fade is two lengths and a straight ramp, and this is a curve with
    /// whatever shape somebody drew — a hold, a linear run, an ease. The two
    /// **multiply**, like everything else here that decides what is on screen,
    /// so a clip can have both and neither throws the other away.
    ///
    /// Measured from the clip's own start, like the motion and for the same
    /// reason: there is no keyframe name to survive a renumbering, so a ripple
    /// that moves every item after this one moves its animation with it.
    ///
    /// [`None`] stops reading a curve, which is not the same as a curve
    /// holding one — that is what lets an animation be switched off and back
    /// on without losing the shape.
    SetClipOpacity {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The curve, or nothing to stop reading one.
        opacity: Option<crate::curve::Curve>,
    },
    /// Bring a clip's grade on over its own length, or apply it flat.
    ///
    /// Not *which* look — a digest is not a quantity, and two tables have
    /// nothing between them to interpolate. What animates is the **strength**:
    /// how far the picture has travelled from ungraded towards graded, which
    /// is nought for the clip untouched and one for the look applied exactly
    /// as [`Edit::SetClipGrade`] has always applied it.
    ///
    /// Measured from the clip's own start, like the framing, the shape and the
    /// opacity, and for the same reason: there is no keyframe name to survive
    /// a renumbering.
    ///
    /// [`None`] stops reading a curve, which is not the same as a curve
    /// holding one — that is what lets a grade's arrival be switched off and
    /// back on without losing the shape somebody drew.
    SetClipGradeStrength {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The curve, or nothing to apply the look flat.
        strength: Option<crate::curve::Curve>,
    },
    /// Fade a clip up from nothing at its head, and back down at its tail.
    ///
    /// What a transition at a cut cannot do: a dissolve needs two clips, and
    /// the first item of a programme has nothing before it.
    SetClipFades {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// How long it takes to come up.
        fade_in: media_editor_core::Duration,
        /// And to go back down.
        fade_out: media_editor_core::Duration,
    },
    /// Put a mask on a clip, or take one off.
    ///
    /// The shape itself rather than a handle to one, because a mask is
    /// geometry somebody dragged into place on *this* clip rather than
    /// content two projects could share. A grade is named by digest for the
    /// opposite reason: the same table in two projects is the same table.
    SetClipMask {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// The shape, or nothing to take it off.
        mask: Option<crate::mask::Mask>,
    },
    /// Cut an item in two.
    SplitItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// How far into the item to cut.
        offset: i64,
    },
    /// Move a cut, taking from one side and giving to the other.
    ///
    /// The programme's length does not change, which is the difference from
    /// trimming twice: nothing after the cut ever moves, so nothing after the
    /// cut has to be moved back.
    RollCut {
        /// Which track.
        track: usize,
        /// The cut, named by the item after it.
        boundary: usize,
        /// How far to move it, later if positive.
        by: i64,
    },
    /// Move an item along its track, its neighbours absorbing the difference.
    ///
    /// The item itself does not change — same source, same length. It happens
    /// later or earlier, and the items either side give and take to make room.
    SlideItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// How far to move it, later if positive.
        by: i64,
    },
    /// Put a note on the programme at an instant.
    ///
    /// A marker is not an item, does not live on a track, and nothing renders
    /// it. It names a moment of the finished piece and carries text, and it is
    /// the one thing in this model that exists purely for the person editing.
    ///
    /// It does **not** move when an item ripples. A note reading "fix the sync
    /// here" is about a place on the timeline, and moving it because an
    /// unrelated shot got longer would move it away from the thing it is
    /// about. A marker that belongs to a *clip* and travels with it is
    /// [`Edit::AddClipMarker`], which is the other half of the pair.
    AddMarker {
        /// Where the note goes.
        at: media_editor_core::Instant,
        /// What it says. Empty is allowed — it is the commonest kind.
        text: alloc::string::String,
    },
    /// Put a note on a **clip**, at an offset from the clip's own start.
    ///
    /// The other half of [`Edit::AddMarker`]. What differs is what the instant
    /// means and therefore what happens to the note afterwards: this one
    /// travels with the shot through a move, a trim, a lift and a cut, because
    /// it is measured from the shot rather than from the programme.
    AddClipMarker {
        /// Which track.
        track: usize,
        /// Which item on it.
        index: usize,
        /// Where the note goes, from the clip's own start.
        at: media_editor_core::Instant,
        /// What it says.
        text: alloc::string::String,
    },
    /// Take a note off a clip.
    ///
    /// The inverse of [`Edit::AddClipMarker`], and its own inverse is that
    /// edit carrying the text that came off — because nothing else remembers
    /// it once the clip no longer has it.
    RemoveClipMarker {
        /// Which track.
        track: usize,
        /// Which item on it.
        index: usize,
        /// Which note.
        at: media_editor_core::Instant,
    },
    /// Move a note on a clip from one offset to another.
    ///
    /// One edit rather than a remove and an add, for the reason
    /// [`Edit::MoveMarker`] is one.
    MoveClipMarker {
        /// Which track.
        track: usize,
        /// Which item on it.
        index: usize,
        /// Where it is.
        from: media_editor_core::Instant,
        /// Where it goes.
        to: media_editor_core::Instant,
    },
    /// Take the note at an instant off.
    ///
    /// The inverse of [`Edit::AddMarker`], and its own inverse is that edit
    /// carrying the text that came off — because nothing else remembers it
    /// once the sequence no longer has it.
    RemoveMarker {
        /// Which note.
        at: media_editor_core::Instant,
    },
    /// Move the note at one instant to another.
    ///
    /// One edit rather than a remove and an add, because it is one gesture and
    /// therefore one undo step — and because a remove-then-add would put the
    /// text through the history twice for a move that never changed it.
    MoveMarker {
        /// Where it is.
        from: media_editor_core::Instant,
        /// Where it goes.
        to: media_editor_core::Instant,
    },
    /// Take an item off a track, leaving a gap exactly as long.
    ///
    /// The **lift**, and the model had its counterpart from the start:
    /// [`Edit::RemoveItem`] takes an item out and moves everything after it
    /// earlier, which is what an editor calls a ripple delete or an extract.
    /// A lift is the other one — the shot goes, the hole stays, and nothing
    /// after it moves.
    ///
    /// Which of the two somebody wants is not a preference, it is a question
    /// about the *rest of the programme*. Sound cut to picture stays in sync
    /// through a lift and slides through an extract; a title two minutes later
    /// stays where it was written through a lift and arrives early through an
    /// extract. An editor that offered only one would be an editor that
    /// silently made that choice.
    ///
    /// Its inverse carries the item back, exactly as [`Edit::RemoveItem`]'s
    /// does, and puts it in a slot it checks is still the gap it left.
    LiftItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
    },
    /// Put a lifted item back into the gap it left.
    ///
    /// The inverse of [`Edit::LiftItem`], and not a general "replace this
    /// item": the slot has to still be a gap of exactly the item's length, or
    /// this refuses. A history that could drop an item into a slot something
    /// else had happened to since would be a history that describes a project
    /// nobody edited.
    DropItem {
        /// Which track.
        track: usize,
        /// Which item.
        index: usize,
        /// What to put back.
        item: Item,
    },
    /// Cut every named track at an instant.
    ///
    /// The razor, which is one gesture and therefore one edit and one undo
    /// step. [`Edit::SplitItem`] cuts one item on one track; this cuts a
    /// column, and the difference matters exactly when it is undone — a razor
    /// undone one track at a time is not a razor undone.
    ///
    /// The set is passed rather than computed, and that is what makes the
    /// inverse exact. [`crate::Sequence::cuttable_at`] answers which tracks a
    /// blade would land on, and a caller that hands the answer straight back
    /// gets the ordinary razor; a caller that narrows it first gets a blade
    /// that cuts some tracks and not others, which is the same gesture with a
    /// modifier held down. Neither has to be a second edit.
    ///
    /// A track not in the set is untouched. A track *in* the set that cannot
    /// be cut is a refusal for the whole edit: this publishes a column of cuts
    /// or it publishes nothing (R-1.4).
    CutAt {
        /// Where to cut, in the sequence's own timebase.
        at: media_editor_core::Instant,
        /// Which tracks to cut.
        tracks: crate::sequence::TrackSet,
    },
    /// Heal every named track's cut at an instant.
    ///
    /// The exact inverse of [`Edit::CutAt`] over the same set, and the merge
    /// gesture in its own right: drag a blade back over a column of cuts and
    /// the shots either side become the shots they were cut from.
    ///
    /// A cut is only healed when the two items either side of it are one item
    /// cut in two — which is [`crate::Item::continues_into`], and is stronger
    /// than "adjacent". Two different shots that happen to abut are not a shot
    /// that was cut, and fusing them would lose one of them.
    HealAt {
        /// Where the cuts are, in the sequence's own timebase.
        at: media_editor_core::Instant,
        /// Which tracks to heal.
        tracks: crate::sequence::TrackSet,
    },
    /// Join an item with the one after it.
    JoinItems {
        /// Which track.
        track: usize,
        /// The first of the two.
        index: usize,
    },
    /// Add an empty track.
    AddTrack {
        /// Where in the track list.
        index: usize,
        /// What the track carries.
        kind: TrackKind,
    },
    /// Remove an empty track.
    RemoveTrack {
        /// Which track.
        index: usize,
    },
    /// Move a track's fader.
    SetTrackFader {
        /// Which track.
        track: usize,
        /// Where to move it to.
        fader: crate::track::Fader,
    },
    /// Animate a track's opacity over time, or stop animating it.
    ///
    /// [`None`] is not the same as a curve holding one: it is a track with no
    /// automation, which is what lets automation be switched off and back on
    /// without losing the shape somebody drew.
    SetTrackOpacity {
        /// Which track.
        track: usize,
        /// The curve, or nothing to stop reading one.
        opacity: Option<crate::curve::Curve>,
    },
    /// Drive a sound track's fader from a curve, or stop driving it.
    ///
    /// The curve's values are decibels, the same units the static fader is set
    /// in. [`None`] hands the track back to its static fader, which is not the
    /// same as a curve sitting at that value.
    SetTrackLevel {
        /// Which track.
        track: usize,
        /// The curve, or nothing to stop driving it.
        level: Option<crate::curve::Curve>,
    },
    /// Change one keyframe on one of a track's automation lanes.
    ///
    /// The operation is nested rather than spread across four more variants
    /// here, so that [`Edit::apply`] keeps one arm for all of them and the
    /// match that handles them is exhaustive over exactly those four.
    ///
    /// Adding to a lane with no automation starts one, and removing the last
    /// keyframe turns it off — which makes those two each other's exact
    /// inverse. Replacing a whole curve is still [`Edit::SetTrackOpacity`] and
    /// [`Edit::SetTrackLevel`]; this is what a keyframe *drag* is, and the
    /// difference matters to the journal: fifty drags of one keyframe should
    /// not be fifty copies of a thousand-keyframe curve.
    Keyframe {
        /// Which track.
        track: usize,
        /// Which of its two lanes.
        lane: crate::curve::Automation,
        /// What to do to it.
        operation: crate::curve::KeyframeEdit,
    },
    /// Put a dissolve on a cut.
    AddTransition {
        /// Which track.
        track: usize,
        /// The dissolve, and the cut it sits on.
        transition: crate::track::Transition,
    },
    /// Take a dissolve off a cut.
    RemoveTransition {
        /// Which track.
        track: usize,
        /// Which cut.
        boundary: usize,
    },
}

impl Edit {
    /// Apply this edit to a sequence and return the edit that undoes it.
    ///
    /// # Errors
    ///
    /// Any [`ModelStatus`] the change itself refuses. On a refusal the
    /// sequence is unchanged.
    #[expect(
        clippy::too_many_lines,
        reason = "a dispatch is as long as the number of things it dispatches"
    )]
    pub fn apply(&self, sequence: &mut Sequence) -> Result<Self> {
        // This is a dispatch table: one arm per edit, each three to six lines
        // of doing the thing and naming its inverse. Its length is the number
        // of operations the model supports, which is the point of it, and the
        // line lint is measuring the wrong property.
        //
        // Two things did come out of it, and they came out because they
        // deserved to rather than to satisfy a count: `retime` and `slip` are
        // the two arms that read before they write, and that shape is worth a
        // name. What has deliberately *not* happened is splitting the match
        // itself — a subset of a dispatch needs an arm for every variant it
        // does not handle, which is a branch nothing reaches and no test can
        // cover. `Edit::Keyframe` nests its four operations for exactly that
        // reason, and grouping the item edits the same way is the move if this
        // ever needs to shrink again.
        //
        // `expect` rather than `allow`, so that the day this drops back under
        // a hundred lines the compiler says so instead of leaving a stale
        // waiver behind.
        match self.clone() {
            Self::InsertItem { track, index, item } => {
                sequence.track_mut(track)?.insert(index, item)?;
                Ok(Self::RemoveItem { track, index })
            }
            Self::RemoveItem { track, index } => {
                let item = sequence.track_mut(track)?.remove(index)?;
                Ok(Self::InsertItem { track, index, item })
            }
            Self::SetItemDuration {
                track,
                index,
                duration,
            } => {
                let previous = retime(sequence.track_mut(track)?, index, duration)?;
                Ok(Self::SetItemDuration {
                    track,
                    index,
                    duration: previous,
                })
            }
            Self::SetClipSource {
                track,
                index,
                source_start,
            } => {
                let previous = slip(sequence.track_mut(track)?, index, source_start)?;
                Ok(Self::SetClipSource {
                    track,
                    index,
                    source_start: previous,
                })
            }
            Self::SetClipGrade {
                track,
                index,
                grade,
            } => {
                let previous = regrade(sequence.track_mut(track)?, index, grade)?;
                Ok(Self::SetClipGrade {
                    track,
                    index,
                    grade: previous,
                })
            }
            Self::SetClipTransform {
                track,
                index,
                transform,
            } => {
                let previous = remove(sequence.track_mut(track)?, index, transform)?;
                Ok(Self::SetClipTransform {
                    track,
                    index,
                    transform: previous,
                })
            }
            Self::SetClipMotion {
                track,
                index,
                motion,
            } => {
                let previous = remotion(sequence.track_mut(track)?, index, motion)?;
                Ok(Self::SetClipMotion {
                    track,
                    index,
                    motion: previous,
                })
            }
            Self::SetClipFades {
                track,
                index,
                fade_in,
                fade_out,
            } => {
                let previous = refade(sequence.track_mut(track)?, index, fade_in, fade_out)?;
                Ok(Self::SetClipFades {
                    track,
                    index,
                    fade_in: previous.0,
                    fade_out: previous.1,
                })
            }
            Self::SetClipPlayback {
                track,
                index,
                playback,
            } => {
                let previous = replay(sequence.track_mut(track)?, index, &playback)?;
                Ok(Self::SetClipPlayback {
                    track,
                    index,
                    playback: previous,
                })
            }
            Self::SetClipMaskMotion {
                track,
                index,
                motion,
            } => {
                let previous = reshape(sequence.track_mut(track)?, index, motion)?;
                Ok(Self::SetClipMaskMotion {
                    track,
                    index,
                    motion: previous,
                })
            }
            Self::SetClipOpacity {
                track,
                index,
                opacity,
            } => {
                let previous = reveal(sequence.track_mut(track)?, index, opacity)?;
                Ok(Self::SetClipOpacity {
                    track,
                    index,
                    opacity: previous,
                })
            }
            Self::SetClipGradeStrength {
                track,
                index,
                strength,
            } => {
                let previous = bring_on(sequence.track_mut(track)?, index, strength)?;
                Ok(Self::SetClipGradeStrength {
                    track,
                    index,
                    strength: previous,
                })
            }
            Self::SetClipMask { track, index, mask } => {
                let previous = remask(sequence.track_mut(track)?, index, mask)?;
                Ok(Self::SetClipMask {
                    track,
                    index,
                    mask: previous,
                })
            }
            Self::SplitItem {
                track,
                index,
                offset,
            } => {
                sequence.track_mut(track)?.split(index, offset)?;
                Ok(Self::JoinItems { track, index })
            }
            Self::RollCut {
                track,
                boundary,
                by,
            } => {
                sequence.track_mut(track)?.roll(boundary, by)?;
                // Its own inverse with the sign turned round, which is the
                // whole reason a roll is worth having as an edit rather than
                // as two trims: there is nothing to remember.
                Ok(Self::RollCut {
                    track,
                    boundary,
                    by: by
                        .checked_neg()
                        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?,
                })
            }
            Self::SlideItem { track, index, by } => {
                sequence.track_mut(track)?.slide(index, by)?;
                Ok(Self::SlideItem {
                    track,
                    index,
                    by: by
                        .checked_neg()
                        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?,
                })
            }
            Self::AddMarker { at, text } => {
                sequence.add_marker(crate::marker::Marker::new(at, text)?)?;
                Ok(Self::RemoveMarker { at })
            }
            Self::AddClipMarker {
                track,
                index,
                at,
                text,
            } => {
                note(
                    sequence.track_mut(track)?,
                    index,
                    crate::marker::Marker::new(at, text)?,
                )?;
                Ok(Self::RemoveClipMarker { track, index, at })
            }
            Self::RemoveClipMarker { track, index, at } => {
                let held = unnote(sequence.track_mut(track)?, index, at)?;
                Ok(Self::AddClipMarker {
                    track,
                    index,
                    at,
                    text: alloc::string::String::from(held.text()),
                })
            }
            Self::MoveClipMarker {
                track,
                index,
                from,
                to,
            } => {
                let lane = sequence.track_mut(track)?;
                // Taken off before it is put back, so a move onto an offset
                // that already has a note is refused with the clip as it was.
                // Built before the write for the same reason (R-1.4): the
                // moved clip is only published once both halves have worked.
                let held = read_clip(lane, index)?;
                let (bare, taken) = held.without_marker(from)?;
                let moved = bare.with_marker(taken.moved_to(to)?)?;
                lane.replace(index, Item::Clip(moved))?;
                Ok(Self::MoveClipMarker {
                    track,
                    index,
                    from: to,
                    to: from,
                })
            }
            Self::RemoveMarker { at } => {
                let held = sequence.remove_marker(at)?;
                Ok(Self::AddMarker {
                    at,
                    text: alloc::string::String::from(held.text()),
                })
            }
            Self::MoveMarker { from, to } => {
                // Taken off before it is put back, so a move onto an instant
                // that already has a marker is refused with the sequence as it
                // was -- and a move onto its *own* instant is refused too,
                // which is right: an edit that changes nothing still takes a
                // place in the history.
                let held = sequence.remove_marker(from)?;
                let moved = held.moved_to(to)?;
                if let Err(refusal) = sequence.add_marker(moved) {
                    // Put back what was taken, so a refusal publishes nothing
                    // (R-1.4). This cannot itself refuse: the instant was free
                    // a line ago and nothing has been added since.
                    sequence.add_marker(held)?;
                    return Err(refusal);
                }
                Ok(Self::MoveMarker { from: to, to: from })
            }
            Self::LiftItem { track, index } => {
                let lifted = lift(sequence.track_mut(track)?, index)?;
                Ok(Self::DropItem {
                    track,
                    index,
                    item: lifted,
                })
            }
            Self::DropItem { track, index, item } => {
                drop_in(sequence.track_mut(track)?, index, item)?;
                Ok(Self::LiftItem { track, index })
            }
            Self::CutAt { at, tracks } => {
                cut_across(sequence, at, tracks)?;
                Ok(Self::HealAt { at, tracks })
            }
            Self::HealAt { at, tracks } => {
                heal_across(sequence, at, tracks)?;
                Ok(Self::CutAt { at, tracks })
            }
            Self::JoinItems { track, index } => {
                let lane = sequence.track_mut(track)?;
                let offset = lane.item(index)?.duration().ticks();
                lane.join(index)?;
                Ok(Self::SplitItem {
                    track,
                    index,
                    offset,
                })
            }
            Self::AddTrack { index, kind } => {
                sequence.add_track(index, kind)?;
                Ok(Self::RemoveTrack { index })
            }
            Self::RemoveTrack { index } => {
                let kind = sequence.remove_track(index)?;
                Ok(Self::AddTrack { index, kind })
            }
            Self::AddTransition { track, transition } => {
                sequence.track_mut(track)?.add_transition(transition)?;
                Ok(Self::RemoveTransition {
                    track,
                    boundary: transition.boundary(),
                })
            }
            Self::RemoveTransition { track, boundary } => {
                let transition = sequence.track_mut(track)?.remove_transition(boundary)?;
                Ok(Self::AddTransition { track, transition })
            }
            Self::Keyframe {
                track,
                lane,
                operation,
            } => {
                let undo = sequence.track_mut(track)?.edit_keyframe(lane, operation)?;
                Ok(Self::Keyframe {
                    track,
                    lane,
                    operation: undo,
                })
            }
            Self::SetTrackOpacity { track, opacity } => {
                let previous = sequence.track_mut(track)?.set_opacity(opacity)?;
                Ok(Self::SetTrackOpacity {
                    track,
                    opacity: previous,
                })
            }
            Self::SetTrackLevel { track, level } => {
                let previous = sequence.track_mut(track)?.set_level(level)?;
                Ok(Self::SetTrackLevel {
                    track,
                    level: previous,
                })
            }
            Self::SetTrackFader { track, fader } => {
                let previous = sequence.track_mut(track)?.set_fader(fader);
                Ok(Self::SetTrackFader {
                    track,
                    fader: previous,
                })
            }
        }
    }
}

/// Put a look on a clip, giving back the look it had.
///
/// Out of [`Edit::apply`] for the same reason as [`retime`] and [`slip`]: it
/// reads before it writes.
fn regrade(
    lane: &mut crate::track::Track,
    index: usize,
    grade: Option<crate::media::Digest>,
) -> Result<Option<crate::media::Digest>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has nothing to grade. Accepting it and doing nothing would
        // make the inverse a lie: undoing would have nothing to put back and
        // would still claim to have.
        return Err(ModelStatus::NotAClip);
    };
    if grade.is_none() && clip.grade_strength().is_some() {
        // The same guard `remask` carries against taking a shape off an
        // animated clip, for the same reason: a strength with no look to be
        // the strength of is a state no sequence of edits could reach, and
        // dropping the curve here instead would discard work the caller did
        // not name -- and make the inverse a lie, because undoing would put
        // the look back with its arrival already gone.
        return Err(ModelStatus::NoGradeToAnimate);
    }
    lane.replace(index, Item::Clip(clip.with_grade(grade)))?;
    Ok(clip.grade())
}

/// Bring a clip's grade on over its length, giving back the curve it had.
fn bring_on(
    lane: &mut crate::track::Track,
    index: usize,
    strength: Option<crate::curve::Curve>,
) -> Result<Option<crate::curve::Curve>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has no look to bring on, and the same argument `regrade` makes
        // applies: accepting it and doing nothing would make the inverse a lie.
        return Err(ModelStatus::NotAClip);
    };
    // `with_grade_strength` is what refuses a strength with no look and a curve
    // counted another way, so this does not check either again -- one guard, in
    // the type that owns the invariant, rather than one here and one there to
    // keep agreeing. Built before the write, so a refusal leaves the track as
    // it was (R-1.4).
    let brought = clip.with_grade_strength(strength)?;
    lane.replace(index, Item::Clip(brought))?;
    Ok(clip.grade_strength().cloned())
}

/// Take an item off a track, leaving a gap as long, and give it back.
///
/// The transition check is [`crate::track::Track::transition_at`] on the two
/// boundaries this item touches, rather than the "at or after" check a split
/// uses. A lift replaces in place, so nothing is renumbered and a dissolve
/// three cuts later is none of this edit's business — but a dissolve on
/// *either side of this item* would be left mixing a gap, which the layer
/// stack refuses at the frame rather than here.
///
/// # Errors
///
/// [`ModelStatus::NotAClip`] for a gap, which a lift would leave exactly as it
/// found it — and an edit that changes nothing still takes a place in the
/// history and still claims, on undo, to have put something back.
/// [`ModelStatus::TransitionWouldLoseItsClip`], and whatever the track
/// refuses.
fn lift(lane: &mut crate::track::Track, index: usize) -> Result<Item> {
    let held = lane.item(index)?.clone();
    if matches!(held, Item::Gap(_)) {
        return Err(ModelStatus::NotAClip);
    }
    if lane.transition_touching(index) {
        return Err(ModelStatus::TransitionWouldLoseItsClip);
    }
    // Built before the write, so a refusal leaves the track as it was (R-1.4).
    let hole = Item::gap(held.duration())?;
    lane.replace(index, hole)?;
    Ok(held)
}

/// Put a lifted item back, into a slot checked to be the gap it left.
///
/// # Errors
///
/// [`ModelStatus::NotTheGapThatWasLifted`] if the slot is not a gap of exactly
/// that length, and whatever the track refuses.
fn drop_in(lane: &mut crate::track::Track, index: usize, item: Item) -> Result<()> {
    let slot = lane.item(index)?;
    if !matches!(slot, Item::Gap(_)) || slot.duration() != item.duration() {
        return Err(ModelStatus::NotTheGapThatWasLifted);
    }
    lane.replace(index, item)?;
    Ok(())
}

/// Cut every named track at an instant, or cut none of them.
///
/// Two passes, and the split between them is the whole of R-1.4 here. The
/// first pass does **everything that can refuse**: it finds the item under the
/// instant on each named track, asks that item to split — which is where
/// `SplitOutsideItem` lives — checks for a dissolve in the way and for room,
/// and reserves that room. The second pass only writes, and cannot fail.
///
/// The alternative, splitting track by track and unwinding on a refusal, needs
/// an unwind that is itself correct under every partial failure — which is
/// more code, in the path that runs when something has already gone wrong.
/// Computing the answer first and publishing it afterwards is the same shape
/// the save protocol uses, for the same reason.
///
/// # Errors
///
/// [`ModelStatus::UnknownTrack`] for a set naming a track the sequence does
/// not have — refused rather than skipped, because a set that quietly ignored
/// a track would make the inverse describe a different edit.
/// [`ModelStatus::NothingToDo`] for an empty set: an edit that changes nothing
/// would still take a place in the history and would still claim, on undo, to
/// have put something back. Whatever `Item::split` and `Track` refuse.
fn cut_across(
    sequence: &mut Sequence,
    at: media_editor_core::Instant,
    tracks: crate::sequence::TrackSet,
) -> Result<()> {
    if tracks.is_empty() {
        return Err(ModelStatus::NothingToDo);
    }
    let mut planned = alloc::vec::Vec::new();
    planned
        .try_reserve(tracks.len())
        .map_err(|_| ModelStatus::OutOfMemory)?;
    for index in tracks.iter() {
        let lane = sequence.track(index)?;
        let (item, offset) = lane.item_at(at)?.ok_or(ModelStatus::SplitOutsideItem)?;
        let (head, tail) = lane.item(item)?.split(offset)?;
        planned.push((index, item, head, tail));
    }
    for (index, item, ..) in &planned {
        sequence.track_mut(*index)?.make_room_to_split(*item)?;
    }
    for (index, item, head, tail) in planned {
        sequence.track_mut(index)?.place_split(item, head, tail);
    }
    Ok(())
}

/// Heal every named track's cut at an instant, or heal none of them.
///
/// The same two passes and the same reason. Joining is the easier direction —
/// a track loses an item rather than gaining one, so nothing allocates — but
/// `Item::join` still refuses a pair that is not one item cut in two, and a
/// column that healed three tracks and refused the fourth would be a merge
/// nobody could undo in one step.
///
/// # Errors
///
/// As [`cut_across`], plus [`ModelStatus::ItemsNotContiguous`] for a boundary
/// whose two sides are two shots rather than one cut in two.
fn heal_across(
    sequence: &mut Sequence,
    at: media_editor_core::Instant,
    tracks: crate::sequence::TrackSet,
) -> Result<()> {
    if tracks.is_empty() {
        return Err(ModelStatus::NothingToDo);
    }
    let mut planned = alloc::vec::Vec::new();
    planned
        .try_reserve(tracks.len())
        .map_err(|_| ModelStatus::OutOfMemory)?;
    for index in tracks.iter() {
        let lane = sequence.track(index)?;
        let (boundary, offset) = lane.item_at(at)?.ok_or(ModelStatus::ItemsNotContiguous)?;
        if offset != 0 || boundary == 0 {
            // Not a cut. A healed instant has to be an item's first tick, and
            // the first item of a track has nothing before it to join to.
            return Err(ModelStatus::ItemsNotContiguous);
        }
        if lane.has_transition_from(boundary) {
            return Err(ModelStatus::TransitionInTheWay);
        }
        let joined = lane.item(boundary - 1)?.join(lane.item(boundary)?)?;
        planned.push((index, boundary, joined));
    }
    for (index, boundary, joined) in planned {
        sequence.track_mut(index)?.place_join(boundary, joined);
    }
    Ok(())
}

/// Move a clip, giving back where it was.
fn remove(
    lane: &mut crate::track::Track,
    index: usize,
    transform: Option<crate::transform::Transform>,
) -> Result<Option<crate::transform::Transform>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        return Err(ModelStatus::NotAClip);
    };
    if transform.is_none() && clip.motion().is_some() {
        // Taking the framing off an animated clip would leave the animation
        // with nothing to scale and nothing to move. Dropping the motion here
        // instead would be quietly discarding work the caller did not name,
        // and would make the inverse a lie: undoing would put the transform
        // back and the animation would already be gone.
        return Err(ModelStatus::NoTransformToAnimate);
    }
    lane.replace(index, Item::Clip(clip.with_transform(transform)))?;
    Ok(clip.transform())
}

/// Animate a clip, giving back the animation it had.
fn remotion(
    lane: &mut crate::track::Track,
    index: usize,
    motion: Option<crate::transform::Motion>,
) -> Result<Option<crate::transform::Motion>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        return Err(ModelStatus::NotAClip);
    };
    if let Some(wanted) = &motion {
        if clip.transform().is_none() {
            // A motion scales and moves a framing, and a clip with no framing
            // has none to scale. It could be made to mean "animate the
            // identity", which is a second way of saying the same thing and
            // therefore a second thing to keep true of the format, the
            // renderer, and every edit that touches either.
            return Err(ModelStatus::NoTransformToAnimate);
        }
        for held in crate::transform::lanes(wanted).into_iter().flatten() {
            if held.timebase() != clip.duration().timebase() {
                // Caught here rather than at the first frame that reads it. A
                // curve counted at 24 on a clip counted at 25 is refused by
                // `Curve::value_at` either way; the difference is whether the
                // editor is told when they set it or the render is.
                return Err(ModelStatus::WrongTimebase);
            }
        }
    }
    lane.replace(index, Item::Clip(clip.with_motion(motion)))?;
    Ok(clip.motion().cloned())
}

/// Retime or freeze a clip, giving back how it had been playing.
fn replay(
    lane: &mut crate::track::Track,
    index: usize,
    playback: &crate::item::Playback,
) -> Result<crate::item::Playback> {
    use crate::item::Playback;

    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has no media to play at any speed.
        return Err(ModelStatus::NotAClip);
    };
    if lane.kind() == crate::TrackKind::Audio
        && *playback != Playback::At(media_editor_core::Rational::ONE)
    {
        // Sound at a speed other than one needs a resampler, and a resampler
        // needs a filter somebody chose and a decision about pitch. Both are a
        // milestone of their own, and playing the samples at the wrong rate in
        // the meantime would be an answer nobody asked for (R-1.3).
        //
        // A freeze is refused by the same clause and for a sharper reason: a
        // held frame of sound is a held *block* of samples, which is a tone at
        // the block rate. Silence would be a different answer, and choosing
        // one for somebody is exactly what R-1.3 forbids.
        return Err(ModelStatus::SoundCannotBeRetimed);
    }
    // Built before the write, so a refusal leaves the track as it was (R-1.4).
    lane.replace(index, Item::Clip(playback.applied_to(&clip)?))?;
    Ok(clip.playback().clone())
}

/// The clip at an index, or a refusal saying it is not one.
///
/// Three of the note edits want the same two lines, and a gap has nothing to
/// leave a note on.
fn read_clip(lane: &crate::track::Track, index: usize) -> Result<crate::item::Clip> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        return Err(ModelStatus::NotAClip);
    };
    Ok(clip)
}

/// Put a note on a clip.
///
/// Built before the write, so a refusal leaves the track as it was (R-1.4).
fn note(lane: &mut crate::track::Track, index: usize, marker: crate::marker::Marker) -> Result<()> {
    let noted = read_clip(lane, index)?.with_marker(marker)?;
    lane.replace(index, Item::Clip(noted))?;
    Ok(())
}

/// Take a note off a clip, giving back the note.
fn unnote(
    lane: &mut crate::track::Track,
    index: usize,
    at: media_editor_core::Instant,
) -> Result<crate::marker::Marker> {
    let (bare, taken) = read_clip(lane, index)?.without_marker(at)?;
    lane.replace(index, Item::Clip(bare))?;
    Ok(taken)
}

/// Animate a clip's opacity, giving back the curve it had.
fn reveal(
    lane: &mut crate::track::Track,
    index: usize,
    opacity: Option<crate::curve::Curve>,
) -> Result<Option<crate::curve::Curve>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap is transparent already. There is nothing there to reveal.
        return Err(ModelStatus::NotAClip);
    };
    if lane.kind() != crate::TrackKind::Video {
        // A sound clip's loudness is its track's fader and its own fade, in
        // decibels. An opacity is a coverage, and the two are not the same
        // quantity wearing different names -- one multiplies light and the
        // other is a logarithm of amplitude.
        return Err(ModelStatus::OpacityOnSound);
    }
    // Built before the write, so a refusal leaves the track as it was (R-1.4).
    let revealed = clip.with_opacity(opacity)?;
    lane.replace(index, Item::Clip(revealed))?;
    Ok(clip.opacity().cloned())
}

/// Fade a clip, giving back the fades it had.
fn refade(
    lane: &mut crate::track::Track,
    index: usize,
    fade_in: media_editor_core::Duration,
    fade_out: media_editor_core::Duration,
) -> Result<(media_editor_core::Duration, media_editor_core::Duration)> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has nothing to fade. It is already nothing.
        return Err(ModelStatus::NotAClip);
    };
    // Built before the write, so a refusal leaves the track as it was (R-1.4).
    let faded = clip.with_fades(fade_in, fade_out)?;
    lane.replace(index, Item::Clip(faded))?;
    Ok((clip.fade_in(), clip.fade_out()))
}

/// Put a mask on a clip, giving back the one it had.
fn remask(
    lane: &mut crate::track::Track,
    index: usize,
    mask: Option<crate::mask::Mask>,
) -> Result<Option<crate::mask::Mask>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        // A gap has nothing to mask, and the same argument `regrade` makes
        // applies: accepting it and doing nothing would make the inverse a
        // lie.
        return Err(ModelStatus::NotAClip);
    };
    if mask.is_none() && clip.mask_motion().is_some() {
        // The same guard the framing carries, for the same reason: taking the
        // shape off an animated clip would leave the animation with nothing to
        // scale, and dropping the animation here instead would discard work
        // the caller did not name -- and make the inverse a lie, because
        // undoing would put the shape back with the animation already gone.
        return Err(ModelStatus::NoMaskToAnimate);
    }
    lane.replace(index, Item::Clip(clip.with_mask(mask)))?;
    Ok(clip.mask().cloned())
}

/// Animate a clip's mask, giving back the animation it had.
fn reshape(
    lane: &mut crate::track::Track,
    index: usize,
    motion: Option<crate::transform::Motion>,
) -> Result<Option<crate::transform::Motion>> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        return Err(ModelStatus::NotAClip);
    };
    if let Some(wanted) = &motion {
        for held in crate::transform::lanes(wanted).into_iter().flatten() {
            if held.timebase() != clip.duration().timebase() {
                // Caught here rather than at the first frame that reads it,
                // exactly as the framing's animation is.
                return Err(ModelStatus::WrongTimebase);
            }
        }
    }
    // `with_mask_motion` is what refuses an animation with no shape to animate,
    // so this does not check it again -- one guard, in the type that owns the
    // invariant, rather than one here and one there to keep agreeing.
    let reshaped = clip.with_mask_motion(motion)?;
    lane.replace(index, Item::Clip(reshaped))?;
    Ok(clip.mask_motion().cloned())
}

/// Change which part of its media a clip uses, giving back the part it used.
///
/// Out of [`Edit::apply`] for the same reason as [`retime`]: it reads before
/// it writes, which is the shape worth having a name, and the dispatch it came
/// from is long enough already.
fn slip(lane: &mut crate::track::Track, index: usize, source_start: i64) -> Result<i64> {
    let Item::Clip(clip) = lane.item(index)?.clone() else {
        return Err(ModelStatus::NotAClip);
    };
    lane.replace(index, Item::Clip(clip.with_source(source_start)?))?;
    Ok(clip.source_start())
}

/// Change an item's length, giving back the length it had.
///
/// Extracted from [`Edit::apply`] because it is the one arm that reads before
/// it writes, and because the arm it came from had grown past what one
/// function should hold. Splitting the *match* instead would have needed an
/// arm for every variant the split does not handle — a branch nothing can
/// reach and no test can cover — so what comes out is a step rather than a
/// subset of the dispatch.
fn retime(lane: &mut crate::track::Track, index: usize, duration: Duration) -> Result<Duration> {
    let existing = lane.item(index)?.clone();
    let replacement = existing.with_duration(duration)?;
    Ok(lane.replace(index, replacement)?.duration())
}
