/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_ELF64_DYNAMIC_H
#define PHIPIA_ELF64_DYNAMIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ELF64_DYNAMIC_MAX_LOAD_SEGMENTS 16U
#define ELF64_DYNAMIC_MAX_NEEDED 16U
#define ELF64_DYNAMIC_MAX_OBJECTS 16U
#define ELF64_DYNAMIC_CATALOG_BYTES 2048U
#define ELF64_DYNAMIC_MAX_LIFECYCLE_FUNCTIONS 256U
#define ELF64_DYNAMIC_MAX_PREPARATION_BYTES UINT64_C(16777216)

enum elf64_dynamic_status {
    ELF64_DYNAMIC_OK = 0,
    ELF64_DYNAMIC_NULL_ARGUMENT,
    ELF64_DYNAMIC_LENGTH,
    ELF64_DYNAMIC_MAGIC,
    ELF64_DYNAMIC_IDENTITY,
    ELF64_DYNAMIC_TYPE,
    ELF64_DYNAMIC_MACHINE,
    ELF64_DYNAMIC_HEADER,
    ELF64_DYNAMIC_PROGRAM_TABLE,
    ELF64_DYNAMIC_PROGRAM_TYPE,
    ELF64_DYNAMIC_PROGRAM_FLAGS,
    ELF64_DYNAMIC_FILE_RANGE,
    ELF64_DYNAMIC_LOAD_SIZE,
    ELF64_DYNAMIC_ALIGNMENT,
    ELF64_DYNAMIC_ADDRESS,
    ELF64_DYNAMIC_OVERLAP,
    ELF64_DYNAMIC_ENTRY,
    ELF64_DYNAMIC_STACK,
    ELF64_DYNAMIC_SEGMENT,
    ELF64_DYNAMIC_ENTRY_METADATA,
    ELF64_DYNAMIC_DUPLICATE,
    ELF64_DYNAMIC_MISSING,
    ELF64_DYNAMIC_UNSUPPORTED,
    ELF64_DYNAMIC_STRING_TABLE,
    ELF64_DYNAMIC_STRING,
    ELF64_DYNAMIC_SYMBOL_TABLE,
    ELF64_DYNAMIC_SYMBOL,
    ELF64_DYNAMIC_HASH_TABLE,
    ELF64_DYNAMIC_RELOCATION_TABLE,
    ELF64_DYNAMIC_RELOCATION_TYPE,
    ELF64_DYNAMIC_RELOCATION_TARGET,
    ELF64_DYNAMIC_RELOCATION_OVERFLOW,
    ELF64_DYNAMIC_MEMORY_SIZE,
    ELF64_DYNAMIC_UNDEFINED_SYMBOL,
    ELF64_DYNAMIC_DEPENDENCY_MISSING,
    ELF64_DYNAMIC_DEPENDENCY_AMBIGUOUS,
    ELF64_DYNAMIC_DEPENDENCY_CYCLE,
    ELF64_DYNAMIC_DEPENDENCY_BOUND,
    ELF64_DYNAMIC_AUTHENTICATION,
    ELF64_DYNAMIC_CATALOG,
    ELF64_DYNAMIC_LIFECYCLE,
    ELF64_DYNAMIC_STATUS_COUNT
};

enum elf64_dynamic_hash_style {
    ELF64_DYNAMIC_HASH_SYSV = 0,
    ELF64_DYNAMIC_HASH_GNU
};

struct elf64_dynamic_name {
    uint8_t bytes[64];
    uint8_t length;
};

struct elf64_dynamic_segment {
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t mapping_start;
    uint64_t mapping_end;
    uint32_t flags;
    uint32_t reserved;
};

struct elf64_dynamic_tls {
    uint64_t file_offset;
    uint64_t virtual_address;
    uint64_t file_size;
    uint64_t memory_size;
    uint64_t alignment;
};

struct elf64_dynamic_catalog_entry {
    struct elf64_dynamic_name name;
    uint8_t sha256[32];
};

struct elf64_dynamic_catalog {
    struct elf64_dynamic_catalog_entry entries[ELF64_DYNAMIC_MAX_OBJECTS];
    uint8_t entry_count;
    uint8_t reserved[7];
};

