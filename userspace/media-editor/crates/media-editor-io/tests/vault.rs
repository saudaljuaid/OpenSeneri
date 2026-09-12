// SPDX-License-Identifier: GPL-3.0-only
//! The vault: pasting photographs and footage into a project.
//!
//! A vault is one of Phipia's files holding many pieces of material, each
//! keyed by what it is and carrying the name it arrived with — because
//! Phipia's filesystem holds sixty-four entries in a directory and cannot
//! express a name longer than eight and three, and a hundred photographs with
//! real names are neither of those things.

use media_editor_core::{Digest, Rational, Timebase};
use media_editor_io::vault::{
    ENTRY_BYTES, HEADER_BYTES, MAX_ITEMS, MAX_NAME_BYTES, MAX_PAYLOAD_BYTES, Vault, decode, encode,
};
use media_editor_io::{IoStatus, Reel, bmp, sprw};
use media_editor_media::colour::{
    ColourDescription, MatrixCoefficients, Primaries, Range, TransferFunction,
};
use media_editor_media::{Frame, FrameDescription, Geometry, PixelFormat, Plane};

const RATE: Timebase = Timebase::FILM_24;

/// The description a decoded bitmap has: eight-bit RGB, sRGB, full range.
fn described(width: u32, height: u32) -> FrameDescription {
    FrameDescription::new(
        Geometry::new(width, height).expect("a geometry"),
        PixelFormat::Rgb8,
        ColourDescription {
            primaries: Primaries::Bt709,
            transfer: TransferFunction::Srgb,
            matrix: MatrixCoefficients::Identity,
            range: Range::Full,
        },
        None,
        None,
        Rational::ONE,
    )
    .expect("a description")
}

/// A frame of `width` by `height` whose pixels vary, so a mix-up shows.
fn picture(width: u32, height: u32, tint: u8) -> Frame {
    let stride = (width as usize) * 3;
    let mut samples = std::vec::Vec::new();
    for row in 0..height as usize {
        for column in 0..width as usize {
            samples.push(u8::try_from(column % 251).expect("a byte"));
            samples.push(u8::try_from(row % 241).expect("a byte"));
            samples.push(tint);
        }
    }
    Frame::new(
        described(width, height),
        std::vec![Plane::new(samples, stride).expect("a plane")],
    )
    .expect("a frame")
}

/// One frame, packaged as the reel a vault holds.
fn reel_of(frames: std::vec::Vec<Frame>) -> std::vec::Vec<u8> {
    sprw::encode(&Reel::new(RATE, frames).expect("a reel")).expect("an encoding")
}

#[test]
fn material_goes_in_and_comes_back_by_what_it_is() {
    let mut vault = Vault::new();
    let bytes = reel_of(std::vec![picture(4, 3, 10)]);
    let digest = vault
        .insert("sunset_beach_take3.bmp", &bytes)
        .expect("room");
    assert_eq!(digest, Digest::of(&bytes), "the digest is of the material");
    assert!(vault.holds(digest));
    let item = vault.get(digest).expect("the item");
    assert_eq!(
        item.name(),
        "sunset_beach_take3.bmp",
        "the name Phipia's filesystem could not have kept"
    );
    assert_eq!(item.bytes(), bytes.as_slice());
    assert_eq!(vault.len(), 1);
}

#[test]
fn a_name_longer_than_eight_and_three_is_the_whole_point() {
    // The name above is twenty-two bytes with two dots and a mixture of case.
    // Phipia's `parse_component` refuses it three separate ways, and the vault
    // carries it exactly as typed.
    assert_eq!(
        media_editor_io::phipia::Name::new("sunset_beach_take3.bmp"),
        Err(IoStatus::NameTooLong)
    );
    let mut vault = Vault::new();
    vault
        .insert(
            "sunset_beach_take3.bmp",
            &reel_of(std::vec![picture(2, 2, 1)]),
        )
        .expect("room");
    assert_eq!(vault.items()[0].name(), "sunset_beach_take3.bmp");
}

#[test]
fn the_same_material_twice_is_refused() {
    let mut vault = Vault::new();
    let bytes = reel_of(std::vec![picture(4, 3, 10)]);
    vault.insert("first.bmp", &bytes).expect("room");
    assert_eq!(
        vault.insert("second.bmp", &bytes),
        Err(IoStatus::VaultItemTwice),
        "one piece of material under two names is still one piece"
    );
}

#[test]
fn a_name_that_is_not_text_is_refused() {
    let mut vault = Vault::new();
    let bytes = reel_of(std::vec![picture(2, 2, 3)]);
    assert_eq!(vault.insert("", &bytes), Err(IoStatus::VaultNameNotText));
    assert_eq!(
        vault.insert("bell\u{7}.bmp", &bytes),
        Err(IoStatus::VaultNameNotText)
    );
    let long = "x".repeat(MAX_NAME_BYTES + 1);
    assert_eq!(vault.insert(&long, &bytes), Err(IoStatus::VaultNameTooLong));
    // And exactly the bound is fine, which is the case a bound gets wrong.
    assert!(vault.insert(&"x".repeat(MAX_NAME_BYTES), &bytes).is_ok());
}

