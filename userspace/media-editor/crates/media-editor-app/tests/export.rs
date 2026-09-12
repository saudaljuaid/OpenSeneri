// SPDX-License-Identifier: GPL-3.0-only
//! A sequence written out, without ever holding a frame.
//!
//! The property at the top: **an exported reel is the frames `render` would
//! have made.** Every link under it — the row form of the graph, the scan, the
//! winder, the appending seam — exists to make that sentence true while the
//! largest thing in memory is one row.

use media_editor_abi::seam::{Slot, Storage};
use media_editor_app::export::{self, Dub, Job, WINDOW_BYTES};
use media_editor_app::mixdown::{self, SampleSource};
use media_editor_app::{SlateStatus, timeline};
use media_editor_audio::{AudioBuffer, SampleRate};
use media_editor_core::{Digest, Duration, Instant, Rational, TimeRange, Timebase};
use media_editor_io::memory::{Fault, MemoryStorage};
use media_editor_io::sprw::{self, Spool, TRAILER_BYTES};
use media_editor_io::status::IoStatus;
use media_editor_media::colour::{AlphaState, ColourDescription};
use media_editor_media::{Frame, FrameDescription, FramePool, Geometry, PixelFormat};
use media_editor_model::{Clip, Edit, Item, MediaAsset, MediaId, Project, SequenceId, TrackKind};
use media_editor_render::{Library, Look, RenderStatus};

const RATE: Timebase = Timebase::FILM_24;

/// Four by three, so a frame is more than one row and the two are not the
/// same accident.
fn described() -> FrameDescription {
    FrameDescription::square(
        Geometry::new(4, 3).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description")
}

fn frames(count: i64) -> Duration {
    Duration::new(count, RATE).expect("a duration")
}

fn at(frame: i64) -> Instant {
    Instant::new(frame, RATE)
}

fn span(from: i64, count: i64) -> TimeRange {
    TimeRange::new(at(from), frames(count)).expect("a span")
}

/// A picture-only export of a span into the vault.
fn job(
    project: &Project,
    sequence: SequenceId,
    span: TimeRange,
    description: FrameDescription,
) -> Job<'_> {
    Job {
        project,
        sequence,
        span,
        description,
        into: Slot::Vault,
    }
}

/// A library that serves whole frames and rows out of the same flat colours.
struct Flat {
    colours: std::vec::Vec<(Digest, [u8; 4])>,
}

impl Flat {
    fn colour(&self, media: Digest) -> [u8; 4] {
        self.colours
            .iter()
            .find(|(id, _)| *id == media)
            .map_or([0, 0, 0, 255], |(_, colour)| *colour)
    }
}

impl Library for Flat {
    fn frame(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
    ) -> Result<Frame, RenderStatus> {
        // The tick is in the picture, so a reel whose frames all came from the
        // same instant is a reel that looks wrong rather than one that looks
        // fine. An exporter with an off-by-one in its playhead is exactly the
        // bug a flat colour would hide.
        let mut colour = self.colour(media);
        colour[1] = u8::try_from(tick.rem_euclid(200)).expect("a byte");
        let mut bytes = std::vec::Vec::new();
        for _ in 0..description.geometry().width() * description.geometry().height() {
            bytes.extend_from_slice(&colour);
        }
        Frame::from_packed(description, &bytes).map_err(RenderStatus::Media)
    }

    fn look(&mut self, _look: Digest) -> Result<Look, RenderStatus> {
        Err(RenderStatus::UnknownNode)
    }

    fn row(
        &mut self,
        media: Digest,
        tick: i64,
        description: FrameDescription,
        row: usize,
    ) -> Result<Frame, RenderStatus> {
        let one = media_editor_render::row_description(description, row)?;
        let mut colour = self.colour(media);
        colour[1] = u8::try_from(tick.rem_euclid(200)).expect("a byte");
        let mut bytes = std::vec::Vec::new();
        for _ in 0..one.geometry().width() {
            bytes.extend_from_slice(&colour);
        }
        Frame::from_packed(one, &bytes).map_err(RenderStatus::Media)
    }
}

fn media(project: &mut Project, tag: u8) -> MediaId {
    let mut bytes = [0_u8; 32];
    bytes[0] = tag;
    let asset = MediaAsset::new(
        media_editor_model::media::Digest::new(bytes),
        RATE,
        frames(1000),
    )
    .expect("an asset");
    project.add_media(asset).expect("an identifier")
}

fn digest_of(project: &Project, id: MediaId) -> Digest {
    project.media().get(id).expect("an asset").digest()
}

fn lay(project: &mut Project, sequence: SequenceId, track: usize, items: &[Item]) {
    while project
        .sequence(sequence)
        .expect("a sequence")
        .track_count()
        <= track
    {
        let index = project
            .sequence(sequence)
            .expect("a sequence")
            .track_count();
        project
            .apply(
                sequence,
                Edit::AddTrack {
                    index,
                    kind: TrackKind::Video,
                },
            )
            .expect("a track");
    }
    for (index, item) in items.iter().enumerate() {
        project
            .apply(
                sequence,
                Edit::InsertItem {
                    track,
                    index,
                    item: item.clone(),
                },
            )
            .expect("an insert");
    }
}

/// A source whose samples say where they came from.
///
/// Every sample is a function of its own position in the media, so a block
/// written in the wrong place, or fetched from the wrong instant, is a
/// different number rather than the same silence.
struct Tone {
    rate: SampleRate,
    channels: usize,
    asked: std::vec::Vec<(Digest, i64, usize)>,
}

