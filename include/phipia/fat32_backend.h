/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FAT32_BACKEND_H
#define PHIPIA_FAT32_BACKEND_H

#include <phipia/fat32_fs.h>

/* Private VFS backend contract; kernel services use the phipfs_* VFS API. */
bool fat32_backend_self_test(size_t *completed_tests);
void fat32_backend_initialize(void);
enum phipfs_status fat32_backend_mount(enum phipfs_volume volume);
enum phipfs_status fat32_backend_unmount(enum phipfs_volume volume);
enum phipfs_status fat32_backend_sync(enum phipfs_volume volume);
struct phipfs_drive_info fat32_backend_drive(enum phipfs_volume volume);
uint64_t fat32_backend_completion_count(enum phipfs_volume volume);
enum phipfs_status fat32_backend_open(
    enum phipfs_volume volume,
    const char *path,
    enum phipfs_access access,
    phipfs_handle *handle
);
enum phipfs_status fat32_backend_close(phipfs_handle handle);
enum phipfs_status fat32_backend_read(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
);
enum phipfs_status fat32_backend_pread(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
);
enum phipfs_status fat32_backend_write(
    phipfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
);
enum phipfs_status fat32_backend_seek(
    phipfs_handle handle,
    int64_t offset,
    enum phipfs_seek_origin origin,
    uint64_t *position
);
enum phipfs_status fat32_backend_stat_path(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_stat *stat
);
enum phipfs_status fat32_backend_list(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
);
enum phipfs_status fat32_backend_create(
    enum phipfs_volume volume,
    const char *path,
    uint16_t mode
);
enum phipfs_status fat32_backend_truncate(
    enum phipfs_volume volume,
    const char *path,
    uint64_t size
);
enum phipfs_status fat32_backend_mkdir(
    enum phipfs_volume volume,
    const char *path
);
enum phipfs_status fat32_backend_rename(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
);
enum phipfs_status fat32_backend_unlink(
    enum phipfs_volume volume,
    const char *path
);
enum phipfs_status fat32_backend_rmdir(
    enum phipfs_volume volume,
    const char *path
);
enum phipfs_status fat32_backend_link(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
);

#endif
