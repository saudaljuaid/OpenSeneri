/* SPDX-License-Identifier: GPL-3.0-only */
/* Journaled ext4 VFS backend over the checked Rust ext4plus adapter. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/ext4_fs.h>
#include <phipia/nvme.h>
#include <phipia/wall_clock.h>

#define EXT4_MAX_HANDLES PHIPFS_MAX_HANDLES
#define EXT4_CONTROLLER_SYSTEM 0U
#define EXT4_CONTROLLER_DATA 1U
#define EXT4_TRANSACTION_PROBE_MAX_BYTES (64U * 4096U)
#define EXT4_POWER_CUT_BOUNDARY_COUNT 10U
#define EXT4_POWER_CUT_EXIT_PORT UINT16_C(0xf4)
#define EXT4_POWER_CUT_EXIT_VALUE UINT32_C(0x6e)

struct ext4_mount_state {
    struct nvme_volume_session session;
    struct phipia_ext4_identity identity;
    /* begin_operation serializes use; keep a full LBA off the syscall stack. */
    uint8_t block_buffer[NVME_BLOCK_BYTES];
    uintptr_t rust_mount;
    uint64_t generation;
    uint64_t media_bytes;
    uint64_t admitted_media_bytes;
    uint64_t cached_free_bytes;
    uint64_t completion_count;
    uint32_t controller_index;
    bool active;
    bool mounting;
    bool detaching;
    bool operation_active;
    bool close_failed;
    bool orphan_cleanup_pending;
    bool healthy;
};

struct ext4_handle_state {
    uintptr_t directory_snapshot;
    uint64_t generation;
    uint64_t mount_generation;
    uint64_t inode;
    uint64_t offset;
    uint64_t size;
    enum phipfs_volume volume;
    enum phipfs_access access;
    char path[PHIPFS_MAX_PATH];
    bool directory;
    bool active;
    bool closing;
};

static struct ext4_mount_state ext4_mounts[PHIPFS_VOLUME_COUNT];
static struct ext4_handle_state ext4_handles[EXT4_MAX_HANDLES];
static enum phipfs_status ext4_last_mount_status[PHIPFS_VOLUME_COUNT];
static struct phipia_ext4_mount_diagnostic
    ext4_mount_diagnostics[PHIPFS_VOLUME_COUNT];
static uint64_t next_mount_generation = UINT64_C(1);
static uint64_t next_handle_generation = UINT64_C(1);
static bool ext4_test_configured;
static uint32_t ext4_test_power_cut_boundary;
static uint32_t ext4_test_durable_boundary;
static bool ext4_test_storage_failure_armed;
static bool ext4_test_storage_failure_seen;
static uint32_t ext4_test_storage_failure_target;
static uint32_t ext4_test_storage_operation;
static enum phipia_ext4_test_storage_kind ext4_test_storage_failure_kind =
    PHIPIA_EXT4_TEST_STORAGE_KIND_COUNT;

extern int32_t phipia_ext4_mount(uintptr_t context, uint64_t media_bytes,
    struct phipia_ext4_identity *identity, uintptr_t *mounted_out);
extern int32_t phipia_ext4_prepare_unmount(uintptr_t mounted);
extern int32_t phipia_ext4_sync(uintptr_t mounted, const uint64_t *open_inodes, size_t open_count);
extern int32_t phipia_ext4_free_bytes(uintptr_t mounted, uint64_t *free_bytes);
extern int32_t phipia_ext4_unmount(uintptr_t mounted);
extern int32_t phipia_ext4_stat(uintptr_t mounted, const uint8_t *path,
    size_t path_length, struct phipia_ext4_metadata *metadata);
extern int32_t phipia_ext4_lstat(uintptr_t mounted, const uint8_t *path,
    size_t path_length, struct phipia_ext4_metadata *metadata);
extern int32_t phipia_ext4_pread(uintptr_t mounted, const uint8_t *path,
    size_t path_length, uint64_t offset, uint8_t *destination,
    size_t capacity, size_t *read_out);
extern int32_t phipia_ext4_transaction_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length, uint64_t offset,
    const uint8_t *source, size_t source_length, size_t *written_out);
extern int32_t phipia_ext4_truncate_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length, uint64_t size);
extern int32_t phipia_ext4_create_file_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length, uint16_t mode);
extern int32_t phipia_ext4_unlink_file_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length, const uint64_t *open_inodes, size_t open_count,
    bool remove_directory);
extern int32_t phipia_ext4_link_file_probe(uintptr_t mounted,
    const uint8_t *source, size_t source_length, const uint8_t *destination,
    size_t destination_length);
extern int32_t phipia_ext4_create_directory_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length);
extern int32_t phipia_ext4_remove_directory_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length, const uint64_t *open_inodes, size_t open_count);
extern int32_t phipia_ext4_rename_probe(uintptr_t mounted,
    const uint8_t *source, size_t source_length, const uint8_t *destination,
    size_t destination_length);
extern int32_t phipia_ext4_directory_entry(uintptr_t mounted,
    const uint8_t *path, size_t path_length, uint64_t index,
    struct phipia_ext4_directory_entry *entry, bool *present);
extern int32_t phipia_ext4_symlink(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *target, size_t target_bytes);
extern int32_t phipia_ext4_rename_replace(uintptr_t mounted,
    const uint8_t *source, size_t source_bytes,
    const uint8_t *destination, size_t destination_bytes,
    const uint64_t *open_inodes, size_t open_count);
extern int32_t phipia_ext4_readlink(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint8_t *output, size_t capacity, size_t *read_bytes);
extern int32_t phipia_ext4_append(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *source, size_t source_bytes,
    uint64_t maximum_size, uint64_t *start, size_t *written_bytes);
extern int32_t phipia_ext4_chmod(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, uint16_t mode);
extern int32_t phipia_ext4_set_xattr(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *name, size_t name_bytes,
    const uint8_t *value, size_t value_bytes, uint8_t remove);
extern int32_t phipia_ext4_get_xattr(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, const uint8_t *name, size_t name_bytes,
    uint8_t *output, size_t capacity, size_t *length);
extern int32_t phipia_ext4_directory_snapshot(uintptr_t mounted, const uint8_t *path,
    size_t path_bytes, struct phipia_ext4_metadata *metadata, uintptr_t *snapshot);
extern int32_t phipia_ext4_snapshot_entry(uintptr_t snapshot, uint64_t index,
    struct phipia_ext4_directory_entry *entry, bool *present);
extern void phipia_ext4_snapshot_free(uintptr_t snapshot);
extern int32_t phipia_ext4_stat_inode(uintptr_t mounted, uint64_t inode, struct phipia_ext4_metadata *metadata);
extern int32_t phipia_ext4_truncate_inode(uintptr_t mounted, uint64_t inode, uint64_t size);
extern int32_t phipia_ext4_set_times(uintptr_t mounted, const uint8_t *path, size_t path_bytes,
    uint64_t atime_seconds, uint32_t atime_nanos, uint64_t mtime_seconds, uint32_t mtime_nanos);
extern int32_t phipia_ext4_pread_inode(uintptr_t mounted, uint64_t inode, uint64_t offset,
    uint8_t *output, size_t capacity, size_t *count);
extern int32_t phipia_ext4_write_inode(uintptr_t mounted, uint64_t inode, uint64_t offset,
    const uint8_t *source, size_t length, size_t *count);
extern int32_t phipia_ext4_append_inode(uintptr_t mounted, uint64_t inode,
    const uint8_t *source, size_t length, uint64_t maximum_size, uint64_t *start, size_t *count);

_Static_assert(sizeof(struct phipia_ext4_metadata) == 40U,
    "ext4 metadata C/Rust ABI drift");
_Static_assert(offsetof(struct phipia_ext4_metadata, file_type) == 28U,
    "ext4 metadata C/Rust ABI offset drift");
_Static_assert(sizeof(struct phipia_ext4_directory_entry) == 304U,
    "ext4 directory C/Rust ABI drift");
_Static_assert(sizeof(struct phipia_ext4_identity) == 48U,
    "ext4 identity C/Rust ABI drift");
_Static_assert(offsetof(struct phipia_ext4_identity, recovered_transactions) ==
        32U,
    "ext4 identity C/Rust ABI offset drift");
_Static_assert(offsetof(struct phipia_ext4_identity, recovery_performed) == 44U,
    "ext4 recovery C/Rust ABI offset drift");

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *to = destination;
    const uint8_t *from = source;

    for (size_t index = 0U; index < length; ++index) {
        to[index] = from[index];
    }
}