impl SampleSource for Tone {
    fn samples(
        &mut self,
        media: Digest,
        start: i64,
        count: usize,
    ) -> Result<AudioBuffer, media_editor_audio::AudioStatus> {
        self.asked.push((media, start, count));
        let held: std::vec::Vec<std::vec::Vec<i32>> = (0..self.channels)
            .map(|channel| {
                (0..count)
                    .map(|index| {
                        let at = start.wrapping_add(i64::try_from(index).expect("a sample"));
                        i32::try_from(
                            (at.wrapping_mul(29).wrapping_add(
                                i64::from(i32::try_from(channel).expect("a channel")) * 7717,
                            ))
                            .rem_euclid(20_000),
                        )
                        .expect("a sample")
                            - 10_000
                    })
                    .collect()
            })
            .collect();
        AudioBuffer::new(self.rate, held)
    }
}

/// A source at one loud constant, so two of them sum past full scale.
struct Loud {
    level: i32,
    rate: SampleRate,
    channels: usize,
}

impl SampleSource for Loud {
    fn samples(
        &mut self,
        _media: Digest,
        _start: i64,
        count: usize,
    ) -> Result<AudioBuffer, media_editor_audio::AudioStatus> {
        let held = (0..self.channels)
            .map(|_| std::vec![self.level; count])
            .collect();
        AudioBuffer::new(self.rate, held)
    }
}

