// SPDX-License-Identifier: GPL-3.0-only
//! Host harness for the measured BusyBox uname ELF64 conjunction.

#![allow(dead_code)]

#[path = "../src/rust/linux_elf64.rs"]
mod linux_elf64;

static BUSYBOX: &[u8] = include_bytes!(env!("PHIPIA_UNAME_BUSYBOX_BINARY"));

#[test]
fn measured_uname_busybox_is_the_only_accepted_conjunction() {
    let image = linux_elf64::parse_uname(BUSYBOX).unwrap();
    assert_eq!(image.valid, 1);
    assert_eq!(image.program_header_count, 5);
    assert_eq!(image.segment_count, 4);
    assert_eq!(image.non_load_count, 1);
    assert_eq!(image.entry, 0x0000_4000_0100_107A);
    assert_eq!(linux_elf64::self_test_uname(), 24);

    assert!(matches!(
        linux_elf64::parse(BUSYBOX),
        Err(linux_elf64::Status::FileLength)
    ));
}

#[test]
fn uname_header_mutations_are_refused() {
    let mut changed = BUSYBOX.to_vec();
    changed[4] = 1;
    assert_eq!(
        linux_elf64::parse_uname(&changed),
        Err(linux_elf64::Status::Class)
    );

    changed = BUSYBOX.to_vec();
    changed[64 + 56 + 4] |= 2;
    assert_eq!(
        linux_elf64::parse_uname(&changed),
        Err(linux_elf64::Status::SegmentFlags)
    );

    changed = BUSYBOX.to_vec();
    changed.push(0);
    assert_eq!(
        linux_elf64::parse_uname(&changed),
        Err(linux_elf64::Status::FileLength)
    );
}
