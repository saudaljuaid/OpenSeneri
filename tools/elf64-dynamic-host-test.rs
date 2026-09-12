// SPDX-License-Identifier: GPL-3.0-only
#![deny(warnings)]
#![allow(dead_code)]

#[path = "../src/rust/elf64_dynamic.rs"]
mod elf64_dynamic;

use elf64_dynamic::{HashStyle, Object, PermissionIntent, Status};

const FILE_BYTES: usize = 0x3000;
const STRTAB: usize = 0x1100;
const SYMTAB: usize = 0x1200;
const HASH: usize = 0x1300;
const RELA: usize = 0x1400;
const JMPREL: usize = 0x1480;
const DYNAMIC: usize = 0x2200;
const DYNAMIC_BYTES: usize = 0x200;

#[derive(Clone, Copy)]
enum FixtureHash {
    SysV,
    Gnu,
}

struct Fixture {
    bytes: Vec<u8>,
    first_load: usize,
    relro: usize,
    dynamic_null: usize,
    hash: usize,
}

#[test]
fn compiled_boundary_controls_are_stable() {
    assert_eq!(Status::Ok as i32, 0);
    assert_eq!(Status::NullArgument as i32, 1);
    assert_eq!(Status::Count as i32, 41);
    assert_eq!(elf64_dynamic::self_test(), 9);
}

