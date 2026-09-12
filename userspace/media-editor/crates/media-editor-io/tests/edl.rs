// SPDX-License-Identifier: GPL-3.0-only
//! CMX 3600, and the four ways a reader of one goes quietly wrong.

use media_editor_core::{CoreStatus, Timecode};
use media_editor_io::IoStatus;
use media_editor_io::edl::{self, Channel, Transition};

/// A short, well-formed, non-drop list.
const SIMPLE: &str = "\
TITLE:   REEL ONE
FCM: NON-DROP FRAME
001  AX       V     C        00:00:11:18 00:01:09:28 00:00:00:00 00:00:58:10
* FROM CLIP NAME: interview_wide_take_04.mov
002  BX       V     C        00:02:46:24 00:03:16:26 00:00:58:10 00:01:28:12
003  AX       A     C        00:00:10:00 00:01:50:00 00:00:00:00 00:01:40:00
";

fn parse(text: &str) -> edl::EditDecisionList {
    edl::parse(text).expect("a list")
}

fn stamp(hours: u8, minutes: u8, seconds: u8, frames: u8) -> Timecode {
    Timecode::new(hours, minutes, seconds, frames, 30, false).expect("a timecode")
}

#[test]
fn a_simple_list_reads_as_what_it_says() {
    let list = parse(SIMPLE);
    assert_eq!(list.title(), "REEL ONE");
    assert_eq!(list.events().len(), 3);

    let first = &list.events()[0];
    assert_eq!(first.number(), 1);
    assert_eq!(first.reel(), "AX");
    assert_eq!(first.channel(), Channel::Video);
    assert_eq!(first.transition(), Transition::Cut);
    assert_eq!(first.source_in(), stamp(0, 0, 11, 18));
    assert_eq!(first.source_out(), stamp(0, 1, 9, 28));
    assert_eq!(first.record_in(), stamp(0, 0, 0, 0));
    assert_eq!(first.record_out(), stamp(0, 0, 58, 10));
    assert_eq!(
        first.from_clip_name(),
        Some("interview_wide_take_04.mov"),
        "the comment carries what eight characters could not"
    );

    assert_eq!(list.events()[2].channel(), Channel::Audio(1));
}

#[test]
fn the_out_point_is_exclusive() {
    // The trap. `00:00:10:00` as a source out means the last frame used is the
    // one before it, so a ten-second clip at 30 is 300 frames and not 301.
    //
    // An importer that reads out as inclusive makes every clip in the file one
    // frame too long. Nothing rejects it, nothing looks wrong on a timeline,
    // and the programme is over-length by one frame per edit.
    let text = "\
TITLE:   ONE SECOND
FCM: NON-DROP FRAME
001  AX       V     C        00:00:00:00 00:00:01:00 00:00:00:00 00:00:01:00
";
    let list = parse(text);
    let event = &list.events()[0];
    assert_eq!(event.source_frames().expect("frames"), 30);
    assert_eq!(event.record_frames().expect("frames"), 30);

    // And the arithmetic holds across a minute boundary, where an implementation
    // that subtracted the fields rather than the frame numbers would not.
    let text = "\
TITLE:   ACROSS A MINUTE
FCM: NON-DROP FRAME
001  AX       V     C        00:00:59:00 00:01:01:00 00:00:00:00 00:00:02:00
";
    assert_eq!(
        parse(text).events()[0].source_frames().expect("frames"),
        60,
        "two seconds is sixty frames however the digits roll over"
    );
}

#[test]
fn an_event_whose_out_precedes_its_in_is_refused() {
    // Not a zero-length event: a file that does not describe an edit.
    for line in [
        "001  AX       V     C        00:00:10:00 00:00:05:00 00:00:00:00 00:00:05:00",
        "001  AX       V     C        00:00:00:00 00:00:05:00 00:00:10:00 00:00:05:00",
        "001  AX       V     C        00:00:05:00 00:00:05:00 00:00:00:00 00:00:05:00",
    ] {
        let text = std::format!("TITLE: X\nFCM: NON-DROP FRAME\n{line}\n");
        assert_eq!(edl::parse(&text), Err(IoStatus::EdlNegativeDuration));
    }
}