#[test]
fn material_comes_out_again() {
    let mut vault = Vault::new();
    let first = reel_of(std::vec![picture(4, 3, 10)]);
    let second = reel_of(std::vec![picture(4, 3, 20)]);
    let one = vault.insert("one.bmp", &first).expect("room");
    let two = vault.insert("two.bmp", &second).expect("room");
    let taken = vault.remove(one).expect("the item");
    assert_eq!(taken.name(), "one.bmp");
    assert!(!vault.holds(one));
    assert!(vault.holds(two), "taking one out did not disturb the other");
    assert_eq!(
        vault.remove(one),
        Err(IoStatus::VaultItemAbsent),
        "it came out twice"
    );
}

#[test]
fn a_vault_survives_the_file() {
    let mut vault = Vault::new();
    for tint in 0..4_u8 {
        let bytes = reel_of(std::vec![picture(8, 5, tint)]);
        vault
            .insert(&std::format!("take {tint} (final).bmp"), &bytes)
            .expect("room");
    }
    let file = encode(&vault).expect("an encoding");
    assert_eq!(decode(&file).expect("a decoding"), vault);
    // And the encoding is canonical: one vault, one file.
    assert_eq!(
        encode(&decode(&file).expect("a decoding")).expect("again"),
        file
    );
}

#[test]
fn the_file_is_the_size_the_layout_says() {
    // Derived by hand: fifty-six bytes of header, a hundred and twelve for
    // each entry, then the material end to end.
    let mut vault = Vault::new();
    let first = reel_of(std::vec![picture(4, 3, 1)]);
    let second = reel_of(std::vec![picture(6, 2, 2)]);
    vault.insert("a.bmp", &first).expect("room");
    vault.insert("b.bmp", &second).expect("room");
    let file = encode(&vault).expect("an encoding");
    assert_eq!(
        file.len(),
        HEADER_BYTES + 2 * ENTRY_BYTES + first.len() + second.len()
    );
    assert_eq!(HEADER_BYTES, 56);
    assert_eq!(ENTRY_BYTES, 112);
}

#[test]
fn a_vault_of_nothing_is_a_header_and_a_seal() {
    let file = encode(&Vault::new()).expect("an encoding");
    assert_eq!(file.len(), HEADER_BYTES);
    assert_eq!(decode(&file).expect("a decoding"), Vault::new());
}

#[test]
fn a_mangled_vault_is_refused_before_a_field_is_read() {
    let mut vault = Vault::new();
    vault
        .insert("a.bmp", &reel_of(std::vec![picture(4, 3, 1)]))
        .expect("room");
    let file = encode(&vault).expect("an encoding");
    for at in [0_usize, 4, 8, 16, 56, 100] {
        let mut broken = file.clone();
        broken[at] ^= 0xFF;
        let refusal = decode(&broken);
        assert!(
            refusal.is_err(),
            "a flipped byte at {at} was accepted: {refusal:?}"
        );
    }
    // Every byte, not only the sampled ones. A seal that covered the payload
    // and not the header would pass the sweep below at every offset above
    // fifty-six and fail at none of them.
    for at in 0..file.len() {
        let mut broken = file.clone();
        broken[at] ^= 0x01;
        assert!(
            decode(&broken).is_err(),
            "a flipped bit at {at} was accepted"
        );
    }
}

#[test]
fn a_vault_that_is_not_one_is_refused() {
    assert_eq!(decode(&[]), Err(IoStatus::NotAVault));
    assert_eq!(
        decode(b"not a vault at all, not even close, no"),
        Err(IoStatus::NotAVault)
    );
}

#[test]
fn the_bounds_are_derived_from_phipias() {
    // Two hundred and fifty-six items at a hundred and twelve bytes is 28,672
    // of index; with the fifty-six-byte header that is 28,728, and Phipia's
    // sixteen-mebibyte file leaves 16,748,488 for material.
    assert_eq!(MAX_ITEMS, 256);
    assert_eq!(HEADER_BYTES + ENTRY_BYTES * MAX_ITEMS, 28_728);
    assert_eq!(MAX_PAYLOAD_BYTES, 16_748_488);
    assert_eq!(
        HEADER_BYTES + ENTRY_BYTES * MAX_ITEMS + MAX_PAYLOAD_BYTES,
        media_editor_io::phipia::MAX_FILE_BYTES
    );
    // And a vault holds four times what one of Phipia's directories does.
    assert_eq!(
        MAX_ITEMS,
        4 * media_editor_io::phipia::MAX_DIRECTORY_ENTRIES
    );
}

