// SPDX-License-Identifier: GPL-3.0-only
//! Media the project refers to but never modifies.

use alloc::vec::Vec;

use media_editor_core::{Duration, Id, Timebase};

/// A content digest: what a media asset *is*, independent of where it lives.
///
/// The project references media by this and relinks by this (R-9.5), so moving
/// a file breaks nothing and replacing a file's contents is detected rather
/// than silently used. It is [`media_editor_core::Digest`]: one definition of
/// content identity for the whole application, so that a media digest, a cache
/// key, and a saved file's digest are all the same kind of thing.
pub use media_editor_core::Digest;

/// A media asset the project can cut from.
///
/// It is described, not owned: Media Editor never modifies source media (R-9.6),
/// so this record is read-only for the whole life of the project.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MediaAsset {
    digest: Digest,
    timebase: Timebase,
    duration: Duration,
    location: Option<Location>,
    source: MediaSource,
    captions: alloc::vec::Vec<crate::caption::Caption>,
}

/// Where an asset's frames come from.
///
/// The only thing that separates a title from a recording. Everything else a
/// project does to media — cutting, trimming, dissolving, grading, masking,
/// moving — does not care and must not have to.
///
/// The *planner* acts on this, not the graph, and that is the same decision
/// M8.7 made about offline media for the same reason: a node that chose for
/// itself whether to fetch or to draw would be a node whose cache key did not
/// record which it had done.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum MediaSource {
    /// Bytes somewhere, named by the digest of their content.
    Recorded,
    /// A picture the program makes out of words.
    Title(crate::title::Title),
    /// A picture the program makes out of another sequence.
    ///
    /// The third kind, and it arrived with no new machinery for the same
    /// reason the second did: a clip cuts from media, so a clip cuts from a
    /// nested sequence — and trims it, grades it, masks it, frames it, fades
    /// it and animates all four, without one line that knows what a nest is.
    /// That is the claim [`MediaSource::Title`] was built on, cashed a second
    /// time.
    ///
    /// It names the sequence by **identifier** rather than by content, and
    /// that is the one place a nest differs from the other two. A title is
    /// named by what it says, so the same words in two projects are one asset;
    /// a recording is named by its bytes. A sequence is a thing somebody is
    /// still editing, and a digest over its contents would give the asset a
    /// new identity at every keystroke — which would repoint, or orphan, every
    /// clip that referred to it. So the identifier is the identity, and the
    /// *render* is where the content gets to matter: a nested clip becomes a
    /// subgraph rather than a fetch, so its cache key is a function of what is
    /// in the nest without anybody arranging that.
    Nested(crate::sequence::SequenceId),
}

/// How many bytes a location hint may be.
///
/// Two hundred and fifty-five: longer than any path Phipia's FAT32 volumes can
/// express in the ASCII 8.3 subset they admit, and a bound a hostile project
/// file cannot talk its way past (R-11.2).
pub const MAX_LOCATION_BYTES: usize = 255;

/// Where the bytes were last seen.
///
/// A **hint**, and the word is load-bearing. The digest is what the media
/// *is*; this is only where somebody found it, and the model never resolves
/// it, never opens it, and never compares two assets by it. A project whose
/// media has moved is a project with a stale hint and correct identity, which
/// is the right way round: the hint can be repaired by looking, and identity
/// cannot be recovered by guessing.
///
/// It is bytes rather than a string because a path is whatever the platform
/// says it is, and Phipia's is not decided yet. Refusing to interpret it here
/// is what keeps this crate free of the operating system.
#[derive(Clone, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Location {
    bytes: Vec<u8>,
}

impl Location {
    /// A hint from its bytes.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::EmptyLocation`] for no bytes at all — a hint that
    /// says nothing is worse than none, because it looks like an answer — and
    /// [`crate::ModelStatus::CapacityExhausted`] past
    /// [`MAX_LOCATION_BYTES`].
    pub fn new(bytes: &[u8]) -> crate::Result<Self> {
        if bytes.is_empty() {
            return Err(crate::ModelStatus::EmptyLocation);
        }
        if bytes.len() > MAX_LOCATION_BYTES {
            return Err(crate::ModelStatus::CapacityExhausted);
        }
        let mut held = Vec::new();
        held.try_reserve(bytes.len())
            .map_err(|_| crate::ModelStatus::OutOfMemory)?;
        held.extend_from_slice(bytes);
        Ok(Self { bytes: held })
    }

    /// The bytes, uninterpreted.
    #[must_use]
    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }
}

impl MediaAsset {
    /// Describe an asset.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::Time`] wrapping
    /// [`media_editor_core::CoreStatus::TimebaseMismatch`] if the duration is not
    /// counted in the asset's own timebase.
    pub fn new(digest: Digest, timebase: Timebase, duration: Duration) -> crate::Result<Self> {
        if duration.timebase() != timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        Ok(Self {
            digest,
            timebase,
            duration,
            location: None,
            source: MediaSource::Recorded,
            captions: alloc::vec::Vec::new(),
        })
    }

    /// An asset the program draws rather than reads.
    ///
    /// Its digest is the digest of the title's own description, which is what
    /// content addressing already meant: the same title in two projects is the
    /// same title, two clips of it share a cached frame, and changing a word
    /// makes a *different* asset rather than quietly changing what every clip
    /// of it shows.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::Time`] wrapping a timebase mismatch or an
    /// overflow.
    pub fn titled(
        title: crate::title::Title,
        timebase: Timebase,
        duration: Duration,
    ) -> crate::Result<Self> {
        if duration.timebase() != timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        Ok(Self {
            digest: title.digest()?,
            timebase,
            duration,
            // Nothing to find, so nothing to hint at. A title that carried a
            // location would be inviting somebody to relink it to a file, and
            // the file would be a different asset the moment it was opened.
            location: None,
            source: MediaSource::Title(title),
            captions: alloc::vec::Vec::new(),
        })
    }