static size_t path_length(const char *path)
{
    size_t length = 0U;

    if (path == NULL) {
        return PHIPFS_MAX_PATH;
    }
    while (length < PHIPFS_MAX_PATH && path[length] != '\0') {
        ++length;
    }
    return length;
}

static bool token_has_prefix(const char *token, size_t token_length,
    const char *prefix, size_t prefix_length)
{
    if (token == NULL || prefix == NULL || token_length < prefix_length) {
        return false;
    }
    for (size_t index = 0U; index < prefix_length; ++index) {
        if (token[index] != prefix[index]) {
            return false;
        }
    }
    return true;
}

bool ext4_backend_test_configure_power_cut(const char *command_line,
    size_t command_line_length)
{
    static const char prefix[] = "phipia.ext4-cut=";
    uint32_t selected = 0U;
    size_t offset = 0U;
    bool found = false;

    if (ext4_test_configured || command_line == NULL) {
        return false;
    }
    while (offset < command_line_length) {
        size_t start;
        size_t length;
        uint32_t value = 0U;

        while (offset < command_line_length && command_line[offset] == ' ') {
            ++offset;
        }
        start = offset;
        while (offset < command_line_length && command_line[offset] != ' ') {
            ++offset;
        }
        length = offset - start;
        if (!token_has_prefix(command_line + start, length, prefix,
                sizeof(prefix) - 1U)) {
            continue;
        }
        if (found || length == sizeof(prefix) - 1U) {
            return false;
        }
        for (size_t index = sizeof(prefix) - 1U; index < length; ++index) {
            const char digit = command_line[start + index];

            if (digit < '0' || digit > '9' ||
                value > (UINT32_MAX - (uint32_t)(digit - '0')) / 10U) {
                return false;
            }
            value = value * 10U + (uint32_t)(digit - '0');
        }
        if (value == 0U || value > EXT4_POWER_CUT_BOUNDARY_COUNT) {
            return false;
        }
        selected = value;
        found = true;
    }
    ext4_test_configured = true;
    ext4_test_power_cut_boundary = selected;
    ext4_test_durable_boundary = 0U;
    return true;
}

bool ext4_backend_test_power_cut_configured(void)
{
    return ext4_test_configured && ext4_test_power_cut_boundary != 0U;
}

bool ext4_backend_test_fail_storage_once(uint32_t operation_ordinal)
{
    if (!ext4_test_configured || ext4_test_power_cut_boundary != 0U ||
        ext4_test_storage_failure_armed || operation_ordinal == 0U) {
        return false;
    }
    ext4_test_storage_failure_armed = true;
    ext4_test_storage_failure_seen = false;
    ext4_test_storage_failure_target = operation_ordinal;
    ext4_test_storage_operation = 0U;
    ext4_test_storage_failure_kind = PHIPIA_EXT4_TEST_STORAGE_KIND_COUNT;
    return true;
}

bool ext4_backend_test_storage_failure_observed(
    enum phipia_ext4_test_storage_kind expected_kind)
{
    const bool observed = ext4_test_storage_failure_seen &&
        !ext4_test_storage_failure_armed &&
        ext4_test_storage_failure_kind == expected_kind;

    ext4_test_storage_failure_seen = false;
    ext4_test_storage_failure_kind = PHIPIA_EXT4_TEST_STORAGE_KIND_COUNT;
    return observed;
}

static bool fail_test_storage_operation(
    enum phipia_ext4_test_storage_kind kind)
{
    if (!ext4_test_storage_failure_armed) {
        return false;
    }
    if (ext4_test_storage_operation == UINT32_MAX) {
        return true;
    }
    ++ext4_test_storage_operation;
    if (ext4_test_storage_operation != ext4_test_storage_failure_target) {
        return false;
    }
    ext4_test_storage_failure_armed = false;
    ext4_test_storage_failure_seen = true;
    ext4_test_storage_failure_kind = kind;
    return true;
}

static const char *flush_boundary_name(uint32_t boundary)
{
    switch (boundary) {
    case PHIPIA_EXT4_FLUSH_FILESYSTEM_STATE: return "filesystem-state";
    case PHIPIA_EXT4_FLUSH_ORDERED_DATA: return "ordered-data";
    case PHIPIA_EXT4_FLUSH_JOURNAL_PAYLOAD: return "journal-payload";
    case PHIPIA_EXT4_FLUSH_COMMIT: return "commit";
    case PHIPIA_EXT4_FLUSH_CHECKPOINT: return "checkpoint";
    case PHIPIA_EXT4_FLUSH_JOURNAL_STATE: return "journal-state";
    default: return NULL;
    }
}

static void report_durable_boundary(uint32_t boundary)
{
    const char *name = flush_boundary_name(boundary);

    if (!ext4_test_configured || name == NULL) {
        return;
    }
    ++ext4_test_durable_boundary;
    console_write("ST EXT4 DURABLE ");
    console_write_u64(ext4_test_durable_boundary);
    console_putc(' ');
    console_write(name);
    console_putc('\n');
    if (ext4_test_durable_boundary != ext4_test_power_cut_boundary) {
        return;
    }
    console_write("ST EXT4 POWER CUT ");
    console_write_u64(ext4_test_durable_boundary);
    console_putc(' ');
    console_write(name);
    console_putc('\n');
    cpu_out32(EXT4_POWER_CUT_EXIT_PORT, EXT4_POWER_CUT_EXIT_VALUE);
    console_halt();
}

static bool valid_volume(enum phipfs_volume volume)
{
    return volume >= PHIPFS_VOLUME_SYSTEM && volume < PHIPFS_VOLUME_COUNT;
}

static uint64_t generation(uint64_t *next)
{
    uint64_t value = *next;

    ++*next;
    if (value == 0U || value > (UINT64_MAX >> 8U)) {
        value = 1U;
        *next = 2U;
    }
    return value;
}

static enum phipfs_status map_status(int32_t status)
{
    switch (status) {
    case PHIPIA_EXT4_STATUS_OK:
        return PHIPFS_STATUS_OK;
    case PHIPIA_EXT4_STATUS_NULL_ARGUMENT:
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    case PHIPIA_EXT4_STATUS_VOLUME:
        return PHIPFS_STATUS_NOT_MOUNTED;
    case PHIPIA_EXT4_STATUS_IO:
        return PHIPFS_STATUS_IO;
    case PHIPIA_EXT4_STATUS_INVALID:
        return PHIPFS_STATUS_CORRUPT;
    case PHIPIA_EXT4_STATUS_NOT_FOUND:
        return PHIPFS_STATUS_NOT_FOUND;
    case PHIPIA_EXT4_STATUS_NOT_DIRECTORY:
        return PHIPFS_STATUS_NOT_DIRECTORY;
    case PHIPIA_EXT4_STATUS_IS_DIRECTORY:
        return PHIPFS_STATUS_IS_DIRECTORY;
    case PHIPIA_EXT4_STATUS_RANGE:
        return PHIPFS_STATUS_RANGE;
    case PHIPIA_EXT4_STATUS_SPECIAL:
        return PHIPFS_STATUS_ACCESS;
    case PHIPIA_EXT4_STATUS_EXISTS:
        return PHIPFS_STATUS_EXISTS;
    case PHIPIA_EXT4_STATUS_NOT_EMPTY:
        return PHIPFS_STATUS_NOT_EMPTY;
    case PHIPIA_EXT4_STATUS_FULL:
        return PHIPFS_STATUS_FULL;
    case PHIPIA_EXT4_STATUS_READ_ONLY:
        return PHIPFS_STATUS_READ_ONLY;
    case PHIPIA_EXT4_STATUS_BUSY:
        return PHIPFS_STATUS_BUSY;
    default:
        return PHIPFS_STATUS_CORRUPT;
    }
}

static enum phipfs_status end_operation(struct ext4_mount_state *mount,
    struct phipia_ext4_mount_diagnostic *diagnostic);

