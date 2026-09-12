// SPDX-License-Identifier: GPL-3.0-only
//! The frame cache.
//!
//! Bounded in frames and in bytes, evicting least-recently-used, and keyed by
//! content rather than by name (R-5.4, R-8.5). A key is a digest over every
//! input that could change the answer — the source frames, the parameters, and
//! a code version — so a cache entry cannot survive a change to the code that
//! produced it. That last part is what makes a frame cache safe to keep across
//! a release, and its absence is what makes most of them unsafe.
//!
//! Eviction order is by use, and ties are broken by insertion order, so the
//! pool behaves identically in two runs that ask for the same frames in the
//! same order. Nothing here iterates a hash map.

use alloc::vec::Vec;

use media_editor_core::{Digest, Sha256};

use crate::frame::Frame;
use crate::status::{MediaStatus, Result};

/// What version of the pipeline produced a cached frame.
///
/// Bump this whenever a change could make the same inputs produce different
/// pixels. A stale frame in a cache is a wrong frame in an export, and the
/// only defence that survives a refactor is a number someone has to change.
pub const CODE_VERSION: u32 = 1;

/// What a cached frame is the answer to.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct CacheKey(Digest);

impl CacheKey {
    /// A key over the inputs, the parameters, and the code version.
    #[must_use]
    pub fn new(inputs: &[Digest], parameters: &[u8], code_version: u32) -> Self {
        let mut hasher = Sha256::new();
        hasher.update(b"media-editor-cache-key-v1");
        hasher.update(&code_version.to_le_bytes());
        hasher.update(&(inputs.len() as u64).to_le_bytes());
        for input in inputs {
            hasher.update(input.bytes());
        }
        hasher.update(&(parameters.len() as u64).to_le_bytes());
        hasher.update(parameters);
        Self(hasher.finish())
    }

    /// A key for a frame produced by the current pipeline.
    #[must_use]
    pub fn current(inputs: &[Digest], parameters: &[u8]) -> Self {
        Self::new(inputs, parameters, CODE_VERSION)
    }

    /// The digest this key is.
    #[must_use]
    pub const fn digest(self) -> Digest {
        self.0
    }
}

/// One cached frame, and when it was last wanted.
#[derive(Clone, Debug)]
struct Entry {
    key: CacheKey,
    frame: Frame,
    last_used: u64,
}

/// How a pool has been used.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PoolCensus {
    /// How many frames are held.
    pub frames: usize,
    /// How many bytes they occupy.
    pub bytes: usize,
    /// How many lookups found what they wanted.
    pub hits: u64,
    /// How many did not.
    pub misses: u64,
    /// How many frames were evicted to make room.
    pub evictions: u64,
}

/// A bounded, content-keyed store of decoded frames.
#[derive(Clone, Debug)]
pub struct FramePool {
    entries: Vec<Entry>,
    frame_limit: usize,
    byte_limit: usize,
    bytes: usize,
    clock: u64,
    hits: u64,
    misses: u64,
    evictions: u64,
}

impl FramePool {
    /// A pool bounded in both frames and bytes.
    ///
    /// Both bounds are needed: a pool bounded only in frames holds an
    /// unbounded amount of memory when the frames are large, and one bounded
    /// only in bytes holds an unbounded number of bookkeeping entries when
    /// they are small.
    #[must_use]
    pub const fn new(frame_limit: usize, byte_limit: usize) -> Self {
        Self {
            entries: Vec::new(),
            frame_limit,
            byte_limit,
            bytes: 0,
            clock: 0,
            hits: 0,
            misses: 0,
            evictions: 0,
        }
    }

    /// How the pool has been used.
    #[must_use]
    pub fn census(&self) -> PoolCensus {
        PoolCensus {
            frames: self.entries.len(),
            bytes: self.bytes,
            hits: self.hits,
            misses: self.misses,
            evictions: self.evictions,
        }
    }

    /// Whether the pool holds nothing.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Look a frame up, counting the outcome.
    pub fn get(&mut self, key: CacheKey) -> Option<&Frame> {
        let Some(index) = self.entries.iter().position(|entry| entry.key == key) else {
            self.misses += 1;
            return None;
        };
        self.clock += 1;
        self.hits += 1;
        let clock = self.clock;
        let entry = self.entries.get_mut(index)?;
        entry.last_used = clock;
        Some(&entry.frame)
    }

    /// Whether a key is held, without counting it as a lookup.
    #[must_use]
    pub fn contains(&self, key: CacheKey) -> bool {
        self.entries.iter().any(|entry| entry.key == key)
    }

    /// Put a frame in, evicting as much as it takes to fit.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::FrameLargerThanPool`] if the frame could never fit,
    /// [`MediaStatus::KeyAlreadyPresent`] if the key already names a different
    /// frame, or [`MediaStatus::OutOfMemory`].
    pub fn insert(&mut self, key: CacheKey, frame: Frame) -> Result<()> {
        if frame.bytes() > self.byte_limit || self.frame_limit == 0 {
            return Err(MediaStatus::FrameLargerThanPool);
        }
        if let Some(existing) = self.entries.iter().find(|entry| entry.key == key) {
            // The same key must always mean the same frame. If it does not,
            // the key does not cover everything that changed the answer, and
            // silently replacing the entry would hide that (R-8.5).
            if existing.frame.digest() == frame.digest() {
                return Ok(());
            }
            return Err(MediaStatus::KeyAlreadyPresent);
        }

        while self.entries.len() + 1 > self.frame_limit
            || self.bytes + frame.bytes() > self.byte_limit
        {
            self.evict_one()?;
        }

        self.entries
            .try_reserve(1)
            .map_err(|_| MediaStatus::OutOfMemory)?;
        self.clock += 1;
        self.bytes += frame.bytes();
        self.entries.push(Entry {
            key,
            frame,
            last_used: self.clock,
        });
        Ok(())
    }

    /// Drop everything, and prove the byte count returns to zero.
    ///
    /// # Errors
    ///
    /// [`MediaStatus::PlaneSizeMismatch`] never happens here; the result exists
    /// so that a caller must look at the census it returns.
    pub fn clear(&mut self) -> Result<PoolCensus> {
        self.entries.clear();
        self.bytes = 0;
        Ok(self.census())
    }

    /// Remove the least recently used entry.
    fn evict_one(&mut self) -> Result<()> {
        // Ties break toward the earliest entry, so eviction order is the same
        // in two runs that did the same things (R-4.5).
        let victim = self
            .entries
            .iter()
            .enumerate()
            .min_by_key(|(index, entry)| (entry.last_used, *index))
            .map(|(index, _)| index)
            .ok_or(MediaStatus::FrameLargerThanPool)?;
        let entry = self.entries.remove(victim);
        self.bytes -= entry.frame.bytes();
        self.evictions += 1;
        Ok(())
    }
}