fn put_u16(bytes: &mut [u8], offset: usize, value: u16) {
    bytes[offset..offset + 2].copy_from_slice(&value.to_le_bytes());
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn put_i64(bytes: &mut [u8], offset: usize, value: i64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn get_u32(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("u32"))
}

fn get_u64(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(bytes[offset..offset + 8].try_into().expect("u64"))
}

fn program(
    bytes: &mut [u8],
    index: usize,
    kind: u32,
    flags: u32,
    offset: u64,
    address: u64,
    file_size: u64,
    memory_size: u64,
    alignment: u64,
) -> usize {
    let record = 64 + index * 56;
    put_u32(bytes, record, kind);
    put_u32(bytes, record + 4, flags);
    put_u64(bytes, record + 8, offset);
    put_u64(bytes, record + 16, address);
    put_u64(bytes, record + 24, address);
    put_u64(bytes, record + 32, file_size);
    put_u64(bytes, record + 40, memory_size);
    put_u64(bytes, record + 48, alignment);
    record
}

fn dynamic(bytes: &mut [u8], index: usize, tag: i64, value: u64) {
    let record = DYNAMIC + index * 16;
    put_i64(bytes, record, tag);
    put_u64(bytes, record + 8, value);
}

fn add_string(bytes: &mut [u8], cursor: &mut usize, value: &str) -> u64 {
    let result = *cursor - STRTAB;
    bytes[*cursor..*cursor + value.len()].copy_from_slice(value.as_bytes());
    *cursor += value.len();
    bytes[*cursor] = 0;
    *cursor += 1;
    result as u64
}

fn sysv_hash(name: &[u8]) -> u32 {
    let mut hash = 0u32;
    for byte in name {
        hash = hash.wrapping_shl(4).wrapping_add(u32::from(*byte));
        let high = hash & 0xf000_0000;
        if high != 0 {
            hash ^= high >> 24;
        }
        hash &= !high;
    }
    hash
}

fn gnu_hash(name: &[u8]) -> u32 {
    name.iter().fold(5381u32, |hash, byte| {
        hash.wrapping_mul(33).wrapping_add(u32::from(*byte))
    })
}

fn write_hash(bytes: &mut [u8], style: FixtureHash, names: &[&[u8]]) {
    match style {
        FixtureHash::SysV => {
            let buckets = 3usize;
            put_u32(bytes, HASH, buckets as u32);
            put_u32(bytes, HASH + 4, names.len() as u32);
            let bucket_offset = HASH + 8;
            let chain_offset = bucket_offset + buckets * 4;
            let mut tails = [0u32; 3];
            for (symbol, name) in names.iter().enumerate().skip(1) {
                let bucket = sysv_hash(name) as usize % buckets;
                if get_u32(bytes, bucket_offset + bucket * 4) == 0 {
                    put_u32(bytes, bucket_offset + bucket * 4, symbol as u32);
                } else {
                    put_u32(
                        bytes,
                        chain_offset + tails[bucket] as usize * 4,
                        symbol as u32,
                    );
                }
                tails[bucket] = symbol as u32;
            }
        }
        FixtureHash::Gnu => {
            put_u32(bytes, HASH, 1);
            put_u32(bytes, HASH + 4, 1);
            put_u32(bytes, HASH + 8, 1);
            put_u32(bytes, HASH + 12, 5);
            let mut bloom = 0u64;
            for name in names.iter().skip(1) {
                let hash = gnu_hash(name);
                bloom |= (1u64 << (hash % 64)) | (1u64 << ((hash >> 5) % 64));
            }
            put_u64(bytes, HASH + 16, bloom);
            put_u32(bytes, HASH + 24, 1);
            for (index, name) in names.iter().enumerate().skip(1) {
                let mut hash = gnu_hash(name) & !1;
                if index + 1 == names.len() {
                    hash |= 1;
                }
                put_u32(bytes, HASH + 28 + (index - 1) * 4, hash);
            }
        }
    }
}

fn fixture(
    soname: &str,
    needed: &[&str],
    defines_external: bool,
    style: FixtureHash,
    relocations: bool,
) -> Fixture {
    let mut bytes = vec![0u8; FILE_BYTES];
    bytes[..4].copy_from_slice(b"\x7fELF");
    bytes[4..9].copy_from_slice(&[2, 1, 1, 0, 0]);
    put_u16(&mut bytes, 16, 3);
    put_u16(&mut bytes, 18, 62);
    put_u32(&mut bytes, 20, 1);
    put_u64(&mut bytes, 24, 0);
    put_u64(&mut bytes, 32, 64);
    put_u16(&mut bytes, 52, 64);
    put_u16(&mut bytes, 54, 56);
    put_u16(&mut bytes, 56, 6);
    let first_load = program(&mut bytes, 0, 1, 5, 0, 0, 0x1000, 0x1000, 0x1000);
    program(&mut bytes, 1, 1, 4, 0x1000, 0x1000, 0x1000, 0x1000, 0x1000);
    program(&mut bytes, 2, 1, 6, 0x2000, 0x2000, 0x1000, 0x1000, 0x1000);
    program(
        &mut bytes,
        3,
        2,
        6,
        DYNAMIC as u64,
        DYNAMIC as u64,
        DYNAMIC_BYTES as u64,
        DYNAMIC_BYTES as u64,
        8,
    );
    program(&mut bytes, 4, 0x6474_e551, 6, 0, 0, 0, 0, 16);
    let relro = program(
        &mut bytes,
        5,
        0x6474_e552,
        4,
        0x2100,
        0x2100,
        0x100,
        0x100,
        1,
    );

    let mut string_cursor = STRTAB + 1;
    let soname_offset = add_string(&mut bytes, &mut string_cursor, soname);
    let mut needed_offsets = Vec::new();
    for name in needed {
        needed_offsets.push(add_string(&mut bytes, &mut string_cursor, name));
    }
    let external_offset = add_string(&mut bytes, &mut string_cursor, "external");
    let local_offset = add_string(&mut bytes, &mut string_cursor, "local");
    let string_size = string_cursor - STRTAB;

    put_u32(&mut bytes, SYMTAB + 24, external_offset as u32);
    bytes[SYMTAB + 24 + 4] = 0x12;
    put_u16(
        &mut bytes,
        SYMTAB + 24 + 6,
        if defines_external { 1 } else { 0 },
    );
    put_u64(
        &mut bytes,
        SYMTAB + 24 + 8,
        if defines_external { 0x180 } else { 0 },
    );
    put_u64(&mut bytes, SYMTAB + 24 + 16, 8);
    put_u32(&mut bytes, SYMTAB + 48, local_offset as u32);
    bytes[SYMTAB + 48 + 4] = 0x11;
    put_u16(&mut bytes, SYMTAB + 48 + 6, 1);
    put_u64(&mut bytes, SYMTAB + 48 + 8, 0x2180);
    put_u64(&mut bytes, SYMTAB + 48 + 16, 8);
    write_hash(
        &mut bytes,
        style,
        &[&b""[..], &b"external"[..], &b"local"[..]],
    );

    if relocations {
        let relocations = [
            (0x2100, 0u32, 8u32, 0x2188i64),
            (0x2108, 1, 6, 0),
            (0x2110, 2, 1, 5),
            (0x2118, 1, 2, 0),
        ];
        for (index, (offset, symbol, kind, addend)) in relocations.iter().enumerate() {
            let record = RELA + index * 24;
            put_u64(&mut bytes, record, *offset);
            put_u64(
                &mut bytes,
                record + 8,
                (u64::from(*symbol) << 32) | u64::from(*kind),
            );
            put_i64(&mut bytes, record + 16, *addend);
        }
        put_u64(&mut bytes, JMPREL, 0x2120);
        put_u64(&mut bytes, JMPREL + 8, (1u64 << 32) | 7);
    }

    let mut dynamic_index = 0usize;
    for offset in needed_offsets {
        dynamic(&mut bytes, dynamic_index, 1, offset);
        dynamic_index += 1;
    }
    dynamic(&mut bytes, dynamic_index, 14, soname_offset);
    dynamic_index += 1;
    dynamic(&mut bytes, dynamic_index, 5, STRTAB as u64);
    dynamic_index += 1;
    dynamic(&mut bytes, dynamic_index, 10, string_size as u64);
    dynamic_index += 1;
    dynamic(&mut bytes, dynamic_index, 6, SYMTAB as u64);
    dynamic_index += 1;
    dynamic(&mut bytes, dynamic_index, 11, 24);
    dynamic_index += 1;
    dynamic(
        &mut bytes,
        dynamic_index,
        if matches!(style, FixtureHash::SysV) {
            4
        } else {
            0x6fff_fef5
        },
        HASH as u64,
    );
    dynamic_index += 1;
    if relocations {
        dynamic(&mut bytes, dynamic_index, 7, RELA as u64);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 8, 4 * 24);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 9, 24);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 0x6fff_fff9, 1);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 23, JMPREL as u64);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 2, 24);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 20, 7);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 3, 0x2120);
        dynamic_index += 1;
        dynamic(&mut bytes, dynamic_index, 30, 8);
        dynamic_index += 1;
    }
    dynamic(&mut bytes, dynamic_index, 0, 0);
    Fixture {
        bytes,
        first_load,
        relro,
        dynamic_null: DYNAMIC + dynamic_index * 16,
        hash: HASH,
    }
}

