// SPDX-License-Identifier: GPL-3.0-only
//! Host harness for the bounded BusyBox cat FAT16 contract.

#![allow(dead_code)]

#[path = "../src/rust/fat16.rs"]
mod fat16;
#[path = "../src/rust/linux_fat16.rs"]
mod linux_fat16;

static BUSYBOX: &[u8] = include_bytes!(env!("PHIPIA_CAT_BUSYBOX_BINARY"));

#[test]
fn pinned_cat_payload_is_exact() {
    let payload = linux_fat16::validate_cat_payload(BUSYBOX).unwrap();
    assert_eq!(payload.byte_count, linux_fat16::CAT_FILE_BYTES);
    assert_eq!(payload.deterministic, 1);
    assert_eq!(payload.sha256, linux_fat16::CAT_SHA256);
    assert_eq!(linux_fat16::self_test_cat(), 12);

    let mut changed = BUSYBOX.to_vec();
    changed[BUSYBOX.len() / 2] ^= 1;
    assert!(matches!(
        linux_fat16::validate_cat_payload(&changed),
        Err(linux_fat16::Status::PayloadDigest)
    ));
}

#[test]
fn cat_query_is_separate_and_canonical() {
    assert_eq!(
        linux_fat16::make_cat_query().canonical_name,
        *b"CATBOX     "
    );
    assert_ne!(
        linux_fat16::make_cat_query().canonical_name,
        linux_fat16::make_query().canonical_name
    );
    assert_eq!(linux_fat16::CAT_FILE_CLUSTERS, 10);
}

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
fn cat_is_independently_selected_and_required() {
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
    put_entry(
        &mut root,
        2,
        &linux_fat16::CAT_NAME,
        21,
        linux_fat16::CAT_FILE_BYTES,
    );
    let entry = linux_fat16::find_cat_root(
        &root,
        &geometry(),
        &linux_fat16::make_cat_query(),
        linux_fat16::CAT_FILE_BYTES,
    )
    .unwrap();
    assert_eq!(entry.first_cluster, 21);

    root[64] = 0;
    assert!(matches!(
        linux_fat16::find_cat_root(
            &root,
            &geometry(),
            &linux_fat16::make_cat_query(),
            linux_fat16::CAT_FILE_BYTES,
        ),
        Err(linux_fat16::Status::TargetAbsent)
    ));
}
