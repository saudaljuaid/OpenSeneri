/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_VFS_BACKEND_H
#define PHIPIA_VFS_BACKEND_H

#include <phipia/fat32_fs.h>

/*
 * Concrete filesystem contract owned by the VFS. Backend handles and path
 * cookies never escape vfs.c; callers see only VFS file descriptions and
 * directory iterators.
 */
struct vfs_backend_ops {
    enum phipfs_status (*mount)(enum phipfs_volume volume);
    enum phipfs_status (*unmount)(enum phipfs_volume volume);
    enum phipfs_status (*sync)(enum phipfs_volume volume);
    struct phipfs_drive_info (*drive)(enum phipfs_volume volume);
    uint64_t (*completion_count)(enum phipfs_volume volume);
    enum phipfs_status (*open)(enum phipfs_volume volume, const char *path,
        enum phipfs_access access, phipfs_handle *handle);
    enum phipfs_status (*close)(phipfs_handle handle);
    enum phipfs_status (*read)(phipfs_handle handle, uint8_t *destination,
        size_t capacity, size_t *read_bytes);
    enum phipfs_status (*pread)(phipfs_handle handle, uint8_t *destination,
        size_t capacity, uint64_t offset, size_t *read_bytes);
    enum phipfs_status (*write)(phipfs_handle handle, const uint8_t *source,
        size_t source_bytes, size_t *written_bytes);
    enum phipfs_status (*seek)(phipfs_handle handle, int64_t offset,
        enum phipfs_seek_origin origin, uint64_t *position);
    enum phipfs_status (*stat_path)(enum phipfs_volume volume,
        const char *path, struct phipfs_stat *stat);
    enum phipfs_status (*list)(enum phipfs_volume volume, const char *path,
        struct phipfs_list_entry *entries, size_t capacity,
        size_t *entry_count);
    enum phipfs_status (*directory_open)(enum phipfs_volume volume,
        const char *path, phipfs_handle *handle);
    enum phipfs_status (*directory_read)(phipfs_handle handle,
        struct phipfs_list_entry *entry, bool *present);
    enum phipfs_status (*directory_close)(phipfs_handle handle);
    enum phipfs_status (*create)(enum phipfs_volume volume, const char *path,
        uint16_t mode);
    enum phipfs_status (*truncate)(enum phipfs_volume volume, const char *path,
        uint64_t size);
    enum phipfs_status (*mkdir)(enum phipfs_volume volume, const char *path);
    enum phipfs_status (*mkdir_mode)(enum phipfs_volume volume, const char *path, uint16_t mode);
    enum phipfs_status (*rename)(enum phipfs_volume volume, const char *source,
        const char *destination);
    enum phipfs_status (*unlink)(enum phipfs_volume volume, const char *path);
    enum phipfs_status (*rmdir)(enum phipfs_volume volume, const char *path);
    enum phipfs_status (*link)(enum phipfs_volume volume, const char *source,
        const char *destination);
    bool case_sensitive;
    /* Mutations validate all path components under their transaction lock.
     * A retained transaction must be retried without pre-reading its hidden view. */
    bool validates_mutation_paths;
    enum phipfs_status (*remove)(enum phipfs_volume volume, const char *path);
    enum phipfs_status (*append)(phipfs_handle handle, const uint8_t *source,
        size_t source_bytes, size_t *written_bytes);
    enum phipfs_status (*rename_replace)(enum phipfs_volume volume,
        const char *source, const char *destination);
    enum phipfs_status (*symlink)(enum phipfs_volume volume, const char *path,
        const char *target);
    enum phipfs_status (*readlink)(enum phipfs_volume volume, const char *path,
        uint8_t *output, size_t capacity, size_t *read_bytes);
    enum phipfs_status (*chmod)(enum phipfs_volume volume, const char *path, uint16_t mode);
    enum phipfs_status (*ftruncate)(phipfs_handle handle, uint64_t size);
    enum phipfs_status (*set_times)(enum phipfs_volume volume, const char *path,
        const struct phipfs_times *times);
    enum phipfs_status (*set_xattr)(enum phipfs_volume volume, const char *path,
        const char *name, const uint8_t *value, size_t length, bool remove);
    enum phipfs_status (*get_xattr)(enum phipfs_volume volume, const char *path,
        const char *name, uint8_t *output, size_t capacity, size_t *length);
    /* Metadata and handle must come from the same protected inode lookup. */
    enum phipfs_status (*open_with_stat)(enum phipfs_volume volume, const char *path,
        enum phipfs_access access, phipfs_handle *handle, struct phipfs_stat *stat);
    enum phipfs_status (*directory_open_with_stat)(enum phipfs_volume volume,
        const char *path, phipfs_handle *handle, struct phipfs_stat *stat);
};

#endif