fn assert_refused(bytes: &[u8], expected: Status) {
    match elf64_dynamic::parse(bytes) {
        Err(actual) => assert_eq!(actual, expected),
        Ok(_) => panic!("malformed ET_DYN was admitted"),
    }
}

#[test]
fn parses_sysv_and_gnu_hash_objects_and_looks_up_exports() {
    for style in [FixtureHash::SysV, FixtureHash::Gnu] {
        let fixture = fixture("libdep.so", &[], true, style, false);
        let image = elf64_dynamic::parse(&fixture.bytes).expect("parse ET_DYN provider");
        assert_eq!(image.soname.as_bytes(), b"libdep.so");
        assert_eq!(image.symbol_count, 3);
        assert!(
            elf64_dynamic::lookup(&image, &fixture.bytes, b"external")
                .expect("hash lookup")
                .is_some()
        );
        assert!(
            elf64_dynamic::lookup(&image, &fixture.bytes, b"absent")
                .expect("hash miss")
                .is_none()
        );
        assert_eq!(
            image.hash_style,
            if matches!(style, FixtureHash::SysV) {
                HashStyle::SysV
            } else {
                HashStyle::Gnu
            }
        );
    }
}

#[test]
fn rejects_identical_duplicate_export_symbols() {
    let mut fixture = fixture("libdep.so", &[], true, FixtureHash::SysV, false);
    let first = fixture.bytes[SYMTAB + 24..SYMTAB + 48].to_vec();
    fixture.bytes[SYMTAB + 48..SYMTAB + 72].copy_from_slice(&first);
    assert_refused(&fixture.bytes, Status::Symbol);
}

