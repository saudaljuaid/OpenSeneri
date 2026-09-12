/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ELF64_H
#define PHIPIA_ELF64_H

#include <stddef.h>
#include <stdint.h>

#define ELF64_FILE_BYTES 128U
#define ELF64_SEGMENT_COUNT 1U
#define ELF64_PAGE_BYTES UINT64_C(4096)
#define ELF64_LOAD_ADDRESS UINT64_C(0x0000400000000000)
#define ELF64_ENTRY_ADDRESS UINT64_C(0x0000400000000078)
#define ELF64_PROOF_RESULT UINT32_C(0x53415037)
#define ELF64_PROOF_VECTOR UINT8_C(0x81)
#define ELF64_PARSER_ROBUSTNESS_CONTROLS 34U

enum elf64_status {
    ELF64_STATUS_OK = 0,
    ELF64_STATUS_NULL_ARGUMENT,
    ELF64_STATUS_TRUNCATED,
    ELF64_STATUS_FILE_LENGTH,
    ELF64_STATUS_MAGIC,
    ELF64_STATUS_CLASS,
    ELF64_STATUS_DATA,
    ELF64_STATUS_IDENT_VERSION,
    ELF64_STATUS_ABI,
    ELF64_STATUS_IDENT_PADDING,
    ELF64_STATUS_TYPE,
    ELF64_STATUS_MACHINE,
    ELF64_STATUS_HEADER_VERSION,
    ELF64_STATUS_HEADER_FLAGS,
    ELF64_STATUS_HEADER_SIZE,
    ELF64_STATUS_PROGRAM_OFFSET,
    ELF64_STATUS_PROGRAM_SIZE,
    ELF64_STATUS_PROGRAM_COUNT,
    ELF64_STATUS_SECTION_TABLE,
    ELF64_STATUS_PROGRAM_TABLE,
    ELF64_STATUS_SEGMENT_TYPE,
    ELF64_STATUS_SEGMENT_FLAGS,
    ELF64_STATUS_FILE_RANGE,
    ELF64_STATUS_LOAD_SIZE,
    ELF64_STATUS_ALIGNMENT,
    ELF64_STATUS_VIRTUAL_ADDRESS,
    ELF64_STATUS_ADDRESS_OVERFLOW,
    ELF64_STATUS_ENTRY,
    ELF64_STATUS_PHYSICAL_ADDRESS,
    ELF64_STATUS_CODE
};

_Static_assert(ELF64_STATUS_OK == 0, "Rust/C ELF64 status 0 changed");
_Static_assert(ELF64_STATUS_NULL_ARGUMENT == 1,
    "Rust/C ELF64 status 1 changed");
_Static_assert(ELF64_STATUS_TRUNCATED == 2,
    "Rust/C ELF64 status 2 changed");
_Static_assert(ELF64_STATUS_FILE_LENGTH == 3,
    "Rust/C ELF64 status 3 changed");
_Static_assert(ELF64_STATUS_MAGIC == 4, "Rust/C ELF64 status 4 changed");
_Static_assert(ELF64_STATUS_CLASS == 5, "Rust/C ELF64 status 5 changed");
_Static_assert(ELF64_STATUS_DATA == 6, "Rust/C ELF64 status 6 changed");
_Static_assert(ELF64_STATUS_IDENT_VERSION == 7,
    "Rust/C ELF64 status 7 changed");
_Static_assert(ELF64_STATUS_ABI == 8, "Rust/C ELF64 status 8 changed");
_Static_assert(ELF64_STATUS_IDENT_PADDING == 9,
    "Rust/C ELF64 status 9 changed");
_Static_assert(ELF64_STATUS_TYPE == 10, "Rust/C ELF64 status 10 changed");
_Static_assert(ELF64_STATUS_MACHINE == 11,
    "Rust/C ELF64 status 11 changed");
_Static_assert(ELF64_STATUS_HEADER_VERSION == 12,
    "Rust/C ELF64 status 12 changed");
_Static_assert(ELF64_STATUS_HEADER_FLAGS == 13,
    "Rust/C ELF64 status 13 changed");
_Static_assert(ELF64_STATUS_HEADER_SIZE == 14,
    "Rust/C ELF64 status 14 changed");
_Static_assert(ELF64_STATUS_PROGRAM_OFFSET == 15,
    "Rust/C ELF64 status 15 changed");
_Static_assert(ELF64_STATUS_PROGRAM_SIZE == 16,
    "Rust/C ELF64 status 16 changed");
_Static_assert(ELF64_STATUS_PROGRAM_COUNT == 17,
    "Rust/C ELF64 status 17 changed");
_Static_assert(ELF64_STATUS_SECTION_TABLE == 18,
    "Rust/C ELF64 status 18 changed");
_Static_assert(ELF64_STATUS_PROGRAM_TABLE == 19,
    "Rust/C ELF64 status 19 changed");
_Static_assert(ELF64_STATUS_SEGMENT_TYPE == 20,
    "Rust/C ELF64 status 20 changed");
_Static_assert(ELF64_STATUS_SEGMENT_FLAGS == 21,
    "Rust/C ELF64 status 21 changed");
_Static_assert(ELF64_STATUS_FILE_RANGE == 22,
    "Rust/C ELF64 status 22 changed");
_Static_assert(ELF64_STATUS_LOAD_SIZE == 23,
    "Rust/C ELF64 status 23 changed");
_Static_assert(ELF64_STATUS_ALIGNMENT == 24,
    "Rust/C ELF64 status 24 changed");
_Static_assert(ELF64_STATUS_VIRTUAL_ADDRESS == 25,
    "Rust/C ELF64 status 25 changed");
_Static_assert(ELF64_STATUS_ADDRESS_OVERFLOW == 26,
    "Rust/C ELF64 status 26 changed");
_Static_assert(ELF64_STATUS_ENTRY == 27, "Rust/C ELF64 status 27 changed");
_Static_assert(ELF64_STATUS_PHYSICAL_ADDRESS == 28,
    "Rust/C ELF64 status 28 changed");
_Static_assert(ELF64_STATUS_CODE == 29, "Rust/C ELF64 status 29 changed");

struct elf64_validated_image {
    uint32_t valid;
    uint32_t segment_count;
    uint16_t elf_type;
    uint16_t machine;
    uint32_t program_flags;
    uint64_t entry;
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
    uint64_t mapping_start;
    uint64_t mapping_end;
    uint8_t code[8];
};

uint32_t phipia_elf64_self_test(void);
enum elf64_status phipia_elf64_parse(
    const uint8_t *input,
    size_t input_len,
    struct elf64_validated_image *out
);

#endif
