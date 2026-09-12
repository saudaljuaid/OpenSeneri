// SPDX-License-Identifier: GPL-3.0-only
//! An editable programme: a timebase and its tracks.

use alloc::vec::Vec;

use media_editor_core::{Duration, Id, Instant, Timebase};

use crate::bounded::insert_bounded;
use crate::status::{ModelStatus, Result};
use crate::track::{Track, TrackKind};

/// How many tracks one sequence may hold. A policy bound: past this a
/// sequence is a rendering problem rather than an editing one.
pub const MAX_TRACKS_PER_SEQUENCE: usize = 128;

/// Which tracks an edit names, as a set.
///
/// A razor cuts *every* track at an instant, and a merge heals every cut it
/// made. That is one gesture and it must be one undo step, so it is one edit —
/// and an edit that names several tracks needs a way to say which.
///
/// A bitmask rather than a list, and the reason is the inverse. Cutting at an
/// instant does not cut a track whose material already ends there, or whose
/// cut is already there; healing has to put back exactly what the cut took and
/// nothing else. So the set the cut *performed* travels in its inverse, and a
/// set has to be a value an edit can hold without allocating (R-5.2) and
/// without a bound of its own to keep agreeing with
/// [`MAX_TRACKS_PER_SEQUENCE`].
///
/// One hundred and twenty-eight tracks, one hundred and twenty-eight bits.
/// The two numbers are the same number, checked at compile time below, so the
/// day one moves the other is a build failure rather than a silently truncated
/// set.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct TrackSet(u128);

const _: () = assert!(
    MAX_TRACKS_PER_SEQUENCE == u128::BITS as usize,
    "a track set has one bit per track, so the two bounds are one bound"
);

impl TrackSet {
    /// No tracks.
    pub const NONE: Self = Self(0);

    /// Whether this names no tracks at all.
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// How many tracks this names.
    ///
    /// Returns `usize` for direct use in capacity calculations. The fallback
    /// preserves the policy bound on an unsupported narrow platform.
    #[must_use]
    pub fn len(self) -> usize {
        usize::try_from(self.0.count_ones()).unwrap_or(MAX_TRACKS_PER_SEQUENCE)
    }

    /// Whether this names a track.
    #[must_use]
    pub const fn holds(self, index: usize) -> bool {
        index < MAX_TRACKS_PER_SEQUENCE && self.0 & (1 << index) != 0
    }

    /// The same set, with a track added.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] for an index past the bound. Refused
    /// rather than ignored: a set that quietly dropped a track would make a
    /// cut and its inverse describe different edits, which is the one thing a
    /// set exists here to prevent.
    pub const fn with(self, index: usize) -> Result<Self> {
        if index >= MAX_TRACKS_PER_SEQUENCE {
            return Err(ModelStatus::UnknownTrack);
        }
        Ok(Self(self.0 | (1 << index)))
    }

    /// The tracks this names, lowest first.
    ///
    /// In index order rather than in the order they were added, which is what
    /// makes an edit's effect independent of how its set was built (R-4.5).
    pub fn iter(self) -> impl Iterator<Item = usize> {
        (0..MAX_TRACKS_PER_SEQUENCE).filter(move |index| self.holds(*index))
    }
}

/// A cut: everything the timeline shows, at one rate.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Sequence {
    timebase: Timebase,
    tracks: Vec<Track>,
    /// In time order, at most one per instant.
    ///
    /// Beside the tracks rather than on one of them, because a marker is about
    /// the *programme* at a moment and not about any one layer of it. A note
    /// reading "the sync drifts here" is not a fact about V2.
    markers: Vec<crate::marker::Marker>,
}

impl Sequence {
    /// An empty sequence at a rate.
    #[must_use]
    pub const fn new(timebase: Timebase) -> Self {
        Self {
            timebase,
            tracks: Vec::new(),
            markers: Vec::new(),
        }
    }

    /// The rate everything in this sequence is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// How many tracks there are.
    #[must_use]
    pub fn track_count(&self) -> usize {
        self.tracks.len()
    }

    /// The tracks, in order.
    #[must_use]
    pub fn tracks(&self) -> &[Track] {
        &self.tracks
    }

    /// One track.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] if the index names nothing.
    pub fn track(&self, index: usize) -> Result<&Track> {
        self.tracks.get(index).ok_or(ModelStatus::UnknownTrack)
    }

    /// One track, for modification.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] if the index names nothing.
    pub fn track_mut(&mut self, index: usize) -> Result<&mut Track> {
        self.tracks.get_mut(index).ok_or(ModelStatus::UnknownTrack)
    }

    /// Add a track at an index, moving the ones after it down.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] for an index past the end,
    /// [`ModelStatus::CapacityExhausted`], or [`ModelStatus::OutOfMemory`].
    pub fn add_track(&mut self, index: usize, kind: TrackKind) -> Result<()> {
        if index > self.tracks.len() {
            return Err(ModelStatus::UnknownTrack);
        }
        let track = Track::new(kind, self.timebase);
        insert_bounded(&mut self.tracks, index, track, MAX_TRACKS_PER_SEQUENCE).map_err(|status| {
            match status {
                // `insert_bounded` speaks about items; here the thing being
                // indexed is a track, and the refusal should say so.
                ModelStatus::UnknownItem => ModelStatus::UnknownTrack,
                other => other,
            }
        })
    }

    /// Remove an empty track.
    ///
    /// A track with items on it is refused rather than emptied, so that
    /// removing a track can never destroy work in one step (R-9.1).
    ///
    /// # Errors
    ///
    /// [`ModelStatus::UnknownTrack`] or [`ModelStatus::TrackNotEmpty`].
    pub fn remove_track(&mut self, index: usize) -> Result<TrackKind> {
        let track = self.track(index)?;
        if !track.is_empty() {
            return Err(ModelStatus::TrackNotEmpty);
        }
        let kind = track.kind();
        self.tracks.remove(index);
        Ok(kind)
    }