#[test]
fn lookup_rejects_a_hash_table_changed_after_parse() {
    let fixture = fixture("libdep.so", &[], true, FixtureHash::SysV, false);
    let image = elf64_dynamic::parse(&fixture.bytes).expect("parse ET_DYN provider");
    let mut changed = fixture.bytes;
    put_u32(&mut changed, fixture.hash, 0);
    assert_eq!(
        elf64_dynamic::lookup(&image, &changed, b"external"),
        Err(Status::HashTable)
    );
}

#[test]
fn admits_the_linker_pie_flag_but_no_other_flags1_bits() {
    let fixture = fixture("libroot.so", &[], true, FixtureHash::SysV, false);
    let mut changed = fixture.bytes.clone();
    put_i64(&mut changed, fixture.dynamic_null, 0x6fff_fffb);
    put_u64(&mut changed, fixture.dynamic_null + 8, 0x0800_0001);
    elf64_dynamic::parse(&changed).expect("NOW plus PIE flags");

    put_u64(&mut changed, fixture.dynamic_null + 8, 0x0800_0003);
    assert_refused(&changed, Status::DynamicUnsupported);

    changed = fixture.bytes.clone();
    put_i64(&mut changed, fixture.dynamic_null, 30);
    put_u64(&mut changed, fixture.dynamic_null + 8, 0x18);
    assert_refused(&changed, Status::DynamicUnsupported);
}

#[test]
fn admits_a_bounded_zero_filled_relro_tail() {
    let fixture = fixture("libroot.so", &[], true, FixtureHash::SysV, false);
    let mut changed = fixture.bytes.clone();
    put_u64(&mut changed, fixture.relro + 40, 0xf00);
    let image = elf64_dynamic::parse(&changed).expect("zero-filled RELRO tail");
    assert_eq!(
        image.permission_intent(0x2fff),
        Some(PermissionIntent::Read)
    );

    let mut outside_load = changed.clone();
    put_u64(&mut outside_load, fixture.relro + 40, 0xf01);
    assert_refused(&outside_load, Status::ProgramType);

    let mut wrong_offset = changed.clone();
    put_u64(&mut wrong_offset, fixture.relro + 8, 0x2101);
    assert_refused(&wrong_offset, Status::ProgramType);

    let mut file_larger_than_memory = fixture.bytes.clone();
    put_u64(&mut file_larger_than_memory, fixture.relro + 40, 0x80);
    assert_refused(&file_larger_than_memory, Status::ProgramType);
}

