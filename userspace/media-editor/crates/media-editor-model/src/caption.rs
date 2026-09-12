// SPDX-License-Identifier: GPL-3.0-only
//! Words somebody said, and where in the recording they said them.
//!
//! A caption is **not** an item, not a track, and not a position on the
//! timeline. It is a range of the *media* — a source in-point and out-point —
//! carrying text and a voice. Where it appears in the programme is not stored
//! anywhere: it is computed, every time it is asked for, from whatever clips
//! happen to be reading that stretch of that recording.
//!
//! ## Why that is the whole design
//!
//! Every editor that stores captions on the timeline loses caption sync, and
//! it loses it the same way: a caption is a position in the programme, an edit
//! moves the programme, and now every edit has to remember to move the
//! captions too. Cut, ripple, roll, slip, slide, retime, undo — each is a
//! place the bookkeeping can be forgotten, and each of them is forgotten
//! somewhere.
//!
//! Anchored in the source, there is no bookkeeping. **Not one line of
//! [`crate::Edit`] knows captions exist**, and every one of those operations
//! moves them correctly:
//!
//! - **cut** a clip in two and each half projects the part of the transcript
//!   it reads, so a sentence across the cut appears twice, clipped to each
//!   side;
//! - **ripple** or **roll** and the words move with the picture, because the
//!   picture is what says which source ticks are on screen when;
//! - **slip** and the words slip too, which is exactly right: slipping changes
//!   *which* of the recording is shown, and the transcript is of the recording;
//! - **retime** and they stretch, because the map from timeline to source is
//!   the retime and nothing else;
//! - **undo** and they are wherever the edit they came back from puts them,
//!   with no inverse of their own to get wrong.
//!
//! The cost is that finding a caption's timeline position means **inverting**
//! the map a clip applies to time — see [`crate::Clip::offset_reaching`],
//! which is where the interesting arithmetic is.
//!
//! ## One voice says one thing at a time
//!
//! Two captions of the same voice may not overlap: a person is not saying two
//! things at once, and a reader given both has no way to choose. Two *voices*
//! overlapping is a conversation, and is allowed — it is the case a single
//! sorted list of captions gets wrong, so it is the case the tests are about.

use alloc::string::String;

use crate::status::{ModelStatus, Result};

/// How many characters a caption may carry.
///
/// The same bound a marker and a title's line have, and for the same reason —
/// one number for "a line of text somebody typed" is better than three that
/// drift. It is also about right for the job: broadcast practice is thirty-two
/// to forty-two characters a line and two lines a caption.
pub const MAX_CAPTION_TEXT: usize = 128;

/// How many captions one asset may carry.
///
/// **Sixty-four, and this bound is a statement about where transcripts
/// eventually have to live rather than about how many captions are useful.**
///
/// A caption is 24 bytes of fields plus up to [`MAX_CAPTION_TEXT`] characters,
/// which is 512 bytes of UTF-8 in the worst case: 536 bytes each, so 34,304
/// for sixty-four of them. A Phipia program is mapped **76 KiB**, and the
/// project file is read whole — so one fully captioned asset is already 45% of
/// the program's entire address space, and two are over it.
///
/// Sixty-four captions is about five minutes of speech. A transcript of an
/// interview is thousands, and thousands do not fit and never will: the
/// arithmetic above is not a limitation of this build, it is a fact about a
/// file that is loaded in one piece.
///
/// So a transcript belongs where the material does — in the reel, read a
/// window at a time through the vault, exactly as a hundred photographs turned
/// out to belong in a store inside one file rather than in a hundred files.
/// That is named in the roadmap and is not built here. What is built here is
/// the part that does not change when it moves: what a caption *is*, and where
/// on the timeline it lands.
pub const MAX_CAPTIONS_PER_ASSET: usize = 64;

/// How many voices one asset may distinguish.
///
/// Eight. A voice is a speaker, and the number exists so that a caption can
/// say *who* without carrying a name per caption — a name would be text, and
/// text is what the bound above is made of.
pub const MAX_VOICES: u8 = 8;

