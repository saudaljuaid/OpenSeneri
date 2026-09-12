// SPDX-License-Identifier: GPL-3.0-only

// The shared parser module also contains the independently exercised uname
// contract. This legacy echo harness intentionally reaches only the echo half.
#![allow(dead_code)]

#[path = "../src/rust/linux_elf64.rs"]
mod linux_elf64;

static BUSYBOX: &[u8] = include_bytes!(env!("PHIPIA_BUSYBOX_BINARY"));

fn changed(offset: usize, value: u8, expected: linux_elf64::Status) {
    let mut image = BUSYBOX.to_vec();
    image[offset] = value;
    assert_eq!(linux_elf64::parse(&image), Err(expected));
}

fn changed_u16(offset: usize, value: u16, expected: linux_elf64::Status) {
    let mut image = BUSYBOX.to_vec();
    image[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
    assert_eq!(linux_elf64::parse(&image), Err(expected));
}

fn changed_u32(offset: usize, value: u32, expected: linux_elf64::Status) {
    let mut image = BUSYBOX.to_vec();
    image[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
    assert_eq!(linux_elf64::parse(&image), Err(expected));
}

fn changed_u64(offset: usize, value: u64, expected: linux_elf64::Status) {
    let mut image = BUSYBOX.to_vec();
    image[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
    assert_eq!(linux_elf64::parse(&image), Err(expected));
}

#[test]
fn measured_busybox_is_the_only_accepted_conjunction() {
    assert_eq!(linux_elf64::Status::Ok as i32, 0);
    assert_eq!(linux_elf64::Status::NullArgument as i32, 1);
    assert_eq!(linux_elf64::ValidatedImage::invalid().valid, 0);
    let image = linux_elf64::parse(BUSYBOX).unwrap();
    assert_eq!(image.valid, 1);
    assert_eq!(image.program_header_count, 5);
    assert_eq!(image.segment_count, 4);
    assert_eq!(image.non_load_count, 1);
    assert_eq!(image.entry, 0x0000_4000_0100_107A);
    assert_eq!(linux_elf64::self_test(), 24);
}

#[test]
fn every_short_file_and_one_long_file_are_refused() {
    for length in 0..BUSYBOX.len() {
        assert_eq!(
            linux_elf64::parse(&BUSYBOX[..length]),
            Err(linux_elf64::Status::Truncated)
        );
    }
    let mut long = BUSYBOX.to_vec();
    long.push(0);
    assert_eq!(
        linux_elf64::parse(&long),
        Err(linux_elf64::Status::FileLength)
    );
}

#[test]
fn malformed_header_and_program_states_are_named() {
    changed(0, 0, linux_elf64::Status::Magic);
    changed(4, 1, linux_elf64::Status::Class);
    changed(5, 2, linux_elf64::Status::Data);
    changed(6, 0, linux_elf64::Status::IdentVersion);
    changed(7, 3, linux_elf64::Status::Abi);
    changed(9, 1, linux_elf64::Status::IdentPadding);
    changed_u16(16, 3, linux_elf64::Status::Type);
    changed_u16(18, 3, linux_elf64::Status::Machine);
    changed_u32(20, 0, linux_elf64::Status::HeaderVersion);
    changed_u32(48, 1, linux_elf64::Status::HeaderFlags);
    changed_u16(52, 63, linux_elf64::Status::HeaderSize);
    changed_u64(32, 0, linux_elf64::Status::ProgramOffset);
    changed_u16(54, 55, linux_elf64::Status::ProgramSize);
    changed_u16(56, 0, linux_elf64::Status::ProgramCount);
    changed_u16(56, 9, linux_elf64::Status::ProgramCount);

    const PH0: usize = 64;
    const PH1: usize = PH0 + 56;
    const PH2: usize = PH1 + 56;
    const PH3: usize = PH2 + 56;
    const PH4: usize = PH3 + 56;

    for unsupported in [2u32, 3, 4, 7, 0x6474_E553] {
        changed_u32(PH0, unsupported, linux_elf64::Status::SegmentType);
    }
    changed_u32(PH1 + 4, 7, linux_elf64::Status::SegmentFlags);
    changed_u64(PH0 + 32, 0, linux_elf64::Status::LoadSize);
    changed_u64(PH3 + 40, 0, linux_elf64::Status::LoadSize);
    changed_u64(PH1 + 8, u64::MAX, linux_elf64::Status::FileRange);
    let mut wrapped_file = BUSYBOX.to_vec();
    wrapped_file[PH1 + 32..PH1 + 40].copy_from_slice(&u64::MAX.to_le_bytes());
    wrapped_file[PH1 + 40..PH1 + 48].copy_from_slice(&u64::MAX.to_le_bytes());
    assert_eq!(
        linux_elf64::parse(&wrapped_file),
        Err(linux_elf64::Status::FileRange)
    );
    changed_u64(PH0 + 48, 3, linux_elf64::Status::Alignment);
    changed_u64(PH0 + 8, 1, linux_elf64::Status::Alignment);
    let mut wrapped_address = BUSYBOX.to_vec();
    wrapped_address[PH0 + 16..PH0 + 24].copy_from_slice(&0xFFFF_FFFF_FFFF_F000u64.to_le_bytes());
    wrapped_address[PH0 + 40..PH0 + 48].copy_from_slice(&0x2000u64.to_le_bytes());
    assert_eq!(
        linux_elf64::parse(&wrapped_address),
        Err(linux_elf64::Status::AddressOverflow)
    );
    changed_u64(
        PH0 + 16,
        0x0000_8000_0000_0000,
        linux_elf64::Status::VirtualAddress,
    );
    changed_u64(
        PH2 + 16,
        0x0000_4000_0100_1000,
        linux_elf64::Status::Overlap,
    );
    changed_u64(24, 0, linux_elf64::Status::Entry);
    changed_u32(PH4 + 4, 7, linux_elf64::Status::Stack);
    changed_u64(PH4 + 32, 1, linux_elf64::Status::Stack);
    changed_u32(PH4, 1, linux_elf64::Status::ProgramCount);
    changed_u64(PH0 + 24, 0, linux_elf64::Status::MeasuredConjunction);
}