#[test]
fn applies_relative_global_absolute_pc32_and_jump_slot_relocations() {
    let root_fixture = fixture("libroot.so", &["libdep.so"], false, FixtureHash::Gnu, true);
    let dependency_fixture = fixture("libdep.so", &[], true, FixtureHash::SysV, false);
    let root = elf64_dynamic::parse(&root_fixture.bytes).expect("parse root");
    let dependency = elf64_dynamic::parse(&dependency_fixture.bytes).expect("parse dependency");
    assert_eq!(root.needed_count, 1);
    assert_eq!(root.needed[0].as_bytes(), b"libdep.so");
    let root_bias = 0x4000_0000u64;
    let dependency_bias = 0x6000_0000u64;
    let scope = [
        Object {
            image: &root,
            file: &root_fixture.bytes,
            load_bias: root_bias,
            tls_offset: 0,
        },
        Object {
            image: &dependency,
            file: &dependency_fixture.bytes,
            load_bias: dependency_bias,
            tls_offset: 0,
        },
    ];
    let mut memory = vec![0u8; root.memory_bytes()];
    let mut wrong_memory = vec![0u8; root.memory_bytes() - 1];
    assert_eq!(
        elf64_dynamic::load_image(&root, &root_fixture.bytes, &mut wrong_memory),
        Err(Status::MemorySize)
    );
    elf64_dynamic::load_image(&root, &root_fixture.bytes, &mut memory).expect("load segments");
    elf64_dynamic::apply_relocations(&root, &root_fixture.bytes, &mut memory, root_bias, &scope)
        .expect("apply admitted relocations");
    assert_eq!(get_u64(&memory, 0x2100), root_bias + 0x2188);
    assert_eq!(get_u64(&memory, 0x2108), dependency_bias + 0x180);
    assert_eq!(get_u64(&memory, 0x2110), root_bias + 0x2185);
    let pc32 = i32::from_le_bytes(memory[0x2118..0x211c].try_into().expect("pc32"));
    assert_eq!(
        i64::from(pc32),
        (dependency_bias + 0x180) as i64 - (root_bias + 0x2118) as i64
    );
    assert_eq!(get_u64(&memory, 0x2120), dependency_bias + 0x180);
    assert_eq!(
        root.permission_intent(0x100),
        Some(PermissionIntent::ReadExecute)
    );
    assert_eq!(root.permission_intent(0x2100), Some(PermissionIntent::Read));
}

#[test]
fn dependency_order_is_needed_stable_bounded_and_cycle_checked() {
    let root_fixture = fixture(
        "libroot.so",
        &["libb.so", "liba.so"],
        false,
        FixtureHash::SysV,
        false,
    );
    let a_fixture = fixture("liba.so", &[], true, FixtureHash::SysV, false);
    let b_fixture = fixture("libb.so", &["liba.so"], true, FixtureHash::Gnu, false);
    let root = elf64_dynamic::parse(&root_fixture.bytes).expect("root");
    let a = elf64_dynamic::parse(&a_fixture.bytes).expect("a");
    let b = elf64_dynamic::parse(&b_fixture.bytes).expect("b");
    let images = [b, a];
    let mut order = [usize::MAX; elf64_dynamic::MAX_DEPENDENCY_OBJECTS];
    let count =
        elf64_dynamic::dependency_order(&root, &images, &mut order).expect("dependency order");
    assert_eq!(&order[..count], &[1, 0]);

    let cycle_a_fixture = fixture("liba.so", &["libb.so"], true, FixtureHash::SysV, false);
    let cycle_a = elf64_dynamic::parse(&cycle_a_fixture.bytes).expect("cycle a");
    assert_eq!(
        elf64_dynamic::dependency_order(&root, &[b, cycle_a], &mut order),
        Err(Status::DependencyCycle)
    );
    assert_eq!(
        elf64_dynamic::dependency_order(&root, &[a], &mut order),
        Err(Status::DependencyMissing)
    );
    assert_eq!(
        elf64_dynamic::dependency_order(&root, &[a, a], &mut order),
        Err(Status::DependencyAmbiguous)
    );
    let oversized = vec![a; elf64_dynamic::MAX_DEPENDENCY_OBJECTS + 1];
    assert_eq!(
        elf64_dynamic::dependency_order(&root, &oversized, &mut order),
        Err(Status::DependencyBound)
    );
}

fn catalog(names: &[&str]) -> Vec<u8> {
    let mut bytes = vec![0u8; elf64_dynamic::CATALOG_BYTES];
    bytes[..8].copy_from_slice(b"PHIPDYN1");
    put_u16(&mut bytes, 8, 1);
    put_u16(&mut bytes, 10, 64);
    put_u32(&mut bytes, 12, elf64_dynamic::CATALOG_BYTES as u32);
    put_u16(&mut bytes, 16, names.len() as u16);
    put_u16(&mut bytes, 18, 96);
    for (index, name) in names.iter().enumerate() {
        let offset = 64 + index * 96;
        bytes[offset..offset + name.len()].copy_from_slice(name.as_bytes());
        bytes[offset + 64..offset + 96].fill((index + 1) as u8);
    }
    bytes
}