static void release_operation(struct ext4_mount_state *mount)
{
    /* A close that reenters an operation retires the public handle at once,
     * but its backing state must survive until the operation stops using it. */
    for (;;) {
        for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
            struct ext4_handle_state *state = &ext4_handles[index];

            if (state->active && state->closing && valid_volume(state->volume) &&
                &ext4_mounts[state->volume] == mount) {
                if (state->directory_snapshot != 0U) phipia_ext4_snapshot_free(state->directory_snapshot);
                zero_bytes(state, sizeof(*state));
            }
        }
        __atomic_store_n(&mount->operation_active, false, __ATOMIC_RELEASE);
        // A close can arrive after its slot was scanned but before the guard
        // was released. Either reclaim it now or leave it to the next owner.
        bool pending_close = false;
        for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
            const struct ext4_handle_state *state = &ext4_handles[index];
            if (state->active && state->closing && valid_volume(state->volume) &&
                &ext4_mounts[state->volume] == mount) pending_close = true;
        }
        if (!pending_close) return;
        bool idle = false;
        if (!__atomic_compare_exchange_n(&mount->operation_active, &idle,
                true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    }
}

static enum phipfs_status reserve_operation(struct ext4_mount_state *mount)
{
    if (mount == NULL) return PHIPFS_STATUS_NOT_MOUNTED;
    bool expected_idle = false;
    if (!__atomic_compare_exchange_n(&mount->operation_active, &expected_idle,
            true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return PHIPFS_STATUS_BUSY;
    }
    if (!mount->active && !mount->mounting) {
        release_operation(mount);
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    if (mount->detaching) {
        release_operation(mount);
        return PHIPFS_STATUS_BUSY;
    }
    if (mount->active && (!mount->healthy || mount->close_failed)) {
        release_operation(mount);
        return PHIPFS_STATUS_IO;
    }
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status begin_operation(struct ext4_mount_state *mount, bool writable)
{
    const enum phipfs_status reserved = reserve_operation(mount);
    enum nvme_status status;

    if (reserved != PHIPFS_STATUS_OK) return reserved;
    /* Reserve the coordinator before opening storage: controller setup can
     * invoke callbacks, and must not admit a second user of this session. */
    status = nvme_volume_open(&mount->session, mount->controller_index,
        writable);
    if (status != NVME_STATUS_OK) {
        if (mount->session.active) mount->close_failed = true;
        release_operation(mount);
        return PHIPFS_STATUS_IO;
    }
    if (mount->session.logical_block_bytes == 0U ||
        mount->session.namespace_blocks >
            UINT64_MAX / mount->session.logical_block_bytes) {
        return end_operation(mount, NULL) == PHIPFS_STATUS_OK ?
            PHIPFS_STATUS_RANGE : PHIPFS_STATUS_IO;
    }
    mount->media_bytes = mount->session.namespace_blocks *
        mount->session.logical_block_bytes;
    if (mount->active && mount->admitted_media_bytes != 0U &&
        mount->media_bytes != mount->admitted_media_bytes) {
        mount->healthy = false;
        (void)end_operation(mount, NULL);
        return PHIPFS_STATUS_IO;
    }
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status end_operation(
    struct ext4_mount_state *mount,
    struct phipia_ext4_mount_diagnostic *diagnostic
)
{
    enum nvme_status status;

    if (mount == NULL || !mount->operation_active) {
        return PHIPFS_STATUS_CORRUPT;
    }
    // Capacity queries must not borrow the Rust coordinator while another
    // operation holds it mutably. Capture the last readable count here.
    uint64_t free_bytes = 0U;
    if (mount->rust_mount != 0U &&
        phipia_ext4_free_bytes(mount->rust_mount, &free_bytes) == PHIPIA_EXT4_STATUS_OK) {
        __atomic_store_n(&mount->cached_free_bytes, free_bytes, __ATOMIC_RELEASE);
    }
    status = nvme_volume_close(&mount->session);
    if (diagnostic != NULL) {
        diagnostic->nvme_close_status = (int32_t)status;
        diagnostic->nvme_teardown_status =
            (int32_t)mount->session.close_teardown_status;
        diagnostic->nvme_resource_mismatches =
            mount->session.close_resource_mismatches;
    }
    if (status != NVME_STATUS_OK) {
        // Keep the generation/ownership cookie for explicit teardown retry.
        // Freeze new operations: the filesystem mutation may already be durable,
        // so automatically repeating an append here could apply it twice.
        mount->close_failed = true;
        release_operation(mount);
        return PHIPFS_STATUS_IO;
    }
    zero_bytes(&mount->session, sizeof(mount->session));
    mount->close_failed = false;
    ++mount->completion_count;
    release_operation(mount);
    return PHIPFS_STATUS_OK;
}

/* Rust may access storage only through the lease installed by begin_operation(). */
int32_t phipia_ext4_block_read(
    uintptr_t context,
    uint64_t start_byte,
    uint8_t *destination,
    size_t length
)
{
    struct ext4_mount_state *mount = (struct ext4_mount_state *)context;
    struct nvme_volume_session *session;
    uint64_t position = start_byte;
    size_t remaining = length;

    if (mount == NULL || destination == NULL || !mount->operation_active) {
        return -1;
    }
    session = &mount->session;
    if (!session->active || session->logical_block_bytes == 0U ||
        session->logical_block_bytes > sizeof(mount->block_buffer) ||
        start_byte > mount->media_bytes ||
        length > mount->media_bytes - start_byte) {
        return -1;
    }
    while (remaining != 0U) {
        const uint64_t lba = position / session->logical_block_bytes;
        const size_t within = (size_t)(position % session->logical_block_bytes);
        size_t chunk = session->logical_block_bytes - within;

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (nvme_volume_read(session, lba, mount->block_buffer,
                session->logical_block_bytes) != NVME_STATUS_OK) {
            return -1;
        }
        copy_bytes(destination, &mount->block_buffer[within], chunk);
        destination += chunk;
        position += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* Write one checked byte range during an explicitly writable operation. */
int32_t phipia_ext4_block_write(
    uintptr_t context,
    uint64_t start_byte,
    const uint8_t *source,
    size_t length
)
{
    struct ext4_mount_state *mount = (struct ext4_mount_state *)context;
    struct nvme_volume_session *session;
    uint64_t position = start_byte;
    size_t remaining = length;

    if (mount == NULL || source == NULL || !mount->operation_active) {
        return -1;
    }
    session = &mount->session;
    if (!session->active || !session->writable ||
        session->logical_block_bytes == 0U ||
        session->logical_block_bytes > sizeof(mount->block_buffer) ||
        start_byte > mount->media_bytes ||
        length > mount->media_bytes - start_byte) {
        return -1;
    }
    if (fail_test_storage_operation(PHIPIA_EXT4_TEST_STORAGE_WRITE)) {
        return -1;
    }
    while (remaining != 0U) {
        const uint64_t lba = position / session->logical_block_bytes;
        const size_t within = (size_t)(position % session->logical_block_bytes);
        size_t chunk = session->logical_block_bytes - within;

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (within == 0U && chunk == session->logical_block_bytes) {
            if (nvme_volume_write(session, lba, source, chunk) !=
                    NVME_STATUS_OK) {
                return -1;
            }
        } else {
            if (nvme_volume_read(session, lba, mount->block_buffer,
                    session->logical_block_bytes) != NVME_STATUS_OK) {
                return -1;
            }
            copy_bytes(&mount->block_buffer[within], source, chunk);
            if (nvme_volume_write(session, lba, mount->block_buffer,
                    session->logical_block_bytes) != NVME_STATUS_OK) {
                return -1;
            }
        }
        source += chunk;
        position += chunk;
        remaining -= chunk;
    }
    return 0;
}

/* A transaction timestamp is sampled once and retained in its staged images. */
uint64_t phipia_ext4_current_time(uintptr_t context)
{
    struct ext4_mount_state *mount = (struct ext4_mount_state *)context;
    int64_t seconds;
    if (mount == NULL || !mount->operation_active || !mount->session.active ||
        !mount->session.writable || wall_clock_read_unix_seconds(&seconds) != WALL_CLOCK_STATUS_OK ||
        seconds < 0) return UINT64_MAX;
    return (uint64_t)seconds;
}

/* Establish one real NVMe durability boundary for the Rust journal executor. */
int32_t phipia_ext4_block_flush(uintptr_t context, uint32_t boundary)
{
    struct ext4_mount_state *mount = (struct ext4_mount_state *)context;
    struct nvme_volume_session *session;
    enum nvme_status status;

    if (mount == NULL || !mount->operation_active ||
        flush_boundary_name(boundary) == NULL) {
        return -1;
    }
    session = &mount->session;
    if (!session->active || !session->writable) {
        return -1;
    }
    if (fail_test_storage_operation(PHIPIA_EXT4_TEST_STORAGE_FLUSH)) {
        return -1;
    }
    status = nvme_volume_flush(session);
    if (status != NVME_STATUS_OK) {
        return -1;
    }
    report_durable_boundary(boundary);
    return 0;
}

static enum phipfs_status checked_metadata(
    struct ext4_mount_state *mount,
    const char *path,
    struct phipia_ext4_metadata *metadata,
    bool follow
)
{
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (metadata == NULL || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(metadata, sizeof(*metadata));
    status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(follow ? phipia_ext4_stat(mount->rust_mount,
        (const uint8_t *)path, length, metadata) :
        phipia_ext4_lstat(mount->rust_mount, (const uint8_t *)path, length, metadata));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

static enum phipfs_status checked_stat(struct ext4_mount_state *mount,
    const char *path, struct phipia_ext4_metadata *metadata)
{
    return checked_metadata(mount, path, metadata, true);
}

static void fill_stat(
    const struct phipia_ext4_metadata *source,
    struct phipfs_stat *destination
)
{
    zero_bytes(destination, sizeof(*destination));
    destination->size = source->size;
    destination->object_id = source->inode;
    destination->uid = source->uid;
    destination->gid = source->gid;
    destination->mode = source->mode;
    destination->links = source->links;
    destination->directory = source->file_type == PHIPIA_EXT4_FILE_DIRECTORY;
    destination->read_only = false;
}

static enum phipfs_status handle_state(
    phipfs_handle handle,
    struct ext4_handle_state **state
)
{
    const uint64_t encoded = handle & UINT64_C(0xff);
    const uint64_t encoded_generation = handle >> 8U;
    size_t index;

    if (state == NULL || encoded == 0U || encoded > EXT4_MAX_HANDLES ||
        encoded_generation == 0U) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    index = (size_t)(encoded - 1U);
    if (!ext4_handles[index].active || ext4_handles[index].closing ||
        ext4_handles[index].generation != encoded_generation ||
        !valid_volume(ext4_handles[index].volume) ||
        !ext4_mounts[ext4_handles[index].volume].active ||
        ext4_handles[index].mount_generation !=
            ext4_mounts[ext4_handles[index].volume].generation) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    *state = &ext4_handles[index];
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status allocate_handle(enum phipfs_volume volume,
    const char *path, uint64_t inode, uint64_t size,
    enum phipfs_access access, bool directory, uintptr_t snapshot, phipfs_handle *handle)
{
    const size_t length = path_length(path);
    size_t slot = EXT4_MAX_HANDLES;

    if (handle == NULL || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (!ext4_handles[index].active) {
            slot = index;
            break;
        }
    }
    if (slot == EXT4_MAX_HANDLES) {
        return PHIPFS_STATUS_NO_HANDLES;
    }
    zero_bytes(&ext4_handles[slot], sizeof(ext4_handles[slot]));
    ext4_handles[slot].generation = generation(&next_handle_generation);
    ext4_handles[slot].mount_generation = ext4_mounts[volume].generation;
    ext4_handles[slot].inode = inode;
    ext4_handles[slot].size = size;
    ext4_handles[slot].volume = volume;
    ext4_handles[slot].access = access;
    ext4_handles[slot].directory = directory;
    ext4_handles[slot].directory_snapshot = snapshot;
    copy_bytes(ext4_handles[slot].path, path, length + 1U);
    ext4_handles[slot].active = true;
    *handle = ext4_handles[slot].generation << 8U | (uint64_t)(slot + 1U);
    return PHIPFS_STATUS_OK;
}

static void update_open_sizes(enum phipfs_volume volume, uint64_t inode,
    uint64_t size)
{
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (ext4_handles[index].active &&
            ext4_handles[index].volume == volume &&
            ext4_handles[index].mount_generation ==
                ext4_mounts[volume].generation &&
            ext4_handles[index].inode == inode) {
            ext4_handles[index].size = size;
        }
    }
}

static size_t collect_open_inodes(enum phipfs_volume volume, uint64_t *inodes, bool include_directories)
{
    size_t count = 0U;
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (ext4_handles[index].active &&
            ext4_handles[index].volume == volume &&
            ext4_handles[index].mount_generation ==
                ext4_mounts[volume].generation &&
            (include_directories || !ext4_handles[index].directory)) {
            inodes[count++] = ext4_handles[index].inode;
        }
    }
    return count;
}

static bool volume_has_open_handles(enum phipfs_volume volume)
{
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (ext4_handles[index].active && ext4_handles[index].volume == volume) {
            return true;
        }
    }
    return false;
}

void ext4_backend_initialize(void)
{
    zero_bytes(ext4_mounts, sizeof(ext4_mounts));
    zero_bytes(ext4_handles, sizeof(ext4_handles));
    ext4_test_durable_boundary = 0U;
    ext4_test_storage_failure_armed = false;
    ext4_test_storage_failure_seen = false;
    ext4_test_storage_failure_target = 0U;
    ext4_test_storage_operation = 0U;
    ext4_test_storage_failure_kind = PHIPIA_EXT4_TEST_STORAGE_KIND_COUNT;
    for (enum phipfs_volume volume = PHIPFS_VOLUME_SYSTEM;
         volume < PHIPFS_VOLUME_COUNT; ++volume) {
        ext4_last_mount_status[volume] = PHIPFS_STATUS_NOT_MOUNTED;
        ext4_mount_diagnostics[volume].begin_status = PHIPFS_STATUS_NOT_MOUNTED;
        ext4_mount_diagnostics[volume].rust_status = PHIPIA_EXT4_STATUS_COUNT;
        ext4_mount_diagnostics[volume].close_status = PHIPFS_STATUS_NOT_MOUNTED;
        ext4_mount_diagnostics[volume].nvme_close_status = NVME_STATUS_COUNT;
        ext4_mount_diagnostics[volume].nvme_teardown_status =
            NVME_STATUS_COUNT;
        ext4_mount_diagnostics[volume].nvme_resource_mismatches = 0U;
    }
}

static enum phipfs_status retry_session_close(struct ext4_mount_state *mount)
{
    bool expected_idle = false;
    if (!__atomic_compare_exchange_n(&mount->operation_active, &expected_idle,
            true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return PHIPFS_STATUS_BUSY;
    if (mount->detaching) {
        release_operation(mount);
        return PHIPFS_STATUS_BUSY;
    }
    if (mount->session.active && nvme_volume_close(&mount->session) != NVME_STATUS_OK) {
        release_operation(mount);
        return PHIPFS_STATUS_IO;
    }
    zero_bytes(&mount->session, sizeof(mount->session));
    mount->close_failed = false;
    release_operation(mount);
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status release_failed_mount(struct ext4_mount_state *mount)
{
    enum phipfs_status status = retry_session_close(mount);
    if (status != PHIPFS_STATUS_OK) return status;
    if (mount->rust_mount != 0U) {
        status = begin_operation(mount, true);
        if (status != PHIPFS_STATUS_OK) return status;
        mount->detaching = true;
        status = map_status(phipia_ext4_prepare_unmount(mount->rust_mount));
        const enum phipfs_status close_status = end_operation(mount, NULL);
        if (status != PHIPFS_STATUS_OK || close_status != PHIPFS_STATUS_OK) {
            mount->detaching = false;
            return status != PHIPFS_STATUS_OK ? status : close_status;
        }
        status = map_status(phipia_ext4_unmount(mount->rust_mount));
        if (status != PHIPFS_STATUS_OK) { mount->detaching = false; return status; }
    }
    zero_bytes(mount, sizeof(*mount));
    return PHIPFS_STATUS_OK;
}

enum phipfs_status ext4_backend_mount(enum phipfs_volume volume)
{
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;
    int32_t rust_status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    if (mount->active) {
        ext4_last_mount_status[volume] = PHIPFS_STATUS_ALREADY_MOUNTED;
        return PHIPFS_STATUS_ALREADY_MOUNTED;
    }
    if (mount->mounting) {
        status = release_failed_mount(mount);
        if (status != PHIPFS_STATUS_OK) {
            ext4_last_mount_status[volume] = status;
            return status;
        }
    }
    zero_bytes(mount, sizeof(*mount));
    mount->controller_index = volume == PHIPFS_VOLUME_SYSTEM ?
        EXT4_CONTROLLER_SYSTEM : EXT4_CONTROLLER_DATA;
    mount->mounting = true;
    mount->healthy = true;
    ext4_mount_diagnostics[volume].begin_status = PHIPFS_STATUS_NOT_MOUNTED;
    ext4_mount_diagnostics[volume].rust_status = PHIPIA_EXT4_STATUS_COUNT;
    ext4_mount_diagnostics[volume].close_status = PHIPFS_STATUS_NOT_MOUNTED;
    ext4_mount_diagnostics[volume].nvme_close_status = NVME_STATUS_COUNT;
    ext4_mount_diagnostics[volume].nvme_teardown_status = NVME_STATUS_COUNT;
    ext4_mount_diagnostics[volume].nvme_resource_mismatches = 0U;
    status = begin_operation(mount, true);
    ext4_mount_diagnostics[volume].begin_status = status;
    if (status != PHIPFS_STATUS_OK) {
        if (!mount->close_failed && !mount->session.active) zero_bytes(mount, sizeof(*mount));
        ext4_last_mount_status[volume] = status;
        return status;
    }
    rust_status = phipia_ext4_mount((uintptr_t)mount, mount->media_bytes,
        &mount->identity, &mount->rust_mount);
    ext4_mount_diagnostics[volume].rust_status = rust_status;
    close_status = end_operation(mount, &ext4_mount_diagnostics[volume]);
    ext4_mount_diagnostics[volume].close_status = close_status;
    status = map_status(rust_status);
    if (status != PHIPFS_STATUS_OK || close_status != PHIPFS_STATUS_OK) {
        const enum phipfs_status result = status != PHIPFS_STATUS_OK ?
            status : close_status;

        // The next mount attempt explicitly retires the retained Rust object
        // and NVMe lease. Neither owner may be discarded on a cleanup error.
        if (mount->rust_mount == 0U && !mount->close_failed && !mount->session.active) {
            zero_bytes(mount, sizeof(*mount));
        }
        ext4_last_mount_status[volume] = result;
        return result;
    }
    mount->generation = generation(&next_mount_generation);
    mount->admitted_media_bytes = mount->media_bytes;
    mount->mounting = false;
    mount->active = true;
    ext4_last_mount_status[volume] = PHIPFS_STATUS_OK;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status ext4_backend_last_mount_status(enum phipfs_volume volume)
{
    return valid_volume(volume) ? ext4_last_mount_status[volume] :
        PHIPFS_STATUS_INVALID_ARGUMENT;
}

bool ext4_backend_mount_diagnostic(enum phipfs_volume volume,
    struct phipia_ext4_mount_diagnostic *diagnostic)
{
    if (!valid_volume(volume) || diagnostic == NULL) {
        return false;
    }
    *diagnostic = ext4_mount_diagnostics[volume];
    return true;
}

enum phipfs_status ext4_backend_unmount(enum phipfs_volume volume)
{
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;
    int32_t rust_status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    if (!mount->active) {
        if (mount->mounting) return release_failed_mount(mount);
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    if (volume_has_open_handles(volume)) return PHIPFS_STATUS_BUSY;
    const bool was_frozen = mount->close_failed;
    if (was_frozen) {
        status = retry_session_close(mount);
        if (status != PHIPFS_STATUS_OK) return status;
    }
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        if (was_frozen) mount->close_failed = true;
        return status;
    }
    // Recheck after acquiring the lease: an opening handle may have completed
    // between the preliminary census and coordinator admission.
    if (volume_has_open_handles(volume)) {
        (void)end_operation(mount, NULL);
        return PHIPFS_STATUS_BUSY;
    }
    mount->detaching = true;
    rust_status = phipia_ext4_prepare_unmount(mount->rust_mount);
    close_status = end_operation(mount, NULL);
    status = map_status(rust_status);
    if (status != PHIPFS_STATUS_OK || close_status != PHIPFS_STATUS_OK) {
        mount->detaching = false;
        if (was_frozen) mount->close_failed = true;
        return status != PHIPFS_STATUS_OK ? status : close_status;
    }
    if (phipia_ext4_unmount(mount->rust_mount) != PHIPIA_EXT4_STATUS_OK) {
        mount->detaching = false;
        if (was_frozen) mount->close_failed = true;
        return PHIPFS_STATUS_CORRUPT;
    }
    zero_bytes(mount, sizeof(*mount));
    return PHIPFS_STATUS_OK;
}

enum phipfs_status ext4_backend_sync(enum phipfs_volume volume)
{
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    if (!mount->active) {
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    if (mount->close_failed) {
        // Explicit sync can finish retained NVMe teardown before resuming a
        // journal plan. Ordinary reads/writes remain frozen, so an append is
        // never implicitly submitted a second time by a new user operation.
        status = retry_session_close(mount);
        if (status != PHIPFS_STATUS_OK) return status;
    }
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    uint64_t open_inodes[EXT4_MAX_HANDLES];
    const size_t open_count = collect_open_inodes(volume, open_inodes, true);
    status = map_status(phipia_ext4_sync(mount->rust_mount, open_inodes, open_count));
    if (status == PHIPFS_STATUS_OK && open_count == 0U) mount->orphan_cleanup_pending = false;
    if (status == PHIPFS_STATUS_OK) {
        /* Sync may finish a previously refused write or truncate. Refresh
         * cached EOFs while the same lease excludes another mutation; keep
         * each file position unchanged. A failed refresh remains retryable. */
        for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
            struct ext4_handle_state *state = &ext4_handles[index];
            struct phipia_ext4_metadata metadata;

            if (!state->active || state->directory || state->volume != volume ||
                state->mount_generation != mount->generation) {
                continue;
            }
            zero_bytes(&metadata, sizeof(metadata));
            status = map_status(phipia_ext4_stat_inode(mount->rust_mount, state->inode, &metadata));
            if (status != PHIPFS_STATUS_OK) {
                break;
            }
            if (metadata.inode != state->inode) {
                status = PHIPFS_STATUS_STALE_HANDLE;
                break;
            }
            state->size = metadata.size;
        }
    }
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

struct phipfs_drive_info ext4_backend_drive(enum phipfs_volume volume)
{
    struct phipfs_drive_info drive = {0};
    struct ext4_mount_state *mount;

    if (!valid_volume(volume)) {
        return drive;
    }
    mount = &ext4_mounts[volume];
    drive.volume = volume;
    drive.volume_id = (uint32_t)mount->identity.uuid[0] |
        (uint32_t)mount->identity.uuid[1] << 8U |
        (uint32_t)mount->identity.uuid[2] << 16U |
        (uint32_t)mount->identity.uuid[3] << 24U;
    drive.total_bytes = mount->media_bytes;
    drive.free_bytes = __atomic_load_n(&mount->cached_free_bytes, __ATOMIC_ACQUIRE);
    drive.present = mount->active;
    drive.mounted = mount->active;
    drive.read_only = false;
    drive.healthy = mount->healthy && !mount->close_failed;
    return drive;
}

uint64_t ext4_backend_completion_count(enum phipfs_volume volume)
{
    return valid_volume(volume) ? ext4_mounts[volume].completion_count : 0U;
}

bool ext4_backend_recovery_report(enum phipfs_volume volume,
    struct phipia_ext4_recovery_report *report)
{
    const struct ext4_mount_state *mount;

    if (!valid_volume(volume) || report == NULL ||
        !ext4_mounts[volume].active) {
        return false;
    }
    mount = &ext4_mounts[volume];
    report->transactions = mount->identity.recovered_transactions;
    report->replayed_blocks = mount->identity.replayed_blocks;
    report->consumed_slots = mount->identity.consumed_slots;
    report->performed = mount->identity.recovery_performed != 0U;
    return true;
}

enum phipfs_status ext4_backend_open(enum phipfs_volume volume,
    const char *path, enum phipfs_access access, phipfs_handle *handle)
{
    struct phipfs_stat stat;
    return ext4_backend_open_with_stat(volume, path, access, handle, &stat);
}

enum phipfs_status ext4_backend_open_with_stat(enum phipfs_volume volume,
    const char *path, enum phipfs_access access, phipfs_handle *handle, struct phipfs_stat *stat)
{
    struct phipia_ext4_metadata metadata;
    const size_t length = path_length(path);
    phipfs_handle opened = 0U;
    enum phipfs_status status;

    if (handle == NULL || stat == NULL || !valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH ||
        (access != PHIPFS_ACCESS_READ && access != PHIPFS_ACCESS_WRITE &&
            access != PHIPFS_ACCESS_READ_WRITE)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    struct ext4_mount_state *mount = &ext4_mounts[volume];
    status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_stat(mount->rust_mount,
        (const uint8_t *)path, length, &metadata));
    if (status == PHIPFS_STATUS_OK && metadata.file_type == PHIPIA_EXT4_FILE_DIRECTORY) {
        status = PHIPFS_STATUS_IS_DIRECTORY;
    }
    // Register the inode while its lookup still owns the volume lease, so
    // unlink/final-close guards cannot miss a successfully opening handle.
    if (status == PHIPFS_STATUS_OK) {
        status = allocate_handle(volume, path, metadata.inode, metadata.size,
            access, false, 0U, &opened);
    }
    const enum phipfs_status close_status = end_operation(mount, NULL);
    if (status == PHIPFS_STATUS_OK) status = close_status;
    if (status != PHIPFS_STATUS_OK && opened != 0U) {
        (void)ext4_backend_close(opened);
        opened = 0U;
    }
    *handle = opened;
    if (status == PHIPFS_STATUS_OK) fill_stat(&metadata, stat);
    return status;
}

enum phipfs_status ext4_backend_close(phipfs_handle handle)
{
    struct ext4_handle_state *state;
    enum phipfs_status status = handle_state(handle, &state);

    if (status == PHIPFS_STATUS_OK) {
        const enum phipfs_volume volume = state->volume;
        struct ext4_mount_state *mount = &ext4_mounts[volume];
        const bool cleanup = mount->orphan_cleanup_pending;
        bool idle = false;

        state->closing = true;
        if (!__atomic_compare_exchange_n(&mount->operation_active, &idle,
                true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            // The active operation owns final release; sync/unmount can
            // subsequently finish any orphan cleanup it leaves pending.
            return PHIPFS_STATUS_OK;
        }
        release_operation(mount);
        /* Close releases the descriptor; it is not a durability barrier.
         * Writes have already reported their commit result. Try reclaiming
         * unreferenced orphans now; a refused plan remains mount-owned and
         * retryable through sync/unmount, including forced process teardown. */
        if (cleanup) (void)ext4_backend_sync(volume);
    }
    return status;
}

static enum phipfs_status read_handle(phipfs_handle handle,
    uint8_t *destination, size_t capacity, uint64_t offset,
    size_t *read_bytes, bool advance)
{
    struct ext4_handle_state *state;
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (read_bytes == NULL || (capacity != 0U && destination == NULL)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *read_bytes = 0U;
    status = handle_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (state->directory) {
        return PHIPFS_STATUS_IS_DIRECTORY;
    }
    if ((state->access & PHIPFS_ACCESS_READ) == 0U) {
        return PHIPFS_STATUS_ACCESS;
    }
    if (capacity == 0U) {
        return PHIPFS_STATUS_OK;
    }
    /* A retained transaction or failed EOF refresh invalidates the cached
     * size. Let the checked inode reader decide EOF under the volume lease. */
    mount = &ext4_mounts[state->volume];
    status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (advance) offset = state->offset;
    status = map_status(phipia_ext4_pread_inode(mount->rust_mount, state->inode, offset,
        destination, capacity, read_bytes));
    if (status == PHIPFS_STATUS_OK) {
        if (*read_bytes > capacity || *read_bytes > UINT64_MAX - offset) {
            status = PHIPFS_STATUS_CORRUPT;
        } else if (advance) {
            state->offset = offset + *read_bytes;
        }
    }
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_pread(phipfs_handle handle,
    uint8_t *destination, size_t capacity, uint64_t offset, size_t *read_bytes)
{
    return read_handle(handle, destination, capacity, offset, read_bytes, false);
}

enum phipfs_status ext4_backend_read(phipfs_handle handle,
    uint8_t *destination, size_t capacity, size_t *read_bytes)
{
    return read_handle(handle, destination, capacity, 0U, read_bytes, true);
}

enum phipfs_status ext4_backend_transaction_probe(enum phipfs_volume volume,
    const char *path, uint64_t offset, const uint8_t *source,
    size_t source_bytes, size_t *written_bytes)
{
    struct ext4_mount_state *mount;
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U ||
        length >= PHIPFS_MAX_PATH || source == NULL || source_bytes == 0U ||
        source_bytes > EXT4_TRANSACTION_PROBE_MAX_BYTES ||
        written_bytes == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *written_bytes = 0U;
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_transaction_probe(mount->rust_mount,
        (const uint8_t *)path, length, offset, source, source_bytes,
        written_bytes));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_truncate_probe(enum phipfs_volume volume,
    const char *path, uint64_t size)
{
    struct ext4_mount_state *mount;
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_truncate_probe(mount->rust_mount,
        (const uint8_t *)path, length, size));
    if (status == PHIPFS_STATUS_OK) {
        struct phipia_ext4_metadata metadata;

        zero_bytes(&metadata, sizeof(metadata));
        status = map_status(phipia_ext4_stat(mount->rust_mount,
            (const uint8_t *)path, length, &metadata));
        if (status == PHIPFS_STATUS_OK) {
            /* Publish the checkpointed size before another writer can
             * acquire the volume. Never overwrite its newer EOF after close. */
            update_open_sizes(volume, metadata.inode, metadata.size);
        }
    }
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_create_file_probe(enum phipfs_volume volume,
    const char *path, uint16_t mode)
{
    struct ext4_mount_state *mount;
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_create_file_probe(mount->rust_mount,
        (const uint8_t *)path, length, mode));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

static enum phipfs_status remove_path(enum phipfs_volume volume,
    const char *path, bool remove_directory)
{
    struct ext4_mount_state *mount;
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    uint64_t open_inodes[EXT4_MAX_HANDLES];
    const size_t open_count = collect_open_inodes(volume, open_inodes, true);
    if (open_count != 0U) mount->orphan_cleanup_pending = true;
    status = map_status(phipia_ext4_unlink_file_probe(mount->rust_mount,
        (const uint8_t *)path, length, open_inodes, open_count, remove_directory));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_unlink_file_probe(enum phipfs_volume volume, const char *path)
{
    return remove_path(volume, path, false);
}

enum phipfs_status ext4_backend_remove(enum phipfs_volume volume, const char *path)
{
    return remove_path(volume, path, true);
}

enum phipfs_status ext4_backend_link_file_probe(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    struct ext4_mount_state *mount;
    const size_t source_length = path_length(source);
    const size_t destination_length = path_length(destination);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || source_length == 0U ||
        source_length >= PHIPFS_MAX_PATH || destination_length == 0U ||
        destination_length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_link_file_probe(mount->rust_mount,
        (const uint8_t *)source, source_length,
        (const uint8_t *)destination, destination_length));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_create_directory_probe(
    enum phipfs_volume volume, const char *path)
{
    struct ext4_mount_state *mount;
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_create_directory_probe(mount->rust_mount,
        (const uint8_t *)path, length));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_remove_directory_probe(
    enum phipfs_volume volume, const char *path)
{
    struct ext4_mount_state *mount;
    const size_t length = path_length(path);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    uint64_t open_inodes[EXT4_MAX_HANDLES];
    const size_t open_count = collect_open_inodes(volume, open_inodes, true);
    if (open_count != 0U) mount->orphan_cleanup_pending = true;
    status = map_status(phipia_ext4_remove_directory_probe(mount->rust_mount,
        (const uint8_t *)path, length, open_inodes, open_count));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_rename_probe(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    struct ext4_mount_state *mount;
    const size_t source_length = path_length(source);
    const size_t destination_length = path_length(destination);
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || source_length == 0U ||
        source_length >= PHIPFS_MAX_PATH || destination_length == 0U ||
        destination_length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_rename_probe(mount->rust_mount,
        (const uint8_t *)source, source_length,
        (const uint8_t *)destination, destination_length));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_write(phipfs_handle handle,
    const uint8_t *source, size_t source_bytes, size_t *written_bytes)
{
    struct ext4_handle_state *state;
    struct ext4_mount_state *mount;
    uint64_t end;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (written_bytes == NULL || (source_bytes != 0U && source == NULL)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *written_bytes = 0U;
    status = handle_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (state->directory) {
        return PHIPFS_STATUS_IS_DIRECTORY;
    }
    if ((state->access & PHIPFS_ACCESS_WRITE) == 0U) {
        return PHIPFS_STATUS_ACCESS;
    }
    if (source_bytes == 0U) {
        return PHIPFS_STATUS_OK;
    }
    if (source_bytes > EXT4_TRANSACTION_PROBE_MAX_BYTES ||
        state->offset > PHIPFS_MAX_FILE_BYTES ||
        source_bytes > PHIPFS_MAX_FILE_BYTES - state->offset) {
        return PHIPFS_STATUS_RANGE;
    }
    mount = &ext4_mounts[state->volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_write_inode(mount->rust_mount, state->inode, state->offset,
        source, source_bytes, written_bytes));
    if (status == PHIPFS_STATUS_OK) {
        if (*written_bytes > source_bytes || *written_bytes > UINT64_MAX - state->offset) {
            status = PHIPFS_STATUS_CORRUPT;
        } else {
            end = state->offset + *written_bytes;
            state->offset = end;
            if (end > state->size) update_open_sizes(state->volume, state->inode, end);
        }
    }
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_append(phipfs_handle handle,
    const uint8_t *source, size_t source_bytes, size_t *written_bytes)
{
    struct ext4_handle_state *state;
    struct ext4_mount_state *mount;
    uint64_t start = 0U;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (written_bytes == NULL || (source_bytes != 0U && source == NULL)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *written_bytes = 0U;
    status = handle_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) return status;
    if (state->directory) return PHIPFS_STATUS_IS_DIRECTORY;
    if ((state->access & PHIPFS_ACCESS_WRITE) == 0U) return PHIPFS_STATUS_ACCESS;
    if (source_bytes == 0U) return PHIPFS_STATUS_OK;
    if (source_bytes > EXT4_TRANSACTION_PROBE_MAX_BYTES) return PHIPFS_STATUS_RANGE;
    mount = &ext4_mounts[state->volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_append_inode(mount->rust_mount, state->inode, source,
        source_bytes, PHIPFS_MAX_FILE_BYTES, &start, written_bytes));
    if (status == PHIPFS_STATUS_OK) {
        if (*written_bytes > source_bytes || start > PHIPFS_MAX_FILE_BYTES ||
            *written_bytes > PHIPFS_MAX_FILE_BYTES - start) {
            status = PHIPFS_STATUS_CORRUPT;
        } else {
            // Publish the durable EOF before releasing the writer lease. A
            // later append must not have its newer size overwritten by us.
            state->offset = start + *written_bytes;
            update_open_sizes(state->volume, state->inode, state->offset);
        }
    }
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_seek(phipfs_handle handle, int64_t offset,
    enum phipfs_seek_origin origin, uint64_t *position)
{
    struct ext4_handle_state *state;
    struct ext4_mount_state *mount = NULL;
    bool storage_lease = false;
    uint64_t base;
    uint64_t target;
    enum phipfs_status status;

    if (position == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *position = 0U;
    status = handle_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (origin == PHIPFS_SEEK_END) {
        struct phipia_ext4_metadata metadata;
        struct ext4_mount_state *candidate = &ext4_mounts[state->volume];

        status = begin_operation(candidate, false);
        if (status != PHIPFS_STATUS_OK) return status;
        mount = candidate;
        storage_lease = true;
        zero_bytes(&metadata, sizeof(metadata));
        status = map_status(phipia_ext4_stat_inode(mount->rust_mount, state->inode, &metadata));
        if (status != PHIPFS_STATUS_OK) goto done;
        if (metadata.inode != state->inode) {
            status = PHIPFS_STATUS_STALE_HANDLE;
            goto done;
        }
        update_open_sizes(state->volume, state->inode, metadata.size);
    } else {
        struct ext4_mount_state *candidate = &ext4_mounts[state->volume];
        status = reserve_operation(candidate);
        if (status != PHIPFS_STATUS_OK) return status;
        mount = candidate;
    }
    base = origin == PHIPFS_SEEK_START ? 0U :
        (origin == PHIPFS_SEEK_CURRENT ? state->offset :
            (origin == PHIPFS_SEEK_END ? state->size : UINT64_MAX));
    if (base == UINT64_MAX) {
        status = PHIPFS_STATUS_INVALID_ARGUMENT;
        goto done;
    }
    if (offset < 0) {
        const uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
        if (magnitude > base) {
            status = PHIPFS_STATUS_RANGE;
            goto done;
        }
        target = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) {
            status = PHIPFS_STATUS_RANGE;
            goto done;
        }
        target = base + (uint64_t)offset;
    }
    state->offset = target;
    *position = target;
done:
    if (mount != NULL) {
        if (storage_lease) {
            const enum phipfs_status close_status = end_operation(mount, NULL);
            if (status == PHIPFS_STATUS_OK) status = close_status;
        } else { release_operation(mount); }
    }
    return status;
}

enum phipfs_status ext4_backend_stat_path(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *stat)
{
    struct phipia_ext4_metadata metadata;
    enum phipfs_status status;

    if (stat == NULL || !valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(stat, sizeof(*stat));
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status == PHIPFS_STATUS_OK) {
        fill_stat(&metadata, stat);
    }
    return status;
}

static void fill_entry(const struct phipia_ext4_directory_entry *source,
    struct phipfs_list_entry *destination)
{
    zero_bytes(destination, sizeof(*destination));
    copy_bytes(destination->name, source->name, source->name_length);
    destination->name[source->name_length] = '\0';
    destination->size = source->metadata.size;
    destination->object_id = source->metadata.inode;
    destination->mode = source->metadata.mode;
    destination->directory =
        source->metadata.file_type == PHIPIA_EXT4_FILE_DIRECTORY;
}

static enum phipfs_status indexed_entry(struct ext4_handle_state *state,
    uint64_t index, struct phipfs_list_entry *entry, bool *present)
{
    struct phipia_ext4_directory_entry raw;
    enum phipfs_status status;
    zero_bytes(&raw, sizeof(raw));
    status = map_status(phipia_ext4_snapshot_entry(state->directory_snapshot, index, &raw, present));
    if (status == PHIPFS_STATUS_OK && *present) {
        if (raw.name_length == 0U || raw.name_length >=
                PHIPFS_MAX_COMPONENT_BYTES) {
            status = PHIPFS_STATUS_NAME;
        } else {
            fill_entry(&raw, entry);
        }
    }
    return status;
}

enum phipfs_status ext4_backend_directory_open(enum phipfs_volume volume,
    const char *path, phipfs_handle *handle)
{
    struct phipfs_stat stat;
    return ext4_backend_directory_open_with_stat(volume, path, handle, &stat);
}

enum phipfs_status ext4_backend_directory_open_with_stat(enum phipfs_volume volume,
    const char *path, phipfs_handle *handle, struct phipfs_stat *stat)
{
    struct phipia_ext4_metadata metadata;
    uintptr_t snapshot = 0U;
    phipfs_handle opened = 0U;
    const size_t length = path_length(path);
    enum phipfs_status status;

    if (!valid_volume(volume) || handle == NULL || stat == NULL || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    struct ext4_mount_state *mount = &ext4_mounts[volume];
    status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_directory_snapshot(mount->rust_mount,
        (const uint8_t *)path, length, &metadata, &snapshot));
    if (status == PHIPFS_STATUS_OK && (metadata.file_type != PHIPIA_EXT4_FILE_DIRECTORY || snapshot == 0U)) {
        status = PHIPFS_STATUS_CORRUPT;
    }
    if (status == PHIPFS_STATUS_OK) {
        status = allocate_handle(volume, path, metadata.inode, metadata.size,
            PHIPFS_ACCESS_READ, true, snapshot, &opened);
    }
    const enum phipfs_status close_status = end_operation(mount, NULL);
    if (status == PHIPFS_STATUS_OK) status = close_status;
    if (status != PHIPFS_STATUS_OK) {
        if (opened != 0U) {
            (void)ext4_backend_close(opened);
            opened = 0U;
        } else if (snapshot != 0U) {
            phipia_ext4_snapshot_free(snapshot);
        }
    }
    *handle = opened;
    if (status == PHIPFS_STATUS_OK) fill_stat(&metadata, stat);
    return status;
}

enum phipfs_status ext4_backend_directory_read(phipfs_handle handle,
    struct phipfs_list_entry *entry, bool *present)
{
    struct ext4_handle_state *state;
    enum phipfs_status status;

    if (entry == NULL || present == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(entry, sizeof(*entry));
    *present = false;
    status = handle_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (!state->directory) {
        return PHIPFS_STATUS_NOT_DIRECTORY;
    }
    struct ext4_mount_state *mount = &ext4_mounts[state->volume];
    status = reserve_operation(mount);
    if (status != PHIPFS_STATUS_OK) return status;
    status = indexed_entry(state, state->offset, entry, present);
    if (status == PHIPFS_STATUS_OK && *present) {
        ++state->offset;
    }
    release_operation(mount);
    return status;
}

enum phipfs_status ext4_backend_directory_close(phipfs_handle handle)
{
    return ext4_backend_close(handle);
}

enum phipfs_status ext4_backend_list(enum phipfs_volume volume,
    const char *path, struct phipfs_list_entry *entries, size_t capacity,
    size_t *entry_count)
{
    phipfs_handle handle = 0U;
    size_t count = 0U;
    enum phipfs_status status;

    if (entry_count == NULL || (capacity != 0U && entries == NULL)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *entry_count = 0U;
    status = ext4_backend_directory_open(volume, path, &handle);
    while (status == PHIPFS_STATUS_OK && count < capacity) {
        bool present = false;

        status = ext4_backend_directory_read(handle, &entries[count], &present);
        if (status != PHIPFS_STATUS_OK || !present) {
            break;
        }
        ++count;
    }
    if (status == PHIPFS_STATUS_OK && count == capacity) {
        struct phipfs_list_entry ignored;
        bool present = false;

        status = ext4_backend_directory_read(handle, &ignored, &present);
        if (status == PHIPFS_STATUS_OK && present) {
            status = PHIPFS_STATUS_RANGE;
        }
    }
    if (handle != 0U) {
        enum phipfs_status close_status = ext4_backend_directory_close(handle);

        if (status == PHIPFS_STATUS_OK) {
            status = close_status;
        }
    }
    *entry_count = count;
    return status;
}

enum phipfs_status ext4_backend_create(enum phipfs_volume volume,
    const char *path, uint16_t mode)
{
    return ext4_backend_create_file_probe(volume, path, mode);
}

enum phipfs_status ext4_backend_truncate(enum phipfs_volume volume,
    const char *path, uint64_t size)
{
    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (size > PHIPFS_MAX_FILE_BYTES) {
        return PHIPFS_STATUS_RANGE;
    }
    /* Retry the exact mutation before reading checkpointed metadata; the
     * coordinator keeps retained journal plans hidden from public stat. */
    return ext4_backend_truncate_probe(volume, path, size);
}

enum phipfs_status ext4_backend_mkdir(enum phipfs_volume volume,
    const char *path)
{
    return ext4_backend_create_directory_probe(volume, path);
}

enum phipfs_status ext4_backend_rename(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    /* File handles retain inode identity; directory handles own snapshots.
     * Avoid pre-stat so a retained journal rename can retry after I/O refusal. */
    return ext4_backend_rename_probe(volume, source, destination);
}

enum phipfs_status ext4_backend_unlink(enum phipfs_volume volume,
    const char *path)
{
    return ext4_backend_unlink_file_probe(volume, path);
}

enum phipfs_status ext4_backend_rmdir(enum phipfs_volume volume,
    const char *path)
{
    return ext4_backend_remove_directory_probe(volume, path);
}

enum phipfs_status ext4_backend_link(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    return ext4_backend_link_file_probe(volume, source, destination);
}

enum phipfs_status ext4_backend_set_times(enum phipfs_volume volume, const char *path,
    const struct phipfs_times *times)
{
    const size_t length = path_length(path);
    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH || times == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    struct ext4_mount_state *mount = &ext4_mounts[volume];
    enum phipfs_status status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_set_times(mount->rust_mount, (const uint8_t *)path, length,
        times->atime_seconds, times->atime_nanos, times->mtime_seconds, times->mtime_nanos));
    enum phipfs_status close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_ftruncate(phipfs_handle handle, uint64_t size)
{
    struct ext4_handle_state *state;
    enum phipfs_status status = handle_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) return status;
    if (state->directory) return PHIPFS_STATUS_IS_DIRECTORY;
    if ((state->access & PHIPFS_ACCESS_WRITE) == 0U) return PHIPFS_STATUS_ACCESS;
    if (size > PHIPFS_MAX_FILE_BYTES) return PHIPFS_STATUS_RANGE;
    struct ext4_mount_state *mount = &ext4_mounts[state->volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_truncate_inode(mount->rust_mount, state->inode, size));
    if (status == PHIPFS_STATUS_OK) update_open_sizes(state->volume, state->inode, size);
    enum phipfs_status close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_chmod(enum phipfs_volume volume,
    const char *path, uint16_t mode)
{
    const size_t length = path_length(path);
    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    struct ext4_mount_state *mount = &ext4_mounts[volume];
    enum phipfs_status status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_chmod(mount->rust_mount, (const uint8_t *)path, length, mode));
    enum phipfs_status close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

static size_t xattr_name_length(const char *name)
{
    size_t length = 0U;
    if (name == NULL) return 0U;
    while (length < 256U && name[length] != '\0') ++length;
    return length;
}

enum phipfs_status ext4_backend_set_xattr(enum phipfs_volume volume,
    const char *path, const char *name, const uint8_t *value, size_t length, bool remove)
{
    const size_t path_bytes = path_length(path);
    const size_t name_bytes = xattr_name_length(name);
    const uint8_t empty = 0U;
    if (!valid_volume(volume) || path_bytes == 0U || path_bytes >= PHIPFS_MAX_PATH ||
        name_bytes == 0U || name_bytes > 255U || length > 4096U ||
        (length != 0U && value == NULL) || (remove && length != 0U)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    struct ext4_mount_state *mount = &ext4_mounts[volume];
    enum phipfs_status status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_set_xattr(mount->rust_mount, (const uint8_t *)path,
        path_bytes, (const uint8_t *)name, name_bytes, value == NULL ? &empty : value,
        length, remove ? 1U : 0U));
    enum phipfs_status close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_get_xattr(enum phipfs_volume volume,
    const char *path, const char *name, uint8_t *output, size_t capacity, size_t *length)
{
    const size_t path_bytes = path_length(path);
    const size_t name_bytes = xattr_name_length(name);
    uint8_t empty = 0U;
    if (length == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *length = 0U;
    if (!valid_volume(volume) || path_bytes == 0U || path_bytes >= PHIPFS_MAX_PATH ||
        name_bytes == 0U || name_bytes > 255U || capacity > 4096U ||
        (capacity != 0U && output == NULL)) return PHIPFS_STATUS_INVALID_ARGUMENT;
    struct ext4_mount_state *mount = &ext4_mounts[volume];
    enum phipfs_status status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) return status;
    status = map_status(phipia_ext4_get_xattr(mount->rust_mount, (const uint8_t *)path,
        path_bytes, (const uint8_t *)name, name_bytes, output == NULL ? &empty : output,
        capacity, length));
    enum phipfs_status close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_symlink(enum phipfs_volume volume,
    const char *path, const char *target)
{
    const size_t length = path_length(path);
    const size_t target_length = path_length(target);
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH ||
        target_length == 0U || target_length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_symlink(mount->rust_mount,
        (const uint8_t *)path, length, (const uint8_t *)target, target_length));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_rename_replace(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    const size_t source_length = path_length(source);
    const size_t destination_length = path_length(destination);
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (!valid_volume(volume) || source_length == 0U || source_length >= PHIPFS_MAX_PATH ||
        destination_length == 0U || destination_length >= PHIPFS_MAX_PATH) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    uint64_t open_inodes[EXT4_MAX_HANDLES];
    const size_t open_count = collect_open_inodes(volume, open_inodes, true);
    if (open_count != 0U) mount->orphan_cleanup_pending = true;
    status = map_status(phipia_ext4_rename_replace(mount->rust_mount,
        (const uint8_t *)source, source_length,
        (const uint8_t *)destination, destination_length, open_inodes, open_count));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_readlink(enum phipfs_volume volume,
    const char *path, uint8_t *output, size_t capacity, size_t *read_bytes)
{
    const size_t length = path_length(path);
    struct ext4_mount_state *mount;
    enum phipfs_status status;
    enum phipfs_status close_status;

    if (read_bytes == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *read_bytes = 0U;
    if (!valid_volume(volume) || length == 0U || length >= PHIPFS_MAX_PATH ||
        output == NULL || capacity == 0U) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_readlink(mount->rust_mount,
        (const uint8_t *)path, length, output, capacity, read_bytes));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}