#[test]
fn a_vault_full_of_material_refuses_the_next() {
    let mut vault = Vault::new();
    for index in 0..MAX_ITEMS {
        // Each piece differs, so no two collide on their digest.
        let bytes = std::format!("material number {index}").into_bytes();
        vault
            .insert(&std::format!("m{index}.bmp"), &bytes)
            .expect("room");
    }
    assert_eq!(vault.len(), MAX_ITEMS);
    assert_eq!(
        vault.insert("one.more", b"one more"),
        Err(IoStatus::VaultFull)
    );
}

#[test]
fn material_past_what_one_file_holds_is_refused() {
    let mut vault = Vault::new();
    let bytes = std::vec![7_u8; MAX_PAYLOAD_BYTES];
    vault
        .insert("whole.raw", &bytes)
        .expect("exactly the bound");
    assert_eq!(vault.payload_bytes(), MAX_PAYLOAD_BYTES);
    assert_eq!(
        vault.insert("one.more", b"x"),
        Err(IoStatus::VaultTooLarge),
        "one byte past what one of Phipia's files holds"
    );
}

#[test]
fn a_bitmap_becomes_material_and_renders() {
    use media_editor_render::Library;

    // The whole path, end to end: a 24-bit BMP of the kind Phipia's importer
    // reads, decoded, packaged as a reel, pasted into a vault, and then asked
    // for by digest exactly as a source node in the render graph asks.
    let frame = picture(8, 5, 42);
    let file = bmp::encode(&frame).expect("a bitmap");
    let decoded = bmp::decode(&file).expect("a frame");
    assert_eq!(decoded, frame, "the bitmap did not come back as it went in");

    let mut vault = Vault::new();
    let digest = vault
        .insert("beach take 3.bmp", &reel_of(std::vec![decoded]))
        .expect("room");

    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut shelf = media_editor_io::vault::Shelf::new(&vault, &looks);
    assert!(shelf.available(digest));
    assert!(!shelf.available(Digest::of(b"never pasted in")));
    let back = shelf
        .frame(digest, 0, described(8, 5))
        .expect("the frame the vault holds");
    assert_eq!(back, frame);
    assert_eq!(
        shelf.frame(digest, 1, described(8, 5)),
        Err(media_editor_render::RenderStatus::FrameAbsent),
        "a photograph is a reel of one frame and has no second one"
    );
    assert_eq!(
        shelf.frame(Digest::of(b"never pasted in"), 0, described(8, 5)),
        Err(media_editor_render::RenderStatus::MediaAbsent)
    );
}

#[test]
fn footage_is_the_same_thing_with_more_frames() {
    use media_editor_render::Library;

    let frames: std::vec::Vec<Frame> = (0..6).map(|tint| picture(4, 4, tint)).collect();
    let mut vault = Vault::new();
    let digest = vault
        .insert("a walk on the beach.spr", &reel_of(frames.clone()))
        .expect("room");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut shelf = media_editor_io::vault::Shelf::new(&vault, &looks);
    for (tick, frame) in frames.iter().enumerate() {
        assert_eq!(
            &shelf
                .frame(
                    digest,
                    i64::try_from(tick).expect("a tick"),
                    described(4, 4)
                )
                .expect("a frame"),
            frame,
            "frame {tick} is not the one that went in"
        );
    }
}

#[test]
fn material_that_cannot_be_read_says_so_rather_than_saying_it_is_missing() {
    use media_editor_render::Library;

    // Held and damaged is a different answer from not held, and telling
    // somebody the second when it is the first sends them looking for a drive.
    let mut vault = Vault::new();
    let digest = vault
        .insert("damaged.spr", b"SPRW and then nonsense")
        .expect("room");
    let looks: std::vec::Vec<(Digest, media_editor_render::Look)> = std::vec::Vec::new();
    let mut shelf = media_editor_io::vault::Shelf::new(&vault, &looks);
    assert!(shelf.available(digest), "the vault does hold it");
    assert_eq!(
        shelf.frame(digest, 0, described(4, 4)),
        Err(media_editor_render::RenderStatus::MediaUnreadable)
    );
}

/// The file with its seal recomputed.
///
/// Every check below the seal is unreachable without this, exactly as the
/// project file's tests are: a mutated byte is refused as a broken seal long
/// before the field it changed is looked at. Written out here rather than
/// borrowed from the module, so the layout is stated twice and a change to one
/// copy shows up as a disagreement.
fn resealed(mut file: std::vec::Vec<u8>) -> std::vec::Vec<u8> {
    let mut hasher = media_editor_core::Sha256::new();
    hasher.update(&file[..24]);
    hasher.update(&file[HEADER_BYTES..]);
    let sealed = hasher.finish();
    file[24..HEADER_BYTES].copy_from_slice(sealed.bytes());
    file
}