#[test]
fn dependency_catalog_is_exact_sorted_and_zero_tailed() {
    let bytes = catalog(&["liba.so", "libb.so"]);
    let parsed = elf64_dynamic::parse_catalog(&bytes).expect("canonical catalog");
    assert_eq!(parsed.entry_count, 2);
    assert_eq!(parsed.entries[0].name.as_bytes(), b"liba.so");
    assert_eq!(parsed.entries[1].sha256, [2; 32]);

    let mut changed = catalog(&["libb.so", "liba.so"]);
    assert!(matches!(
        elf64_dynamic::parse_catalog(&changed),
        Err(Status::Catalog)
    ));
    changed = bytes.clone();
    changed[64 + 8] = b'x';
    assert!(matches!(
        elf64_dynamic::parse_catalog(&changed),
        Err(Status::Catalog)
    ));
    changed = bytes.clone();
    changed[64 + 2 * 96] = 1;
    assert!(matches!(
        elf64_dynamic::parse_catalog(&changed),
        Err(Status::Catalog)
    ));
    assert_eq!(
        elf64_dynamic::parse_catalog(&bytes[..bytes.len() - 1])
            .expect_err("short catalog"),
        Status::Catalog
    );
}

#[test]
fn initial_exec_tls_relocation_uses_defining_object_offset() {
    let mut root_fixture = fixture(
        "libroot.so",
        &["libdep.so"],
        false,
        FixtureHash::SysV,
        true,
    );
    let mut dependency_fixture = fixture(
        "libdep.so",
        &[],
        true,
        FixtureHash::SysV,
        false,
    );
    root_fixture.bytes[SYMTAB + 24 + 4] = 0x16;
    put_u64(
        &mut root_fixture.bytes,
        RELA + 24 + 8,
        (1u64 << 32) | 18,
    );
    put_u64(
        &mut root_fixture.bytes,
        RELA + 3 * 24 + 8,
        (2u64 << 32) | 2,
    );
    put_u64(
        &mut root_fixture.bytes,
        JMPREL + 8,
        (2u64 << 32) | 7,
    );
    put_u16(&mut dependency_fixture.bytes, 56, 7);
    program(
        &mut dependency_fixture.bytes,
        6,
        7,
        4,
        0x2180,
        0x2180,
        4,
        16,
        8,
    );
    put_i64(&mut dependency_fixture.bytes, dependency_fixture.dynamic_null, 30);
    put_u64(
        &mut dependency_fixture.bytes,
        dependency_fixture.dynamic_null + 8,
        0x18,
    );
    dependency_fixture.bytes[SYMTAB + 24 + 4] = 0x16;
    put_u64(&mut dependency_fixture.bytes, SYMTAB + 24 + 8, 0);
    put_u64(&mut dependency_fixture.bytes, SYMTAB + 24 + 16, 4);

    let root = elf64_dynamic::parse(&root_fixture.bytes).expect("TLS consumer");
    let dependency =
        elf64_dynamic::parse(&dependency_fixture.bytes).expect("TLS provider");
    let scope = [
        Object {
            image: &root,
            file: &root_fixture.bytes,
            load_bias: 0x4000_0000,
            tls_offset: 0,
        },
        Object {
            image: &dependency,
            file: &dependency_fixture.bytes,
            load_bias: 0x6000_0000,
            tls_offset: -16,
        },
    ];
    let mut memory = vec![0u8; root.memory_bytes()];
    elf64_dynamic::load_image(&root, &root_fixture.bytes, &mut memory).expect("load");
    elf64_dynamic::apply_relocations(
        &root,
        &root_fixture.bytes,
        &mut memory,
        0x4000_0000,
        &scope,
    )
    .expect("TPOFF64");
    assert_eq!(get_u64(&memory, 0x2108), (-16i64) as u64);
}