    /// How long the sequence is: as long as its longest track.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping an overflow.
    pub fn duration(&self) -> Result<Duration> {
        let mut longest = Duration::zero(self.timebase);
        for track in &self.tracks {
            let track_duration = track.duration()?;
            if track_duration.compare(longest)? == core::cmp::Ordering::Greater {
                longest = track_duration;
            }
        }
        Ok(longest)
    }

    /// Which tracks a cut at this instant would actually cut.
    ///
    /// The razor's other half. A blade dragged down a timeline cuts every
    /// track it crosses *material* on — and crosses, rather than lands on the
    /// end of: a track whose clip finishes exactly there has nothing to cut,
    /// and a track already cut there is already cut. Neither is a refusal,
    /// because neither is a mistake the person holding the blade made; they
    /// are simply not in the set.
    ///
    /// A track with a transition across the instant is left out too. A cut
    /// under a dissolve would leave the transition describing a boundary that
    /// is no longer the one it was drawn on, and
    /// [`crate::Track::split`] refuses it — so this refuses to *name* it,
    /// which is the same decision made where the caller can still see it.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch or an overflow.
    pub fn cuttable_at(&self, instant: Instant) -> Result<TrackSet> {
        if instant.timebase() != self.timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        let mut named = TrackSet::NONE;
        for (index, track) in self.tracks.iter().enumerate() {
            let Some((item, offset)) = track.item_at(instant)? else {
                continue;
            };
            // Offset nought is a boundary, which is where a cut already is.
            // `Track::item_at` never answers past an item's end, so the other
            // side of the range needs no check here.
            if offset == 0 || track.has_transition_from(item + 1) {
                continue;
            }
            named = named.with(index)?;
        }
        Ok(named)
    }

    /// Which tracks a merge at this instant would actually heal.
    ///
    /// The exact question `cuttable_at` asks, from the other side: a track is
    /// in this set when it has a boundary *at* the instant and the two items
    /// either side of it are one item cut in two — which is what
    /// [`crate::Item::continues_into`] answers and is a stronger condition
    /// than "adjacent". Two shots that happen to abut are not a shot that was
    /// cut, and healing them would fuse two different pieces of material.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch or an overflow.
    pub fn healable_at(&self, instant: Instant) -> Result<TrackSet> {
        if instant.timebase() != self.timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        let mut named = TrackSet::NONE;
        for (index, track) in self.tracks.iter().enumerate() {
            // A boundary is an item whose *start* is the instant, which is
            // exactly the case `item_at` answers with an offset of nought.
            // Past the end of the track it answers nothing, and a boundary
            // there has no item after it to join.
            let Some((boundary, offset)) = track.item_at(instant)? else {
                continue;
            };
            if offset != 0 || boundary == 0 {
                continue;
            }
            // The condition `Track::join(boundary - 1)` checks, asked here so
            // the two cannot drift apart.
            if track.has_transition_from(boundary) {
                continue;
            }
            if track
                .item(boundary - 1)?
                .continues_into(track.item(boundary)?)
            {
                named = named.with(index)?;
            }
        }
        Ok(named)
    }

    /// The notes on this programme, in time order.
    #[must_use]
    pub fn markers(&self) -> &[crate::marker::Marker] {
        &self.markers
    }

    /// The marker at an instant, if there is one.
    #[must_use]
    pub fn marker_at(&self, instant: Instant) -> Option<&crate::marker::Marker> {
        self.markers.iter().find(|held| held.at() == instant)
    }

    /// Put a note on the programme.
    ///
    /// Inserted in time order rather than appended, so that reading the list
    /// is reading the timeline. Which also means the list's *order* carries no
    /// information a caller has to preserve — two projects with the same notes
    /// have the same list, whatever order somebody wrote them in (R-4.5).
    ///
    /// # Errors
    ///
    /// [`ModelStatus::Time`] wrapping a timebase mismatch, or an instant
    /// before the programme starts — a note before nought is a note on nothing.
    /// [`ModelStatus::MarkerExists`] if one is already there: two markers at an
    /// instant is the same nothing as none, because neither can be named.
    /// [`ModelStatus::CapacityExhausted`] or [`ModelStatus::OutOfMemory`].
    pub(crate) fn add_marker(&mut self, marker: crate::marker::Marker) -> Result<()> {
        if marker.at().timebase() != self.timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        if marker.at().ticks() < 0 {
            return Err(ModelStatus::MarkerBeforeStart);
        }
        let mut place = self.markers.len();
        for (index, held) in self.markers.iter().enumerate() {
            if held.at() == marker.at() {
                return Err(ModelStatus::MarkerExists);
            }
            if held.at().ticks() > marker.at().ticks() {
                place = index;
                break;
            }
        }
        insert_bounded(
            &mut self.markers,
            place,
            marker,
            crate::marker::MAX_MARKERS_PER_SEQUENCE,
        )
    }

    /// Take the note at an instant off, and hand it back.
    ///
    /// # Errors
    ///
    /// [`ModelStatus::NoSuchMarker`] if there is none there.
    pub(crate) fn remove_marker(&mut self, at: Instant) -> Result<crate::marker::Marker> {
        let place = self
            .markers
            .iter()
            .position(|held| held.at() == at)
            .ok_or(ModelStatus::NoSuchMarker)?;
        Ok(self.markers.remove(place))
    }
}

/// A reference to a sequence in a project.
pub type SequenceId = Id<Sequence>;
