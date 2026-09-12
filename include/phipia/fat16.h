/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FAT16_H
#define PHIPIA_FAT16_H

#include <stddef.h>
#include <stdint.h>

#define FAT16_BLOCK_BYTES 4096U
#define FAT16_TOTAL_SECTORS UINT64_C(4096)
#define FAT16_ROOT_ENTRIES 128U
#define FAT16_FILE_BYTES 128U
#define FAT16_FILE_CLUSTER UINT16_C(2)
#define FAT16_PARSER_ROBUSTNESS_CONTROLS 22U
#define FAT16_CANONICAL_NAME_BYTES 11U
#define FAT16_SHA256_BYTES 32U

enum fat16_status {
    FAT16_STATUS_OK = 0,
    FAT16_STATUS_NULL_ARGUMENT,
    FAT16_STATUS_TRUNCATED,
    FAT16_STATUS_BOOT_SIGNATURE,
    FAT16_STATUS_BYTES_PER_SECTOR,
    FAT16_STATUS_SECTORS_PER_CLUSTER,
    FAT16_STATUS_RESERVED_COUNT,
    FAT16_STATUS_FAT_COUNT,
    FAT16_STATUS_TOTAL_SECTORS,
    FAT16_STATUS_FAT_SIZE,
    FAT16_STATUS_ROOT_GEOMETRY,
    FAT16_STATUS_SPAN_OVERFLOW,
    FAT16_STATUS_SPAN_RANGE,
    FAT16_STATUS_FAT_CLASS,
    FAT16_STATUS_MEDIA,
    FAT16_STATUS_FAT_RESERVED,
    FAT16_STATUS_ROOT_END_MISSING,
    FAT16_STATUS_TARGET_ABSENT,
    FAT16_STATUS_TARGET_DUPLICATE,
    FAT16_STATUS_LONG_NAME,
    FAT16_STATUS_DELETED,
    FAT16_STATUS_UNSUPPORTED_ENTRY,
    FAT16_STATUS_NAME_MALFORMED,
    FAT16_STATUS_CLUSTER_RANGE,
    FAT16_STATUS_FAT_ENTRY,
    FAT16_STATUS_MULTI_CLUSTER,
    FAT16_STATUS_FILE_SIZE,
    FAT16_STATUS_CLUSTER_TRANSLATION,
    FAT16_STATUS_TRAILING_STATE,
    FAT16_STATUS_PAYLOAD_LENGTH,
    FAT16_STATUS_PAYLOAD_CONTENT,
    FAT16_STATUS_PAYLOAD_DIGEST,
    FAT16_STATUS_COUNT
};

struct fat16_geometry {
    uint64_t total_sectors;
    uint64_t root_dir_sectors;
    uint64_t first_fat_sector;
    uint64_t fat_sectors;
    uint64_t first_root_sector;
    uint64_t first_data_sector;
    uint64_t data_sectors;
    uint64_t cluster_count;
    uint64_t namespace_blocks;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t root_entries;
    uint32_t media;
};

struct fat16_root_query {
    uint8_t canonical_name[FAT16_CANONICAL_NAME_BYTES];
};

struct fat16_root_entry {
    uint8_t canonical_name[FAT16_CANONICAL_NAME_BYTES];
    uint8_t attribute;
    uint16_t first_cluster;
    uint32_t file_size;
};

struct fat16_fat_state {
    uint16_t media_entry;
    uint16_t reserved_entry;
    uint16_t file_cluster;
    uint16_t file_entry;
};

struct fat16_extent {
    uint16_t cluster;
    uint16_t fat_value;
    uint64_t lba;
    uint32_t file_size;
    uint32_t cluster_bytes;
};

struct fat16_payload {
    uint8_t sha256[FAT16_SHA256_BYTES];
    uint32_t byte_count;
    uint32_t deterministic;
};

enum fat16_status phipia_fat16_parse_bpb(
    const uint8_t *block,
    size_t block_len,
    uint64_t namespace_blocks,
    uint32_t namespace_block_bytes,
    struct fat16_geometry *out
);
enum fat16_status phipia_fat16_make_query(
    const uint8_t *name,
    size_t name_len,
    struct fat16_root_query *out
);
enum fat16_status phipia_fat16_find_root(
    const uint8_t *block,
    size_t block_len,
    const struct fat16_geometry *geometry,
    const struct fat16_root_query *query,
    uint32_t destination_bytes,
    struct fat16_root_entry *out
);
enum fat16_status phipia_fat16_parse_fat(
    const uint8_t *block,
    size_t block_len,
    const struct fat16_geometry *geometry,
    struct fat16_fat_state *out
);
enum fat16_status phipia_fat16_validate_extent(
    const struct fat16_geometry *geometry,
    const struct fat16_root_entry *entry,
    const struct fat16_fat_state *fat,
    struct fat16_extent *out
);
enum fat16_status phipia_fat16_validate_payload(
    const uint8_t *data,
    size_t data_len,
    struct fat16_payload *out
);

#endif