#[test]
fn local_static_tls_relocation_uses_the_object_block_offset() {
    let mut local_fixture = fixture("libroot.so", &[], true, FixtureHash::SysV, true);
    put_u16(&mut local_fixture.bytes, 56, 7);
    program(&mut local_fixture.bytes, 6, 7, 4, 0x2180, 0x2180, 4, 16, 8);
    put_u64(&mut local_fixture.bytes, local_fixture.dynamic_null - 8, 0x18);
    put_u64(&mut local_fixture.bytes, RELA + 24 + 8, 18);
    let image = elf64_dynamic::parse(&local_fixture.bytes).expect("local static TLS");
    let scope = [Object {
        image: &image,
        file: &local_fixture.bytes,
        load_bias: 0x4000_0000,
        tls_offset: -32,
    }];
    let mut memory = vec![0u8; image.memory_bytes()];
    elf64_dynamic::load_image(&image, &local_fixture.bytes, &mut memory).expect("load");
    elf64_dynamic::apply_relocations(
        &image,
        &local_fixture.bytes,
        &mut memory,
        0x4000_0000,
        &scope,
    )
    .expect("local TPOFF64");
    assert_eq!(get_u64(&memory, 0x2108), (-32i64) as u64);

    let mut outside_tls = local_fixture.bytes.clone();
    put_i64(&mut outside_tls, RELA + 24 + 16, 16);
    assert_refused(&outside_tls, Status::RelocationTarget);

    let mut missing_tls = fixture("libroot.so", &[], true, FixtureHash::SysV, true);
    put_u64(&mut missing_tls.bytes, RELA + 24 + 8, 18);
    assert_refused(&missing_tls.bytes, Status::RelocationTarget);
}

#[test]
fn lifecycle_requires_runtime_executable_targets_and_preserves_order() {
    let root_fixture = fixture("libroot.so", &[], true, FixtureHash::SysV, false);
    let mut root = elf64_dynamic::parse(&root_fixture.bytes).expect("root");
    root.init = 0x100;
    root.fini = 0x140;
    root.init_array = 0x2200;
    root.init_array_count = 2;
    root.fini_array = 0x2210;
    root.fini_array_count = 2;
    let bias = 0x4000_0000;
    let object = Object {
        image: &root,
        file: &root_fixture.bytes,
        load_bias: bias,
        tls_offset: 0,
    };
    let scope = [object];
    let mut memory = vec![0u8; root.memory_bytes()];
    put_u64(&mut memory, 0x2200, bias + 0x110);
    put_u64(&mut memory, 0x2208, bias + 0x120);
    put_u64(&mut memory, 0x2210, bias + 0x150);
    put_u64(&mut memory, 0x2218, bias + 0x160);
    let mut constructors = [0u64; elf64_dynamic::MAX_LIFECYCLE_FUNCTIONS];
    let mut constructor_count = 0;
    elf64_dynamic::append_initializers(
        &object,
        &memory,
        &scope,
        &mut constructors,
        &mut constructor_count,
    )
    .expect("constructors");
    assert_eq!(
        &constructors[..constructor_count],
        &[bias + 0x100, bias + 0x110, bias + 0x120]
    );
    let mut destructors = [0u64; elf64_dynamic::MAX_LIFECYCLE_FUNCTIONS];
    let mut destructor_count = 0;
    elf64_dynamic::append_finalizers(
        &object,
        &memory,
        &scope,
        &mut destructors,
        &mut destructor_count,
    )
    .expect("destructors");
    assert_eq!(
        &destructors[..destructor_count],
        &[bias + 0x160, bias + 0x150, bias + 0x140]
    );
    put_u64(&mut memory, 0x2200, bias + 0x2100);
    let mut refused_count = 0;
    assert_eq!(
        elf64_dynamic::append_initializers(
            &object,
            &memory,
            &scope,
            &mut constructors,
            &mut refused_count,
        ),
        Err(Status::Lifecycle)
    );
}

