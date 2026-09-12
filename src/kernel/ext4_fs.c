/* SPDX-License-Identifier: GPL-3.0-only */
/* Journaled ext4 VFS backend over the checked Rust ext4plus adapter. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/console.h>
#include <phipia/cpu.h>
#include <phipia/ext4_fs.h>
#include <phipia/nvme.h>

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
    uint64_t completion_count;
    uint32_t controller_index;
    bool active;
    bool mounting;
    bool operation_active;
    bool healthy;
};

struct ext4_handle_state {
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
extern int32_t phipia_ext4_sync(uintptr_t mounted);
extern int32_t phipia_ext4_free_bytes(uintptr_t mounted, uint64_t *free_bytes);
extern int32_t phipia_ext4_unmount(uintptr_t mounted);
extern int32_t phipia_ext4_stat(uintptr_t mounted, const uint8_t *path,
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
    const uint8_t *path, size_t path_length);
extern int32_t phipia_ext4_link_file_probe(uintptr_t mounted,
    const uint8_t *source, size_t source_length, const uint8_t *destination,
    size_t destination_length);
extern int32_t phipia_ext4_create_directory_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length);
extern int32_t phipia_ext4_remove_directory_probe(uintptr_t mounted,
    const uint8_t *path, size_t path_length);
extern int32_t phipia_ext4_rename_probe(uintptr_t mounted,
    const uint8_t *source, size_t source_length, const uint8_t *destination,
    size_t destination_length);
extern int32_t phipia_ext4_directory_entry(uintptr_t mounted,
    const uint8_t *path, size_t path_length, uint64_t index,
    struct phipia_ext4_directory_entry *entry, bool *present);

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
    default:
        return PHIPFS_STATUS_CORRUPT;
    }
}

static enum phipfs_status begin_operation(
    struct ext4_mount_state *mount,
    bool writable
)
{
    enum nvme_status status;

    if (mount == NULL || (!mount->active && !mount->mounting)) {
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    if (mount->active && !mount->healthy) {
        return PHIPFS_STATUS_IO;
    }
    if (mount->operation_active) {
        return PHIPFS_STATUS_BUSY;
    }
    status = nvme_volume_open(&mount->session, mount->controller_index,
        writable);
    if (status != NVME_STATUS_OK) {
        return PHIPFS_STATUS_IO;
    }
    if (mount->session.logical_block_bytes == 0U ||
        mount->session.namespace_blocks >
            UINT64_MAX / mount->session.logical_block_bytes) {
        (void)nvme_volume_close(&mount->session);
        zero_bytes(&mount->session, sizeof(mount->session));
        return PHIPFS_STATUS_RANGE;
    }
    mount->media_bytes = mount->session.namespace_blocks *
        mount->session.logical_block_bytes;
    if (mount->active && mount->admitted_media_bytes != 0U &&
        mount->media_bytes != mount->admitted_media_bytes) {
        (void)nvme_volume_close(&mount->session);
        zero_bytes(&mount->session, sizeof(mount->session));
        mount->healthy = false;
        return PHIPFS_STATUS_IO;
    }
    mount->operation_active = true;
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
    status = nvme_volume_close(&mount->session);
    if (diagnostic != NULL) {
        diagnostic->nvme_close_status = (int32_t)status;
        diagnostic->nvme_teardown_status =
            (int32_t)mount->session.close_teardown_status;
        diagnostic->nvme_resource_mismatches =
            mount->session.close_resource_mismatches;
    }
    mount->operation_active = false;
    zero_bytes(&mount->session, sizeof(mount->session));
    if (status != NVME_STATUS_OK) {
        mount->healthy = false;
        return PHIPFS_STATUS_IO;
    }
    ++mount->completion_count;
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

static enum phipfs_status checked_stat(
    struct ext4_mount_state *mount,
    const char *path,
    struct phipia_ext4_metadata *metadata
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
    status = map_status(phipia_ext4_stat(mount->rust_mount,
        (const uint8_t *)path, length, metadata));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
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
    if (!ext4_handles[index].active ||
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
    enum phipfs_access access, bool directory, phipfs_handle *handle)
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

static bool inode_is_open(enum phipfs_volume volume, uint64_t inode)
{
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (ext4_handles[index].active &&
            ext4_handles[index].volume == volume &&
            ext4_handles[index].mount_generation ==
                ext4_mounts[volume].generation &&
            ext4_handles[index].inode == inode) {
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
        zero_bytes(mount, sizeof(*mount));
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

        if (mount->rust_mount != 0U) {
            (void)phipia_ext4_unmount(mount->rust_mount);
        }
        zero_bytes(mount, sizeof(*mount));
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
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (ext4_handles[index].active &&
            ext4_handles[index].volume == volume) {
            return PHIPFS_STATUS_BUSY;
        }
    }
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    rust_status = phipia_ext4_prepare_unmount(mount->rust_mount);
    close_status = end_operation(mount, NULL);
    status = map_status(rust_status);
    if (status != PHIPFS_STATUS_OK || close_status != PHIPFS_STATUS_OK) {
        return status != PHIPFS_STATUS_OK ? status : close_status;
    }
    if (phipia_ext4_unmount(mount->rust_mount) != PHIPIA_EXT4_STATUS_OK) {
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
    status = begin_operation(mount, true);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_sync(mount->rust_mount));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

struct phipfs_drive_info ext4_backend_drive(enum phipfs_volume volume)
{
    struct phipfs_drive_info drive = {0};
    struct ext4_mount_state *mount;
    uint64_t free_bytes = 0U;

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
    if (mount->active &&
        phipia_ext4_free_bytes(mount->rust_mount, &free_bytes) ==
            PHIPIA_EXT4_STATUS_OK) {
        drive.free_bytes = free_bytes;
    }
    drive.present = mount->active;
    drive.mounted = mount->active;
    drive.read_only = false;
    drive.healthy = mount->healthy;
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
    struct phipia_ext4_metadata metadata;
    enum phipfs_status status;

    if (handle == NULL || !valid_volume(volume) ||
        (access != PHIPFS_ACCESS_READ && access != PHIPFS_ACCESS_WRITE &&
            access != PHIPFS_ACCESS_READ_WRITE)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (metadata.file_type == PHIPIA_EXT4_FILE_DIRECTORY) {
        return PHIPFS_STATUS_IS_DIRECTORY;
    }
    return allocate_handle(volume, path, metadata.inode, metadata.size, access,
        false, handle);
}

enum phipfs_status ext4_backend_close(phipfs_handle handle)
{
    struct ext4_handle_state *state;
    enum phipfs_status status = handle_state(handle, &state);

    if (status == PHIPFS_STATUS_OK) {
        zero_bytes(state, sizeof(*state));
    }
    return status;
}

enum phipfs_status ext4_backend_pread(phipfs_handle handle,
    uint8_t *destination, size_t capacity, uint64_t offset,
    size_t *read_bytes)
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
    if (capacity == 0U || offset >= state->size) {
        return PHIPFS_STATUS_OK;
    }
    mount = &ext4_mounts[state->volume];
    status = begin_operation(mount, false);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = map_status(phipia_ext4_pread(mount->rust_mount,
        (const uint8_t *)state->path, path_length(state->path), offset,
        destination, capacity, read_bytes));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_read(phipfs_handle handle,
    uint8_t *destination, size_t capacity, size_t *read_bytes)
{
    struct ext4_handle_state *state;
    enum phipfs_status status = handle_state(handle, &state);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = ext4_backend_pread(handle, destination, capacity, state->offset,
        read_bytes);
    if (status == PHIPFS_STATUS_OK && read_bytes != NULL) {
        if (*read_bytes > UINT64_MAX - state->offset) {
            return PHIPFS_STATUS_RANGE;
        }
        state->offset += *read_bytes;
    }
    return status;
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

enum phipfs_status ext4_backend_unlink_file_probe(enum phipfs_volume volume,
    const char *path)
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
    status = map_status(phipia_ext4_unlink_file_probe(mount->rust_mount,
        (const uint8_t *)path, length));
    close_status = end_operation(mount, NULL);
    return status != PHIPFS_STATUS_OK ? status : close_status;
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
    status = map_status(phipia_ext4_remove_directory_probe(mount->rust_mount,
        (const uint8_t *)path, length));
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
    status = map_status(phipia_ext4_transaction_probe(mount->rust_mount,
        (const uint8_t *)state->path, path_length(state->path), state->offset,
        source, source_bytes, written_bytes));
    close_status = end_operation(mount, NULL);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (close_status != PHIPFS_STATUS_OK) {
        return close_status;
    }
    if (*written_bytes > source_bytes ||
        *written_bytes > UINT64_MAX - state->offset) {
        return PHIPFS_STATUS_CORRUPT;
    }
    end = state->offset + *written_bytes;
    state->offset = end;
    if (end > state->size) {
        update_open_sizes(state->volume, state->inode, end);
    }
    return PHIPFS_STATUS_OK;
}

enum phipfs_status ext4_backend_seek(phipfs_handle handle, int64_t offset,
    enum phipfs_seek_origin origin, uint64_t *position)
{
    struct ext4_handle_state *state;
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
    base = origin == PHIPFS_SEEK_START ? 0U :
        (origin == PHIPFS_SEEK_CURRENT ? state->offset :
            (origin == PHIPFS_SEEK_END ? state->size : UINT64_MAX));
    if (base == UINT64_MAX) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (offset < 0) {
        const uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
        if (magnitude > base) {
            return PHIPFS_STATUS_RANGE;
        }
        target = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) {
            return PHIPFS_STATUS_RANGE;
        }
        target = base + (uint64_t)offset;
    }
    state->offset = target;
    *position = target;
    return PHIPFS_STATUS_OK;
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
    struct ext4_mount_state *mount = &ext4_mounts[state->volume];
    enum phipfs_status status = begin_operation(mount, false);
    enum phipfs_status close_status;

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    zero_bytes(&raw, sizeof(raw));
    status = map_status(phipia_ext4_directory_entry(mount->rust_mount,
        (const uint8_t *)state->path, path_length(state->path), index, &raw,
        present));
    close_status = end_operation(mount, NULL);
    if (status == PHIPFS_STATUS_OK && *present) {
        if (raw.name_length == 0U || raw.name_length >=
                PHIPFS_MAX_COMPONENT_BYTES) {
            status = PHIPFS_STATUS_NAME;
        } else {
            fill_entry(&raw, entry);
        }
    }
    return status != PHIPFS_STATUS_OK ? status : close_status;
}

enum phipfs_status ext4_backend_directory_open(enum phipfs_volume volume,
    const char *path, phipfs_handle *handle)
{
    struct phipia_ext4_metadata metadata;
    enum phipfs_status status;

    if (!valid_volume(volume) || handle == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (metadata.file_type != PHIPIA_EXT4_FILE_DIRECTORY) {
        return PHIPFS_STATUS_NOT_DIRECTORY;
    }
    return allocate_handle(volume, path, metadata.inode, metadata.size,
        PHIPFS_ACCESS_READ, true, handle);
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
    status = indexed_entry(state, state->offset, entry, present);
    if (status == PHIPFS_STATUS_OK && *present) {
        ++state->offset;
    }
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
    struct phipia_ext4_metadata metadata;
    enum phipfs_status status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    if (size > PHIPFS_MAX_FILE_BYTES) {
        return PHIPFS_STATUS_RANGE;
    }
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = ext4_backend_truncate_probe(volume, path, size);
    if (status == PHIPFS_STATUS_OK) {
        update_open_sizes(volume, metadata.inode, size);
    }
    return status;
}

enum phipfs_status ext4_backend_mkdir(enum phipfs_volume volume,
    const char *path)
{
    return ext4_backend_create_directory_probe(volume, path);
}

enum phipfs_status ext4_backend_rename(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    struct phipia_ext4_metadata metadata;
    enum phipfs_status status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    status = checked_stat(&ext4_mounts[volume], source, &metadata);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (inode_is_open(volume, metadata.inode)) {
        return PHIPFS_STATUS_BUSY;
    }
    return ext4_backend_rename_probe(volume, source, destination);
}

enum phipfs_status ext4_backend_unlink(enum phipfs_volume volume,
    const char *path)
{
    struct phipia_ext4_metadata metadata;
    enum phipfs_status status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (inode_is_open(volume, metadata.inode)) {
        return PHIPFS_STATUS_BUSY;
    }
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
