// SPDX-License-Identifier: GPL-3.0-only
//! SHA-256 against the published vectors.
//!
//! An implementation of a standard is worth exactly what its vectors are
//! worth. These are the FIPS 180-4 examples plus the one-million-character
//! message, which is the one that catches a wrong length field, a wrong
//! padding boundary, and a compression function that is right for one block
//! and wrong for the next.

use media_editor_core::{Digest, Sha256};

fn hexadecimal(digest: Digest) -> std::string::String {
    std::format!("{digest}").to_lowercase()
}

#[test]
fn the_empty_message() {
    assert_eq!(
        hexadecimal(Digest::of(b"")),
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
}

#[test]
fn the_one_block_vector() {
    assert_eq!(
        hexadecimal(Digest::of(b"abc")),
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
}

#[test]
fn the_two_block_vector() {
    let message = b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    assert_eq!(
        hexadecimal(Digest::of(message)),
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );
}

#[test]
fn the_long_two_block_vector() {
    let message = b"abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn\
hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
    assert_eq!(
        hexadecimal(Digest::of(message)),
        "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"
    );
}

#[test]
fn a_million_characters() {
    let mut hasher = Sha256::new();
    let chunk = [b'a'; 1000];
    for _ in 0..1000 {
        hasher.update(&chunk);
    }
    assert_eq!(
        hexadecimal(hasher.finish()),
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"
    );
}

#[test]
fn the_result_does_not_depend_on_how_the_message_was_offered() {
    // A digest that changed with the chunking would make every cache key in
    // the media pipeline depend on how a file happened to be read (R-8.5).
    let message: std::vec::Vec<u8> = (0..=255_u8).cycle().take(4099).collect();
    let whole = Digest::of(&message);
    for chunk in [1_usize, 7, 63, 64, 65, 127, 128, 1024] {
        let mut hasher = Sha256::new();
        for piece in message.chunks(chunk) {
            hasher.update(piece);
        }
        assert_eq!(hasher.finish(), whole, "chunked by {chunk}");
    }
}

#[test]
fn a_digest_prints_the_way_phipia_pins_one() {
    assert_eq!(
        std::format!("{}", Digest::of(b"abc")),
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD"
    );
}

#[test]
fn one_changed_bit_changes_the_digest() {
    let mut message = [0_u8; 200];
    let before = Digest::of(&message);
    message[137] ^= 0x01;
    assert_ne!(Digest::of(&message), before);
}