/// A sequence with one picture track and *two* sound tracks, so a mix has
/// something to sum.
fn with_two_loud_tracks(rate: Timebase) -> (Project, SequenceId, Flat) {
    let (mut project, sequence, library) = with_a_sound_track(rate);
    // Whatever the picture track is laid from: `with_a_sound_track` lays one
    // clip of one asset on each of its tracks, so the second sound track can
    // read the same one.
    let shot = match project
        .sequence(sequence)
        .expect("a sequence")
        .track(0)
        .expect("a track")
        .items()
        .first()
        .expect("a clip")
    {
        Item::Clip(clip) => clip.media(),
        Item::Gap(_) => panic!("the picture track holds a clip"),
    };
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 2,
                kind: TrackKind::Audio,
            },
        )
        .expect("a second sound track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 2,
                index: 0,
                item: Item::Clip(
                    Clip::new(shot, 0, Duration::new(50, rate).expect("a duration"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("an insert");
    (project, sequence, library)
}

fn tone(rate: SampleRate, channels: usize) -> Tone {
    Tone {
        rate,
        channels,
        asked: std::vec::Vec::new(),
    }
}

/// A sequence with sound on it, at a timebase where a frame is not a whole
/// number of samples.
fn with_a_sound_track(rate: Timebase) -> (Project, SequenceId, Flat) {
    let mut project = Project::new();
    let sequence = project.add_sequence(rate).expect("a sequence");
    let shot = {
        let mut bytes = [0_u8; 32];
        bytes[0] = 9;
        let asset = MediaAsset::new(
            media_editor_model::media::Digest::new(bytes),
            rate,
            Duration::new(1000, rate).expect("a duration"),
        )
        .expect("an asset");
        project.add_media(asset).expect("an identifier")
    };
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a picture track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(shot, 0, Duration::new(50, rate).expect("a duration"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("an insert");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 1,
                kind: TrackKind::Audio,
            },
        )
        .expect("a sound track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 1,
                index: 0,
                item: Item::Clip(
                    Clip::new(shot, 0, Duration::new(50, rate).expect("a duration"))
                        .expect("a clip"),
                ),
            },
        )
        .expect("an insert");
    let digest = project.media().get(shot).expect("an asset").digest();
    let library = Flat {
        colours: std::vec![(digest, [40, 0, 80, 255])],
    };
    (project, sequence, library)
}

/// Two layers, the upper one fading, over black -- the same programme the row
/// form's own property test uses, so the two agree about what is being made.
fn programme() -> (Project, SequenceId, Flat) {
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let under = media(&mut project, 1);
    let over = media(&mut project, 2);
    lay(
        &mut project,
        sequence,
        0,
        &[Item::Clip(Clip::new(under, 0, frames(20)).expect("a clip"))],
    );
    lay(
        &mut project,
        sequence,
        1,
        &[Item::Clip(
            Clip::new(over, 0, frames(20))
                .expect("a clip")
                .with_fades(frames(6), frames(6))
                .expect("fades"),
        )],
    );
    let library = Flat {
        colours: std::vec![
            (digest_of(&project, under), [10, 0, 200, 255]),
            (digest_of(&project, over), [200, 0, 10, 255]),
        ],
    };
    (project, sequence, library)
}

#[test]
fn an_exported_reel_is_the_frames_the_renderer_makes() {
    // The property, at the top of the program. A reel written a row at a time
    // out of a scan, and the same frames rendered whole out of `render`: the
    // same pictures, in the same order.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    let report = export::export(
        &job(&project, sequence, span(2, 5), described()),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("an export");

    let file = storage.stored().expect("a committed reel").to_vec();
    let reel = sprw::decode(&file).expect("a reel");
    assert_eq!(reel.len(), 5, "five frames were asked for");
    assert_eq!(reel.timebase(), RATE);
    assert_eq!(reel.description(), &described());
    for (index, frame) in reel.frames().iter().enumerate() {
        let whole = timeline::render(
            &project,
            sequence,
            at(2 + i64::try_from(index).expect("a frame")),
            described(),
            &mut FramePool::new(64, 1 << 20),
            &mut library,
        )
        .expect("a render");
        assert_eq!(
            frame, &whole,
            "frame {index} of the export is not frame {index} of the programme"
        );
    }
    // And the digest handed back is the file's own trailer, which is what a
    // caller records to say which reel this was.
    assert_eq!(&file[file.len() - TRAILER_BYTES..], report.digest.bytes());
}

#[test]
fn exporting_never_holds_more_than_one_row() {
    // The measurement, and the reason every link of the chain exists. Not how
    // many appends -- how large the largest of them was.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    let wide = FrameDescription::square(
        Geometry::new(64, 3).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    export::export(
        &job(&project, sequence, span(0, 4), wide),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("an export");

    // Sixty-four wide and three tall, four bytes a pixel: a row is 256 bytes
    // and a frame is 768. The largest thing this path ever handed to storage
    // is **one row** -- a third of one picture, and the header at 64 and the
    // trailer at 32 are both smaller still. Four wide would have made the
    // header the largest thing written and proved nothing.
    assert_eq!(storage.largest_append(), 256);
    // One append for the header, one per row of every frame, one for the
    // trailer: 1 + 3 x 4 + 1 = 14. `clear` writes rather than appends, so it
    // is not in this count.
    assert_eq!(storage.appends(), 14);
    // Nothing on this path read a whole slot. The verification walks in
    // windows, so its largest read is a window and not a file.
    assert_eq!(storage.whole_reads(), 0, "something loaded a slot");
    assert!(
        storage.largest_read() <= WINDOW_BYTES,
        "a read of {} bytes is larger than the window",
        storage.largest_read()
    );
}

#[test]
fn an_export_taller_than_a_tile_is_the_render_of_it_band_by_band() {
    // The export computes a band of rows at a time and hands them to the
    // winder one at a time. A picture taller than one tile therefore crosses
    // a band boundary — several, here — and the property that matters is that
    // nothing about the answer changes: the reel is the frames `render`
    // makes, whatever the boundaries fall between.
    //
    // It is a turn, because a turn is what bands are for: consecutive rows of
    // one read overlapping bands of the source, and a band fetches the union
    // once. Forty rows against a tile of sixteen is three bands, the last of
    // them short.
    let cosine = Rational::new(4, 5).expect("a cosine");
    let sine = Rational::new(3, 5).expect("a sine");
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 11);
    let framed = Clip::new(shot, 0, frames(20))
        .expect("a clip")
        .with_transform(Some(
            media_editor_model::Transform::new(
                [cosine, sine.checked_neg().expect("a sine"), sine, cosine],
                (Rational::ZERO, Rational::ZERO),
                media_editor_model::Resampling::Area,
            )
            .expect("a transform"),
        ));
    lay(&mut project, sequence, 0, &[Item::Clip(framed)]);
    let deep = FrameDescription::square(
        Geometry::new(12, 40).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let mut library = Flat {
        colours: std::vec![(digest_of(&project, shot), [70, 120, 30, 255])],
    };
    let mut storage = MemoryStorage::new(1 << 20);
    export::export(
        &job(&project, sequence, span(0, 2), deep),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("an export");
    let reel = sprw::decode(storage.stored().expect("a reel")).expect("a reel");
    assert_eq!(reel.len(), 2);
    for (index, frame) in reel.frames().iter().enumerate() {
        let whole = timeline::render(
            &project,
            sequence,
            at(i64::try_from(index).expect("a frame")),
            deep,
            &mut FramePool::new(64, 1 << 20),
            &mut library,
        )
        .expect("a render");
        assert_eq!(frame, &whole, "frame {index} of a banded export");
    }
    // And it still never held a frame: every read of the store stayed inside
    // one window. A band is rows of *output* held while they are drawn, not a
    // frame read back.
    assert!(
        storage.largest_read() <= WINDOW_BYTES,
        "a read of {} bytes is larger than the window",
        storage.largest_read()
    );
}

#[test]
fn a_framed_programme_exports_a_row_at_a_time_and_is_the_render_of_it() {
    // What the band bought, at the top of the program rather than in the
    // middle of it: a shot scaled down and moved -- the commonest thing a
    // cutter does to a shot -- goes through the whole row path and comes out
    // as the frames `render` makes. Before the band this was `NotRowLocal` and
    // an export of it was impossible.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 7);
    let framed = Clip::new(shot, 0, frames(20))
        .expect("a clip")
        .with_transform(Some(
            media_editor_model::Transform::scaled(
                Rational::new(1, 2).expect("a scale"),
                Rational::new(1, 2).expect("a scale"),
                (Rational::new(1, 4).expect("a move"), Rational::ZERO),
                media_editor_model::Resampling::Bilinear,
            )
            .expect("a transform"),
        ));
    lay(&mut project, sequence, 0, &[Item::Clip(framed)]);
    let mut library = Flat {
        colours: std::vec![(digest_of(&project, shot), [90, 40, 160, 255])],
    };
    let mut storage = MemoryStorage::new(1 << 20);
    export::export(
        &job(&project, sequence, span(0, 3), described()),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("an export");
    let reel = sprw::decode(storage.stored().expect("a reel")).expect("a reel");
    assert_eq!(reel.len(), 3);
    for (index, frame) in reel.frames().iter().enumerate() {
        let whole = timeline::render(
            &project,
            sequence,
            at(i64::try_from(index).expect("a frame")),
            described(),
            &mut FramePool::new(64, 1 << 20),
            &mut library,
        )
        .expect("a render");
        assert_eq!(frame, &whole, "frame {index} of a framed export");
    }
    // And it never held a frame: every read of the store stayed inside one
    // window, which is the invariant the whole row path exists for.
    assert!(
        storage.largest_read() <= WINDOW_BYTES,
        "a read of {} bytes is larger than the window",
        storage.largest_read()
    );
}

#[test]
fn a_programme_that_cannot_be_scanned_is_refused_before_anything_is_committed() {
    // What is left that an export cannot scan, now that strips have made
    // every invertible transform scannable: a vertical downscale steep enough
    // that even one column of one row reads more source rows than a band may
    // hold. Narrowing a strip does nothing to a map that takes horizontals to
    // horizontals, so there is no slicing that helps. That is `BandTooTall`,
    // and it is a question about a *row* -- so the export refuses part way
    // through the first frame, with the destination slot untouched.
    //
    // A half scale stood here once and a turn after it, and both scan now.
    // The picture is two hundred rows tall because the band is clamped to the
    // picture: a shrink of any steepness over a three-row frame reads three
    // rows, which is nobody's problem. At two hundred rows a 1/256 shrink
    // reads a hundred of them for the row at the centre, and sixty-four is
    // the bound.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let shot = media(&mut project, 3);
    let framed = Clip::new(shot, 0, frames(20))
        .expect("a clip")
        .with_transform(Some(
            media_editor_model::Transform::scaled(
                Rational::ONE,
                Rational::new(1, 256).expect("a scale"),
                (Rational::ZERO, Rational::ZERO),
                media_editor_model::Resampling::Area,
            )
            .expect("a transform"),
        ));
    lay(&mut project, sequence, 0, &[Item::Clip(framed)]);
    let mut library = Flat {
        colours: std::vec![(digest_of(&project, shot), [1, 2, 3, 255])],
    };
    let tall = FrameDescription::square(
        Geometry::new(4, 200).expect("a geometry"),
        PixelFormat::Rgba8,
        ColourDescription::srgb_full(),
        None,
        Some(AlphaState::Premultiplied),
    )
    .expect("a description");
    let mut storage = MemoryStorage::new(1 << 20);
    storage
        .write(Slot::Vault, b"the last reel")
        .expect("a write");
    assert_eq!(
        export::export(
            &job(&project, sequence, span(0, 4), tall),
            &mut library,
            None,
            None,
            &mut storage,
        )
        .err(),
        Some(SlateStatus::Render(RenderStatus::BandTooTall))
    );
    assert_eq!(storage.stored(), Some(&b"the last reel"[..]));
    assert_eq!(storage.commits(), 0);
}

#[test]
fn an_export_interrupted_at_any_append_leaves_the_last_reel_alone() {
    // R-9.4 for a save that takes fourteen steps rather than one, driven
    // through every one of them. The failure a whole-file write cannot have:
    // a drive that fills up partway through a picture.
    let (project, sequence, _) = programme();
    let previous = b"the last reel, which must survive all of this".to_vec();
    for step in 0..14 {
        let (_, _, mut library) = programme();
        let mut storage = MemoryStorage::new(1 << 20);
        storage.write(Slot::Vault, &previous).expect("a write");
        storage.set_fault(Fault::OnAppend(step));
        let refusal = export::export(
            &job(&project, sequence, span(0, 4), described()),
            &mut library,
            None,
            None,
            &mut storage,
        );
        assert!(refusal.is_err(), "append {step} was supposed to be refused");
        assert_eq!(
            storage.stored(),
            Some(previous.as_slice()),
            "the last reel did not survive a refusal at append {step}"
        );
        assert_eq!(
            storage.commits(),
            0,
            "something was committed at step {step}"
        );
    }
}

#[test]
fn a_store_that_keeps_something_else_is_caught_before_the_commit() {
    // The read-back step, which is the whole reason a save is four steps and
    // not one. A store that accepted every append and kept a different byte
    // must not reach the commit.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    storage
        .write(Slot::Vault, b"the last reel")
        .expect("a write");
    storage.set_fault(Fault::Corrupting);
    let refusal = export::export(
        &job(&project, sequence, span(0, 4), described()),
        &mut library,
        None,
        None,
        &mut storage,
    );
    assert!(
        matches!(refusal, Err(SlateStatus::File(_))),
        "a corrupted recording was not caught: {refusal:?}"
    );
    assert_eq!(storage.stored(), Some(&b"the last reel"[..]));
    assert_eq!(storage.commits(), 0);
}

#[test]
fn a_committed_reel_reads_back_through_the_spool_without_being_loaded() {
    // What the export is for: a reel in a slot, read a frame at a time by
    // something that never holds it.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    export::export(
        &job(&project, sequence, span(3, 6), described()),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("an export");
    let file = storage.stored().expect("a reel").to_vec();
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    assert_eq!(spool.len(), 6);
    spool.verify(&file.as_slice(), WINDOW_BYTES).expect("sound");
    // Frame three of the export is the programme at instant three, and the
    // green channel carries the tick -- so a reel of six identical frames,
    // which a playhead that never moved would produce, fails here.
    let greens: std::vec::Vec<u8> = (0..6)
        .map(|index| {
            spool
                .frame(&file.as_slice(), index)
                .expect("a frame")
                .to_packed()
                .expect("bytes")[1]
        })
        .collect();
    assert_eq!(greens, std::vec![3, 4, 5, 6, 7, 8]);
}

#[test]
fn an_export_of_nothing_is_refused_rather_than_written() {
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    assert_eq!(
        export::export(
            &job(&project, sequence, span(0, 0), described()),
            &mut library,
            None,
            None,
            &mut storage,
        )
        .err(),
        Some(SlateStatus::File(IoStatus::EmptyReel)),
        "a reel of no frames is not a file"
    );
    assert_eq!(storage.commits(), 0);
}

#[test]
fn a_sample_damaged_after_the_digest_was_computed_is_caught_by_the_walk() {
    // The half of the verification that only the walk can do. `Corrupting`
    // damages every append, the trailer included, and a file whose samples and
    // trailer are both wrong is caught by comparing the trailer against what
    // was computed -- without reading a sample. This damages **one row** and
    // leaves the trailer alone, so the file states exactly the digest the
    // winder computed and does not hash to it.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    storage
        .write(Slot::Vault, b"the last reel")
        .expect("a write");
    // Append 0 is the header and append 13 is the trailer, so 5 is a row in
    // the middle of the second frame.
    storage.set_fault(Fault::CorruptingAppend(5));
    let refusal = export::export(
        &job(&project, sequence, span(0, 4), described()),
        &mut library,
        None,
        None,
        &mut storage,
    );
    assert_eq!(
        refusal.err(),
        Some(SlateStatus::File(IoStatus::DigestMismatch)),
        "a damaged row reached the commit"
    );
    assert_eq!(storage.stored(), Some(&b"the last reel"[..]));
    assert_eq!(storage.commits(), 0);
}

#[test]
fn a_verification_that_reads_the_wrong_slot_is_caught_by_the_digest_it_expected() {
    // The half of the verification that only the trailer comparison can do,
    // and the failure it is actually for: not a bad sector, but a *reader
    // pointed at the wrong file*. The vault already holds a reel of exactly
    // the same shape and length as the one being staged, so everything about
    // it is sound -- its magic, its version, its count, its own digest. It is
    // simply not the reel that was just written, and "does this file state the
    // digest I computed" is the only question that notices.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    export::export(
        &job(&project, sequence, span(0, 4), described()),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("a first export");
    let first = storage.stored().expect("a reel").to_vec();

    // A second export of a *different* span, the same length in bytes.
    storage.set_fault(Fault::ReadsTheWrongSlot);
    let refusal = export::export(
        &job(&project, sequence, span(9, 4), described()),
        &mut library,
        None,
        None,
        &mut storage,
    );
    assert_eq!(
        refusal.err(),
        Some(SlateStatus::File(IoStatus::WriteNotVerified)),
        "a sound reel that was not the one written passed for it"
    );
    assert_eq!(
        storage.stored(),
        Some(first.as_slice()),
        "the first reel survived"
    );
    assert_eq!(storage.commits(), 1, "only the first export committed");
}

#[test]
fn an_export_after_a_save_writes_a_reel_and_not_a_reel_with_a_prefix() {
    // The scratch slot is where the last save was assembled and it may still
    // hold it. A reel's header belongs at offset nought, so an export empties
    // the slot before it begins -- and this is the test that says so, because
    // every other one here starts from storage nobody has used.
    let (project, sequence, mut library) = programme();
    let mut storage = MemoryStorage::new(1 << 20);
    storage
        .write(Slot::Scratch, b"whatever the last save left behind")
        .expect("a staged file");
    let report = export::export(
        &job(&project, sequence, span(1, 3), described()),
        &mut library,
        None,
        None,
        &mut storage,
    )
    .expect("an export");
    let file = storage.stored().expect("a reel");
    assert_eq!(
        &file[..4],
        b"SPRW",
        "the reel does not begin at offset nought"
    );
    let reel = sprw::decode(file).expect("a reel");
    assert_eq!(reel.len(), 3);
    assert_eq!(&file[file.len() - TRAILER_BYTES..], report.digest.bytes());
}

#[test]
fn an_exported_reels_sound_is_the_mix_over_the_whole_span() {
    // The sound half of the property at the top, and the sentence the whole
    // streaming mix rests on: **mixing frame by frame gives exactly what
    // mixing the span at once gives.** It holds because that is already how
    // `mix` works inside itself -- a frame's samples run from `floor_into` of
    // one instant to `floor_into` of the next, so consecutive frames tile the
    // span with no gap and no overlap.
    //
    // At 30000/1001 into 48 kHz a frame is 1601.6 samples, so this is the rate
    // where a writer that assumed a fixed block size would drift.
    let rate = Timebase::NTSC_30;
    let (project, sequence, mut library) = with_a_sound_track(rate);
    let mut source = tone(SampleRate::Hz48000, 2);
    let mut storage = MemoryStorage::new(1 << 20);
    let span = TimeRange::new(
        Instant::new(4, rate),
        Duration::new(7, rate).expect("a duration"),
    )
    .expect("a span");
    let report = export::export(
        &Job {
            project: &project,
            sequence,
            span,
            description: described(),
            into: Slot::Vault,
        },
        &mut library,
        Some(Dub {
            rate: SampleRate::Hz48000,
            channels: 2,
            source: &mut source,
        }),
        None,
        &mut storage,
    )
    .expect("an export");
    assert!(report.mix.is_clean(), "nothing should have clipped");

    let file = storage.stored().expect("a reel").to_vec();
    let reel = sprw::decode(&file).expect("a reel");
    let carried = reel.sound().expect("sound");

    // The whole span in one go, through the same mixer.
    let mut whole_source = tone(SampleRate::Hz48000, 2);
    let (whole, _) = mixdown::mix(
        &project,
        sequence,
        span,
        SampleRate::Hz48000,
        2,
        &mut whole_source,
    )
    .expect("a mix");
    assert_eq!(carried, &whole, "the two mixes are not one sound");

    // Seven frames at 30000/1001 into 48 kHz: 7 x 1601.6 = 11,211.2, so the
    // span holds 11,211 samples or 11,212 depending where it starts -- and
    // this one starts at frame four.
    assert!(
        carried.len() == 11_211 || carried.len() == 11_212,
        "seven frames came to {} samples",
        carried.len()
    );
    assert_eq!(carried.channel_count(), 2);
    assert_eq!(carried.rate(), SampleRate::Hz48000);
}

#[test]
fn exporting_sound_never_holds_more_than_one_frame_of_it() {
    // The measurement for the sound half. Ten seconds of 48 kHz stereo is
    // 3,840,000 bytes against the 76 KiB a Phipia program is mapped; one frame
    // of it is 1602 samples of two channels, which is 12,816 bytes.
    let rate = Timebase::NTSC_30;
    let (project, sequence, mut library) = with_a_sound_track(rate);
    let mut source = tone(SampleRate::Hz48000, 2);
    let mut storage = MemoryStorage::new(1 << 20);
    export::export(
        &Job {
            project: &project,
            sequence,
            span: TimeRange::new(
                Instant::new(0, rate),
                Duration::new(10, rate).expect("a duration"),
            )
            .expect("a span"),
            description: described(),
            into: Slot::Vault,
        },
        &mut library,
        Some(Dub {
            rate: SampleRate::Hz48000,
            channels: 2,
            source: &mut source,
        }),
        None,
        &mut storage,
    )
    .expect("an export");
    // A block of 1602 stereo samples is 1602 x 2 x 4 = 12,816 bytes, and a
    // block of 1601 is 12,808. The largest append is one of those and never
    // the whole sound section, which at ten frames is 128,088.
    assert!(
        storage.largest_append() == 12_816 || storage.largest_append() == 12_808,
        "the largest append was {}",
        storage.largest_append()
    );
    // One append for the header, one per row of every frame, one per frame of
    // sound, one for the trailer: 1 + 3 x 10 + 10 + 1 = 42.
    assert_eq!(storage.appends(), 42);
    assert_eq!(storage.whole_reads(), 0);
}

#[test]
fn an_exported_reel_reads_its_sound_back_a_frame_at_a_time() {
    // What the export is for, on the sound side: a take in a slot whose sound
    // is read one frame at a time by something that never holds it.
    let rate = Timebase::NTSC_30;
    let (project, sequence, mut library) = with_a_sound_track(rate);
    let mut source = tone(SampleRate::Hz48000, 2);
    let mut storage = MemoryStorage::new(1 << 20);
    export::export(
        &Job {
            project: &project,
            sequence,
            span: TimeRange::new(
                Instant::new(0, rate),
                Duration::new(5, rate).expect("a duration"),
            )
            .expect("a span"),
            description: described(),
            into: Slot::Vault,
        },
        &mut library,
        Some(Dub {
            rate: SampleRate::Hz48000,
            channels: 2,
            source: &mut source,
        }),
        None,
        &mut storage,
    )
    .expect("an export");
    let file = storage.stored().expect("a reel").to_vec();
    let spool = Spool::open(&file.as_slice()).expect("a spool");
    spool.verify(&file.as_slice(), WINDOW_BYTES).expect("sound");
    let carried = spool.sound().expect("a sound description");
    assert_eq!(carried.channels(), 2);

    let reel = sprw::decode(&file).expect("a reel");
    let whole = reel.sound().expect("sound");
    let mut gathered: std::vec::Vec<i32> = std::vec::Vec::new();
    let mut lengths = std::vec::Vec::new();
    for index in 0..5 {
        let block = spool
            .sound_block(&file.as_slice(), index)
            .expect("a block back");
        gathered.extend_from_slice(block.channel(0).expect("a channel"));
        lengths.push(block.len());
    }
    assert_eq!(gathered, whole.channel(0).expect("a channel"));
    // Five frames at 1601.6: 8008 samples, made of 1601s and 1602s.
    assert_eq!(lengths.iter().sum::<usize>(), 8008);
    assert!(lengths.contains(&1601) && lengths.contains(&1602));
}

#[test]
fn a_silent_export_and_a_sounded_one_are_the_same_pictures() {
    // Sound is a second section, so adding it must not move a single picture
    // byte. This is what says the two halves of the file are independent.
    let rate = Timebase::NTSC_30;
    let (project, sequence, mut library) = with_a_sound_track(rate);
    let span = TimeRange::new(
        Instant::new(0, rate),
        Duration::new(4, rate).expect("a duration"),
    )
    .expect("a span");
    let make = |dub: Option<Dub<'_>>, library: &mut Flat| {
        let mut storage = MemoryStorage::new(1 << 20);
        export::export(
            &Job {
                project: &project,
                sequence,
                span,
                description: described(),
                into: Slot::Vault,
            },
            library,
            dub,
            None,
            &mut storage,
        )
        .expect("an export");
        storage.stored().expect("a reel").to_vec()
    };
    let silent = make(None, &mut library);
    let mut source = tone(SampleRate::Hz48000, 2);
    let sounded = make(
        Some(Dub {
            rate: SampleRate::Hz48000,
            channels: 2,
            source: &mut source,
        }),
        &mut library,
    );
    let pictures = silent.len() - TRAILER_BYTES;
    assert_eq!(
        silent[80..pictures],
        sounded[80..pictures],
        "adding sound moved a picture"
    );
    // The header says what the sound is, in bytes 64..80 and nowhere else: a
    // rate tag of 2, two channels, and 6406 samples -- four frames at
    // 30000/1001 into 48 kHz is 4 x 1601.6 = 6406.4, so 6406.
    assert_eq!(silent[64..80], [0; 16], "a silent reel declares nothing");
    assert_eq!(sounded[64], SampleRate::Hz48000.tag());
    assert_eq!(sounded[65], 2);
    assert_eq!(sounded[66..72], [0; 6], "the reserved bytes are reserved");
    assert_eq!(
        u64::from_le_bytes(sounded[72..80].try_into().expect("eight bytes")),
        6406
    );
    assert_eq!(
        silent[..64],
        sounded[..64],
        "and nothing else in the header"
    );
    // The sounded file is longer by exactly the samples: 6406 x 2 x 4.
    assert_eq!(sounded.len() - silent.len(), 6406 * 2 * 4);
    // And the silent reel decodes to a reel with no sound.
    assert_eq!(sprw::decode(&silent).expect("a reel").sound(), None);
}

#[test]
fn an_export_whose_sound_fails_partway_leaves_the_last_reel_alone() {
    // R-9.4 over the sound half. The pictures are already written when the
    // drive fills up, which is exactly the case a whole-file writer cannot
    // reach: half a reel is on the disk and none of it may be committed.
    let rate = Timebase::NTSC_30;
    let (project, sequence, _) = with_a_sound_track(rate);
    let previous = b"the last reel".to_vec();
    // 1 header + 3 rows x 4 frames + 4 blocks + 1 trailer = 18.
    for step in 13..18 {
        let (_, _, mut library) = with_a_sound_track(rate);
        let mut source = tone(SampleRate::Hz48000, 2);
        let mut storage = MemoryStorage::new(1 << 20);
        storage.write(Slot::Vault, &previous).expect("a write");
        storage.set_fault(Fault::OnAppend(step));
        let refusal = export::export(
            &Job {
                project: &project,
                sequence,
                span: TimeRange::new(
                    Instant::new(0, rate),
                    Duration::new(4, rate).expect("a duration"),
                )
                .expect("a span"),
                description: described(),
                into: Slot::Vault,
            },
            &mut library,
            Some(Dub {
                rate: SampleRate::Hz48000,
                channels: 2,
                source: &mut source,
            }),
            None,
            &mut storage,
        );
        assert!(refusal.is_err(), "append {step} was supposed to be refused");
        assert_eq!(
            storage.stored(),
            Some(previous.as_slice()),
            "the last reel did not survive a refusal at append {step}"
        );
        assert_eq!(storage.commits(), 0);
    }
}

#[test]
fn the_declared_sample_count_follows_where_the_span_starts() {
    // The subtlety this format had to face. A span of `n` frames holds
    // `floor((s+n)*r) - floor(s*r)` samples, which is *not* `floor(n*r)` and
    // varies with where the span starts. At 30000/1001 into 48 kHz, r is
    // 1601.6, so three frames from nought hold floor(4804.8) = 4804 and three
    // frames from one hold floor(6406.4) - floor(1601.6) = 6406 - 1601 =
    // 4805. Both worked out by hand.
    //
    // An exporter that counted from the frame count alone would be one sample
    // out on the second of those, every time -- and would then declare a
    // header its own mixer could not fill.
    let rate = Timebase::NTSC_30;
    let (project, sequence, mut library) = with_a_sound_track(rate);
    let mut counts = std::vec::Vec::new();
    for start in 0..6 {
        let mut source = tone(SampleRate::Hz48000, 2);
        let mut storage = MemoryStorage::new(1 << 20);
        export::export(
            &Job {
                project: &project,
                sequence,
                span: TimeRange::new(
                    Instant::new(start, rate),
                    Duration::new(3, rate).expect("a duration"),
                )
                .expect("a span"),
                description: described(),
                into: Slot::Vault,
            },
            &mut library,
            Some(Dub {
                rate: SampleRate::Hz48000,
                channels: 2,
                source: &mut source,
            }),
            None,
            &mut storage,
        )
        .expect("an export");
        let reel = sprw::decode(storage.stored().expect("a reel")).expect("a reel");
        counts.push(reel.sound().expect("sound").len());
    }
    // floor((s+3)*1601.6) - floor(s*1601.6), for s = 0..6, each worked out
    // from the definition:
    //
    //   s=0   floor(4804.8)  - 0             = 4804 - 0    = 4804
    //   s=1   floor(6406.4)  - floor(1601.6) = 6406 - 1601 = 4805
    //   s=2   floor(8008.0)  - floor(3203.2) = 8008 - 3203 = 4805
    //   s=3   floor(9609.6)  - floor(4804.8) = 9609 - 4804 = 4805
    //   s=4   floor(11211.2) - floor(6406.4) = 11211 - 6406 = 4805
    //   s=5   floor(12812.8) - floor(8008.0) = 12812 - 8008 = 4804
    //
    // The first draft of this comment had s=3 at 4804 and the test caught it,
    // which is the reason the subtraction is written out rather than summarised.
    // 4804 comes back where the start is a whole number of samples in -- s=0
    // and s=5, since 5 x 1601.6 is 8008 exactly -- and it is 4805 everywhere
    // between.
    assert_eq!(counts, std::vec![4804, 4805, 4805, 4805, 4805, 4804]);
    // And both values really are in there, which is what makes the sweep worth
    // running rather than one case worth asserting.
    assert!(counts.contains(&4804) && counts.contains(&4805));
}

#[test]
fn an_export_says_when_the_mix_clipped() {
    // The one thing about an export that a person has to be told and that no
    // later reader can recover: the samples in the file are the clipped ones,
    // and they look exactly like samples somebody meant. Two loud tracks
    // summing past full scale is how a real mix gets there.
    let rate = Timebase::FILM_24;
    let (project, sequence, mut library) = with_two_loud_tracks(rate);
    let mut source = Loud {
        level: 6_000_000,
        rate: SampleRate::Hz48000,
        channels: 2,
    };
    let mut storage = MemoryStorage::new(1 << 22);
    let report = export::export(
        &Job {
            project: &project,
            sequence,
            span: TimeRange::new(
                Instant::new(0, rate),
                Duration::new(3, rate).expect("a duration"),
            )
            .expect("a span"),
            description: described(),
            into: Slot::Vault,
        },
        &mut library,
        Some(Dub {
            rate: SampleRate::Hz48000,
            channels: 2,
            source: &mut source,
        }),
        None,
        &mut storage,
    )
    .expect("an export");

    // Two tracks at 6,000,000 sum to 12,000,000 against a full scale of
    // 8,388,607, so every sample of every block clips and the overshoot is
    // 12,000,000 - 8,388,607 = 3,611,393. Three frames at 24 into 48 kHz is
    // 2000 samples each, two channels: 3 x 2000 x 2 = 12,000 samples.
    assert!(
        !report.mix.is_clean(),
        "a mix past full scale reported clean"
    );
    assert_eq!(report.mix.clipped, 12_000);
    assert_eq!(report.mix.overshoot, 3_611_393);
    // Counted across every block rather than reported for the last one, which
    // is what a report that forgot each block would give: 4,000.
    assert_ne!(report.mix.clipped, 4_000, "only the last block was counted");
    // And the reel is still committed: clipping is a fact about the sound, not
    // a refusal.
    assert_eq!(storage.commits(), 1);
    let reel = sprw::decode(storage.stored().expect("a reel")).expect("a reel");
    assert_eq!(
        reel.sound().expect("sound").channel(0).expect("a channel")[0],
        8_388_607,
        "the samples in the file are the clipped ones"
    );
}

#[test]
fn a_sidecar_says_what_the_reel_says() {
    // The property that decides whether a sidecar is worth having: it is the
    // *same words*, from the same projection, in the same order, counted from
    // the same nought. Two files that disagreed about what was said would be
    // worse than one file with no captions at all -- so this exports the reel
    // and spots the sidecar from one job, and reads both back.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let asset = MediaAsset::new(Digest::of(b"an interview"), RATE, frames(1000))
        .expect("an asset")
        .with_captions(std::vec![
            media_editor_model::caption::Caption::new(0, 10, 0, "the first thing")
                .expect("a caption"),
            media_editor_model::caption::Caption::new(10, 20, 1, "the second thing")
                .expect("a caption"),
        ])
        .expect("a transcript");
    let media = project.add_media(asset).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(Clip::new(media, 0, frames(40)).expect("a clip")),
            },
        )
        .expect("an insert");
    let job = Job {
        project: &project,
        sequence,
        span: span(0, 20),
        description: described(),
        into: Slot::Vault,
    };

    let mut library = Flat {
        colours: std::vec![(digest_of(&project, media), [30, 0, 90, 255])],
    };
    let mut storage = MemoryStorage::new(1 << 20);
    let mut held = media_editor_model::caption::Held::new(&project);
    export::export(&job, &mut library, None, Some(&mut held), &mut storage).expect("an export");
    let reel = sprw::decode(storage.stored().expect("a reel")).expect("a reel");

    let mut sidecar = std::vec::Vec::new();
    let mut spotting = media_editor_model::caption::Held::new(&project);
    let cues = export::spot(&job, Some(&mut spotting), &mut sidecar).expect("a sidecar");
    assert_eq!(
        cues,
        reel.captions().len(),
        "the two files count differently"
    );

    // `RATE` is 24, so a tick is 1000 ÷ 24 = 125/3 milliseconds:
    //
    //   tick  0 :     0 ÷ 3 =    0        exactly
    //   tick 10 : 1,250 ÷ 3 =  416.666... -> 417
    //   tick 20 : 2,500 ÷ 3 =  833.333... -> 833
    //
    // At speed one the captions are where the source says: 0..10 and 10..20.
    let expected = "WEBVTT\n\
                    \n\
                    1\n\
                    00:00:00.000 --> 00:00:00.417\n\
                    <v Voice 0>the first thing</v>\n\
                    \n\
                    2\n\
                    00:00:00.417 --> 00:00:00.833\n\
                    <v Voice 1>the second thing</v>\n\
                    \n";
    assert_eq!(core::str::from_utf8(&sidecar).expect("text"), expected);

    // And the words are the reel's own, character for character.
    for (index, caption) in reel.captions().iter().enumerate() {
        assert!(
            core::str::from_utf8(&sidecar)
                .expect("text")
                .contains(caption.text()),
            "caption {index} is in the reel and not in the sidecar"
        );
    }
}

#[test]
fn a_sidecar_of_a_programme_with_nothing_said_is_a_signature_and_no_cues() {
    // Not an empty file and not a refusal. A programme nobody speaks in has a
    // transcript of no words, and the honest sidecar for it is a valid WebVTT
    // file that says nothing -- which is what a player needs to be told in
    // order to show nothing rather than to report a broken file.
    let (project, sequence, _) = programme();
    let job = job(&project, sequence, span(0, 4), described());
    let mut sidecar = std::vec::Vec::new();
    assert_eq!(
        export::spot(&job, None, &mut sidecar).expect("a sidecar"),
        0
    );
    assert_eq!(&sidecar, b"WEBVTT\n\n");
}

#[test]
fn an_exported_reel_carries_the_words_the_programme_showed() {
    // The whole chain closing: captions anchored in a recording's source,
    // projected onto the programme by inverting the retime, and written into
    // the exported reel as *its* transcript -- rebased to its own nought,
    // because a reel is measured from its own start.
    let mut project = Project::new();
    let sequence = project.add_sequence(RATE).expect("a sequence");
    let asset = MediaAsset::new(Digest::of(b"an interview"), RATE, frames(1000))
        .expect("an asset")
        .with_captions(std::vec![
            media_editor_model::caption::Caption::new(0, 10, 0, "the first thing")
                .expect("a caption"),
            media_editor_model::caption::Caption::new(10, 20, 0, "the second thing")
                .expect("a caption"),
        ])
        .expect("a transcript");
    let media = project.add_media(asset).expect("room");
    project
        .apply(
            sequence,
            Edit::AddTrack {
                index: 0,
                kind: TrackKind::Video,
            },
        )
        .expect("a track");
    // At half speed, so the words stretch: source 0..10 is programme 0..20.
    project
        .apply(
            sequence,
            Edit::InsertItem {
                track: 0,
                index: 0,
                item: Item::Clip(
                    Clip::new(media, 0, frames(40))
                        .expect("a clip")
                        .with_speed(Rational::new(1, 2).expect("a speed"))
                        .expect("a speed"),
                ),
            },
        )
        .expect("an insert");
    let mut library = Flat {
        colours: std::vec![(digest_of(&project, media), [30, 0, 90, 255])],
    };
    let mut storage = MemoryStorage::new(1 << 20);
    let mut held = media_editor_model::caption::Held::new(&project);
    export::export(
        &Job {
            project: &project,
            sequence,
            // From frame 10, so the rebasing is visible: a caption at
            // programme 20..40 is at 10..30 of a reel that begins at 10.
            span: span(10, 25),
            description: described(),
            into: Slot::Vault,
        },
        &mut library,
        None,
        Some(&mut held),
        &mut storage,
    )
    .expect("an export");

    let reel = sprw::decode(storage.stored().expect("a reel")).expect("a reel");
    let words: std::vec::Vec<(i64, i64, &str)> = reel
        .captions()
        .iter()
        .map(|caption| (caption.from(), caption.to(), caption.text()))
        .collect();
    // Half speed puts "the first thing" at programme 0..20 and "the second"
    // at 20..40. The span is 10..35, so the first is clipped to 10..20 and the
    // second to 20..35 -- and rebased to the reel's nought, 0..10 and 10..25.
    assert_eq!(
        words,
        std::vec![(0, 10, "the first thing"), (10, 25, "the second thing")]
    );
    // And the reel reads them back over its own ranges.
    let spool = Spool::open(&storage.stored().expect("a reel")).expect("a spool");
    assert_eq!(spool.spoken().count(), 2);
    let over = spool
        .captions(&storage.stored().expect("a reel"), 12, 14)
        .expect("a read");
    assert_eq!(over.len(), 1);
    assert_eq!(over[0].text(), "the second thing");
}