/// Words, a stretch of the recording they belong to, and who said them.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Caption {
    from: i64,
    to: i64,
    voice: u8,
    text: String,
}

impl Caption {
    /// A caption over a half-open range of source ticks.
    ///
    /// Half-open, like every range in this program: the tick a caption ends at
    /// is the tick the next one may begin at, and two captions that shared a
    /// tick would both be on screen for a frame that belongs to one of them.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::EmptyCaption`] for a range that ends at or before it
    /// begins, [`ModelStatus::CaptionTextTooLong`] past
    /// [`MAX_CAPTION_TEXT`] characters, and [`ModelStatus::UnknownVoice`] past
    /// [`MAX_VOICES`].
    pub fn new(from: i64, to: i64, voice: u8, text: &str) -> Result<Self> {
        if to <= from {
            return Err(ModelStatus::EmptyCaption);
        }
        if text.chars().count() > MAX_CAPTION_TEXT {
            return Err(ModelStatus::CaptionTextTooLong);
        }
        if voice >= MAX_VOICES {
            return Err(ModelStatus::UnknownVoice);
        }
        let mut held = String::new();
        held.try_reserve(text.len())
            .map_err(|_| ModelStatus::OutOfMemory)?;
        held.push_str(text);
        Ok(Self {
            from,
            to,
            voice,
            text: held,
        })
    }

    /// The first source tick these words cover.
    #[must_use]
    pub const fn from(&self) -> i64 {
        self.from
    }

    /// The first source tick past them.
    #[must_use]
    pub const fn to(&self) -> i64 {
        self.to
    }

    /// Which speaker.
    #[must_use]
    pub const fn voice(&self) -> u8 {
        self.voice
    }

    /// What was said.
    #[must_use]
    pub fn text(&self) -> &str {
        &self.text
    }

    /// Whether this caption covers any of the ticks another does.
    ///
    /// Half-open at both ends, so two captions that meet do not overlap.
    #[must_use]
    pub const fn overlaps(&self, other: &Self) -> bool {
        self.from < other.to && other.from < self.to
    }
}

/// Check a set of captions is one anybody may carry.
///
/// **The overlap rule and nothing else.** It counted them too, against
/// [`MAX_CAPTIONS_PER_ASSET`], and that turned out to be two rules in one
/// function: one voice not saying two things at once is a fact about
/// *captions*, and sixty-four of them is a fact about a **project file**,
/// which is read in one piece. A reel is read a window at a time and carries
/// sixteen thousand. So the count is checked where the container is known and
/// this is checked everywhere.
///
/// # Errors
///
/// [`ModelStatus::CaptionsOverlap`] if two captions of one voice cover the
/// same tick.
pub fn checked(captions: &[Caption]) -> Result<()> {
    // Every pair rather than neighbours of a sorted list, because the list is
    // not sorted and must not have to be: two voices interleave, so "the next
    // caption" is not "the next caption of this voice" and a sorted sweep
    // would compare the wrong pairs. Sixty-four is small enough that the
    // square of it is 4,096 comparisons, which is less work than a sort.
    for (index, one) in captions.iter().enumerate() {
        for other in captions.iter().skip(index + 1) {
            if one.voice() == other.voice() && one.overlaps(other) {
                return Err(ModelStatus::CaptionsOverlap);
            }
        }
    }
    Ok(())
}

/// How many captions one query may report.
///
/// A query over an instant returns at most one caption per voice per track,
/// which is small. A query over a *span* is bounded by nothing the model can
/// see — a long programme of captioned shots could report thousands — so it is
/// bounded here, and reaching the bound is a refusal rather than a truncation
/// (R-1.4): a partial transcript that looks whole is worse than none.
pub const MAX_CAPTIONS_SHOWN: usize = 512;

