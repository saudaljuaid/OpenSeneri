// SPDX-License-Identifier: GPL-3.0-only
//! A caption sidecar, against the numbers a person works out by hand.
//!
//! Every timestamp asserted here is derived in its own comment from the
//! definition — `round(ticks × 1000 ÷ rate)` — and never read back out of the
//! code. That is the whole point of a golden test for a format: a file this
//! program writes and only this program reads is a file nobody has checked.

use media_editor_core::{Instant, Timebase};
use media_editor_io::status::IoStatus;
use media_editor_io::vtt::{MAGIC, MAX_CUE_BYTES, Spotter, milliseconds, sidecar_bytes};
use media_editor_model::caption::Caption;

/// 29.97: the rate every rounding question is really about.
const NTSC: Timebase = Timebase::NTSC_30;

fn said(from: i64, to: i64, voice: u8, text: &str) -> Caption {
    Caption::new(from, to, voice, text).expect("a caption")
}

fn written(captions: &[(Caption, Timebase)]) -> std::vec::Vec<u8> {
    let mut out = std::vec::Vec::new();
    let mut spotter = Spotter::begin(&mut out).expect("a signature");
    for (caption, timebase) in captions {
        spotter.cue(&mut out, caption, *timebase).expect("a cue");
    }
    out
}

#[test]
fn a_millisecond_is_the_moment_the_frame_is_shown() {
    // At 30000/1001 a tick is 1001/30000 of a second, so tick t is
    // t × 1001 ÷ 30 milliseconds. Every number below is that division, done
    // by hand.
    //
    //   t =  0 :        0 ÷ 30 =    0          exactly
    //   t =  1 :     1001 ÷ 30 =   33.3666...  -> 33
    //   t = 15 :    15015 ÷ 30 =  500.5        -> 501, a tie, rounded up
    //   t = 29 :    29029 ÷ 30 =  967.6333...  -> 968
    //   t = 30 :    30030 ÷ 30 = 1001          exactly
    //   t = 60 :    60060 ÷ 30 = 2002          exactly
    for (tick, expected) in [
        (0, 0),
        (1, 33),
        (15, 501),
        (29, 968),
        (30, 1001),
        (60, 2002),
    ] {
        assert_eq!(
            milliseconds(Instant::new(tick, NTSC)).expect("a millisecond"),
            expected,
            "tick {tick} at 30000/1001"
        );
    }

    // At 24 a tick is 1000 ÷ 24 = 125/3 milliseconds.
    //
    //   t =  1 :  125 ÷ 3 =   41.666... -> 42
    //   t =  3 :  375 ÷ 3 =  125        exactly
    //   t = 12 : 1500 ÷ 3 =  500        exactly
    //   t = 24 : 3000 ÷ 3 = 1000        exactly
    for (tick, expected) in [(1, 42), (3, 125), (12, 500), (24, 1000)] {
        assert_eq!(
            milliseconds(Instant::new(tick, Timebase::FILM_24)).expect("a millisecond"),
            expected,
            "tick {tick} at 24"
        );
    }
}

#[test]
fn a_sidecar_timed_from_timecode_would_drift_a_part_in_a_thousand() {
    // The reason this module computes wall clock rather than reusing
    // `Timecode`. Non-drop timecode labels frame n as n ÷ 30 seconds; the
    // frame is actually shown at n × 1001 ÷ 30000 seconds. Thirty thousand
    // frames makes both divisions exact, so the gap is visible with no
    // rounding in the way:
    //
    //   shown at : 30,000 × 1001 ÷ 30 = 1,001,000 ms
    //   labelled : 30,000 × 1000 ÷ 30 = 1,000,000 ms
    //
    // A thousand milliseconds in a thousand seconds -- one part in a
    // thousand, always in the same direction. Over an hour that is 3.6
    // seconds, which is a line of dialogue.
    let shown = milliseconds(Instant::new(30_000, NTSC)).expect("a millisecond");
    let labelled = milliseconds(Instant::new(30_000, Timebase::TELEVISION_30)).expect("a label");
    assert_eq!(shown, 1_001_000);
    assert_eq!(labelled, 1_000_000);
    assert_eq!(shown - labelled, 1_000, "one part in a thousand");

    // And at an hour, where the numbers stop being round -- which is itself
    // the fact drop-frame timecode exists for. An hour of 29.97 is
    // 30000 × 3600 ÷ 1001 = 108,000,000 ÷ 1001 frames, and that is **not a
    // whole number**: no frame at this rate falls exactly on the hour.
    //
    //   frame 107,892 : 107,892 × 1001 ÷ 30 = 3,599,996.4 -> 3,599,996 ms
    //   its label     : 107,892 × 1000 ÷ 30 = 3,596,400   exactly
    //
    // Three and a half seconds apart, four milliseconds short of the hour.
    let rate = NTSC.rate();
    let exact = 3600 * rate.numerator();
    let near = exact / rate.denominator();
    assert_eq!(near, 107_892, "frames in an hour of 29.97, rounded down");
    assert_eq!(
        exact % rate.denominator(),
        108,
        "an hour is not a whole number of frames at this rate"
    );
    // And the leftover is not a curiosity. Non-drop timecode labels 108,000
    // frames as one hour, so it counts 108 frames the hour does not have --
    // which is exactly how many labels drop-frame skips per hour: two a
    // minute except every tenth, 2 x 60 - 2 x 6.
    assert_eq!(108_000 - near, 108);
    assert_eq!(2 * 60 - 2 * 6, 108);
    assert_eq!(
        milliseconds(Instant::new(near, NTSC)).expect("a millisecond"),
        3_599_996
    );
    assert_eq!(
        milliseconds(Instant::new(near, Timebase::TELEVISION_30)).expect("a label"),
        3_596_400
    );
}