#[test]
fn the_frame_code_mode_applies_to_everything_after_it() {
    // `FCM` is stateful. A file that switches halfway through has two kinds of
    // timecode in it, and a parser that reads only the first line mistimes the
    // second half by three and a half seconds an hour.
    let text = "\
TITLE:   MIXED
FCM: NON-DROP FRAME
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
FCM: DROP FRAME
002  AX       V     C        00:10:00;00 00:10:10;00 00:00:10;00 00:00:20;00
";
    let list = parse(text);
    assert!(!list.events()[0].source_in().is_drop_frame());
    assert!(
        list.events()[1].source_in().is_drop_frame(),
        "the second FCM line was read, not just the first"
    );

    // The two count differently, which is the whole reason the mode exists.
    let non_drop = Timecode::new(0, 10, 0, 0, 30, false)
        .expect("a timecode")
        .to_frame_number()
        .expect("a frame");
    let drop = Timecode::new(0, 10, 0, 0, 30, true)
        .expect("a timecode")
        .to_frame_number()
        .expect("a frame");
    assert_ne!(non_drop, drop);
    assert_eq!(non_drop - drop, 18, "ten minutes drops eighteen labels");
}

#[test]
fn punctuation_and_the_frame_code_mode_must_agree() {
    // Drop-frame is stated twice in this format — once by the `FCM` line and
    // once by the semicolon before the frames field. When a file says both and
    // says them differently, it is not readable without guessing, so it is
    // refused (R-1.3).
    let text = "\
TITLE:   CONTRADICTORY
FCM: NON-DROP FRAME
001  AX       V     C        00:00:00;00 00:00:10;00 00:00:00;00 00:00:10;00
";
    assert_eq!(edl::parse(text), Err(IoStatus::EdlFrameCodeModeConflict));

    let text = "\
TITLE:   THE OTHER WAY
FCM: DROP FRAME
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
";
    assert_eq!(edl::parse(text), Err(IoStatus::EdlFrameCodeModeConflict));
}

#[test]
fn a_file_with_no_fcm_line_is_read_from_its_punctuation() {
    // Real systems write the semicolons and leave the `FCM` line out. That is
    // a complete statement on its own, so it is read rather than refused.
    let text = "\
TITLE:   NO FCM
001  AX       V     C        00:00:00;00 00:00:10;00 00:00:00;00 00:00:10;00
";
    let list = parse(text);
    assert!(list.events()[0].source_in().is_drop_frame());

    let text = "\
TITLE:   NO FCM EITHER
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
";
    assert!(!parse(text).events()[0].source_in().is_drop_frame());
}

#[test]
fn a_dropped_label_is_refused_rather_than_rounded() {
    // 00:01:00;00 is not a frame — drop-frame counting skips it. A file that
    // names it is naming something that does not exist, and answering with the
    // neighbouring frame would be repair rather than reading (R-1.3).
    let text = "\
TITLE:   IMPOSSIBLE
FCM: DROP FRAME
001  AX       V     C        00:01:00;00 00:01:10;00 00:00:00;00 00:00:10;00
";
    assert_eq!(
        edl::parse(text),
        Err(IoStatus::Time(CoreStatus::TimecodeMalformed))
    );

    // And 00:10:00;00 *is* a frame, because every tenth minute keeps its
    // labels. A parser that dropped them all would refuse this one too.
    let text = "\
TITLE:   POSSIBLE
FCM: DROP FRAME
001  AX       V     C        00:10:00;00 00:10:10;00 00:00:00;00 00:00:10;00
";
    assert!(edl::parse(text).is_ok());
}

#[test]
fn a_reel_name_is_eight_characters_and_the_format_says_so() {
    let text = "\
TITLE:   TOO LONG
FCM: NON-DROP FRAME
001  VERYLONGNAME V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
";
    assert_eq!(edl::parse(text), Err(IoStatus::EdlReelTooLong));

    assert_eq!(
        edl::event(
            1,
            "NINECHARS",
            Channel::Video,
            Transition::Cut,
            (stamp(0, 0, 0, 0), stamp(0, 0, 10, 0)),
            (stamp(0, 0, 0, 0), stamp(0, 0, 10, 0)),
        ),
        Err(IoStatus::EdlReelTooLong),
        "and the writer cannot produce what the reader would refuse"
    );
}

