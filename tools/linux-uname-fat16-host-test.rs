// SPDX-License-Identifier: GPL-3.0-only
//! Host harness for the bounded BusyBox uname FAT16 contract.

#![allow(dead_code)]

#[path = "../src/rust/fat16.rs"]
mod fat16;
#[path = "../src/rust/linux_fat16.rs"]
mod linux_fat16;

static BUSYBOX: &[u8] = include_bytes!(env!("PHIPIA_UNAME_BUSYBOX_BINARY"));

#[test]
fn pinned_uname_payload_is_exact() {
    let payload = linux_fat16::validate_uname_payload(BUSYBOX).unwrap();
    assert_eq!(payload.byte_count, linux_fat16::UNAME_FILE_BYTES);
    assert_eq!(payload.deterministic, 1);
    assert_eq!(payload.sha256, linux_fat16::UNAME_SHA256);
    assert_eq!(linux_fat16::self_test_uname(), 12);

    let mut changed = BUSYBOX.to_vec();
    changed[BUSYBOX.len() / 2] ^= 1;
    assert!(matches!(
        linux_fat16::validate_uname_payload(&changed),
        Err(linux_fat16::Status::PayloadDigest)
    ));
}

#[test]
fn uname_query_is_separate_and_canonical() {
    assert_eq!(
        linux_fat16::make_uname_query().canonical_name,
        *b"UNAMEBOX   "
    );
    assert_ne!(
        linux_fat16::make_uname_query().canonical_name,
        linux_fat16::make_query().canonical_name
    );
    assert_eq!(linux_fat16::UNAME_FILE_CLUSTERS, 10);
}
