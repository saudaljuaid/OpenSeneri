/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FAT32_FS_H
#define PHIPIA_FAT32_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32.h>

#define PHIPFS_MAX_MOUNTS 2U
#define PHIPFS_MAX_HANDLES 64U
#define PHIPFS_CACHE_ENTRIES 4U
#define PHIPFS_MAX_PATH FAT32_PATH_BYTES
#define PHIPFS_MAX_COMPONENT_BYTES 256U
#define PHIPFS_MAX_DEPTH 16U
#define PHIPFS_MAX_FILE_BYTES UINT32_C(16777216)
#define PHIPFS_MAX_LIST_ENTRIES 64U

enum phipfs_volume {
    PHIPFS_VOLUME_SYSTEM = 0,
    PHIPFS_VOLUME_DATA,
    PHIPFS_VOLUME_COUNT
};

enum phipfs_access {
    PHIPFS_ACCESS_READ = 1U,
    PHIPFS_ACCESS_WRITE = 2U,
    PHIPFS_ACCESS_READ_WRITE = 3U
};

enum phipfs_seek_origin {
    PHIPFS_SEEK_START = 0,
    PHIPFS_SEEK_CURRENT,
    PHIPFS_SEEK_END
};

enum phipfs_status {
    PHIPFS_STATUS_OK = 0,
    PHIPFS_STATUS_ABSENT,
    PHIPFS_STATUS_CORRUPT,
    PHIPFS_STATUS_IO,
    PHIPFS_STATUS_INVALID_ARGUMENT,
    PHIPFS_STATUS_NOT_MOUNTED,
    PHIPFS_STATUS_ALREADY_MOUNTED,
    PHIPFS_STATUS_READ_ONLY,
    PHIPFS_STATUS_NOT_FOUND,
    PHIPFS_STATUS_EXISTS,
    PHIPFS_STATUS_NOT_DIRECTORY,
    PHIPFS_STATUS_IS_DIRECTORY,
    PHIPFS_STATUS_NOT_EMPTY,
    PHIPFS_STATUS_BUSY,
    PHIPFS_STATUS_NO_HANDLES,
    PHIPFS_STATUS_STALE_HANDLE,
    PHIPFS_STATUS_ACCESS,
    PHIPFS_STATUS_RANGE,
    PHIPFS_STATUS_FULL,
    PHIPFS_STATUS_DIRECTORY_FULL,
    PHIPFS_STATUS_NAME,
    PHIPFS_STATUS_PATH,
    PHIPFS_STATUS_WRITEBACK,
    PHIPFS_STATUS_RESET,
    PHIPFS_STATUS_COUNT
};

typedef uint64_t phipfs_handle;
typedef uint64_t phipfs_directory_handle;

struct phipfs_stat {
    uint64_t size;
    uint64_t object_id;
    uint32_t first_cluster;
    uint32_t cluster_count;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t links;
    uint8_t attributes;
    bool directory;
    bool read_only;
};

struct phipfs_list_entry {
    char name[PHIPFS_MAX_COMPONENT_BYTES];
    uint64_t size;
    uint64_t object_id;
    uint16_t mode;
    uint8_t attributes;
    bool directory;
};

struct phipfs_drive_info {
    enum phipfs_volume volume;
    uint32_t volume_id;
    uint64_t total_bytes;
    uint64_t free_bytes;
    bool present;
    bool mounted;
    bool read_only;
    bool healthy;
};

bool phipfs_self_test(size_t *completed_tests);
void phipfs_initialize(void);
enum phipfs_status phipfs_mount(enum phipfs_volume volume);
enum phipfs_status phipfs_unmount(enum phipfs_volume volume);
enum phipfs_status phipfs_sync(enum phipfs_volume volume);
struct phipfs_drive_info phipfs_drive(enum phipfs_volume volume);
uint64_t phipfs_completion_count(enum phipfs_volume volume);
enum phipfs_status phipfs_open(
    enum phipfs_volume volume,
    const char *path,
    enum phipfs_access access,
    phipfs_handle *handle
);
enum phipfs_status phipfs_close(phipfs_handle handle);
enum phipfs_status phipfs_read(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
);
enum phipfs_status phipfs_pread(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
);
enum phipfs_status phipfs_write(
    phipfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
);
enum phipfs_status phipfs_seek(
    phipfs_handle handle,
    int64_t offset,
    enum phipfs_seek_origin origin,
    uint64_t *position
);
enum phipfs_status phipfs_stat_path(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_stat *stat
);
enum phipfs_status phipfs_list(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
);
enum phipfs_status phipfs_directory_open(
    enum phipfs_volume volume,
    const char *path,
    phipfs_directory_handle *handle
);
enum phipfs_status phipfs_directory_read(
    phipfs_directory_handle handle,
    struct phipfs_list_entry *entry,
    bool *present
);
enum phipfs_status phipfs_directory_close(phipfs_directory_handle handle);
enum phipfs_status phipfs_create(enum phipfs_volume volume, const char *path);
enum phipfs_status phipfs_create_mode(enum phipfs_volume volume,
    const char *path, uint16_t mode);
enum phipfs_status phipfs_truncate(
    enum phipfs_volume volume,
    const char *path,
    uint64_t size
);
enum phipfs_status phipfs_mkdir(enum phipfs_volume volume, const char *path);
enum phipfs_status phipfs_rename(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
);
enum phipfs_status phipfs_unlink(enum phipfs_volume volume, const char *path);
enum phipfs_status phipfs_rmdir(enum phipfs_volume volume, const char *path);
enum phipfs_status phipfs_link(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
);
const char *phipfs_status_string(enum phipfs_status status);

#endif