#[test]
fn refuses_wx_unsupported_dynamic_bad_hash_and_relocation_targets() {
    let root_fixture = fixture("libroot.so", &["libdep.so"], false, FixtureHash::Gnu, true);
    let mut changed = root_fixture.bytes.clone();
    put_u32(&mut changed, root_fixture.first_load + 4, 7);
    assert_refused(&changed, Status::ProgramFlags);

    changed = root_fixture.bytes.clone();
    put_i64(&mut changed, root_fixture.dynamic_null, 36);
    put_u64(&mut changed, root_fixture.dynamic_null + 8, 0x1500);
    assert_refused(&changed, Status::DynamicUnsupported);

    changed = root_fixture.bytes.clone();
    put_i64(&mut changed, root_fixture.dynamic_null, 5);
    put_u64(&mut changed, root_fixture.dynamic_null + 8, STRTAB as u64);
    assert_refused(&changed, Status::DynamicDuplicate);

    changed = root_fixture.bytes.clone();
    put_u32(
        &mut changed,
        root_fixture.hash + 24,
        elf64_dynamic::MAX_SYMBOLS as u32,
    );
    assert_refused(&changed, Status::HashTable);

    let sysv_fixture = fixture("libroot.so", &[], true, FixtureHash::SysV, false);
    changed = sysv_fixture.bytes.clone();
    put_u32(&mut changed, sysv_fixture.hash + 24, 1);
    assert_refused(&changed, Status::HashTable);

    changed = root_fixture.bytes.clone();
    put_u64(&mut changed, RELA + 8, 5);
    assert_refused(&changed, Status::RelocationType);

    changed = root_fixture.bytes.clone();
    put_u64(&mut changed, SYMTAB + 24 + 8, 1);
    assert_refused(&changed, Status::Symbol);

    changed = root_fixture.bytes.clone();
    put_u64(&mut changed, RELA, 0x100);
    assert_refused(&changed, Status::RelocationTarget);
}

#[test]
fn relocation_arithmetic_refuses_u64_and_pc32_overflow() {
    let mut root_fixture = fixture("libroot.so", &["libdep.so"], false, FixtureHash::SysV, true);
    put_i64(&mut root_fixture.bytes, RELA + 16, i64::MIN);
    let root = elf64_dynamic::parse(&root_fixture.bytes).expect("overflow object parses");
    let dependency_fixture = fixture("libdep.so", &[], true, FixtureHash::SysV, false);
    let dependency = elf64_dynamic::parse(&dependency_fixture.bytes).expect("dependency");
    let scope = [
        Object {
            image: &root,
            file: &root_fixture.bytes,
            load_bias: 0x4000_0000,
            tls_offset: 0,
        },
        Object {
            image: &dependency,
            file: &dependency_fixture.bytes,
            load_bias: 0x6000_0000,
            tls_offset: 0,
        },
    ];
    let mut memory = vec![0u8; root.memory_bytes()];
    elf64_dynamic::load_image(&root, &root_fixture.bytes, &mut memory).expect("load");
    assert_eq!(
        elf64_dynamic::apply_relocations(
            &root,
            &root_fixture.bytes,
            &mut memory,
            0x4000_0000,
            &scope
        ),
        Err(Status::RelocationOverflow)
    );

    let root_fixture = fixture("libroot.so", &["libdep.so"], false, FixtureHash::SysV, true);
    let root = elf64_dynamic::parse(&root_fixture.bytes).expect("pc32 object");
    let far_scope = [
        Object {
            image: &root,
            file: &root_fixture.bytes,
            load_bias: 0x4000_0000,
            tls_offset: 0,
        },
        Object {
            image: &dependency,
            file: &dependency_fixture.bytes,
            load_bias: 0x0000_7fff_0000_0000,
            tls_offset: 0,
        },
    ];
    let mut memory = vec![0u8; root.memory_bytes()];
    elf64_dynamic::load_image(&root, &root_fixture.bytes, &mut memory).expect("load");
    assert_eq!(
        elf64_dynamic::apply_relocations(
            &root,
            &root_fixture.bytes,
            &mut memory,
            0x4000_0000,
            &far_scope
        ),
        Err(Status::RelocationOverflow)
    );
}

fn main() {}