    /// Media that is another sequence of this project.
    ///
    /// The digest is over the **identifier**, under its own domain tag, and
    /// both halves of that matter. Over the identifier, because a sequence is
    /// still being edited and a digest over its contents would change at every
    /// keystroke — see [`MediaSource::Nested`]. Under a domain tag, because a
    /// bare identifier is eight bytes anybody could also hash: without the tag
    /// a nest could collide with a recording whose *content* happened to hash
    /// the same way, and `Project::add_media` would then refuse the second as
    /// a duplicate of the first.
    ///
    /// The duration is the sequence's own length at the moment of nesting, and
    /// it does not stay right by itself — [`crate::Project`] refreshes it after
    /// every edit, because a nest is the one asset whose length is a fact
    /// about the project rather than about the world.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::Time`] wrapping a timebase mismatch.
    pub fn nesting(
        sequence: crate::sequence::SequenceId,
        timebase: Timebase,
        duration: Duration,
    ) -> crate::Result<Self> {
        if duration.timebase() != timebase {
            return Err(media_editor_core::CoreStatus::TimebaseMismatch.into());
        }
        let mut hasher = media_editor_core::Sha256::new();
        hasher.update(b"media-editor-nested-sequence-v1");
        hasher.update(&sequence.index().to_le_bytes());
        hasher.update(&sequence.generation().get().to_le_bytes());
        Ok(Self {
            digest: hasher.finish(),
            timebase,
            duration,
            // Nothing to find, for the reason a title has nothing to find: a
            // nest is made rather than read, and a location would be inviting
            // somebody to relink it to a file.
            location: None,
            source: MediaSource::Nested(sequence),
            captions: alloc::vec::Vec::new(),
        })
    }

    /// The same asset, saying how long its sequence is now.
    ///
    /// Only a nest has this, and only [`crate::Project`] calls it. A nest's
    /// length is a fact about the project rather than about the world, so it
    /// is the project's business to keep it true — and keeping it in the asset
    /// rather than deriving it at every use is what lets every existing check
    /// go on asking the asset how long it is, without one of them learning
    /// what a nest is.
    #[must_use]
    pub fn relengthened(&self, duration: Duration) -> Self {
        Self {
            duration,
            ..self.clone()
        }
    }

    /// Where this asset's frames come from.
    #[must_use]
    pub const fn source(&self) -> &MediaSource {
        &self.source
    }

    /// The sequence this asset is, if it is one.
    #[must_use]
    pub const fn nested(&self) -> Option<crate::sequence::SequenceId> {
        match &self.source {
            MediaSource::Nested(sequence) => Some(*sequence),
            MediaSource::Recorded | MediaSource::Title(_) => None,
        }
    }

    /// The words this is made of, if it is made of words.
    #[must_use]
    pub const fn title(&self) -> Option<&crate::title::Title> {
        match &self.source {
            MediaSource::Title(title) => Some(title),
            MediaSource::Recorded | MediaSource::Nested(_) => None,
        }
    }

    /// What this asset is.
    #[must_use]
    pub const fn digest(&self) -> Digest {
        self.digest
    }

    /// The rate this asset is counted in.
    #[must_use]
    pub const fn timebase(&self) -> Timebase {
        self.timebase
    }

    /// Where the bytes were last seen, if anybody said.
    #[must_use]
    pub const fn location(&self) -> Option<&Location> {
        self.location.as_ref()
    }

    /// The same asset with a hint about where to find it.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::NotRecordedMedia`] for a generated asset, which
    /// has nowhere to be. Accepting the hint and ignoring it would let a
    /// project hold a title that claims to be a file, and the next thing to
    /// read it would be entitled to believe that.
    pub fn with_location(&self, location: Option<Location>) -> crate::Result<Self> {
        if !matches!(self.source, MediaSource::Recorded) {
            return Err(crate::ModelStatus::NotRecordedMedia);
        }
        Ok(Self {
            location,
            ..self.clone()
        })
    }

    /// The words spoken in this recording, and where in it.
    ///
    /// In **source** time, which is the whole design: see
    /// [`crate::caption`]. Nothing here says where they appear in a
    /// programme, and nothing stores it.
    #[must_use]
    pub fn captions(&self) -> &[crate::caption::Caption] {
        &self.captions
    }

    /// The same asset with a transcript.
    ///
    /// # Errors
    ///
    /// [`crate::ModelStatus::TooManyCaptions`] past
    /// [`crate::caption::MAX_CAPTIONS_PER_ASSET`], and whatever
    /// [`crate::caption::checked`] refuses.
    pub fn with_captions(
        &self,
        captions: alloc::vec::Vec<crate::caption::Caption>,
    ) -> crate::Result<Self> {
        // The count is checked here rather than in `checked`, because
        // sixty-four is a fact about a file read in one piece and not about
        // captions: a reel is read a window at a time and carries sixteen
        // thousand of them.
        if captions.len() > crate::caption::MAX_CAPTIONS_PER_ASSET {
            return Err(crate::ModelStatus::TooManyCaptions);
        }
        crate::caption::checked(&captions)?;
        Ok(Self {
            captions,
            ..self.clone()
        })
    }

    /// How long it runs.
    #[must_use]
    pub const fn duration(&self) -> Duration {
        self.duration
    }
}

/// A reference to a media asset in a project's library.
pub type MediaId = Id<MediaAsset>;