#[test]
fn transitions_carry_a_duration_exactly_when_they_have_one() {
    let text = "\
TITLE:   TRANSITIONS
FCM: NON-DROP FRAME
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
002  BX       V     D    024 00:00:10:00 00:00:20:00 00:00:10:00 00:00:20:00
003  CX       V     W001 012 00:00:20:00 00:00:30:00 00:00:20:00 00:00:30:00
";
    let list = parse(text);
    assert_eq!(list.events()[0].transition(), Transition::Cut);
    assert_eq!(list.events()[1].transition(), Transition::Dissolve(24));
    assert_eq!(
        list.events()[2].transition(),
        Transition::Wipe {
            pattern: 1,
            frames: 12
        }
    );

    // A dissolve of no frames is a cut written the long way round, and a file
    // that says it is describing something it does not mean.
    let text = "\
TITLE:   ZERO
FCM: NON-DROP FRAME
001  AX       V     D    000 00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
";
    assert_eq!(edl::parse(text), Err(IoStatus::EdlMalformedEvent));
}

#[test]
fn contiguity_is_asked_one_channel_at_a_time() {
    // A conformed cut list runs without gaps. One with gaps has black in it,
    // and an importer that closed them would shorten the programme silently.
    //
    // The question is per channel, and this fixture is why: picture and sound
    // both start at the top of the programme, so a whole-list version of it
    // would call every ordinary two-channel list full of holes — an answer
    // that is true of the file and false of the cut.
    let list = parse(SIMPLE);
    assert_eq!(
        list.channels(),
        std::vec![Channel::Video, Channel::Audio(1)]
    );
    assert!(list.is_contiguous(Channel::Video).expect("a verdict"));
    assert!(list.is_contiguous(Channel::Audio(1)).expect("a verdict"));

    let gapped = "\
TITLE:   GAPPED
FCM: NON-DROP FRAME
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
002  BX       V     C        00:00:00:00 00:00:10:00 00:00:15:00 00:00:25:00
";
    assert!(
        !parse(gapped)
            .is_contiguous(Channel::Video)
            .expect("a verdict"),
        "five seconds of black between the events is not nothing"
    );
    assert!(
        parse(gapped)
            .is_contiguous(Channel::Audio(1))
            .expect("a verdict"),
        "and a channel with no events on it has no gaps in it"
    );
}

#[test]
fn a_list_written_and_read_back_is_the_same_list() {
    // The round trip a format has to make. Everything the reader understood is
    // written back, and reading that gives the same list — including the
    // comment, the drop-frame mode, and the transition durations.
    let list = parse(SIMPLE);
    let text = edl::write(&list).expect("text");
    let again = edl::parse(&text).expect("a list");
    assert_eq!(again, list);

    // Writing twice gives the same bytes, so an export is reproducible (R-4.1).
    assert_eq!(edl::write(&again).expect("text"), text);
}

#[test]
fn a_mixed_mode_list_round_trips_with_both_of_its_fcm_lines() {
    let text = "\
TITLE:   MIXED
FCM: NON-DROP FRAME
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
FCM: DROP FRAME
002  AX       V     C        00:10:00;00 00:10:10;00 00:00:10;00 00:00:20;00
FCM: NON-DROP FRAME
003  AX       V     C        00:00:00:00 00:00:10:00 00:00:20:00 00:00:30:00
";
    let list = parse(text);
    let written = edl::write(&list).expect("text");
    assert_eq!(
        written.matches("FCM:").count(),
        3,
        "the mode changes twice, so it is stated three times"
    );
    assert_eq!(edl::parse(&written).expect("a list"), list);
}

#[test]
fn every_truncation_of_an_event_line_is_refused() {
    // A parser is a hostile-input surface (R-11.1). Every prefix of a valid
    // event line either parses as something smaller or is refused by name —
    // none of them panics, and none of them reads past what it was given.
    let line = "001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00";
    for length in 0..line.len() {
        let text = std::format!("TITLE: X\nFCM: NON-DROP FRAME\n{}\n", &line[..length]);
        let result = edl::parse(&text);
        assert!(
            result.is_err() || length == 0,
            "a truncation at {length} produced a list"
        );
    }
    assert!(edl::parse(&std::format!("TITLE: X\n{line}\n")).is_ok());
}