#[test]
fn a_written_sidecar_is_the_file_a_person_would_write_by_hand() {
    // Two cues at 29.97, with the timestamps from the first test:
    //   0 -> 0.000, 15 -> 0.501, 30 -> 1.001, 60 -> 2.002
    // and the second cue's text carrying all three characters that are
    // markup in a WebVTT payload.
    let file = written(&[
        (said(0, 15, 0, "Hello"), NTSC),
        (said(30, 60, 3, "R&D <5>"), NTSC),
    ]);
    let expected = "WEBVTT\n\
                    \n\
                    1\n\
                    00:00:00.000 --> 00:00:00.501\n\
                    <v Voice 0>Hello</v>\n\
                    \n\
                    2\n\
                    00:00:01.001 --> 00:00:02.002\n\
                    <v Voice 3>R&amp;D &lt;5&gt;</v>\n\
                    \n";
    assert_eq!(
        core::str::from_utf8(&file).expect("text"),
        expected,
        "the sidecar is not the file it should be"
    );
    assert!(file.starts_with(MAGIC));
}

#[test]
fn a_long_programme_writes_the_hours_it_needs() {
    // WebVTT's hours field is not two digits, it is *at least* two, and a
    // writer that wrapped at a hundred would put the end of a long programme
    // on top of its beginning.
    //
    // At 24, one hour is 86,400 ticks and 3,600,000 ms. A hundred hours is
    // 8,640,000 ticks and 360,000,000 ms:
    //
    //   360,000,000 ÷ 3,600,000 = 100 hours, 0 minutes, 0 seconds, 0 ms
    let file = written(&[(
        said(8_640_000, 8_640_000 + 24, 0, "still here"),
        Timebase::FILM_24,
    )]);
    let text = core::str::from_utf8(&file).expect("text");
    assert!(
        text.contains("100:00:00.000 --> 100:00:01.000"),
        "the hours wrapped: {text}"
    );
}

#[test]
fn two_lines_are_a_caption_and_a_blank_line_is_not() {
    // One newline is what a caption normally is -- broadcast practice is two
    // lines. A blank line is what ends a cue block, so text holding one would
    // silently become two cues, the second of them malformed.
    let file = written(&[(said(0, 24, 0, "first line\nsecond line"), Timebase::FILM_24)]);
    let text = core::str::from_utf8(&file).expect("text");
    assert!(text.contains("<v Voice 0>first line\nsecond line</v>"));

    let mut out = std::vec::Vec::new();
    let mut spotter = Spotter::begin(&mut out).expect("a signature");
    for (text, expected) in [
        ("a\n\nb", IoStatus::CueTextNotOneBlock),
        ("a\rb", IoStatus::CueTextNotOneBlock),
        ("a\r\n\r\nb", IoStatus::CueTextNotOneBlock),
        ("time --> place", IoStatus::CueTextLooksLikeATiming),
        ("", IoStatus::EmptyCue),
    ] {
        assert_eq!(
            spotter.cue(&mut out, &said(0, 24, 0, text), Timebase::FILM_24),
            Err(expected),
            "text {text:?}"
        );
    }
    // And nothing was written for any of them: the file is still the
    // signature and the one good cue.
    assert_eq!(out.len(), MAGIC.len());
}

