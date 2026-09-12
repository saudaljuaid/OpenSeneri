// SPDX-License-Identifier: GPL-3.0-only
//! The project: media, sequences, and the history over them.
//!
//! This is the single source of truth (R-9.1). Everything else in Media Editor —
//! caches, rendered frames, waveform overviews, interface state — is derived
//! from it and can be thrown away.

use alloc::vec::Vec;

use media_editor_core::Timebase;

use crate::edit::Edit;
use crate::item::Item;
use crate::journal::EditJournal;
use crate::media::{MediaAsset, MediaId};
use crate::sequence::{Sequence, SequenceId};
use crate::status::{ModelStatus, Result};
use crate::store::Store;

/// How many media assets one project may hold.
pub const MAX_MEDIA: usize = 65_536;

/// How many sequences one project may hold.
pub const MAX_SEQUENCES: usize = 1024;

/// A whole editing project.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Project {
    media: Store<MediaAsset>,
    sequences: Store<Sequence>,
    history: EditJournal,
}

impl Default for Project {
    fn default() -> Self {
        Self::new()
    }
}

impl Project {
    /// An empty project.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            media: Store::new(MAX_MEDIA),
            sequences: Store::new(MAX_SEQUENCES),
            history: EditJournal::new(),
        }
    }

    /// The media library.
    #[must_use]
    pub const fn media(&self) -> &Store<MediaAsset> {
        &self.media
    }

    /// The sequences.
    #[must_use]
    pub const fn sequences(&self) -> &Store<Sequence> {
        &self.sequences
    }

    /// The history.
    #[must_use]
    pub const fn history(&self) -> &EditJournal {
        &self.history
    }

    /// Add a media asset to the library, or find the one already there.
    ///
    /// **One asset per digest.** Media is content-addressed, so the same bytes
    /// are the same asset however many times somebody opens the file — adding
    /// it again gives back the identifier it already has rather than a second
    /// one naming the same content.
    ///
    /// That is not a convenience. Two identifiers for one digest quietly
    /// falsified the conform round trip: an export writes the digest, an
    /// import looks it up, and with two candidates it finds the first — so a
    /// sequence cutting the same footage under two identifiers came back
    /// pointing at one of them, with nothing reported as lost. The theorem was
    /// stated three milestones before the case that breaks it was tried.
    ///
    /// The location hint is deliberately *not* part of the comparison, and the
    /// existing one is kept. The same content found in a second place is the
    /// same content; moving the hint is [`Edit::SetMediaLocation`]'s job, and
    /// doing it here would rewrite a project's records as a side effect of
    /// opening a file.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MediaContradiction`] if an asset with this digest is
    /// already held and describes it differently — the same bytes cannot be
    /// two lengths — and [`ModelStatus::CapacityExhausted`] or
    /// [`ModelStatus::OutOfMemory`].
    pub fn add_media(&mut self, asset: MediaAsset) -> Result<MediaId> {
        if let Some(held) = self.find_media(asset.digest()) {
            let existing = self.media.get(held)?;
            if existing.timebase() != asset.timebase() || existing.duration() != asset.duration() {
                return Err(ModelStatus::MediaContradiction);
            }
            return Ok(held);
        }
        self.media.insert(asset)
    }

    /// Which asset holds this content, if any.
    #[must_use]
    pub fn find_media(&self, digest: crate::media::Digest) -> Option<MediaId> {
        self.media
            .iter()
            .find(|(_, asset)| asset.digest() == digest)
            .map(|(id, _)| id)
    }

    /// Say where an asset's bytes were last seen, giving back the old hint.
    ///
    /// **Relinking is this and nothing else.** A hint moves; identity does
    /// not. Pointing a clip at *different* bytes is different media, and the
    /// digest says so rather than a flag — so there is no operation here that
    /// swaps one piece of content for another while keeping its name, because
    /// that is the operation that silently changes what a programme is.
    ///
    /// This is **not in the undo journal**, and that is a limitation rather
    /// than a decision. The journal applies edits to a *sequence*, and the
    /// media library belongs to the project; making a media change undoable
    /// means the journal becoming project-level, which is a larger change than
    /// the hint is worth. The previous hint is returned so a caller can put it
    /// back, which is the whole of what undo would do.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a stale or unknown identifier, and
    /// [`ModelStatus::NotRecordedMedia`] for a title, which has nowhere to be.
    pub fn set_media_location(
        &mut self,
        id: MediaId,
        location: Option<crate::media::Location>,
    ) -> Result<Option<crate::media::Location>> {
        let asset = self.media.get_mut(id)?;
        let previous = asset.location().cloned();
        // Built before the write, so a refusal leaves the library exactly as
        // it was (R-1.4).
        let relinked = asset.with_location(location)?;
        *asset = relinked;
        Ok(previous)
    }

    /// Remove a media asset, if nothing cuts from it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::MediaInUse`] if any sequence still refers to it, or an
    /// identifier refusal.
    pub fn remove_media(&mut self, id: MediaId) -> Result<MediaAsset> {
        for (_, sequence) in self.sequences.iter() {
            for track in sequence.tracks() {
                for item in track.items() {
                    if matches!(item, Item::Clip(clip) if clip.media() == id) {
                        return Err(ModelStatus::MediaInUse);
                    }
                }
            }
        }
        self.media.remove(id)
    }

    /// Add an empty sequence at a rate.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::CapacityExhausted`] or [`ModelStatus::OutOfMemory`].
    pub fn add_sequence(&mut self, timebase: Timebase) -> Result<SequenceId> {
        self.sequences.insert(Sequence::new(timebase))
    }

    /// Borrow a sequence.
    ///
    /// # Errors
    ///
    /// An identifier refusal.
    pub fn sequence(&self, id: SequenceId) -> Result<&Sequence> {
        self.sequences.get(id)
    }

    /// Apply an edit to a sequence and record it in the project's history.
    ///
    /// Every clip the edit introduces is checked against the library first:
    /// the media must exist, share the sequence's timebase, and contain the
    /// whole source range the clip asks for. A refusal leaves both the
    /// sequence and the history untouched.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownMedia`], [`ModelStatus::MediaTimebaseMismatch`],
    /// [`ModelStatus::SourceAfterEnd`], or whatever the edit itself refuses.
    pub fn apply(&mut self, id: SequenceId, edit: Edit) -> Result<()> {
        self.validate(id, &edit)?;
        let sequence = self.sequences.get_mut(id)?;
        self.history.apply(sequence, edit)?;
        // Nesting is the one thing an edit can break at a *distance*. Every
        // other invariant here is about the sequence the edit named, so
        // `validate` can check it beforehand; a nest makes one sequence's
        // length a fact about another's clips, and a trim over there can put a
        // clip over here past the end of what it reads.
        //
        // Checked after rather than before, because "what this sequence will
        // be" is not a question `validate` can answer without performing the
        // edit -- there are two dozen kinds of edit and each changes the length
        // differently. So the edit is performed, the project is asked whether
        // it is still consistent, and a refusal is **undone** before it is
        // returned. Nothing partial is published either way (R-1.4), and the
        // journal ends where it started because the undo pops the entry the
        // apply pushed.
        self.refresh_nests()?;
        if let Err(refusal) = self.check_nesting() {
            self.history.undo(self.sequences.get_mut(id)?)?;
            // And put the lengths back. The refresh above already published
            // the new ones, so undoing the *sequence* alone would leave the
            // library saying a nest is a length no sequence is -- a refused
            // edit having changed something, which is the one thing R-1.4
            // does not allow. A test found exactly that.
            //
            // This cannot loop: the refresh is a refresh, and the state it
            // runs against is the state that was consistent a moment ago.
            self.refresh_nests()?;
            return Err(refusal);
        }
        Ok(())
    }

    /// Bring every nest's length up to date with the sequence it names.
    ///
    /// A nest is the one asset whose length is a fact about the *project*
    /// rather than about the world, so it is the project's business to keep it
    /// true — and keeping it in the asset rather than deriving it at every use
    /// is what lets every existing check go on asking an asset how long it is,
    /// without one of them learning what a nest is.
    ///
    /// A project with no nests takes the first line and leaves, which is every
    /// project this repository held until nesting existed.
    fn refresh_nests(&mut self) -> Result<()> {
        if !self.media.iter().any(|(_, asset)| asset.nested().is_some()) {
            return Ok(());
        }
        let nests: Vec<(MediaId, SequenceId)> = self
            .media
            .iter()
            .filter_map(|(id, asset)| asset.nested().map(|sequence| (id, sequence)))
            .collect();
        for (id, sequence) in &nests {
            let length = self.sequences.get(*sequence)?.duration()?;
            let asset = self.media.get(*id)?;
            if asset.duration() != length {
                let refreshed = asset.relengthened(length);
                *self.media.get_mut(*id)? = refreshed;
            }
        }
        Ok(())
    }

    /// Whether nesting has left this project one an edit could have produced.
    ///
    /// Two questions the shape answers — no sequence contains itself, and
    /// nothing is nested deeper than this program renders — and then the
    /// ordinary source-range check over every clip, because
    /// [`Project::refresh_nests`] may just have changed the answer to it.
    ///
    /// That last one is the reason this runs at all. Every other invariant in
    /// this model is about the sequence an edit named, so `validate` can check
    /// it beforehand. A nest makes one sequence's length a fact about
    /// another's clips, and a trim over there can put a clip over here past
    /// the end of what it reads.
    fn check_nesting(&self) -> Result<()> {
        if !self.media.iter().any(|(_, asset)| asset.nested().is_some()) {
            return Ok(());
        }
        self.check_nesting_shape()?;
        for (_, sequence) in self.sequences.iter() {
            for track in sequence.tracks() {
                for item in track.items() {
                    if let Item::Clip(clip) = item {
                        self.check_source(sequence, clip)?;
                    }
                }
            }
        }
        Ok(())
    }

    /// Whether the nesting graph is acyclic and shallow enough to render.
    ///
    /// A depth-limited walk with an **explicit stack** rather than recursion,
    /// which R-5.5 requires by name for exactly this structure — "in the
    /// timeline, in nested sequences, in effect graphs, and in every parser".
    /// The stack cannot exceed [`crate::MAX_NESTING_DEPTH`] entries, so a cycle
    /// is caught by the depth bound even before the repeat check finds it, and
    /// both are named separately because they are different mistakes.
    fn check_nesting_shape(&self) -> Result<()> {
        for (start, _) in self.sequences.iter() {
            let mut path: Vec<SequenceId> = Vec::new();
            let mut work: Vec<(SequenceId, usize)> = Vec::new();
            work.try_reserve(1).map_err(|_| ModelStatus::OutOfMemory)?;
            work.push((start, 0));
            while let Some((held, step)) = work.pop() {
                if step == 0 {
                    if path.contains(&held) {
                        return Err(ModelStatus::SequenceWouldContainItself);
                    }
                    if path.len() >= crate::MAX_NESTING_DEPTH {
                        return Err(ModelStatus::NestingTooDeep);
                    }
                    path.try_reserve(1).map_err(|_| ModelStatus::OutOfMemory)?;
                    path.push(held);
                }
                let Some(next) = self.nested_under(held, step)? else {
                    path.pop();
                    continue;
                };
                work.try_reserve(2).map_err(|_| ModelStatus::OutOfMemory)?;
                work.push((held, step + 1));
                work.push((next, 0));
            }
        }
        Ok(())
    }

    /// The `step`-th sequence nested inside this one, in a fixed order.
    ///
    /// Index order over tracks and items, so the walk above visits the same
    /// sequences in the same order on every machine (R-4.5) — which matters
    /// because *which* cycle a refusal names would otherwise depend on
    /// iteration order.
    fn nested_under(&self, held: SequenceId, step: usize) -> Result<Option<SequenceId>> {
        let mut seen = 0;
        for track in self.sequences.get(held)?.tracks() {
            for item in track.items() {
                let Item::Clip(clip) = item else {
                    continue;
                };
                let Ok(asset) = self.media.get(clip.media()) else {
                    continue;
                };
                let Some(sequence) = asset.nested() else {
                    continue;
                };
                if seen == step {
                    return Ok(Some(sequence));
                }
                seen += 1;
            }
        }
        Ok(None)
    }

    /// Undo the most recent edit to a sequence.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`], or an identifier refusal.
    pub fn undo(&mut self, id: SequenceId) -> Result<Edit> {
        let sequence = self.sequences.get_mut(id)?;
        self.history.undo(sequence)
    }

    /// Redo the most recently undone edit to a sequence.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NothingToDo`], or an identifier refusal.
    pub fn redo(&mut self, id: SequenceId) -> Result<Edit> {
        let sequence = self.sequences.get_mut(id)?;
        self.history.redo(sequence)
    }

    /// Discard the undo history, keeping the project exactly as it is.
    ///
    /// Loading a file uses this. The edits that build a project out of its
    /// saved structure are how the structure is expressed, not something the
    /// user did, and offering to undo them would offer to undo the file
    /// itself. A freshly opened project has nothing to undo, which is what
    /// every editor means by opening one.
    pub fn forget_history(&mut self) {
        self.history = EditJournal::new();
    }

    /// Check an edit against the media library before it is applied.
    ///
    /// Four edits can put a clip's source range outside its media: inserting a
    /// clip, lengthening one, slipping one, and **retiming** one (a freeze is
    /// the one that cannot, and is checked anyway). Each is
    /// checked here, on the clip the edit would produce, before anything
    /// changes. Removals, splits, joins, and track edits cannot introduce a
    /// reference or widen a range, so they have nothing to check.
    ///
    /// Retiming was missing for a milestone, and the shape of the omission is
    /// worth keeping: `check_source` took a start and a *length on the
    /// timeline*, which was the same thing as what a clip read right up until
    /// a clip could be retimed. A clip at double speed reads twice its length
    /// and nothing noticed. So the question it asks is now the clip's own —
    /// [`crate::Clip::source_span`] — which cannot fall behind the mapping,
    /// because it *is* the mapping.
    fn validate(&self, id: SequenceId, edit: &Edit) -> Result<()> {
        let sequence = self.sequences.get(id)?;
        let held = |track: &usize, index: &usize| -> Result<Option<crate::Clip>> {
            match sequence.track(*track)?.item(*index)? {
                Item::Clip(clip) => Ok(Some(clip.clone())),
                Item::Gap(_) => Ok(None),
            }
        };
        match edit {
            Edit::InsertItem { item, .. } => {
                let Item::Clip(clip) = item else {
                    return Ok(());
                };
                self.check_source(sequence, clip)
            }
            Edit::SetItemDuration {
                track,
                index,
                duration,
            } => {
                let Some(clip) = held(track, index)? else {
                    return Ok(());
                };
                self.check_source(sequence, &clip.with_duration(*duration)?)
            }
            Edit::SetClipSource {
                track,
                index,
                source_start,
            } => {
                let Some(clip) = held(track, index)? else {
                    return Ok(());
                };
                self.check_source(sequence, &clip.with_source(*source_start)?)
            }
            Edit::SetClipPlayback {
                track,
                index,
                playback,
            } => {
                let Some(clip) = held(track, index)? else {
                    return Ok(());
                };
                // The same construction the edit itself uses, asked for by
                // name rather than written out again here.
                self.check_source(sequence, &playback.applied_to(&clip)?)
            }
            _ => Ok(()),
        }
    }

    /// Whether what a clip reads lies inside the media it names.
    fn check_source(&self, sequence: &Sequence, clip: &crate::Clip) -> Result<()> {
        let asset = self
            .media
            .get(clip.media())
            .map_err(|_| ModelStatus::UnknownMedia)?;
        if asset.timebase() != sequence.timebase() {
            return Err(ModelStatus::MediaTimebaseMismatch);
        }
        let (lowest, highest) = clip.source_span()?;
        if lowest < 0 {
            return Err(ModelStatus::SourceBeforeStart);
        }
        // The span's ends are both frames the clip shows, so the last one has
        // to *be* a frame of the asset rather than sit one past the last --
        // which is why this compares against the length rather than to it.
        if highest >= asset.duration().ticks() {
            return Err(ModelStatus::SourceAfterEnd);
        }
        Ok(())
    }
}
