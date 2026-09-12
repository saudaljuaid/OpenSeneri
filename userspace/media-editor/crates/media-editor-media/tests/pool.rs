// SPDX-License-Identifier: GPL-3.0-only
//! The frame pool: its bounds, its eviction order, and the key discipline
//! that keeps it from ever answering the wrong question.

use media_editor_core::Digest;
use media_editor_media::{
    CODE_VERSION, CacheKey, ColourDescription, Frame, FrameDescription, FramePool, Geometry,
    MediaStatus, PixelFormat,
};

fn description(width: u32, height: u32) -> FrameDescription {
    FrameDescription::square(
        Geometry::new(width, height).expect("a geometry"),
        PixelFormat::Gray8,
        ColourDescription::srgb_full(),
        None,
        None,
    )
    .expect("a description")
}

/// A distinguishable frame of a given size.
fn frame(width: u32, height: u32, fill: u8) -> Frame {
    let description = description(width, height);
    let bytes = std::vec![fill; description.packed_bytes().expect("a size")];
    Frame::from_packed(description, &bytes).expect("a frame")
}

fn key(seed: u8) -> CacheKey {
    CacheKey::current(&[Digest::of(&[seed])], b"none")
}

#[test]
fn a_key_covers_its_inputs_its_parameters_and_the_code_version() {
    let input = Digest::of(b"a frame");
    let other = Digest::of(b"another frame");

    let base = CacheKey::current(&[input], b"scale=2");
    assert_eq!(base, CacheKey::current(&[input], b"scale=2"), "stable");
    assert_ne!(
        base,
        CacheKey::current(&[other], b"scale=2"),
        "a different input is a different question"
    );
    assert_ne!(
        base,
        CacheKey::current(&[input], b"scale=3"),
        "a different parameter is a different question"
    );
    assert_ne!(
        base,
        CacheKey::new(&[input], b"scale=2", CODE_VERSION + 1),
        "code that changed is code that may answer differently"
    );
    assert_ne!(
        base,
        CacheKey::current(&[input, other], b"scale=2"),
        "one input is not two"
    );
}

#[test]
fn the_length_of_a_field_is_part_of_the_key() {
    // Without the lengths in the hash, ("ab", "c") and ("a", "bc") would be
    // the same key, and two different renders would share a cache entry.
    let first = CacheKey::current(&[Digest::of(b"ab")], b"c");
    let second = CacheKey::current(&[Digest::of(b"a")], b"bc");
    assert_ne!(first, second);
}

#[test]
fn a_hit_returns_the_frame_and_a_miss_says_so() {
    let mut pool = FramePool::new(4, 4096);
    assert!(pool.get(key(1)).is_none());
    assert_eq!(pool.census().misses, 1);

    let held = frame(8, 8, 200);
    let digest = held.digest();
    pool.insert(key(1), held).expect("room");
    assert_eq!(pool.get(key(1)).map(Frame::digest), Some(digest));
    assert_eq!(pool.census().hits, 1);
    assert_eq!(pool.census().frames, 1);
    assert_eq!(pool.census().bytes, 64);
}

#[test]
fn the_same_key_may_not_name_two_different_frames() {
    // If it could, the key does not cover something that changed the answer,
    // and quietly replacing the entry would hide that.
    let mut pool = FramePool::new(4, 4096);
    pool.insert(key(1), frame(8, 8, 10)).expect("room");
    assert_eq!(
        pool.insert(key(1), frame(8, 8, 20)),
        Err(MediaStatus::KeyAlreadyPresent)
    );
    // Inserting the identical frame again is a no-op, not an error.
    assert_eq!(pool.insert(key(1), frame(8, 8, 10)), Ok(()));
    assert_eq!(pool.census().frames, 1);
}

#[test]
fn the_frame_bound_is_kept() {
    let mut pool = FramePool::new(3, 1_000_000);
    for seed in 0..10_u8 {
        pool.insert(key(seed), frame(4, 4, seed)).expect("room");
        assert!(pool.census().frames <= 3, "after {seed}");
    }
    assert_eq!(pool.census().frames, 3);
    assert_eq!(pool.census().evictions, 7);
}

#[test]
fn the_byte_bound_is_kept() {
    // Frames of 64 bytes each into a pool of 200: three fit, never four.
    let mut pool = FramePool::new(1000, 200);
    for seed in 0..10_u8 {
        pool.insert(key(seed), frame(8, 8, seed)).expect("room");
        assert!(pool.census().bytes <= 200, "after {seed}");
    }
    assert_eq!(pool.census().frames, 3);
}

#[test]
fn a_frame_larger_than_the_pool_is_refused_rather_than_emptying_it() {
    let mut pool = FramePool::new(10, 100);
    pool.insert(key(1), frame(4, 4, 1)).expect("room");
    assert_eq!(
        pool.insert(key(2), frame(64, 64, 2)),
        Err(MediaStatus::FrameLargerThanPool)
    );
    assert_eq!(
        pool.census().frames,
        1,
        "the refused insert did not evict anything"
    );
}

#[test]
fn eviction_is_by_use_and_is_deterministic() {
    // Fill, then touch the oldest so it stops being the oldest, then push one
    // more. The one that goes must be the one that was not touched.
    let mut pool = FramePool::new(3, 1_000_000);
    pool.insert(key(1), frame(4, 4, 1)).expect("room");
    pool.insert(key(2), frame(4, 4, 2)).expect("room");
    pool.insert(key(3), frame(4, 4, 3)).expect("room");

    assert!(pool.get(key(1)).is_some(), "touch the oldest");
    pool.insert(key(4), frame(4, 4, 4)).expect("room");

    assert!(pool.contains(key(1)), "recently used, so it stays");
    assert!(!pool.contains(key(2)), "least recently used, so it goes");
    assert!(pool.contains(key(3)));
    assert!(pool.contains(key(4)));
}

#[test]
fn two_identical_sequences_of_use_leave_identical_pools() {
    // R-4.5: nothing about the pool may depend on iteration order or on
    // anything that differs between runs.
    fn run() -> std::vec::Vec<bool> {
        let mut pool = FramePool::new(4, 1_000_000);
        let mut present = std::vec::Vec::new();
        for seed in 0..12_u8 {
            pool.insert(key(seed), frame(4, 4, seed)).expect("room");
            if seed % 3 == 0 {
                let _ = pool.get(key(seed / 2));
            }
            present.push(pool.contains(key(seed / 2)));
        }
        for seed in 0..12_u8 {
            present.push(pool.contains(key(seed)));
        }
        present
    }
    assert_eq!(run(), run());
}

#[test]
fn clearing_returns_every_byte() {
    let mut pool = FramePool::new(8, 1_000_000);
    for seed in 0..5_u8 {
        pool.insert(key(seed), frame(8, 8, seed)).expect("room");
    }
    assert!(pool.census().bytes > 0);
    let census = pool.clear().expect("a census");
    assert_eq!(census.frames, 0);
    assert_eq!(census.bytes, 0, "the census must balance at teardown");
    assert!(pool.is_empty());
}

#[test]
fn a_pool_with_no_room_holds_nothing() {
    let mut pool = FramePool::new(0, 1_000_000);
    assert_eq!(
        pool.insert(key(1), frame(1, 1, 0)),
        Err(MediaStatus::FrameLargerThanPool)
    );
}