struct elf64_dynamic_image {
    uint64_t entry;
    uint64_t mapping_start;
    uint64_t mapping_end;
    struct elf64_dynamic_segment segments[ELF64_DYNAMIC_MAX_LOAD_SEGMENTS];
    uint8_t segment_count;
    struct elf64_dynamic_name soname;
    struct elf64_dynamic_name needed[ELF64_DYNAMIC_MAX_NEEDED];
    uint8_t needed_count;
    uint64_t string_address;
    uint64_t string_size;
    uint64_t symbol_address;
    uint32_t symbol_count;
    enum elf64_dynamic_hash_style hash_style;
    uint64_t sysv_hash_address;
    uint64_t gnu_hash_address;
    uint64_t rela_address;
    uint32_t rela_count;
    uint64_t plt_rela_address;
    uint32_t plt_rela_count;
    uint32_t relative_count;
    uint64_t relro_start;
    uint64_t relro_end;
    uint64_t init;
    uint64_t fini;
    uint64_t init_array;
    uint32_t init_array_count;
    uint64_t fini_array;
    uint32_t fini_array_count;
    struct elf64_dynamic_tls tls;
    bool bind_now;
};

struct elf64_dynamic_prepared_object {
    const struct elf64_dynamic_image *image;
    const uint8_t *input;
    size_t input_length;
    uint8_t *memory;
    size_t memory_length;
    uint64_t load_bias;
    int64_t tls_offset;
};

struct elf64_dynamic_lifecycle {
    uint64_t constructors[ELF64_DYNAMIC_MAX_LIFECYCLE_FUNCTIONS];
    uint64_t destructors[ELF64_DYNAMIC_MAX_LIFECYCLE_FUNCTIONS];
    uint16_t constructor_count;
    uint16_t destructor_count;
    uint32_t reserved;
};

_Static_assert(ELF64_DYNAMIC_STATUS_COUNT == 41,
    "Rust/C dynamic ELF status count changed");
_Static_assert(sizeof(struct elf64_dynamic_name) == 65U,
    "Rust/C dynamic ELF name changed");
_Static_assert(sizeof(struct elf64_dynamic_segment) == 56U,
    "Rust/C dynamic ELF segment changed");
_Static_assert(sizeof(struct elf64_dynamic_tls) == 40U,
    "Rust/C dynamic ELF TLS changed");
_Static_assert(sizeof(struct elf64_dynamic_catalog) == 1560U,
    "Rust/C dynamic ELF catalog changed");
_Static_assert(sizeof(struct elf64_dynamic_prepared_object) == 56U,
    "Rust/C dynamic ELF prepared object changed");
_Static_assert(sizeof(struct elf64_dynamic_lifecycle) == 4104U,
    "Rust/C dynamic ELF lifecycle changed");
_Static_assert(sizeof(struct elf64_dynamic_image) == 2224U,
    "Rust/C dynamic ELF image changed");
_Static_assert(offsetof(struct elf64_dynamic_image, segment_count) == 920U,
    "Rust/C dynamic ELF segment count moved");
_Static_assert(offsetof(struct elf64_dynamic_image, needed_count) == 2026U,
    "Rust/C dynamic ELF dependency count moved");
_Static_assert(offsetof(struct elf64_dynamic_image, relro_start) == 2112U,
    "Rust/C dynamic ELF RELRO intent moved");
_Static_assert(offsetof(struct elf64_dynamic_image, tls) == 2176U,
    "Rust/C dynamic ELF TLS moved");
_Static_assert(offsetof(struct elf64_dynamic_image, bind_now) == 2216U,
    "Rust/C dynamic ELF bind policy moved");

enum elf64_dynamic_status phipia_elf64_dynamic_parse(
    const uint8_t *input,
    size_t input_length,
    struct elf64_dynamic_image *image
);
enum elf64_dynamic_status phipia_elf64_dynamic_prepare(
    const struct elf64_dynamic_image *image,
    const uint8_t *input,
    size_t input_length,
    uint8_t *memory,
    size_t memory_length,
    uint64_t load_bias
);
enum elf64_dynamic_status phipia_elf64_dynamic_catalog_authenticate(
    const uint8_t *input,
    size_t input_length,
    const uint8_t expected_sha256[32],
    struct elf64_dynamic_catalog *catalog
);
enum elf64_dynamic_status phipia_elf64_dynamic_object_authenticate(
    const uint8_t *input,
    size_t input_length,
    const uint8_t expected_sha256[32],
    struct elf64_dynamic_image *image
);
enum elf64_dynamic_status phipia_elf64_dynamic_dependency_order(
    const struct elf64_dynamic_image *root,
    const struct elf64_dynamic_image *libraries,
    size_t library_count,
    uint8_t *order,
    size_t order_capacity,
    size_t *order_count
);
enum elf64_dynamic_status phipia_elf64_dynamic_relocate_scope(
    const struct elf64_dynamic_prepared_object *objects,
    size_t object_count
);
enum elf64_dynamic_status phipia_elf64_dynamic_lifecycle(
    const struct elf64_dynamic_prepared_object *objects,
    size_t object_count,
    struct elf64_dynamic_lifecycle *lifecycle
);
uint32_t phipia_elf64_dynamic_self_test(void);

#endif
