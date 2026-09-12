// SPDX-License-Identifier: GPL-3.0-only
#![deny(warnings)]
#![allow(dead_code)]

#[path = "../src/rust/sha256.rs"]
mod sha256;
#[path = "../src/rust/native_image.rs"]
mod native_image;

fn main() {}

fn application() -> Option<Vec<u8>> {
    let path = std::env::var("PHIPIA_NATIVE_TEST_ELF").ok()?;
    Some(std::fs::read(path).expect("read SDK application"))
}

fn read_u16(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes(bytes[offset..offset + 2].try_into().expect("u16 field"))
}

fn read_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("u32 field"))
}

fn read_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().expect("u64 field"))
}

fn write_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn write_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn write_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn program_headers(bytes: &[u8]) -> Vec<usize> {
    let offset = read_u64(bytes, 32) as usize;
    let count = read_u16(bytes, 56) as usize;
    (0..count).map(|index| offset + index * 56).collect()
}

fn program_header(bytes: &[u8], kind: u32, occurrence: usize) -> usize {
    program_headers(bytes).into_iter()
        .filter(|offset| read_u32(bytes, *offset) == kind)
        .nth(occurrence).expect("required program header")
}

fn assert_rejected(bytes: &[u8], expected: native_image::Status) {
    match native_image::parse_elf(bytes) {
        Err(actual) => assert_eq!(actual, expected),
        Ok(_) => panic!("malformed ELF was admitted"),
    }
}

#[test]
fn linked_sdk_application_is_general_static_elf() {
    let Some(bytes) = application() else { return; };
    let image = native_image::parse_elf(&bytes).expect("validate SDK application");
    assert_eq!(image.segment_count, 3);
    assert_ne!(image.tls.memory_size, 0);
}

#[test]
fn rejects_identity_type_and_program_table_corruption() {
    let Some(bytes) = application() else { return; };
    let mut malformed = bytes.clone();
    malformed[0] = 0;
    assert_rejected(&malformed, native_image::Status::ElfMagic);

    malformed = bytes.clone();
    malformed[7] = 3;
    assert_rejected(&malformed, native_image::Status::ElfIdentity);

    malformed = bytes.clone();
    write_u16(&mut malformed, 16, 3);
    assert_rejected(&malformed, native_image::Status::ElfType);

    malformed = bytes;
    write_u64(&mut malformed, 32, u64::MAX);
    assert_rejected(&malformed, native_image::Status::ElfProgramTable);
}

#[test]
fn rejects_dynamic_wx_and_executable_stack_programs() {
    const PT_LOAD: u32 = 1;
    const PT_DYNAMIC: u32 = 2;
    const PT_GNU_STACK: u32 = 0x6474_E551;
    let Some(bytes) = application() else { return; };
    let load = program_header(&bytes, PT_LOAD, 0);
    let stack = program_header(&bytes, PT_GNU_STACK, 0);
    let mut malformed = bytes.clone();
    write_u32(&mut malformed, load, PT_DYNAMIC);
    assert_rejected(&malformed, native_image::Status::ElfProgramType);

    malformed = bytes.clone();
    write_u32(&mut malformed, load + 4, 7);
    assert_rejected(&malformed, native_image::Status::ElfProgramFlags);

    malformed = bytes;
    write_u32(&mut malformed, stack + 4, 7);
    assert_rejected(&malformed, native_image::Status::ElfStack);
}

#[test]
fn rejects_invalid_file_address_overlap_and_entry_ranges() {
    const PT_LOAD: u32 = 1;
    let Some(bytes) = application() else { return; };
    let first = program_header(&bytes, PT_LOAD, 0);
    let second = program_header(&bytes, PT_LOAD, 1);
    let mut malformed = bytes.clone();
    write_u64(&mut malformed, first + 32, u64::MAX);
    write_u64(&mut malformed, first + 40, u64::MAX);
    assert_rejected(&malformed, native_image::Status::ElfFileRange);

    malformed = bytes.clone();
    write_u64(&mut malformed, first + 16, 0x0000_3fff_ffff_f000);
    write_u64(&mut malformed, first + 24, 0x0000_3fff_ffff_f000);
    assert_rejected(&malformed, native_image::Status::ElfAddress);

    malformed = bytes.clone();
    let first_address = read_u64(&malformed, first + 16);
    write_u64(&mut malformed, second + 16, first_address);
    write_u64(&mut malformed, second + 24, first_address);
    assert_rejected(&malformed, native_image::Status::ElfOverlap);

    malformed = bytes;
    write_u64(&mut malformed, 24, 0x0000_4000_ffff_fff0);
    assert_rejected(&malformed, native_image::Status::ElfEntry);
}

#[test]
fn rejects_relocation_sections_and_malformed_tls() {
    const PT_TLS: u32 = 7;
    let Some(bytes) = application() else { return; };
    let section_offset = read_u64(&bytes, 40) as usize;
    let section_count = read_u16(&bytes, 60) as usize;
    assert!(section_count > 1);
    let mut malformed = bytes.clone();
    write_u32(&mut malformed, section_offset + 64 + 4, 6);
    write_u64(&mut malformed, section_offset + 64 + 32, 1);
    assert_rejected(&malformed, native_image::Status::ElfRelocation);

    let tls = program_header(&bytes, PT_TLS, 0);
    malformed = bytes;
    write_u32(&mut malformed, tls + 4, 6);
    assert_rejected(&malformed, native_image::Status::ElfTls);
}
