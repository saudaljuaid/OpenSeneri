// SPDX-License-Identifier: GPL-3.0-only
//! Host harness for the bounded BusyBox FAT16 parser.

// This harness imports the complete inherited module while exercising only the
// new chain API; the kernel crate and inherited standalone test use the rest.
#![allow(dead_code)]

#[path = "../src/rust/fat16.rs"]
mod fat16;
#[path = "../src/rust/linux_fat16.rs"]
mod linux_fat16;

static BUSYBOX: &[u8] = include_bytes!(env!("PHIPIA_BUSYBOX_BINARY"));

fn geometry() -> fat16::Geometry {
    fat16::Geometry {
        total_sectors: 4096,
        root_dir_sectors: 1,
        first_fat_sector: 1,
        fat_sectors: 2,
        first_root_sector: 3,
        first_data_sector: 4,
        data_sectors: 4092,
        cluster_count: 4092,
        namespace_blocks: 4096,
        bytes_per_sector: 4096,
        sectors_per_cluster: 1,
        root_entries: 128,
        media: 0xF8,
    }
}

fn put_entry(block: &mut [u8], index: usize, name: &[u8; 11], cluster: u16, size: u32) {
    let offset = index * 32;
    block[offset..offset + 11].copy_from_slice(name);
    block[offset + 11] = 0x20;
    block[offset + 26..offset + 28].copy_from_slice(&cluster.to_le_bytes());
    block[offset + 28..offset + 32].copy_from_slice(&size.to_le_bytes());
}

#[test]
fn pinned_busybox_payload_is_exact() {
    let payload = linux_fat16::validate_payload(BUSYBOX).unwrap();
    assert_eq!(payload.byte_count, linux_fat16::FILE_BYTES);
    assert_eq!(payload.deterministic, 1);
    assert_eq!(payload.sha256, linux_fat16::BUSYBOX_SHA256);

    for length in [0, 1, BUSYBOX.len() - 1] {
        assert!(matches!(
            linux_fat16::validate_payload(&BUSYBOX[..length]),
            Err(linux_fat16::Status::PayloadLength)
        ));
    }
    let mut changed = BUSYBOX.to_vec();
    changed[BUSYBOX.len() / 2] ^= 1;
    assert!(matches!(
        linux_fat16::validate_payload(&changed),
        Err(linux_fat16::Status::PayloadDigest)
    ));
}

#[test]
fn two_profile_root_is_bounded_and_independently_selected() {
    let mut root = [0u8; fat16::BLOCK_BYTES];
    put_entry(
        &mut root,
        0,
        &linux_fat16::BUSYBOX_NAME,
        2,
        linux_fat16::FILE_BYTES,
    );
    put_entry(
        &mut root,
        1,
        &linux_fat16::UNAME_NAME,
        11,
        linux_fat16::UNAME_FILE_BYTES,
    );

    let echo = linux_fat16::find_root(
        &root,
        &geometry(),
        &linux_fat16::make_query(),
        linux_fat16::FILE_BYTES,
    )
    .unwrap();
    let uname = linux_fat16::find_uname_root(
        &root,
        &geometry(),
        &linux_fat16::make_uname_query(),
        linux_fat16::UNAME_FILE_BYTES,
    )
    .unwrap();
    assert_eq!(echo.first_cluster, 2);
    assert_eq!(uname.first_cluster, 11);

    let mut missing_echo = [0u8; fat16::BLOCK_BYTES];
    put_entry(
        &mut missing_echo,
        0,
        &linux_fat16::UNAME_NAME,
        11,
        linux_fat16::UNAME_FILE_BYTES,
    );
    assert!(matches!(
        linux_fat16::find_root(
            &missing_echo,
            &geometry(),
            &linux_fat16::make_query(),
            linux_fat16::FILE_BYTES,
        ),
        Err(linux_fat16::Status::TargetAbsent)
    ));

    let mut changed_peer = root;
    changed_peer[32 + 28..32 + 32]
        .copy_from_slice(&(linux_fat16::UNAME_FILE_BYTES - 1).to_le_bytes());
    assert!(matches!(
        linux_fat16::find_root(
            &changed_peer,
            &geometry(),
            &linux_fat16::make_query(),
            linux_fat16::FILE_BYTES,
        ),
        Err(linux_fat16::Status::FileSize)
    ));

    let mut unknown = root;
    unknown[32..43].copy_from_slice(b"NOTKNOWN   ");
    assert!(matches!(
        linux_fat16::find_root(
            &unknown,
            &geometry(),
            &linux_fat16::make_query(),
            linux_fat16::FILE_BYTES,
        ),
        Err(linux_fat16::Status::UnsupportedEntry)
    ));
}