#[test]
fn a_hostile_file_is_bounded_rather_than_trusted() {
    let long = std::format!("TITLE: X\n{}\n", "A".repeat(edl::MAX_LINE_BYTES + 1));
    assert_eq!(edl::parse(&long), Err(IoStatus::EdlLineTooLong));

    let mut many = std::string::String::from("TITLE: X\nFCM: NON-DROP FRAME\n");
    for _ in 0..=edl::MAX_EVENTS {
        many.push_str(
            "001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00\n",
        );
    }
    assert_eq!(edl::parse(&many), Err(IoStatus::TooMany));

    assert_eq!(edl::parse(""), Err(IoStatus::EdlNoEvents));
    assert_eq!(edl::parse("TITLE:   NOTHING\n"), Err(IoStatus::EdlNoEvents));
    assert_eq!(
        edl::parse("* FROM CLIP NAME: orphan\n"),
        Err(IoStatus::EdlCommentBeforeEvent)
    );
}

#[test]
fn fields_this_build_has_no_meaning_for_are_refused_by_name() {
    let cases = [
        (
            "001  AX       Q     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00",
            IoStatus::EdlUnknownChannel,
        ),
        (
            "001  AX       A0    C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00",
            IoStatus::EdlUnknownChannel,
        ),
        (
            "001  AX       V     Z        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00",
            IoStatus::EdlUnknownTransition,
        ),
        (
            "001  AX       V     C        0:0:0:0 00:00:10:00 00:00:00:00 00:00:10:00",
            IoStatus::EdlMalformedTimecode,
        ),
        (
            "001  AX       V     C        00-00-00-00 00:00:10:00 00:00:00:00 00:00:10:00",
            IoStatus::EdlMalformedTimecode,
        ),
        (
            "abc  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00",
            IoStatus::EdlMalformedEvent,
        ),
        (
            "001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00 extra",
            IoStatus::EdlMalformedEvent,
        ),
    ];
    for (line, expected) in cases {
        let text = std::format!("TITLE: X\nFCM: NON-DROP FRAME\n{line}\n");
        assert_eq!(edl::parse(&text), Err(expected), "{line}");
    }

    assert_eq!(
        edl::parse("TITLE: X\nFCM: SOMETIMES\n"),
        Err(IoStatus::EdlUnknownFrameCodeMode)
    );
}

#[test]
fn a_list_built_here_writes_a_file_a_reader_accepts() {
    // The other direction: nothing was parsed, so this is the writer on its
    // own, and the check is that the reader agrees with it.
    let event = edl::named(
        edl::event(
            7,
            "SLATE001",
            Channel::Audio(2),
            Transition::Dissolve(12),
            (stamp(1, 0, 0, 0), stamp(1, 0, 4, 0)),
            (stamp(0, 0, 0, 0), stamp(0, 0, 4, 0)),
        )
        .expect("an event"),
        "a name far longer than eight characters.wav",
    );
    let list = edl::list("BUILT HERE", std::vec![event]).expect("a list");
    let text = edl::write(&list).expect("text");
    let again = edl::parse(&text).expect("a list");
    assert_eq!(again, list);
    assert_eq!(again.events()[0].channel(), Channel::Audio(2));
    assert_eq!(again.events()[0].transition(), Transition::Dissolve(12));
    assert_eq!(again.events()[0].source_frames().expect("frames"), 120);
}

#[test]
fn keywords_are_read_in_whatever_case_the_file_wrote_them() {
    let text = "\
title:   lower case
fcm: non-drop frame
001  AX       V     C        00:00:00:00 00:00:10:00 00:00:00:00 00:00:10:00
* from clip name: mixed.mov
";
    let list = parse(text);
    assert_eq!(list.title(), "lower case");
    assert_eq!(list.events()[0].from_clip_name(), Some("mixed.mov"));
}
