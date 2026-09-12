/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_EXT4_FS_H
#define PHIPIA_EXT4_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32_fs.h>

#define PHIPIA_EXT4_FILE_REGULAR 1U
#define PHIPIA_EXT4_FILE_DIRECTORY 2U
#define PHIPIA_EXT4_FILE_SYMLINK 3U

enum phipia_ext4_status {
    PHIPIA_EXT4_STATUS_OK = 0,
    PHIPIA_EXT4_STATUS_NULL_ARGUMENT,
    PHIPIA_EXT4_STATUS_VOLUME,
    PHIPIA_EXT4_STATUS_IO,
    PHIPIA_EXT4_STATUS_INVALID,
    PHIPIA_EXT4_STATUS_NOT_FOUND,
    PHIPIA_EXT4_STATUS_NOT_DIRECTORY,
    PHIPIA_EXT4_STATUS_IS_DIRECTORY,
    PHIPIA_EXT4_STATUS_RANGE,
    PHIPIA_EXT4_STATUS_SPECIAL,
    PHIPIA_EXT4_STATUS_EXISTS,
    PHIPIA_EXT4_STATUS_NOT_EMPTY,
    PHIPIA_EXT4_STATUS_COUNT
};

struct phipia_ext4_metadata {
    uint64_t inode;
    uint64_t size;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t links;
    uint8_t file_type;
    uint8_t reserved[7];
};

struct phipia_ext4_directory_entry {
    struct phipia_ext4_metadata metadata;
    uint16_t name_length;
    uint8_t name[255];
    uint8_t reserved;
};

struct phipia_ext4_identity {
    uint8_t label[16];
    uint8_t uuid[16];
    uint32_t recovered_transactions;
    uint32_t replayed_blocks;
    uint32_t consumed_slots;
    uint8_t recovery_performed;
    uint8_t reserved[3];
};

struct phipia_ext4_recovery_report {
    uint32_t transactions;
    uint32_t replayed_blocks;
    uint32_t consumed_slots;
    bool performed;
};

struct phipia_ext4_mount_diagnostic {
    enum phipfs_status begin_status;
    int32_t rust_status;
    enum phipfs_status close_status;
    int32_t nvme_close_status;
    int32_t nvme_teardown_status;
    uint32_t nvme_resource_mismatches;
};

enum phipia_ext4_flush_boundary {
    PHIPIA_EXT4_FLUSH_FILESYSTEM_STATE = 0,
    PHIPIA_EXT4_FLUSH_ORDERED_DATA,
    PHIPIA_EXT4_FLUSH_JOURNAL_PAYLOAD,
    PHIPIA_EXT4_FLUSH_COMMIT,
    PHIPIA_EXT4_FLUSH_CHECKPOINT,
    PHIPIA_EXT4_FLUSH_JOURNAL_STATE,
    PHIPIA_EXT4_FLUSH_COUNT
};

enum phipia_ext4_test_storage_kind {
    PHIPIA_EXT4_TEST_STORAGE_WRITE = 0,
    PHIPIA_EXT4_TEST_STORAGE_FLUSH,
    PHIPIA_EXT4_TEST_STORAGE_KIND_COUNT
};

/* Private Rust/C storage callbacks; valid only during a backend operation. */
int32_t phipia_ext4_block_read(
    uintptr_t context,
    uint64_t start_byte,
    uint8_t *destination,
    size_t length
);
int32_t phipia_ext4_block_write(
    uintptr_t context,
    uint64_t start_byte,
    const uint8_t *source,
    size_t length
);
int32_t phipia_ext4_block_flush(uintptr_t context, uint32_t boundary);

void ext4_backend_initialize(void);
enum phipfs_status ext4_backend_mount(enum phipfs_volume volume);
enum phipfs_status ext4_backend_last_mount_status(enum phipfs_volume volume);
bool ext4_backend_mount_diagnostic(enum phipfs_volume volume,
    struct phipia_ext4_mount_diagnostic *diagnostic);
