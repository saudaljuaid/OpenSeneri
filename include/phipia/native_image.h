/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_NATIVE_IMAGE_H
#define PHIPIA_NATIVE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#include <phipia/abi/base.h>

#define NATIVE_MANIFEST_BYTES 1024U
#define NATIVE_MANIFEST_NAME_BYTES 32U
#define NATIVE_MANIFEST_ID_BYTES 16U
#define NATIVE_MANIFEST_PATH_BYTES 16U
#define NATIVE_MANIFEST_ARGUMENTS 8U
#define NATIVE_MANIFEST_ARGUMENT_BYTES 32U
#define NATIVE_ELF_MAX_PROGRAM_HEADERS 32U
#define NATIVE_ELF_MAX_LOAD_SEGMENTS 16U
#define NATIVE_ELF_MAX_FILE_BYTES UINT32_C(16777216)
#define NATIVE_ELF_MIN_ADDRESS UINT64_C(0x0000400000000000)
#define NATIVE_ELF_MAX_ADDRESS UINT64_C(0x0000400100000000)

enum native_image_status {
    NATIVE_IMAGE_OK = 0,
    NATIVE_IMAGE_NULL_ARGUMENT,
    NATIVE_IMAGE_MANIFEST_LENGTH,
    NATIVE_IMAGE_MANIFEST_MAGIC,
    NATIVE_IMAGE_MANIFEST_VERSION,
    NATIVE_IMAGE_MANIFEST_SIZE,
    NATIVE_IMAGE_MANIFEST_RESERVED,
    NATIVE_IMAGE_MANIFEST_TEXT,
    NATIVE_IMAGE_MANIFEST_LIMIT,
    NATIVE_IMAGE_MANIFEST_CAPABILITY,
    NATIVE_IMAGE_MANIFEST_ARGUMENT,
    NATIVE_IMAGE_ELF_LENGTH,
    NATIVE_IMAGE_ELF_MAGIC,
    NATIVE_IMAGE_ELF_IDENTITY,
    NATIVE_IMAGE_ELF_TYPE,
    NATIVE_IMAGE_ELF_MACHINE,
    NATIVE_IMAGE_ELF_HEADER,
    NATIVE_IMAGE_ELF_PROGRAM_TABLE,
    NATIVE_IMAGE_ELF_PROGRAM_TYPE,
    NATIVE_IMAGE_ELF_PROGRAM_FLAGS,
    NATIVE_IMAGE_ELF_FILE_RANGE,
    NATIVE_IMAGE_ELF_LOAD_SIZE,
    NATIVE_IMAGE_ELF_ALIGNMENT,
    NATIVE_IMAGE_ELF_ADDRESS,
    NATIVE_IMAGE_ELF_OVERLAP,
    NATIVE_IMAGE_ELF_ENTRY,
    NATIVE_IMAGE_ELF_STACK,
    NATIVE_IMAGE_ELF_TLS,
    NATIVE_IMAGE_ELF_SECTION_TABLE,
    NATIVE_IMAGE_ELF_RELOCATION,
    NATIVE_IMAGE_DIGEST_MISMATCH,
    NATIVE_IMAGE_STATUS_COUNT
};

struct native_manifest {
    uint32_t valid;
    uint32_t abi_version;
    uint64_t capabilities;
    uint64_t memory_limit;
    uint16_t max_handles;
    uint16_t max_threads;
    uint16_t argument_count;
    uint16_t reserved;
    uint8_t name[NATIVE_MANIFEST_NAME_BYTES];
    uint8_t identifier[NATIVE_MANIFEST_ID_BYTES];
    uint8_t executable[NATIVE_MANIFEST_PATH_BYTES];
    uint8_t executable_sha256[32];
    uint8_t resource_directory[NATIVE_MANIFEST_PATH_BYTES];
    uint8_t data_namespace[NATIVE_MANIFEST_PATH_BYTES];
    uint8_t icon[NATIVE_MANIFEST_PATH_BYTES];
    uint8_t arguments[NATIVE_MANIFEST_ARGUMENTS]
        [NATIVE_MANIFEST_ARGUMENT_BYTES];
    uint8_t dynamic_catalog[NATIVE_MANIFEST_PATH_BYTES];
    uint8_t dynamic_catalog_sha256[32];
};

struct native_elf_segment {
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t mapping_start;
    uint64_t mapping_end;
    uint32_t flags;
    uint32_t reserved;
};

struct native_elf_tls {
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

struct native_validated_image {
    uint32_t valid;
    uint32_t program_header_count;
    uint32_t segment_count;
    uint32_t reserved;
    uint64_t entry;
    uint64_t mapping_start;
    uint64_t mapping_end;
    struct native_elf_tls tls;
    struct native_elf_segment segments[NATIVE_ELF_MAX_LOAD_SEGMENTS];
};

_Static_assert(sizeof(struct native_manifest) == 480U,
    "Rust/C native manifest result changed");
_Static_assert(sizeof(struct native_elf_segment) == 56U,
    "Rust/C native ELF segment changed");
_Static_assert(sizeof(struct native_elf_tls) == 40U,
    "Rust/C native ELF TLS result changed");
_Static_assert(sizeof(struct native_validated_image) == 976U,
    "Rust/C native ELF result changed");

enum native_image_status phipia_native_image_validate(
    const uint8_t *manifest_bytes,
    size_t manifest_length,
    const uint8_t *elf_bytes,
    size_t elf_length,
    struct native_manifest *manifest,
    struct native_validated_image *image
);
enum native_image_status phipia_native_manifest_authenticate(
    const uint8_t *manifest_bytes,
    size_t manifest_length,
    const uint8_t *elf_bytes,
    size_t elf_length,
    struct native_manifest *manifest
);
uint32_t phipia_native_image_self_test(void);

#endif