/// Where a recording's words come from.
///
/// The third of these — [`media_editor_render::Library`] serves pictures and
/// `media_editor_audio::SampleSource` serves samples — and it arrives for the
/// same reason both of those did: the words do not fit in the project file.
///
/// Sixty-four captions an asset is 34,304 bytes at worst, which is 45% of the
/// seventy-six kilobytes a Phipia program is mapped, **for one asset**, in a
/// file that is read in one piece. A transcript of an interview is thousands.
/// So a transcript lives where the material does, and this is how a projection
/// asks for it.
///
/// It is asked for a **range** of the recording rather than for all of it,
/// because that is what makes it affordable: what comes back is what overlaps
/// a stretch a clip actually reads, and the rest is never held.
pub trait Transcript {
    /// The captions of this recording that cover any of `[from, to)`.
    ///
    /// # Errors
    ///
    /// Whatever the source cannot do.
    fn captions(
        &mut self,
        media: crate::media::Digest,
        from: i64,
        to: i64,
    ) -> Result<alloc::vec::Vec<Caption>>;
}

/// The transcript a project carries in its own media table.
///
/// What a caller passes when the words are in the project file — which is the
/// only place they can be until a reel carries them, and remains the right
/// place for the few a title or a short insert needs.
pub struct Held<'a> {
    project: &'a crate::Project,
}

impl<'a> Held<'a> {
    /// Read a project's own transcripts.
    #[must_use]
    pub const fn new(project: &'a crate::Project) -> Self {
        Self { project }
    }
}

impl Transcript for Held<'_> {
    fn captions(
        &mut self,
        media: crate::media::Digest,
        from: i64,
        to: i64,
    ) -> Result<alloc::vec::Vec<Caption>> {
        let Some(id) = self.project.find_media(media) else {
            return Ok(alloc::vec::Vec::new());
        };
        let asset = self.project.media().get(id)?;
        let mut found = alloc::vec::Vec::new();
        for caption in asset.captions() {
            if caption.from() < to && from < caption.to() {
                found.try_reserve(1).map_err(|_| ModelStatus::OutOfMemory)?;
                found.push(caption.clone());
            }
        }
        Ok(found)
    }
}

/// A caption, and where in the programme it lands.
///
/// The caption is **owned**, and it was borrowed until a reel could carry a
/// transcript. Once the words can come from storage there is nothing to borrow
/// from: a [`Transcript`] builds them to answer, so the projection moves them
/// rather than copying anything.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Shown {
    /// The first tick of the programme these words are on screen.
    pub from: media_editor_core::Instant,
    /// The first tick past them.
    pub to: media_editor_core::Instant,
    /// Which track it came off, so a caller can order two that coincide.
    pub track: usize,
    /// The words, and the voice that said them.
    pub caption: Caption,
}

/// One step of the path from a nested sequence's timeline up to the
/// programme's: the clip that reads it, and where that clip begins.
#[derive(Clone, Copy)]
struct Lift<'a> {
    clip: &'a crate::Clip,
    begins: i64,
}