enum phipfs_status ext4_backend_unmount(enum phipfs_volume volume);
enum phipfs_status ext4_backend_sync(enum phipfs_volume volume);
struct phipfs_drive_info ext4_backend_drive(enum phipfs_volume volume);
uint64_t ext4_backend_completion_count(enum phipfs_volume volume);
bool ext4_backend_recovery_report(enum phipfs_volume volume,
    struct phipia_ext4_recovery_report *report);
enum phipfs_status ext4_backend_open(enum phipfs_volume volume,
    const char *path, enum phipfs_access access, phipfs_handle *handle);
enum phipfs_status ext4_backend_close(phipfs_handle handle);
enum phipfs_status ext4_backend_read(phipfs_handle handle,
    uint8_t *destination, size_t capacity, size_t *read_bytes);
enum phipfs_status ext4_backend_pread(phipfs_handle handle,
    uint8_t *destination, size_t capacity, uint64_t offset,
    size_t *read_bytes);
/* Shared journaled mutation entry points used by the backend and its probes. */
enum phipfs_status ext4_backend_transaction_probe(enum phipfs_volume volume,
    const char *path, uint64_t offset, const uint8_t *source,
    size_t source_bytes, size_t *written_bytes);
enum phipfs_status ext4_backend_truncate_probe(enum phipfs_volume volume,
    const char *path, uint64_t size);
enum phipfs_status ext4_backend_create_file_probe(enum phipfs_volume volume,
    const char *path, uint16_t mode);
enum phipfs_status ext4_backend_unlink_file_probe(enum phipfs_volume volume,
    const char *path);
enum phipfs_status ext4_backend_link_file_probe(enum phipfs_volume volume,
    const char *source, const char *destination);
enum phipfs_status ext4_backend_create_directory_probe(
    enum phipfs_volume volume, const char *path);
enum phipfs_status ext4_backend_remove_directory_probe(
    enum phipfs_volume volume, const char *path);
enum phipfs_status ext4_backend_rename_probe(enum phipfs_volume volume,
    const char *source, const char *destination);
/* Private ext4-recovery scenario controls; never installed in the VFS table. */
bool ext4_backend_test_configure_power_cut(const char *command_line,
    size_t command_line_length);
bool ext4_backend_test_power_cut_configured(void);
bool ext4_backend_test_fail_storage_once(uint32_t operation_ordinal);
bool ext4_backend_test_storage_failure_observed(
    enum phipia_ext4_test_storage_kind expected_kind);
enum phipfs_status ext4_backend_write(phipfs_handle handle,
    const uint8_t *source, size_t source_bytes, size_t *written_bytes);
enum phipfs_status ext4_backend_seek(phipfs_handle handle, int64_t offset,
    enum phipfs_seek_origin origin, uint64_t *position);
enum phipfs_status ext4_backend_stat_path(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *stat);
enum phipfs_status ext4_backend_list(enum phipfs_volume volume,
    const char *path, struct phipfs_list_entry *entries, size_t capacity,
    size_t *entry_count);
enum phipfs_status ext4_backend_directory_open(enum phipfs_volume volume,
    const char *path, phipfs_handle *handle);
enum phipfs_status ext4_backend_directory_read(phipfs_handle handle,
    struct phipfs_list_entry *entry, bool *present);
enum phipfs_status ext4_backend_directory_close(phipfs_handle handle);
enum phipfs_status ext4_backend_create(enum phipfs_volume volume,
    const char *path, uint16_t mode);
enum phipfs_status ext4_backend_truncate(enum phipfs_volume volume,
    const char *path, uint64_t size);
enum phipfs_status ext4_backend_mkdir(enum phipfs_volume volume,
    const char *path);
enum phipfs_status ext4_backend_rename(enum phipfs_volume volume,
    const char *source, const char *destination);
enum phipfs_status ext4_backend_unlink(enum phipfs_volume volume,
    const char *path);
enum phipfs_status ext4_backend_rmdir(enum phipfs_volume volume,
    const char *path);
enum phipfs_status ext4_backend_link(enum phipfs_volume volume,
    const char *source, const char *destination);

#endif
