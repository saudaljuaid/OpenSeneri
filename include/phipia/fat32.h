/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FAT32_H
#define PHIPIA_FAT32_H

#include <stddef.h>
#include <stdint.h>

#define FAT32_BOOT_BYTES 512U
#define FAT32_CLUSTER_BYTES 512U
#define FAT32_DIRECTORY_ENTRY_BYTES 32U
#define FAT32_SHORT_NAME_BYTES 11U
#define FAT32_PATH_BYTES 127U
#define FAT32_EOC_MIN UINT32_C(0x0FFFFFF8)
#define FAT32_EOC UINT32_C(0x0FFFFFFF)
#define FAT32_BAD UINT32_C(0x0FFFFFF7)
#define FAT32_SYSTEM_VOLUME_ID UINT32_C(0x20000001)
#define FAT32_DATA_VOLUME_ID UINT32_C(0x20000002)

enum fat32_status {
    FAT32_STATUS_OK = 0,
    FAT32_STATUS_NULL_ARGUMENT,
    FAT32_STATUS_TRUNCATED,
    FAT32_STATUS_BOOT_SIGNATURE,
    FAT32_STATUS_BYTES_PER_SECTOR,
    FAT32_STATUS_SECTORS_PER_CLUSTER,
    FAT32_STATUS_RESERVED_COUNT,
    FAT32_STATUS_FAT_COUNT,
    FAT32_STATUS_LEGACY_GEOMETRY,
    FAT32_STATUS_TOTAL_SECTORS,
    FAT32_STATUS_FAT_SIZE,
    FAT32_STATUS_SPAN_OVERFLOW,
    FAT32_STATUS_SPAN_RANGE,
    FAT32_STATUS_FAT_CLASS,
    FAT32_STATUS_FAT_CAPACITY,
    FAT32_STATUS_ROOT_CLUSTER,
    FAT32_STATUS_FSINFO_SECTOR,
    FAT32_STATUS_BACKUP_SECTOR,
    FAT32_STATUS_FSINFO_SIGNATURE,
    FAT32_STATUS_FSINFO_HINT,
    FAT32_STATUS_FAT_MISMATCH,
    FAT32_STATUS_FAT_RESERVED,
    FAT32_STATUS_CLUSTER_RANGE,
    FAT32_STATUS_CLUSTER_FREE,
    FAT32_STATUS_CLUSTER_RESERVED,
    FAT32_STATUS_CLUSTER_BAD,
    FAT32_STATUS_DIRECTORY_ENTRY,
    FAT32_STATUS_LONG_NAME_UNSUPPORTED,
    FAT32_STATUS_LONG_NAME_MALFORMED,
    FAT32_STATUS_LONG_NAME_ENCODING,
    FAT32_STATUS_PATH_EMPTY,
    FAT32_STATUS_PATH_ABSOLUTE,
    FAT32_STATUS_PATH_TOO_LONG,
    FAT32_STATUS_PATH_MALFORMED,
    FAT32_STATUS_COMPONENT_TOO_LONG,
    FAT32_STATUS_NAME_MALFORMED,
    FAT32_STATUS_ABOVE_ROOT,
    FAT32_STATUS_COUNT
};

struct fat32_geometry {
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t fat_copies;
    uint64_t total_sectors;
    uint64_t fat_sectors;
    uint64_t first_fat_sector;
    uint64_t first_data_sector;
    uint64_t cluster_count;
    uint32_t maximum_cluster;
    uint32_t root_cluster;
    uint32_t fsinfo_sector;
    uint32_t backup_boot_sector;
    uint32_t volume_id;
    uint8_t volume_label[FAT32_SHORT_NAME_BYTES];
    uint32_t valid;
};

struct fat32_fsinfo {
    uint32_t free_hint;
    uint32_t next_hint;
    uint32_t free_hint_valid;
    uint32_t next_hint_valid;
};

enum fat32_entry_kind {
    FAT32_ENTRY_INVALID = 0,
    FAT32_ENTRY_END,
    FAT32_ENTRY_DELETED,
    FAT32_ENTRY_ORDINARY
};

struct fat32_directory_entry {
    uint8_t short_name[FAT32_SHORT_NAME_BYTES];
    uint8_t attributes;
    uint8_t kind;
    uint16_t reserved;
    uint32_t first_cluster;
    uint32_t size;
    uint32_t valid;
};

enum fat32_name_kind {
    FAT32_NAME_INVALID = 0,
    FAT32_NAME_CURRENT,
    FAT32_NAME_PARENT,
    FAT32_NAME_ORDINARY
};

struct fat32_name {
    uint8_t canonical[FAT32_SHORT_NAME_BYTES];
    uint8_t kind;
    uint8_t reserved[4];
};

enum fat32_status phipia_fat32_parse_bpb(
    const uint8_t *block,
    size_t block_len,
    uint64_t namespace_blocks,
    uint32_t namespace_block_bytes,
    struct fat32_geometry *out
);
enum fat32_status phipia_fat32_parse_fsinfo(
    const uint8_t *block,
    size_t block_len,
    const struct fat32_geometry *geometry,
    struct fat32_fsinfo *out
);
enum fat32_status phipia_fat32_validate_fat_pair(
    const uint8_t *first,
    size_t first_len,
    const uint8_t *second,
    size_t second_len,
    const struct fat32_geometry *geometry
);
enum fat32_status phipia_fat32_classify_cluster(
    uint32_t value,
    const struct fat32_geometry *geometry,
    uint32_t *out
);
enum fat32_status phipia_fat32_parse_component(
    const uint8_t *component,
    size_t component_len,
    struct fat32_name *out
);
enum fat32_status phipia_fat32_validate_path(
    const uint8_t *path,
    size_t path_len,
    uint32_t *component_count
);
enum fat32_status phipia_fat32_parse_directory_entry(
    const uint8_t *entry,
    size_t entry_len,
    struct fat32_directory_entry *out
);

#endif
