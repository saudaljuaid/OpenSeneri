// SPDX-License-Identifier: GPL-3.0-only
//! Frames: what can be described, and what cannot.

use media_editor_core::Rational;
use media_editor_media::{
    AlphaState, ChromaSiting, ColourDescription, Frame, FrameDescription, Geometry,
    MatrixCoefficients, MediaStatus, PixelFormat, Plane, Primaries, Range, TransferFunction,
};

const ALL_FORMATS: [PixelFormat; 6] = [
    PixelFormat::Rgba8,
    PixelFormat::Rgb8,
    PixelFormat::Gray8,
    PixelFormat::Yuv420p8,
    PixelFormat::Yuv422p8,
    PixelFormat::Yuv444p8,
];

fn geometry(width: u32, height: u32) -> Geometry {
    Geometry::new(width, height).expect("a usable geometry")
}

/// The alpha tag a format demands: present exactly when the format has alpha.
fn alpha_for(format: PixelFormat) -> Option<AlphaState> {
    if format.has_alpha() {
        Some(AlphaState::Straight)
    } else {
        None
    }
}

fn rgb_description(width: u32, height: u32) -> FrameDescription {
    FrameDescription::square(
        geometry(width, height),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description")
}

fn yuv_description(width: u32, height: u32, format: PixelFormat) -> FrameDescription {
    let siting = if format.is_subsampled() {
        Some(ChromaSiting::Left)
    } else {
        None
    };
    FrameDescription::square(
        geometry(width, height),
        format,
        ColourDescription::bt709_limited(),
        siting,
        None,
    )
    .expect("a description")
}

#[test]
fn a_zero_dimension_is_not_a_geometry() {
    assert_eq!(Geometry::new(0, 10), Err(MediaStatus::EmptyGeometry));
    assert_eq!(Geometry::new(10, 0), Err(MediaStatus::EmptyGeometry));
    assert_eq!(
        Geometry::new(16_385, 10),
        Err(MediaStatus::GeometryTooLarge)
    );
    assert!(Geometry::new(16_384, 1).is_ok());
}

#[test]
fn a_subsampled_format_refuses_a_dimension_it_cannot_halve() {
    assert_eq!(
        PixelFormat::Yuv420p8.accepts(geometry(11, 10)),
        Err(MediaStatus::OddDimension)
    );
    assert_eq!(
        PixelFormat::Yuv420p8.accepts(geometry(10, 11)),
        Err(MediaStatus::OddDimension)
    );
    assert_eq!(
        PixelFormat::Yuv422p8.accepts(geometry(11, 11)),
        Err(MediaStatus::OddDimension),
        "4:2:2 halves the width only, so an odd width is what it refuses"
    );
    assert!(
        PixelFormat::Yuv422p8.accepts(geometry(10, 11)).is_ok(),
        "an odd height is fine at 4:2:2"
    );
    assert!(PixelFormat::Yuv444p8.accepts(geometry(11, 11)).is_ok());
    assert!(PixelFormat::Rgba8.accepts(geometry(11, 11)).is_ok());
}

#[test]
fn plane_arithmetic_agrees_with_itself_at_every_size() {
    // The place picture bugs live. Checked over every dimension in a range
    // rather than over three examples, because an off-by-one in a chroma
    // plane's height is a green band at the bottom of an export.
    for format in ALL_FORMATS {
        for width in 1..=48_u32 {
            for height in 1..=48_u32 {
                let shape = geometry(width, height);
                let Ok(total) = format.packed_frame_bytes(shape) else {
                    assert!(
                        format.is_subsampled(),
                        "only a subsampled format may refuse a size"
                    );
                    continue;
                };
                let mut summed = 0_usize;
                for plane in 0..format.plane_count() {
                    let plane_shape = format
                        .plane_geometry(shape, plane)
                        .expect("a plane geometry");
                    let row = format.plane_row_bytes(shape, plane).expect("a row");
                    summed += row * plane_shape.height() as usize;
                }
                assert_eq!(total, summed, "{format:?} at {width}x{height}");
            }
        }
    }
}

#[test]
fn packed_sizes_are_what_the_formats_say_they_are() {
    let shape = geometry(64, 32);
    assert_eq!(
        PixelFormat::Rgba8.packed_frame_bytes(shape),
        Ok(64 * 32 * 4)
    );
    assert_eq!(PixelFormat::Rgb8.packed_frame_bytes(shape), Ok(64 * 32 * 3));
    assert_eq!(PixelFormat::Gray8.packed_frame_bytes(shape), Ok(64 * 32));
    assert_eq!(
        PixelFormat::Yuv444p8.packed_frame_bytes(shape),
        Ok(64 * 32 * 3)
    );
    assert_eq!(
        PixelFormat::Yuv422p8.packed_frame_bytes(shape),
        Ok(64 * 32 + 2 * (32 * 32)),
        "chroma is half as wide"
    );
    assert_eq!(
        PixelFormat::Yuv420p8.packed_frame_bytes(shape),
        Ok(64 * 32 + 2 * (32 * 16)),
        "chroma is half as wide and half as tall"
    );
}

#[test]
fn a_description_without_a_siting_is_refused_for_a_subsampled_format() {
    assert_eq!(
        FrameDescription::square(
            geometry(16, 16),
            PixelFormat::Yuv420p8,
            ColourDescription::bt709_limited(),
            None,
            None,
        ),
        Err(MediaStatus::SitingMismatch)
    );
    assert_eq!(
        FrameDescription::square(
            geometry(16, 16),
            PixelFormat::Rgba8,
            ColourDescription::srgb_full(),
            Some(ChromaSiting::Left),
            Some(AlphaState::Straight),
        ),
        Err(MediaStatus::SitingMismatch),
        "a format with no chroma plane has nowhere to site it"
    );
}

#[test]
fn a_matrix_that_disagrees_with_the_format_is_refused() {
    assert_eq!(
        FrameDescription::square(
            geometry(16, 16),
            PixelFormat::Rgba8,
            ColourDescription::bt709_limited(),
            None,
            Some(AlphaState::Straight),
        ),
        Err(MediaStatus::MatrixMismatch),
        "these samples are red, green and blue, so no luma matrix applies"
    );
    assert_eq!(
        FrameDescription::square(
            geometry(16, 16),
            PixelFormat::Yuv444p8,
            ColourDescription::srgb_full(),
            None,
            None,
        ),
        Err(MediaStatus::MatrixMismatch),
        "and these are not"
    );
}

#[test]
fn a_pixel_aspect_must_be_positive() {
    assert_eq!(
        FrameDescription::new(
            geometry(16, 16),
            PixelFormat::Rgba8,
            ColourDescription::srgb_full(),
            None,
            Some(AlphaState::Straight),
            Rational::ZERO,
        ),
        Err(MediaStatus::BadPixelAspect)
    );
    // Anamorphic material is ordinary: 720x576 at 16:11 is PAL widescreen.
    assert!(
        FrameDescription::new(
            geometry(720, 576),
            PixelFormat::Rgba8,
            ColourDescription::srgb_full(),
            None,
            Some(AlphaState::Straight),
            Rational::new(16, 11).expect("a ratio"),
        )
        .is_ok()
    );
}

#[test]
fn a_frame_round_trips_through_packed_bytes() {
    for format in ALL_FORMATS {
        let description = if format.is_rgb() {
            FrameDescription::square(
                geometry(12, 8),
                format,
                ColourDescription::srgb_full(),
                None,
                alpha_for(format),
            )
            .expect("a description")
        } else {
            yuv_description(12, 8, format)
        };
        let bytes: std::vec::Vec<u8> = (0..description.packed_bytes().expect("a size"))
            .map(|index| u8::try_from(index % 251).unwrap_or(0))
            .collect();
        let frame = Frame::from_packed(description, &bytes).expect("a frame");
        assert_eq!(
            frame.to_packed().expect("packed bytes"),
            bytes,
            "{format:?}"
        );
        assert_eq!(frame.bytes(), bytes.len());
    }
}

#[test]
fn the_wrong_number_of_bytes_is_refused() {
    let description = rgb_description(4, 4);
    let short = std::vec![0_u8; description.packed_bytes().expect("a size") - 1];
    assert_eq!(
        Frame::from_packed(description, &short),
        Err(MediaStatus::PlaneSizeMismatch)
    );
    let long = std::vec![0_u8; description.packed_bytes().expect("a size") + 1];
    assert_eq!(
        Frame::from_packed(description, &long),
        Err(MediaStatus::PlaneSizeMismatch)
    );
}

#[test]
fn the_wrong_number_of_planes_is_refused() {
    let description = yuv_description(8, 8, PixelFormat::Yuv420p8);
    let plane = Plane::new(std::vec![0_u8; 64], 8).expect("a plane");
    assert_eq!(
        Frame::new(description, std::vec![plane]),
        Err(MediaStatus::PlaneCountMismatch)
    );
}

#[test]
fn a_stride_narrower_than_a_row_is_refused() {
    let description = rgb_description(4, 4);
    let plane = Plane::new(std::vec![0_u8; 4 * 4], 4).expect("a plane");
    assert_eq!(
        Frame::new(description, std::vec![plane]),
        Err(MediaStatus::StrideTooNarrow),
        "four pixels of RGBA is sixteen bytes, not four"
    );
}

#[test]
fn padding_between_rows_is_not_part_of_the_picture() {
    // Two frames with the same pixels and different row padding are the same
    // picture, and a cache keyed by digest must treat them as one.
    let description = rgb_description(3, 2);
    let row = 3 * 4;
    let tight: std::vec::Vec<u8> = (0..row * 2)
        .map(|index| u8::try_from(index % 256).unwrap_or(0))
        .collect();
    let packed = Frame::from_packed(description, &tight).expect("a frame");

    let mut padded_samples = std::vec::Vec::new();
    for line in 0..2 {
        padded_samples.extend_from_slice(&tight[line * row..(line + 1) * row]);
        padded_samples.extend_from_slice(&[0xAA; 8]);
    }
    let padded = Frame::new(
        description,
        std::vec![Plane::new(padded_samples, row + 8).expect("a plane")],
    )
    .expect("a frame");

    assert_eq!(padded.digest(), packed.digest());
    assert_eq!(padded.to_packed().expect("bytes"), tight);
    assert_ne!(padded.bytes(), packed.bytes(), "it does occupy more memory");
}

#[test]
fn the_description_is_part_of_the_digest() {
    // The same bytes meaning different things are different frames. Without
    // this, a cache would hand a Rec. 709 frame to something that asked for
    // sRGB and the difference would reach the export.
    let bytes = std::vec![128_u8; 4 * 4 * 4];
    let srgb = Frame::from_packed(rgb_description(4, 4), &bytes).expect("a frame");

    let other = FrameDescription::square(
        geometry(4, 4),
        PixelFormat::Rgba8,
        ColourDescription::new(
            Primaries::Bt2020,
            TransferFunction::PerceptualQuantiser,
            MatrixCoefficients::Identity,
            Range::Full,
        ),
        None,
        Some(AlphaState::Straight),
    )
    .expect("a description");
    let wide = Frame::from_packed(other, &bytes).expect("a frame");

    assert_ne!(srgb.digest(), wide.digest());
}

#[test]
fn the_same_picture_always_has_the_same_digest() {
    let bytes: std::vec::Vec<u8> = (0..4 * 4 * 4)
        .map(|index: usize| u8::try_from(index * 7 % 256).unwrap_or(0))
        .collect();
    let first = Frame::from_packed(rgb_description(4, 4), &bytes).expect("a frame");
    let second = Frame::from_packed(rgb_description(4, 4), &bytes).expect("a frame");
    assert_eq!(first.digest(), second.digest());
    assert_eq!(first, second);
}

#[test]
fn one_changed_sample_changes_the_digest() {
    let description = rgb_description(4, 4);
    let mut bytes = std::vec![0_u8; description.packed_bytes().expect("a size")];
    let before = Frame::from_packed(description, &bytes).expect("a frame");
    for index in 0..bytes.len() {
        bytes[index] ^= 0x01;
        let after = Frame::from_packed(description, &bytes).expect("a frame");
        assert_ne!(after.digest(), before.digest(), "sample {index}");
        bytes[index] ^= 0x01;
    }
}

#[test]
fn a_blank_frame_is_black_rather_than_full_of_zeroes() {
    // Zero is not black, and this test exists because it used to be treated as
    // though it were. In a limited-range luma plane zero sits *below* the legal
    // floor of sixteen; in a chroma plane zero is not neutral but the most
    // negative value the byte can hold, which is a saturated blue-green. A
    // blank frame filled with zeroes therefore showed up on a vectorscope in
    // the corner of the graticule instead of at the origin — which is how it
    // was found.
    //
    // A blank frame is an opaque black slug, legal in its own range.
    let full = Frame::blank(rgb_description(8, 8)).expect("a frame");
    for pixel in full.to_packed().expect("bytes").chunks_exact(4) {
        assert_eq!(pixel, &[0, 0, 0, 255], "full-range black, opaque");
    }

    let limited = FrameDescription::square(
        geometry(8, 8),
        PixelFormat::Yuv444p8,
        ColourDescription::bt709_limited(),
        None,
        None,
    )
    .expect("a description");
    let frame = Frame::blank(limited).expect("a frame");
    let planes = frame.planes();
    assert!(
        planes[0].samples().iter().all(|sample| *sample == 16),
        "limited-range black is 16; zero is an illegal sample"
    );
    for plane in &planes[1..] {
        assert!(
            plane.samples().iter().all(|sample| *sample == 128),
            "neutral chroma is 128 in either range"
        );
    }

    // And a limited-range single-channel frame gets the same floor.
    let grey = FrameDescription::square(
        geometry(8, 8),
        PixelFormat::Gray8,
        ColourDescription::new(
            Primaries::Bt709,
            TransferFunction::Bt709,
            MatrixCoefficients::Identity,
            Range::Limited,
        ),
        None,
        None,
    )
    .expect("a description");
    assert!(
        Frame::blank(grey)
            .expect("a frame")
            .to_packed()
            .expect("bytes")
            .iter()
            .all(|sample| *sample == 16)
    );
}

#[test]
fn an_alpha_association_is_demanded_exactly_where_there_is_alpha() {
    // Straight and premultiplied samples are different numbers meaning the
    // same picture, and the `over` operator is only correct on one of them.
    // A frame that does not say which it holds is the dark fringe waiting to
    // happen (R-8.2), so it is not a description this program can build.
    assert_eq!(
        FrameDescription::square(
            geometry(16, 16),
            PixelFormat::Rgba8,
            ColourDescription::srgb_full(),
            None,
            None,
        ),
        Err(MediaStatus::AlphaMismatch),
        "these samples have an alpha channel and no statement about it"
    );
    for state in [AlphaState::Straight, AlphaState::Premultiplied] {
        assert_eq!(
            FrameDescription::square(
                geometry(16, 16),
                PixelFormat::Rgb8,
                ColourDescription::srgb_full(),
                None,
                Some(state),
            ),
            Err(MediaStatus::AlphaMismatch),
            "there is no alpha channel here to be {state:?} with respect to"
        );
    }
    for format in ALL_FORMATS {
        let siting = if format.is_subsampled() {
            Some(ChromaSiting::Left)
        } else {
            None
        };
        let colour = if format.is_rgb() {
            ColourDescription::srgb_full()
        } else {
            ColourDescription::bt709_limited()
        };
        let described =
            FrameDescription::square(geometry(16, 16), format, colour, siting, alpha_for(format))
                .expect("a description");
        assert_eq!(
            described.alpha().is_some(),
            format.has_alpha(),
            "{format:?} must say exactly as much about alpha as it has"
        );
    }
}

#[test]
fn the_two_alpha_associations_are_different_frames() {
    // Same bytes, different meaning: a cache that ignored this would hand
    // premultiplied samples to something expecting straight ones, and the
    // difference would reach the export.
    let straight = rgb_description(4, 4);
    let premultiplied = straight
        .with_alpha(AlphaState::Premultiplied)
        .expect("a description");
    assert_ne!(straight, premultiplied);

    let bytes = std::vec![128_u8; 4 * 4 * 4];
    let one = Frame::from_packed(straight, &bytes).expect("a frame");
    let other = Frame::from_packed(premultiplied, &bytes).expect("a frame");
    assert_eq!(one.to_packed(), other.to_packed(), "the samples are equal");
    assert_ne!(one.digest(), other.digest(), "the frames are not");
}

#[test]
fn a_format_without_alpha_cannot_be_given_an_association() {
    let described = FrameDescription::square(
        geometry(4, 4),
        PixelFormat::Rgb8,
        ColourDescription::srgb_full(),
        None,
        None,
    )
    .expect("a description");
    assert_eq!(
        described.with_alpha(AlphaState::Premultiplied),
        Err(MediaStatus::AlphaMismatch)
    );
}

#[test]
fn a_buffer_wrapped_as_a_frame_and_taken_back_is_the_same_allocation() {
    // The property the whole row path turns on, and the only way to state it
    // that a test can check: not "it is fast" but *the bytes never moved*. A
    // window filled from storage becomes a frame and becomes a window again,
    // and the address is the same address, so nothing was copied and nothing
    // was allocated. Compare the contents instead and a copy would pass.
    let description = rgb_description(4, 3);
    let bytes = description.packed_bytes().expect("a size");
    let window: std::vec::Vec<u8> = (0..bytes)
        .map(|index| u8::try_from(index % 251).unwrap_or(0))
        .collect();
    let expected = window.clone();
    let address = window.as_ptr();

    let frame = Frame::from_owned(description, window).expect("a frame");
    assert!(frame.is_packed(), "an interleaved frame is one packed run");
    assert_eq!(
        frame.packed().expect("bytes").as_ptr(),
        address,
        "lending the bytes copied them"
    );

    let back = frame.into_packed().expect("bytes");
    assert_eq!(back.as_ptr(), address, "taking the buffer back copied it");
    assert_eq!(back, expected, "and it is still the picture");
}

#[test]
fn a_planar_frame_packs_rather_than_lends_and_says_so_first() {
    // Three planes cannot be one slice, whatever anybody wants: they are
    // three allocations. So this is the case that copies, and `is_packed`
    // says so *before* the copy rather than after -- which is what lets a
    // caller on a small machine decide instead of discover.
    let description = yuv_description(4, 4, PixelFormat::Yuv420p8);
    let bytes = description.packed_bytes().expect("a size");
    let tight: std::vec::Vec<u8> = (0..bytes)
        .map(|index| u8::try_from(index % 241).unwrap_or(0))
        .collect();
    let frame = Frame::from_packed(description, &tight).expect("a frame");
    assert!(!frame.is_packed(), "a planar frame is not one run");
    assert_eq!(frame.packed().expect("bytes").as_ref(), &tight[..]);
    assert_eq!(frame.clone().into_packed().expect("bytes"), tight);
    // And `from_owned` takes the same buffer and copies out of it, which is
    // the same frame `from_packed` builds -- the difference is cost, not
    // answer, and a milestone that changed the answer would be a bug.
    assert_eq!(
        Frame::from_owned(description, tight.clone()).expect("a frame"),
        frame
    );
}

#[test]
fn a_padded_frame_packs_rather_than_lends() {
    // Padding between rows is not part of the picture, so a padded frame's
    // samples are not the picture's bytes and cannot be lent as them. One
    // plane is not enough; the stride has to be the row.
    let description = rgb_description(3, 2);
    let row = 3 * 4;
    let tight: std::vec::Vec<u8> = (0..row * 2)
        .map(|index| u8::try_from(index % 256).unwrap_or(0))
        .collect();
    let mut padded_samples = std::vec::Vec::new();
    for line in 0..2 {
        padded_samples.extend_from_slice(&tight[line * row..(line + 1) * row]);
        padded_samples.extend_from_slice(&[0xAA; 8]);
    }
    let padded = Frame::new(
        description,
        std::vec![Plane::new(padded_samples, row + 8).expect("a plane")],
    )
    .expect("a frame");
    assert!(!padded.is_packed());
    assert_eq!(padded.packed().expect("bytes").as_ref(), &tight[..]);
    assert_eq!(padded.into_packed().expect("bytes"), tight);
}

#[test]
fn a_buffer_of_the_wrong_size_is_not_taken() {
    // A frame that took a short buffer would be a frame whose rows ran off the
    // end of it. `from_owned` makes no check of its own: `Frame::new` compares
    // the plane's length against the geometry and refuses with the same
    // status, and a control showed the earlier check changed no answer. What
    // this test holds is the refusal, not where it comes from.
    let description = rgb_description(4, 3);
    let bytes = description.packed_bytes().expect("a size");
    for length in [bytes - 1, bytes + 1, 0] {
        assert_eq!(
            Frame::from_owned(description, std::vec![0; length]).err(),
            Some(MediaStatus::PlaneSizeMismatch),
            "a buffer of {length} bytes was taken for a frame of {bytes}"
        );
    }
}