/// Lift a range of one sequence's timeline up through a chain of nests.
///
/// Each step is the *same* question the projection asks of a clip and its
/// media, which is what makes nesting cost nothing new: a nested sequence's
/// timeline **is** the media its clip reads, so "where does this stretch of the
/// media appear" is asked once per level and composed.
fn lifted(range: (i64, i64), path: &[Lift<'_>]) -> Result<Option<(i64, i64)>> {
    let mut held = range;
    // Outward, from the innermost clip to the outermost, which is the order
    // the path was built in reversed -- so it is walked backwards.
    for step in path.iter().rev() {
        let Some((from, to)) = step.clip.offsets_showing(held.0, held.1)? else {
            return Ok(None);
        };
        held = (
            from.checked_add(step.begins)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?,
            to.checked_add(step.begins)
                .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?,
        );
    }
    Ok(Some(held))
}

/// Every caption on screen anywhere in a span, and where.
///
/// **Computed, never stored.** Nothing in the project records a caption's
/// position in the programme; this works it out from the clips that are there
/// at the moment it is asked. That is what "live" means and it is why no edit
/// has to maintain anything: cut, ripple, roll, slip, retime or undo, the next
/// query walks whatever the timeline is now.
///
/// A caption read by two clips — a sentence across a cut — is reported
/// **twice**, once for each, each clipped to the half that shows it. Merging
/// them would be wrong: there may be a minute of other programme between the
/// two halves, and a reader wants what is on screen rather than what was said.
///
/// Nested sequences are walked to [`crate::MAX_NESTING_DEPTH`] with an
/// explicit stack (R-5.5), and each level's ranges are lifted through the clip
/// that reads it — which is the same question asked again, because a nest's
/// timeline is the media its clip reads.
///
/// # Errors
///
/// [`ModelStatus::UnknownSequence`], [`ModelStatus::TooManyCaptions`] past
/// [`MAX_CAPTIONS_SHOWN`], [`ModelStatus::NestingTooDeep`], and whatever
/// [`crate::Clip::offsets_showing`] refuses.
pub fn captions_over(
    project: &crate::Project,
    sequence: crate::SequenceId,
    span: media_editor_core::TimeRange,
    transcript: &mut dyn Transcript,
) -> Result<alloc::vec::Vec<Shown>> {
    let mut found = alloc::vec::Vec::new();
    let timebase = span.timebase();
    let first = span.start().ticks();
    let last = span.end().map_err(ModelStatus::Time)?.ticks();
    // A worklist rather than a call stack, bounded by the model's own nesting
    // depth: a project deep enough to overflow a stack is refused before it is
    // walked rather than after (R-5.5).
    let mut levels = alloc::vec::Vec::new();
    levels
        .try_reserve(1)
        .map_err(|_| ModelStatus::OutOfMemory)?;
    levels.push((sequence, alloc::vec::Vec::new()));
    while let Some((which, path)) = levels.pop() {
        let held = project.sequence(which)?;
        for (index, track) in held.tracks().iter().enumerate() {
            let mut begins = 0_i64;
            for item in track.items() {
                let length = item.duration().ticks();
                let crate::Item::Clip(clip) = item else {
                    begins = begins
                        .checked_add(length)
                        .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
                    continue;
                };
                let asset = project.media().get(clip.media())?;
                if let Some(inner) = asset.nested() {
                    if path.len() >= crate::MAX_NESTING_DEPTH {
                        return Err(ModelStatus::NestingTooDeep);
                    }
                    let mut deeper = alloc::vec::Vec::new();
                    deeper
                        .try_reserve(path.len() + 1)
                        .map_err(|_| ModelStatus::OutOfMemory)?;
                    deeper.extend_from_slice(&path);
                    deeper.push(Lift { clip, begins });
                    levels
                        .try_reserve(1)
                        .map_err(|_| ModelStatus::OutOfMemory)?;
                    levels.push((inner, deeper));
                } else {
                    // The stretch of the recording this clip reads, asked for
                    // as a range rather than fetched entire: a source with a
                    // thousand captions hands back the handful that overlap.
                    let (low, high) = clip.source_span()?;
                    let words = transcript.captions(asset.digest(), low, high + 1)?;
                    for caption in words {
                        let Some(offsets) = clip.offsets_showing(caption.from(), caption.to())?
                        else {
                            continue;
                        };
                        let here = (
                            offsets.0.checked_add(begins).ok_or(ModelStatus::Time(
                                media_editor_core::CoreStatus::Overflow,
                            ))?,
                            offsets.1.checked_add(begins).ok_or(ModelStatus::Time(
                                media_editor_core::CoreStatus::Overflow,
                            ))?,
                        );
                        let Some((from, to)) = lifted(here, &path)? else {
                            continue;
                        };
                        // Clipped to the span asked for, rather than reported
                        // whole: a caller asking what is on screen over three
                        // frames wants three frames of answer.
                        let (from, to) = (from.max(first), to.min(last));
                        if from >= to {
                            continue;
                        }
                        if found.len() >= MAX_CAPTIONS_SHOWN {
                            return Err(ModelStatus::TooManyCaptions);
                        }
                        found.try_reserve(1).map_err(|_| ModelStatus::OutOfMemory)?;
                        found.push(Shown {
                            from: media_editor_core::Instant::new(from, timebase),
                            to: media_editor_core::Instant::new(to, timebase),
                            track: index,
                            caption,
                        });
                    }
                }
                begins = begins
                    .checked_add(length)
                    .ok_or(ModelStatus::Time(media_editor_core::CoreStatus::Overflow))?;
            }
        }
    }
    Ok(found)
}

/// The words on screen over a whole span, cut where they change.
///
/// [`captions_at`] answers for one instant, and an export that wants a whole
/// programme asks it once a frame. That is the wrong shape and the cost is not
/// subtle: each call walks every track, every item and every clip, and inverts
/// each clip's retime for every caption of its asset. A twenty-clip sequence
/// of sixty-four-caption assets over a twenty-four-thousand-frame reel asks
/// thirty million questions to answer what a single walk answers in a
/// thousand.
///
/// What makes one walk enough is that **the set on screen is a step function
/// of time**. It changes only where a caption starts or stops, so the span
/// falls into stretches over which the answer is constant, and there are at
/// most `2k + 1` of them for `k` captions — a number that has nothing to do
/// with how many frames the span holds.
///
/// So this projects once, records the moments the answer changes, and answers
/// any instant in the span by finding which stretch it is in. What it holds is
/// the captions themselves and one tick per edge: **no stretch stores its own
/// lines**, because a caption covers a stretch exactly when it covers the
/// stretch's first instant, and a stretch that stored them would cost `k`
/// entries times `2k + 1` stretches on a machine given seventy-six kibibytes.
pub struct Reading {
    shown: alloc::vec::Vec<Shown>,
    /// The instants the answer changes at, in order and without repeats. The
    /// span's own start and end are the first and last, so there are always at
    /// least two and the stretches are the gaps between them.
    edges: alloc::vec::Vec<i64>,
    timebase: media_editor_core::Timebase,
}

impl Reading {
    /// Project a sequence's captions across a span, once.
    ///
    /// # Errors
    ///
    /// As [`captions_over`], and [`ModelStatus::Time`] wrapping an overflow.
    pub fn over(
        project: &crate::Project,
        sequence: crate::SequenceId,
        span: media_editor_core::TimeRange,
        transcript: &mut dyn Transcript,
    ) -> Result<Self> {
        let shown = captions_over(project, sequence, span, transcript)?;
        let first = span.start().ticks();
        let last = span.end().map_err(ModelStatus::Time)?.ticks();
        let mut edges = alloc::vec::Vec::new();
        // Two ends and two edges a caption, which is every tick this can
        // possibly want and is the allocation made once.
        edges
            .try_reserve(
                shown
                    .len()
                    .checked_mul(2)
                    .and_then(|held| held.checked_add(2))
                    .ok_or(ModelStatus::CapacityExhausted)?,
            )
            .map_err(|_| ModelStatus::OutOfMemory)?;
        edges.push(first);
        edges.push(last);
        for held in &shown {
            // Clamped to the span: a caption that began before it did not
            // change anything *here*, and an edge outside the span would make
            // a stretch nobody can ask about.
            for edge in [held.from.ticks(), held.to.ticks()] {
                if edge > first && edge < last {
                    edges.push(edge);
                }
            }
        }
        edges.sort_unstable();
        edges.dedup();
        Ok(Self {
            shown,
            edges,
            timebase: span.timebase(),
        })
    }

    /// Every caption the span holds, in the order the projection found them.
    #[must_use]
    pub fn shown(&self) -> &[Shown] {
        &self.shown
    }

    /// How many stretches the span falls into.
    ///
    /// One for a span nobody speaks in, and never nought: a span always has
    /// itself.
    #[must_use]
    pub fn len(&self) -> usize {
        self.edges.len().saturating_sub(1)
    }

    /// Whether the span holds no stretches at all, which it never does.
    ///
    /// Present because a type with a `len` and no `is_empty` is a type
    /// somebody will call `len() == 0` on, and here that answer is always
    /// false.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    /// Which stretch an instant falls in.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::OutsideTheReading`] for an instant outside the span this
    /// was projected over, and [`ModelStatus::Time`] wrapping a timebase
    /// mismatch — an instant counted in another rate is not a position in this
    /// span, and answering for it would be answering a different question.
    pub fn stretch(&self, instant: media_editor_core::Instant) -> Result<usize> {
        if instant.timebase() != self.timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        let at = instant.ticks();
        let (Some(first), Some(last)) = (self.edges.first(), self.edges.last()) else {
            return Err(ModelStatus::OutsideTheReading);
        };
        if at < *first || at >= *last {
            return Err(ModelStatus::OutsideTheReading);
        }
        // The last edge at or before the instant. `partition_point` counts the
        // edges strictly before or equal, and the stretch is the gap that
        // begins at the last of them -- so one less.
        Ok(self
            .edges
            .partition_point(|edge| *edge <= at)
            .saturating_sub(1))
    }

    /// A stretch's own span.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::OutsideTheReading`] for a stretch this reading has not
    /// got, and [`ModelStatus::Time`] wrapping an overflow.
    pub fn span(&self, stretch: usize) -> Result<media_editor_core::TimeRange> {
        let from = *self
            .edges
            .get(stretch)
            .ok_or(ModelStatus::OutsideTheReading)?;
        let to = *self
            .edges
            .get(stretch + 1)
            .ok_or(ModelStatus::OutsideTheReading)?;
        media_editor_core::TimeRange::new(
            media_editor_core::Instant::new(from, self.timebase),
            media_editor_core::Duration::new(to - from, self.timebase)
                .map_err(ModelStatus::Time)?,
        )
        .map_err(ModelStatus::Time)
    }

    /// The words on screen throughout a stretch.
    ///
    /// Borrowed rather than cloned: a caption on screen for a minute is one
    /// caption, however many stretches touch it.
    ///
    /// # Errors
    ///
    /// As [`Reading::span`], and [`ModelStatus::OutOfMemory`].
    pub fn lines(&self, stretch: usize) -> Result<alloc::vec::Vec<&Shown>> {
        let over = self.span(stretch)?;
        let from = over.start().ticks();
        let mut found = alloc::vec::Vec::new();
        found
            .try_reserve(self.shown.len())
            .map_err(|_| ModelStatus::OutOfMemory)?;
        for held in &self.shown {
            // A stretch is maximal-constant, so a caption covers all of it
            // exactly when it covers its first instant. That is the whole
            // reason a stretch need not store anything.
            if held.from.ticks() <= from && from < held.to.ticks() {
                found.push(held);
            }
        }
        Ok(found)
    }
}

/// Every caption on screen at one instant.
///
/// What a viewer draws, and the query this whole design exists to make cheap:
/// it is a function of the project at the moment it is asked, so it cannot be
/// stale and there is nothing to invalidate.
///
/// # Errors
///
/// As [`captions_over`].
pub fn captions_at(
    project: &crate::Project,
    sequence: crate::SequenceId,
    instant: media_editor_core::Instant,
    transcript: &mut dyn Transcript,
) -> Result<alloc::vec::Vec<Shown>> {
    let one = media_editor_core::TimeRange::new(
        instant,
        media_editor_core::Duration::new(1, instant.timebase()).map_err(ModelStatus::Time)?,
    )
    .map_err(ModelStatus::Time)?;
    captions_over(project, sequence, one, transcript)
}