#[test]
fn a_cue_shorter_than_the_unit_it_is_written_in_is_refused() {
    // The one thing rounding costs. A tick of 48 kHz is 1/48 of a
    // millisecond, so a caption one tick long rounds to no duration at all:
    //
    //   ms(0) = 0
    //   ms(1) = round(1000 ÷ 48000) = round(0.0208...) = 0
    //
    // A cue of no duration is a cue nobody sees, and a file nobody can tell
    // is wrong -- so it refuses rather than being written (R-1.3).
    let fast = Timebase::AUDIO_48K;
    assert_eq!(
        milliseconds(Instant::new(1, fast)).expect("a millisecond"),
        0
    );
    let mut out = std::vec::Vec::new();
    let mut spotter = Spotter::begin(&mut out).expect("a signature");
    assert_eq!(
        spotter.cue(&mut out, &said(0, 1, 0, "too quick"), fast),
        Err(IoStatus::CueVanishes)
    );
    // Twenty-four ticks is half a millisecond, which still vanishes; the
    // first length that survives is twenty-five.
    //
    //   ms(24) = round(24000 ÷ 48000) = round(0.5) = 1  -- a tie, rounded up
    //   ms(25) = round(25000 ÷ 48000) = round(0.5208) = 1
    assert_eq!(
        milliseconds(Instant::new(24, fast)).expect("a millisecond"),
        1,
        "the tie rounds up, so twenty-four ticks is already a millisecond"
    );
    spotter
        .cue(&mut out, &said(0, 24, 0, "just long enough"), fast)
        .expect("a cue");
    assert_eq!(spotter.count(), 1);
}

#[test]
fn cues_come_out_in_order_or_not_at_all() {
    // WebVTT cues are read in the order they are written, so a writer that
    // took them out of order would produce a file whose captions arrive
    // backwards -- and the refusal is about the caller, not about the
    // arithmetic: rounding is monotone, so two captions in order in ticks are
    // in order in milliseconds and cannot be reordered by this.
    let mut out = std::vec::Vec::new();
    let mut spotter = Spotter::begin(&mut out).expect("a signature");
    spotter
        .cue(&mut out, &said(48, 72, 0, "second"), Timebase::FILM_24)
        .expect("a cue");
    assert_eq!(
        spotter.cue(&mut out, &said(0, 24, 0, "first"), Timebase::FILM_24),
        Err(IoStatus::CueOutOfOrder)
    );
    // A cue starting before the file does is the same refusal, reached from
    // the other side: the sidecar's nought is the programme's start.
    assert_eq!(
        spotter.cue(&mut out, &said(-10, 24, 0, "before"), Timebase::FILM_24),
        Err(IoStatus::CueOutOfOrder)
    );
    // Two cues starting at the same millisecond are *not* out of order: two
    // voices talking at once is a conversation, and it is the case a single
    // sorted list gets wrong.
    spotter
        .cue(&mut out, &said(48, 96, 1, "at once"), Timebase::FILM_24)
        .expect("two voices at one moment");
    assert_eq!(spotter.count(), 2);
}

#[test]
fn a_signature_belongs_at_the_beginning() {
    let mut out = std::vec::Vec::new();
    out.extend_from_slice(b"the last sidecar");
    assert_eq!(Spotter::begin(&mut out).err(), Some(IoStatus::SinkNotEmpty));
}

#[test]
fn the_worst_cue_fits_the_bound_it_is_written_in() {
    // `MAX_CUE_BYTES` is derived in its own documentation; this is the
    // derivation checked against the writer. The worst text is a hundred and
    // twenty-eight ampersands, because `&` is the one byte that becomes five.
    let worst: std::string::String = core::iter::repeat_n('&', 128).collect();
    let file = written(&[(said(0, 24, 7, &worst), Timebase::FILM_24)]);
    let cue = file.len() - MAGIC.len();
    assert_eq!(
        cue,
        // "1\n"                                  2
        // "00:00:00.000 --> 00:00:01.000\n"     30
        // "<v Voice 7>"                          11
        // 128 x "&amp;"                         640
        // "</v>\n\n"                              6
        2 + 30 + 11 + 640 + 6,
        "the worst cue is not the size the bound was derived from"
    );
    assert!(cue <= MAX_CUE_BYTES);
    assert_eq!(sidecar_bytes(2).expect("a size"), 2 * MAX_CUE_BYTES + 8);
}