/// A vault of two pieces of material, for the hostile-file cases below.
fn two() -> (Vault, std::vec::Vec<u8>) {
    let mut vault = Vault::new();
    vault
        .insert("first take.bmp", &reel_of(std::vec![picture(4, 3, 1)]))
        .expect("room");
    vault
        .insert("second take.bmp", &reel_of(std::vec![picture(4, 3, 2)]))
        .expect("room");
    let file = encode(&vault).expect("an encoding");
    (vault, file)
}

#[test]
fn a_file_whose_name_is_edited_is_refused() {
    // The one field nothing but the seal checks. A name of the same length
    // made of different printable bytes is legal in every other way, so this
    // is what says the seal covers the index rather than only the material.
    let (_, file) = two();
    let at = file
        .windows(5)
        .position(|window| window == b"first")
        .expect("the name is in the file");
    let mut edited = file.clone();
    edited[at] = b'w';
    assert_eq!(decode(&edited), Err(IoStatus::DigestMismatch));
    // And with the seal recomputed it loads, which is what proves the refusal
    // above was the seal and not something else objecting to a `w`.
    let renamed = decode(&resealed(edited)).expect("a decoding");
    assert_eq!(renamed.items()[0].name(), "wirst take.bmp");
}

#[test]
fn a_file_whose_spans_leave_a_gap_is_refused() {
    // The second entry's offset, moved on by one. The material is still all
    // there and still hashes correctly; what is wrong is that the file now
    // says there is a byte between the two pieces that nothing accounts for.
    let (vault, file) = two();
    let first = vault.items()[0].bytes().len();
    let at = HEADER_BYTES + ENTRY_BYTES + 32;
    assert_eq!(
        &file[at..at + 8],
        &u64::try_from(first).expect("a length").to_le_bytes(),
        "the second entry's offset is where the layout says"
    );
    let mut gapped = file.clone();
    gapped[at..at + 8].copy_from_slice(&u64::try_from(first + 1).expect("a length").to_le_bytes());
    assert_eq!(
        decode(&resealed(gapped)),
        Err(IoStatus::VaultSpanNotContiguous)
    );
}

#[test]
fn a_file_whose_material_is_not_what_it_says_is_refused() {
    // One byte of the payload changed. The digest in the index is recomputed
    // from the bytes rather than believed, which is what turns a bad cluster
    // on an unjournaled filesystem into a named refusal instead of a picture
    // filed under somebody else's name.
    let (_, file) = two();
    let mut damaged = file.clone();
    let at = HEADER_BYTES + 2 * ENTRY_BYTES + 40;
    damaged[at] ^= 0xFF;
    assert_eq!(
        decode(&resealed(damaged)),
        Err(IoStatus::VaultItemDigestMismatch)
    );
}

#[test]
fn a_file_claiming_more_material_than_the_bound_is_refused() {
    // The count, raised past what a vault holds. Refused on the count itself
    // rather than after reading two hundred and fifty-seven entries out of a
    // file that has two -- a bound a hostile file can talk its way past is not
    // a bound (R-11.2).
    let (_, file) = two();
    let mut many = file.clone();
    many[8..12].copy_from_slice(&u32::try_from(MAX_ITEMS + 1).expect("a count").to_le_bytes());
    assert_eq!(decode(&resealed(many)), Err(IoStatus::TooMany));
}

#[test]
fn a_file_whose_name_field_has_a_tail_is_refused() {
    // The bytes past a name's length are zero, so one vault has exactly one
    // file. A file carrying anything there is a second encoding of the same
    // vault, and two files that decode alike but differ byte for byte are the
    // end of a canonical format.
    let (_, file) = two();
    let mut tailed = file.clone();
    // The first entry's name field, one byte past the end of "first take.bmp".
    let at = HEADER_BYTES + 50 + "first take.bmp".len();
    assert_eq!(tailed[at], 0, "the tail is zero to begin with");
    tailed[at] = b'!';
    assert_eq!(decode(&resealed(tailed)), Err(IoStatus::ReservedFieldSet));
}

#[test]
fn a_file_whose_payload_disagrees_with_its_spans_is_refused() {
    // The header's payload length, raised by one. Every entry still runs end
    // to end, so the gap check passes and the last one falls short of what the
    // header claims -- which is the other half of the same invariant.
    let (vault, file) = two();
    let mut longer = file.clone();
    let payload = u64::try_from(vault.payload_bytes()).expect("a length");
    longer[16..24].copy_from_slice(&(payload + 1).to_le_bytes());
    longer.push(0);
    assert_eq!(
        decode(&resealed(longer)),
        Err(IoStatus::VaultSpanNotContiguous)
    );
}
