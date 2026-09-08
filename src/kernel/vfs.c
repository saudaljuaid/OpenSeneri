/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Phipia's bounded VFS object layer. The public phipfs_* names are retained as
 * the native ABI v1 compatibility surface, while concrete filesystem handles
 * and path rules remain behind a backend contract.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32_backend.h>
#include <phipia/fat32_fs.h>
#include <phipia/ext4_fs.h>
#include <phipia/vfs_backend.h>
#include <phipia/slot_claim.h>
#include <phipia/cpu.h>

#define VFS_MAX_VNODES 128U
#define VFS_VNODE_BUCKETS 64U
#define VFS_MAX_OPEN_FILES PHIPFS_MAX_HANDLES
#define VFS_MAX_DIRECTORY_ITERATORS 32U
#define VFS_NO_INDEX UINT16_MAX

struct vfs_mount_state {
    const struct vfs_backend_ops *backend;
    uint64_t generation;
    size_t references;
    enum phipfs_volume volume;
    bool active;
    bool mounting;
    bool unmounting;
};

struct vfs_vnode_state {
    struct phipfs_stat stat;
    uint64_t generation;
    uint64_t mount_generation;
    size_t references;
    uint16_t next_bucket;
    enum phipfs_volume volume;
    char path[PHIPFS_MAX_PATH];
    bool active;
};

struct vfs_open_file_state {
    const struct vfs_backend_ops *backend;
    uint64_t generation;
    uint64_t vnode_generation;
    phipfs_handle backend_handle;
    uint16_t vnode_index;
    bool active;
    bool opening;
    bool append;
};

struct vfs_directory_state {
    struct phipfs_list_entry entries[PHIPFS_MAX_LIST_ENTRIES];
    const struct vfs_backend_ops *backend;
    phipfs_handle backend_handle;
    uint64_t generation;
    uint64_t vnode_generation;
    size_t count;
    size_t cursor;
    uint16_t vnode_index;
    bool streaming;
    bool active;
    bool opening;
};

static struct vfs_mount_state mounts[PHIPFS_VOLUME_COUNT];
static struct vfs_vnode_state vnodes[VFS_MAX_VNODES];
static bool vnode_reservations[VFS_MAX_VNODES];
static struct vfs_open_file_state open_files[VFS_MAX_OPEN_FILES];
static bool open_file_claims[VFS_MAX_OPEN_FILES];
static struct vfs_directory_state directories[VFS_MAX_DIRECTORY_ITERATORS];
static bool directory_claims[VFS_MAX_DIRECTORY_ITERATORS];
static bool vnode_metadata_owned;
static uint16_t vnode_buckets[VFS_VNODE_BUCKETS];
static uint64_t next_mount_generation = UINT64_C(1);
static uint64_t next_vnode_generation = UINT64_C(1);
static uint64_t next_open_generation = UINT64_C(1);
static uint64_t next_directory_generation = UINT64_C(1);

static const struct vfs_backend_ops fat32_backend_ops = {
    .mount = fat32_backend_mount,
    .unmount = fat32_backend_unmount,
    .sync = fat32_backend_sync,
    .drive = fat32_backend_drive,
    .completion_count = fat32_backend_completion_count,
    .open = fat32_backend_open,
    .close = fat32_backend_close,
    .read = fat32_backend_read,
    .pread = fat32_backend_pread,
    .write = fat32_backend_write,
    .seek = fat32_backend_seek,
    .stat_path = fat32_backend_stat_path,
    .list = fat32_backend_list,
    .directory_open = NULL,
    .directory_read = NULL,
    .directory_close = NULL,
    .create = fat32_backend_create,
    .truncate = fat32_backend_truncate,
    .mkdir = fat32_backend_mkdir,
    .rename = fat32_backend_rename,
    .unlink = fat32_backend_unlink,
    .rmdir = fat32_backend_rmdir,
    .link = fat32_backend_link,
    .case_sensitive = false,
};

static const struct vfs_backend_ops ext4_backend_ops = {
    .open_options = ext4_backend_open_options,
    .fsync = ext4_backend_fsync,
    .fstat = ext4_backend_fstat,
    .publish_file = ext4_backend_publish_file,
    .unlink_held_file = ext4_backend_unlink_held_file,
    .mount = ext4_backend_mount,
    .unmount = ext4_backend_unmount,
    .sync = ext4_backend_sync,
    .drive = ext4_backend_drive,
    .completion_count = ext4_backend_completion_count,
    .open = ext4_backend_open,
    .open_with_stat = ext4_backend_open_with_stat,
    .close = ext4_backend_close,
    .read = ext4_backend_read,
    .pread = ext4_backend_pread,
    .write = ext4_backend_write,
    .seek = ext4_backend_seek,
    .stat_path = ext4_backend_stat_path,
    .lstat_path = ext4_backend_lstat_path,
    .list = ext4_backend_list,
    .directory_open = ext4_backend_directory_open,
    .directory_open_with_stat = ext4_backend_directory_open_with_stat,
    .directory_read = ext4_backend_directory_read,
    .directory_close = ext4_backend_directory_close,
    .create = ext4_backend_create,
    .truncate = ext4_backend_truncate,
    .mkdir = ext4_backend_mkdir,
    .mkdir_mode = ext4_backend_mkdir_mode,
    .rename = ext4_backend_rename,
    .unlink = ext4_backend_unlink,
    .rmdir = ext4_backend_rmdir,
    .link = ext4_backend_link,
    .case_sensitive = true,
    .validates_mutation_paths = true,
    .remove = ext4_backend_remove,
    .append = ext4_backend_append,
    .chmod = ext4_backend_chmod,
    .ftruncate = ext4_backend_ftruncate,
    .set_times = ext4_backend_set_times,
    .set_xattr = ext4_backend_set_xattr,
    .get_xattr = ext4_backend_get_xattr,
    .rename_replace = ext4_backend_rename_replace,
    .symlink = ext4_backend_symlink,
    .readlink = ext4_backend_readlink,
};

static const struct vfs_backend_ops *volume_backends[PHIPFS_VOLUME_COUNT];

_Static_assert(VFS_MAX_OPEN_FILES <= UINT8_MAX,
    "VFS open-file index no longer fits encoded handle");
_Static_assert(VFS_MAX_DIRECTORY_ITERATORS <= UINT8_MAX,
    "VFS directory index no longer fits encoded handle");
_Static_assert(VFS_VNODE_BUCKETS != 0U &&
    (VFS_VNODE_BUCKETS & (VFS_VNODE_BUCKETS - 1U)) == 0U,
    "VFS vnode bucket count must be a power of two");

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

static bool valid_volume(enum phipfs_volume volume)
{
    return volume >= PHIPFS_VOLUME_SYSTEM && volume < PHIPFS_VOLUME_COUNT;
}

static size_t text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return PHIPFS_MAX_PATH;
    }
    while (length < PHIPFS_MAX_PATH && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool text_equal(const char *left, const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (index < PHIPFS_MAX_PATH && left[index] == right[index]) {
        if (left[index] == '\0') {
            return true;
        }
        ++index;
    }
    return false;
}

static uint64_t next_generation(uint64_t *counter, uint64_t maximum)
{
    uint64_t observed = __atomic_load_n(counter, __ATOMIC_RELAXED);
    for (;;) {
        const uint64_t result = observed == 0U || observed > maximum ? 1U : observed;
        const uint64_t following = result + 1U;
        if (__atomic_compare_exchange_n(counter, &observed, following, false,
                __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return result;
    }
}

static enum phipfs_status canonicalize_path(
    const char *path,
    bool case_sensitive,
    bool preserve_components,
    char canonical[PHIPFS_MAX_PATH]
)
{
    size_t component_starts[PHIPFS_MAX_DEPTH];
    const size_t length = text_length(path);
    size_t component_count = 0U;
    size_t walked_components = 0U;
    size_t used = 0U;
    size_t index = 0U;

    if (length >= PHIPFS_MAX_PATH) return PHIPFS_STATUS_NAME_TOO_LONG;
    if (canonical == NULL || length == 0U ||
        path[0] == '/' || path[0] == '\\') {
        return PHIPFS_STATUS_PATH;
    }
    zero_bytes(canonical, PHIPFS_MAX_PATH);
    while (index < length) {
        size_t start = index;
        size_t component_length;

        while (index < length && path[index] != '/') {
            const uint8_t byte = (uint8_t)path[index];

            if ((!preserve_components && byte > UINT8_C(0x7F)) || path[index] == '\\' ||
                path[index] == ':') {
                return PHIPFS_STATUS_PATH;
            }
            ++index;
        }
        component_length = index - start;
        if (component_length == 0U) {
            return PHIPFS_STATUS_PATH;
        }
        if (preserve_components && ++walked_components > PHIPFS_MAX_DEPTH) {
            return PHIPFS_STATUS_PATH;
        }
        if (component_length == 1U && path[start] == '.') {
            /* A mount-relative dot is the retained current vnode. */
        } else if (component_length == 2U && path[start] == '.' &&
                path[start + 1U] == '.') {
            if (component_count == 0U) {
                return PHIPFS_STATUS_PATH;
            }
            used = component_starts[--component_count];
            if (used != 0U) {
                --used;
            }
            canonical[used] = '\0';
        } else {
            if (component_count >= PHIPFS_MAX_DEPTH ||
                used + component_length + (used == 0U ? 0U : 1U) >=
                    PHIPFS_MAX_PATH) {
                return PHIPFS_STATUS_PATH;
            }
            if (used != 0U) {
                canonical[used++] = '/';
            }
            component_starts[component_count++] = used;
            for (size_t offset = 0U; offset < component_length; ++offset) {
                char byte = path[start + offset];

                if (!case_sensitive && byte >= 'a' && byte <= 'z') {
                    byte = (char)(byte - 'a' + 'A');
                }
                canonical[used++] = byte;
            }
            canonical[used] = '\0';
        }
        if (index < length) {
            ++index;
            if (index == length) {
                return PHIPFS_STATUS_PATH;
            }
        }
    }
    if (used == 0U) {
        canonical[0] = '.';
        canonical[1] = '\0';
    }
    if (preserve_components) {
        // Dot components must be walked by the filesystem: link/../file
        // follows the link before '..', and missing/../file must fail lookup.
        for (size_t byte = 0U; byte <= length; ++byte) {
            char value = path[byte];
            if (!case_sensitive && value >= 'a' && value <= 'z') value = (char)(value - 'a' + 'A');
            canonical[byte] = value;
        }
    }
    return PHIPFS_STATUS_OK;
}

static size_t vnode_bucket(enum phipfs_volume volume, const char *path,
    uint64_t object_id)
{
    uint64_t hash = UINT64_C(1469598103934665603) ^ (uint64_t)volume;

    if (object_id != 0U) {
        for (size_t byte = 0U; byte < sizeof(object_id); ++byte) {
            hash ^= (uint8_t)(object_id >> (byte * 8U));
            hash *= UINT64_C(1099511628211);
        }
    } else for (size_t index = 0U; path[index] != '\0'; ++index) {
        hash ^= (uint8_t)path[index];
        hash *= UINT64_C(1099511628211);
    }
    return (size_t)(hash & (VFS_VNODE_BUCKETS - 1U));
}

/* Only bounded in-memory vnode operations run under this lock. Disable local
 * interrupts before acquisition so reentry cannot wait for its own CPU. */
static bool vnode_metadata_acquire(void)
{
    const bool restore_interrupts = cpu_interrupts_enabled();
    cpu_interrupt_disable();
    while (__atomic_test_and_set(&vnode_metadata_owned, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    return restore_interrupts;
}

static void vnode_metadata_release(bool restore_interrupts)
{
    __atomic_clear(&vnode_metadata_owned, __ATOMIC_RELEASE);
    if (restore_interrupts) cpu_interrupt_enable();
}

static bool mount_retain(enum phipfs_volume volume)
{
    size_t references = __atomic_load_n(&mounts[volume].references, __ATOMIC_RELAXED);
    while (references != SIZE_MAX) {
        if (__atomic_compare_exchange_n(&mounts[volume].references, &references, references + 1U,
                false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return true;
    }
    return false;
}

static void mount_release(enum phipfs_volume volume)
{
    size_t references = __atomic_load_n(&mounts[volume].references, __ATOMIC_RELAXED);
    while (references != 0U && !__atomic_compare_exchange_n(&mounts[volume].references, &references,
            references - 1U, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) { }
}

static enum phipfs_status mount_pin(enum phipfs_volume volume, const struct vfs_backend_ops **backend)
{
    if (!valid_volume(volume) || backend == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *backend = NULL;
    const bool restore_interrupts = vnode_metadata_acquire();
    enum phipfs_status status = PHIPFS_STATUS_NOT_MOUNTED;
    if (mounts[volume].active && !mounts[volume].mounting && !mounts[volume].unmounting) {
        status = mount_retain(volume) ? PHIPFS_STATUS_OK : PHIPFS_STATUS_BUSY;
        if (status == PHIPFS_STATUS_OK) *backend = mounts[volume].backend;
    }
    vnode_metadata_release(restore_interrupts);
    return status;
}

static struct vfs_vnode_state vnode_snapshot(size_t index)
{
    struct vfs_vnode_state snapshot = {0};
    const bool restore_interrupts = vnode_metadata_acquire();
    if (index < VFS_MAX_VNODES) snapshot = vnodes[index];
    vnode_metadata_release(restore_interrupts);
    return snapshot;
}

static size_t vnode_reserve(void)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    size_t reserved = VFS_MAX_VNODES;
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index) {
        if (!vnodes[index].active && !vnode_reservations[index]) {
            vnode_reservations[index] = true;
            reserved = index;
            break;
        }
    }
    vnode_metadata_release(restore_interrupts);
    return reserved;
}

static void vnode_unreserve(size_t index)
{
    if (index >= VFS_MAX_VNODES) return;
    const bool restore_interrupts = vnode_metadata_acquire();
    vnode_reservations[index] = false;
    vnode_metadata_release(restore_interrupts);
}

static bool vnode_resources_released(void)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    bool released = true;
    for (size_t index = 0U; index < VFS_MAX_VNODES; ++index)
        if (vnodes[index].active || vnodes[index].references != 0U || vnode_reservations[index]) released = false;
    vnode_metadata_release(restore_interrupts);
    return released;
}

static void vnode_remove_from_bucket(size_t vnode_index)
{
    struct vfs_vnode_state *vnode = &vnodes[vnode_index];
    const size_t bucket = vnode_bucket(vnode->volume, vnode->path,
        vnode->stat.object_id);
    uint16_t *link = &vnode_buckets[bucket];

    for (size_t steps = 0U; steps < VFS_MAX_VNODES &&
         *link != VFS_NO_INDEX; ++steps) {
        if (*link == vnode_index) {
            *link = vnodes[*link].next_bucket;
            return;
        }
        link = &vnodes[*link].next_bucket;
    }
}

static enum phipfs_status vnode_retain_reserved_locked(
    enum phipfs_volume volume,
    const char *canonical,
    const struct phipfs_stat *stat,
    size_t *vnode_index,
    size_t reserved
)
{
    size_t bucket;
    uint16_t current;
    size_t free_index = reserved;

    if (stat == NULL || vnode_index == NULL || !valid_volume(volume) ||
        !mounts[volume].active) {
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    bucket = vnode_bucket(volume, canonical, stat->object_id);
    current = vnode_buckets[bucket];
    for (size_t steps = 0U; steps < VFS_MAX_VNODES &&
         current != VFS_NO_INDEX; ++steps) {
        struct vfs_vnode_state *vnode = &vnodes[current];

        if (vnode->active && vnode->volume == volume &&
            vnode->mount_generation == mounts[volume].generation &&
            ((stat->object_id != 0U &&
                vnode->stat.object_id == stat->object_id) ||
             (stat->object_id == 0U && text_equal(vnode->path, canonical)))) {
            if (vnode->references == SIZE_MAX) {
                return PHIPFS_STATUS_BUSY;
            }
            ++vnode->references;
            vnode->stat = *stat;
            *vnode_index = current;
            return PHIPFS_STATUS_OK;
        }
        current = vnode->next_bucket;
    }
    for (size_t index = 0U; free_index == VFS_MAX_VNODES && index < VFS_MAX_VNODES; ++index) {
        if (!vnodes[index].active && !vnode_reservations[index]) {
            free_index = index;
            break;
        }
    }
    if (free_index == VFS_MAX_VNODES || !mount_retain(volume)) {
        return PHIPFS_STATUS_NO_HANDLES;
    }
    zero_bytes(&vnodes[free_index], sizeof(vnodes[free_index]));
    vnodes[free_index].stat = *stat;
    vnodes[free_index].generation = next_generation(
        &next_vnode_generation, UINT64_MAX);
    vnodes[free_index].mount_generation = mounts[volume].generation;
    vnodes[free_index].references = 1U;
    vnodes[free_index].next_bucket = vnode_buckets[bucket];
    vnodes[free_index].volume = volume;
    copy_bytes(vnodes[free_index].path, canonical,
        text_length(canonical) + 1U);
    vnodes[free_index].active = true;
    vnode_buckets[bucket] = (uint16_t)free_index;
    *vnode_index = free_index;
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status vnode_retain_reserved(enum phipfs_volume volume,
    const char *canonical, const struct phipfs_stat *stat, size_t *vnode_index, size_t reserved)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    const enum phipfs_status status = vnode_retain_reserved_locked(volume, canonical, stat, vnode_index, reserved);
    vnode_metadata_release(restore_interrupts);
    return status;
}

static enum phipfs_status vnode_retain(enum phipfs_volume volume,
    const char *canonical, const struct phipfs_stat *stat, size_t *vnode_index)
{
    return vnode_retain_reserved(volume, canonical, stat, vnode_index, VFS_MAX_VNODES);
}

static void vnode_release_locked(size_t vnode_index, uint64_t generation)
{
    struct vfs_vnode_state *vnode;

    if (vnode_index >= VFS_MAX_VNODES) {
        return;
    }
    vnode = &vnodes[vnode_index];
    if (!vnode->active || vnode->generation != generation ||
        vnode->references == 0U) {
        return;
    }
    --vnode->references;
    if (vnode->references != 0U) {
        return;
    }
    vnode_remove_from_bucket(vnode_index);
    if (valid_volume(vnode->volume)) mount_release(vnode->volume);
    zero_bytes(vnode, sizeof(*vnode));
}

static void vnode_release(size_t vnode_index, uint64_t generation)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    vnode_release_locked(vnode_index, generation);
    vnode_metadata_release(restore_interrupts);
}

static enum phipfs_status resolve_path(
    enum phipfs_volume volume,
    const char *path,
    char canonical[PHIPFS_MAX_PATH],
    size_t *vnode_index
)
{
    char partial[PHIPFS_MAX_PATH];
    struct phipfs_stat stat;
    enum phipfs_status status;
    size_t length;

    if (!valid_volume(volume) || canonical == NULL || vnode_index == NULL) {
        return PHIPFS_STATUS_NOT_MOUNTED;
    }
    const struct vfs_backend_ops *backend;
    status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = canonicalize_path(path, backend->case_sensitive,
        backend->validates_mutation_paths, canonical);
    if (status != PHIPFS_STATUS_OK) {
        goto finished;
    }
    if (backend->validates_mutation_paths ||
        (canonical[0] == '.' && canonical[1] == '\0')) {
        status = backend->stat_path(volume, canonical, &stat);
        if (status == PHIPFS_STATUS_OK) status = vnode_retain(volume, canonical, &stat, vnode_index);
        goto finished;
    }
    zero_bytes(partial, sizeof(partial));
    length = text_length(canonical);
    for (size_t index = 0U; index <= length; ++index) {
        if (index != length && canonical[index] != '/') {
            partial[index] = canonical[index];
            continue;
        }
        partial[index] = '\0';
        status = backend->stat_path(volume, partial, &stat);
        if (status != PHIPFS_STATUS_OK) {
            goto finished;
        }
        if (index != length && !stat.directory) {
            status = PHIPFS_STATUS_NOT_DIRECTORY;
            goto finished;
        }
        if (index != length) {
            partial[index] = '/';
        }
    }
    status = vnode_retain(volume, canonical, &stat, vnode_index);
finished:
    mount_release(volume);
    return status;
}

static enum phipfs_status resolve_metadata_path(enum phipfs_volume volume,
    const char *path, char canonical[PHIPFS_MAX_PATH])
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const bool restore_interrupts = vnode_metadata_acquire();
    const struct vfs_backend_ops *backend = mounts[volume].active && !mounts[volume].mounting &&
        !mounts[volume].unmounting ? mounts[volume].backend : NULL;
    vnode_metadata_release(restore_interrupts);
    return backend != NULL ? canonicalize_path(path, backend->case_sensitive,
        backend->validates_mutation_paths, canonical) : PHIPFS_STATUS_NOT_MOUNTED;
}

static enum phipfs_status resolve_parent(
    enum phipfs_volume volume,
    const char *path,
    char canonical[PHIPFS_MAX_PATH]
)
{
    char parent[PHIPFS_MAX_PATH];
    char resolved_parent[PHIPFS_MAX_PATH];
    size_t vnode_index;
    size_t split = SIZE_MAX;
    enum phipfs_status status = resolve_metadata_path(volume, path, canonical);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (canonical[0] == '.' && canonical[1] == '\0') {
        return PHIPFS_STATUS_ACCESS;
    }
    if (mounts[volume].backend->validates_mutation_paths) {
        return PHIPFS_STATUS_OK;
    }
    for (size_t index = 0U; canonical[index] != '\0'; ++index) {
        if (canonical[index] == '/') {
            split = index;
        }
    }
    zero_bytes(parent, sizeof(parent));
    if (split == SIZE_MAX) {
        parent[0] = '.';
        parent[1] = '\0';
    } else {
        copy_bytes(parent, canonical, split);
        parent[split] = '\0';
    }
    status = resolve_path(volume, parent, resolved_parent, &vnode_index);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (!vnode_snapshot(vnode_index).stat.directory) {
        status = PHIPFS_STATUS_NOT_DIRECTORY;
    }
    vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
    return status;
}

static void install_mount(
    enum phipfs_volume volume,
    const struct vfs_backend_ops *backend
)
{
    struct phipfs_drive_info drive;

    if (backend == NULL) {
        return;
    }
    drive = backend->drive(volume);
    if (!drive.mounted) {
        return;
    }
    const bool restore_interrupts = vnode_metadata_acquire();
    mounts[volume].backend = backend;
    mounts[volume].generation = next_generation(
        &next_mount_generation, UINT64_MAX);
    mounts[volume].volume = volume;
    mounts[volume].active = true;
    vnode_metadata_release(restore_interrupts);
}

static phipfs_handle encode_handle(size_t index, uint64_t generation)
{
    return generation << 8U | (uint64_t)(index + 1U);
}

static enum phipfs_status open_file_state(
    phipfs_handle handle,
    struct vfs_open_file_state **state
)
{
    const uint64_t encoded_index = handle & UINT64_C(0xFF);
    const uint64_t generation = handle >> 8U;
    size_t index;

    if (state == NULL || encoded_index == 0U ||
        encoded_index > VFS_MAX_OPEN_FILES || generation == 0U) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!open_files[index].active ||
        open_files[index].generation != generation) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    *state = &open_files[index];
    return PHIPFS_STATUS_OK;
}

/* Caller owns vnode_metadata_owned. Never export this pointer over a callback. */
static enum phipfs_status checked_open_file_state(phipfs_handle handle,
    struct vfs_open_file_state **state)
{
    const enum phipfs_status status = open_file_state(handle, state);
    if (status != PHIPFS_STATUS_OK) return status;
    if ((*state)->vnode_index >= VFS_MAX_VNODES) return PHIPFS_STATUS_STALE_HANDLE;
    const struct vfs_vnode_state *vnode = &vnodes[(*state)->vnode_index];
    if (!vnode->active || vnode->generation != (*state)->vnode_generation ||
        !mounts[vnode->volume].active || vnode->mount_generation != mounts[vnode->volume].generation)
        return PHIPFS_STATUS_STALE_HANDLE;
    return PHIPFS_STATUS_OK;
}

struct vfs_file_snapshot {
    struct vfs_open_file_state file;
    struct vfs_vnode_state vnode;
};

static enum phipfs_status file_snapshot_pin(phipfs_handle handle,
    struct vfs_file_snapshot *snapshot)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    struct vfs_open_file_state *state;
    enum phipfs_status status = checked_open_file_state(handle, &state);
    if (status == PHIPFS_STATUS_OK) {
        const struct vfs_vnode_state *vnode = &vnodes[state->vnode_index];
        if (!mount_retain(vnode->volume)) status = PHIPFS_STATUS_BUSY;
        else {
            snapshot->file = *state;
            snapshot->vnode = *vnode;
        }
    }
    vnode_metadata_release(restore_interrupts);
    return status;
}

static enum phipfs_status directory_state(
    phipfs_directory_handle handle,
    struct vfs_directory_state **state
)
{
    const uint64_t encoded_index = handle & UINT64_C(0xFF);
    const uint64_t generation = handle >> 8U;
    size_t index;

    if (state == NULL || encoded_index == 0U ||
        encoded_index > VFS_MAX_DIRECTORY_ITERATORS || generation == 0U) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    index = (size_t)(encoded_index - 1U);
    if (!directories[index].active ||
        directories[index].generation != generation) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    *state = &directories[index];
    return PHIPFS_STATUS_OK;
}

bool phipfs_self_test(size_t *completed_tests)
{
    return fat32_backend_self_test(completed_tests);
}

bool phipfs_resources_released(void)
{
    if (!vnode_resources_released()) return false;
    const bool restore_interrupts = vnode_metadata_acquire();
    bool released = true;
    for (size_t index = 0U; index < PHIPFS_VOLUME_COUNT; ++index)
        if (mounts[index].active || mounts[index].mounting || mounts[index].unmounting ||
            __atomic_load_n(&mounts[index].references, __ATOMIC_ACQUIRE) != 0U) released = false;
    for (size_t index = 0U; index < VFS_MAX_OPEN_FILES; ++index)
        if (open_files[index].active || open_files[index].opening || open_files[index].backend_handle != 0U ||
            __atomic_load_n(&open_file_claims[index], __ATOMIC_ACQUIRE)) released = false;
    for (size_t index = 0U; index < VFS_MAX_DIRECTORY_ITERATORS; ++index)
        if (directories[index].active || directories[index].opening || directories[index].backend_handle != 0U ||
            __atomic_load_n(&directory_claims[index], __ATOMIC_ACQUIRE)) released = false;
    vnode_metadata_release(restore_interrupts);
    return released;
}

void phipfs_initialize(void)
{
    zero_bytes(mounts, sizeof(mounts));
    zero_bytes(vnodes, sizeof(vnodes));
    zero_bytes(vnode_reservations, sizeof(vnode_reservations));
    zero_bytes(open_files, sizeof(open_files));
    zero_bytes(open_file_claims, sizeof(open_file_claims));
    zero_bytes(directories, sizeof(directories));
    zero_bytes(directory_claims, sizeof(directory_claims));
    for (size_t index = 0U; index < VFS_VNODE_BUCKETS; ++index) {
        vnode_buckets[index] = VFS_NO_INDEX;
    }
    fat32_backend_initialize();
    ext4_backend_initialize();
    for (enum phipfs_volume volume = PHIPFS_VOLUME_SYSTEM;
         volume < PHIPFS_VOLUME_COUNT; ++volume) {
        if (fat32_backend_drive(volume).mounted) {
            volume_backends[volume] = &fat32_backend_ops;
        } else if (ext4_backend_mount(volume) == PHIPFS_STATUS_OK) {
            volume_backends[volume] = &ext4_backend_ops;
        } else {
            volume_backends[volume] = &fat32_backend_ops;
        }
        install_mount(volume, volume_backends[volume]);
    }
}

enum phipfs_status phipfs_mount(enum phipfs_volume volume)
{
    const struct vfs_backend_ops *backend;
    enum phipfs_status status;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    bool restore_interrupts = vnode_metadata_acquire();
    backend = volume_backends[volume];
    if (mounts[volume].mounting || mounts[volume].unmounting) status = PHIPFS_STATUS_BUSY;
    else if (mounts[volume].active) status = PHIPFS_STATUS_ALREADY_MOUNTED;
    else if (backend == NULL) status = PHIPFS_STATUS_NOT_MOUNTED;
    else { mounts[volume].mounting = true; status = PHIPFS_STATUS_OK; }
    vnode_metadata_release(restore_interrupts);
    if (status != PHIPFS_STATUS_OK) return status;
    status = backend->mount(volume);
    if (status == PHIPFS_STATUS_OK) {
        install_mount(volume, backend);
    }
    restore_interrupts = vnode_metadata_acquire();
    mounts[volume].mounting = false;
    vnode_metadata_release(restore_interrupts);
    return status;
}

enum phipfs_status phipfs_unmount(enum phipfs_volume volume)
{
    enum phipfs_status status;
    const struct vfs_backend_ops *backend = NULL;

    if (!valid_volume(volume)) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    bool restore_interrupts = vnode_metadata_acquire();
    if (mounts[volume].mounting || mounts[volume].unmounting) status = PHIPFS_STATUS_BUSY;
    else if (!mounts[volume].active) status = PHIPFS_STATUS_NOT_MOUNTED;
    else if (__atomic_load_n(&mounts[volume].references, __ATOMIC_ACQUIRE) != 0U) status = PHIPFS_STATUS_BUSY;
    else { backend = mounts[volume].backend; status = PHIPFS_STATUS_OK; }
    if (status != PHIPFS_STATUS_OK) {
        vnode_metadata_release(restore_interrupts);
        return status;
    }
    // Reserve teardown before invoking storage: a callback must not admit a
    // new description that would be erased when this unmount completes.
    // A failure republishes the same generation for retry; it is never free.
    mounts[volume].unmounting = true;
    mounts[volume].active = false;
    vnode_metadata_release(restore_interrupts);
    status = backend->unmount(volume);
    restore_interrupts = vnode_metadata_acquire();
    if (status == PHIPFS_STATUS_OK) {
        zero_bytes(&mounts[volume], sizeof(mounts[volume]));
    } else {
        mounts[volume].active = true;
        mounts[volume].unmounting = false;
    }
    vnode_metadata_release(restore_interrupts);
    return status;
}

enum phipfs_status phipfs_sync(enum phipfs_volume volume)
{
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = backend->sync(volume);
    mount_release(volume);
    return status;
}

struct phipfs_drive_info phipfs_drive(enum phipfs_volume volume)
{
    const struct phipfs_drive_info absent = {0};
    const struct vfs_backend_ops *backend;

    if (!valid_volume(volume)) {
        return absent;
    }
    const bool restore_interrupts = vnode_metadata_acquire();
    backend = mounts[volume].active ? mounts[volume].backend :
        volume_backends[volume];
    vnode_metadata_release(restore_interrupts);
    // Backend operation tables have static lifetime; callbacks acquire their
    // own metadata protection and must run outside the VFS metadata lock.
    return backend != NULL ? backend->drive(volume) : absent;
}

uint64_t phipfs_completion_count(enum phipfs_volume volume)
{
    const struct vfs_backend_ops *backend;

    if (!valid_volume(volume)) {
        return 0U;
    }
    const bool restore_interrupts = vnode_metadata_acquire();
    backend = mounts[volume].active ? mounts[volume].backend :
        volume_backends[volume];
    vnode_metadata_release(restore_interrupts);
    return backend != NULL ? backend->completion_count(volume) : 0U;
}

enum phipfs_status phipfs_open(
    enum phipfs_volume volume,
    const char *path,
    enum phipfs_access access,
    phipfs_handle *handle
)
{
    return phipfs_open_options(volume, path, access, 0U, 0644U, handle);
}

enum phipfs_status phipfs_open_options(enum phipfs_volume volume, const char *path,
    enum phipfs_access access, uint8_t flags, uint16_t mode, phipfs_handle *handle)
{
    char canonical[PHIPFS_MAX_PATH];
    phipfs_handle backend_handle = 0U;
    size_t vnode_index = VFS_NO_INDEX;
    size_t slot = VFS_MAX_OPEN_FILES;
    size_t reserved_vnode = VFS_MAX_VNODES;
    enum phipfs_status status;

    if (handle == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    if ((flags & ~(PHIPFS_OPEN_CREATE | PHIPFS_OPEN_TRUNCATE | PHIPFS_OPEN_EXCLUSIVE)) != 0U || (mode & ~07777U) != 0U ||
        ((flags & PHIPFS_OPEN_EXCLUSIVE) != 0U && (flags & PHIPFS_OPEN_CREATE) == 0U) ||
        (access != PHIPFS_ACCESS_READ && access != PHIPFS_ACCESS_WRITE && access != PHIPFS_ACCESS_READ_WRITE))
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    if ((flags & PHIPFS_OPEN_TRUNCATE) != 0U && (access & PHIPFS_ACCESS_WRITE) == 0U)
        return PHIPFS_STATUS_ACCESS;
    const struct vfs_backend_ops *backend;
    status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = resolve_metadata_path(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) {
        mount_release(volume);
        return status;
    }
    slot = phipia_slot_claim(open_file_claims, VFS_MAX_OPEN_FILES);
    if (slot == VFS_MAX_OPEN_FILES) {
        mount_release(volume);
        return PHIPFS_STATUS_NO_HANDLES;
    }
    // Backend callbacks can reenter VFS, including on another volume. Reserve
    // the description before any create/truncate or open and pin this mount
    // until the resulting vnode takes over its reference. No usable handle is
    // published while the backend operation is incomplete.
    const bool restore_reservation_interrupts = vnode_metadata_acquire();
    open_files[slot].opening = true;
    vnode_metadata_release(restore_reservation_interrupts);
    if (backend->open_options != NULL && flags != 0U) {
        // Destructive prepared opens need storage for their resulting inode
        // before the backend can commit. Other callbacks must not consume it.
        reserved_vnode = vnode_reserve();
        if (reserved_vnode == VFS_MAX_VNODES) {
            status = PHIPFS_STATUS_NO_HANDLES;
            goto failed;
        }
    }
    struct phipfs_stat opened_stat;
    if (backend->open_options == NULL) {
        // Compatibility backends retain their checked parent traversal.
        if ((flags & PHIPFS_OPEN_CREATE) != 0U) {
            status = (flags & PHIPFS_OPEN_EXCLUSIVE) != 0U ? PHIPFS_STATUS_NOT_FOUND :
                phipfs_stat_path(volume, path, &opened_stat);
            if (status == PHIPFS_STATUS_NOT_FOUND) status = phipfs_create_mode(volume, path, mode);
            if (status != PHIPFS_STATUS_OK) goto failed;
        }
        status = resolve_path(volume, path, canonical, &vnode_index);
        if (status != PHIPFS_STATUS_OK) goto failed;
        if (vnode_snapshot(vnode_index).stat.directory) {
            status = PHIPFS_STATUS_IS_DIRECTORY;
            goto failed;
        }
    }
    if (backend->open_options != NULL) {
        // The backend resolves the target and retries any retained creation or
        // inode-bound truncation before exporting a checkpointed vnode.
        status = backend->open_options(volume, canonical, access, flags, mode, &backend_handle, &opened_stat);
    } else if (backend->open_with_stat != NULL) {
        status = backend->open_with_stat(volume, canonical, access, &backend_handle, &opened_stat);
    } else {
        status = backend->open(volume, canonical, access, &backend_handle);
    }
    if (status != PHIPFS_STATUS_OK) {
        goto failed;
    }
    if (backend->open_options != NULL || backend->open_with_stat != NULL) {
        /* A rename/unlink/recreate may occur between resolve_path and open.
         * Bind the VFS description to the inode actually held by the backend. */
        if (vnode_index != VFS_NO_INDEX) vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
        vnode_index = VFS_NO_INDEX;
        status = vnode_retain_reserved(volume, canonical, &opened_stat, &vnode_index, reserved_vnode);
        if (status != PHIPFS_STATUS_OK) {
            (void)backend->close(backend_handle);
            goto failed;
        }
    }
    const bool restore_interrupts = vnode_metadata_acquire();
    zero_bytes(&open_files[slot], sizeof(open_files[slot]));
    open_files[slot].generation = next_generation(
        &next_open_generation, UINT64_MAX >> 8U);
    open_files[slot].backend = mounts[volume].backend;
    open_files[slot].vnode_generation = vnodes[vnode_index].generation;
    open_files[slot].backend_handle = backend_handle;
    open_files[slot].vnode_index = (uint16_t)vnode_index;
    open_files[slot].active = true;
    *handle = encode_handle(slot, open_files[slot].generation);
    vnode_metadata_release(restore_interrupts);
    vnode_unreserve(reserved_vnode);
    mount_release(volume);
    if (backend->open_options == NULL && (flags & PHIPFS_OPEN_TRUNCATE) != 0U) {
        status = phipfs_ftruncate(*handle, 0U);
        if (status != PHIPFS_STATUS_OK) {
            (void)phipfs_close(*handle);
            *handle = 0U;
            return status;
        }
    }
    return PHIPFS_STATUS_OK;
failed:
    vnode_unreserve(reserved_vnode);
    if (vnode_index != VFS_NO_INDEX) vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
    const bool restore_failure_interrupts = vnode_metadata_acquire();
    open_files[slot].opening = false;
    vnode_metadata_release(restore_failure_interrupts);
    mount_release(volume);
    phipia_slot_release(open_file_claims, slot);
    return status;
}

enum phipfs_status phipfs_close(phipfs_handle handle)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    struct vfs_open_file_state *state;
    phipfs_handle backend_handle;
    const struct vfs_backend_ops *backend;
    uint64_t vnode_generation;
    uint16_t vnode_index;
    enum phipfs_status status = open_file_state(handle, &state);

    if (status != PHIPFS_STATUS_OK) {
        vnode_metadata_release(restore_interrupts);
        return status;
    }
    backend_handle = state->backend_handle;
    backend = state->backend;
    vnode_generation = state->vnode_generation;
    vnode_index = state->vnode_index;
    const enum phipfs_volume volume = vnodes[vnode_index].volume;
    const bool pin = vnodes[vnode_index].active &&
        vnodes[vnode_index].generation == vnode_generation && mounts[volume].active &&
        vnodes[vnode_index].mount_generation == mounts[volume].generation;
    if (pin && !mount_retain(volume)) {
        vnode_metadata_release(restore_interrupts);
        return PHIPFS_STATUS_BUSY;
    }
    state->active = false;
    state->backend_handle = 0U;
    // Retire the VFS identity before final-close cleanup can free and reuse
    // the backend inode number during a subsequent namespace operation.
    vnode_release_locked(vnode_index, vnode_generation);
    phipia_slot_release(open_file_claims, (size_t)(state - open_files));
    vnode_metadata_release(restore_interrupts);
    status = backend->close(backend_handle);
    if (pin) mount_release(volume);
    return status;
}

enum phipfs_status phipfs_fstat(phipfs_handle handle, struct phipfs_stat *stat)
{
    struct vfs_file_snapshot snapshot;
    if (stat == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    zero_bytes(stat, sizeof(*stat));
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_open_file_state *state = &snapshot.file;
    const struct vfs_vnode_state *vnode = &snapshot.vnode;
    status = state->backend->fstat != NULL ? state->backend->fstat(state->backend_handle, stat) :
        state->backend->stat_path(vnode->volume, vnode->path, stat);
    mount_release(vnode->volume);
    return status;
}

enum phipfs_status phipfs_publish_file(phipfs_handle handle, const char *source, const char *destination)
{
    struct vfs_file_snapshot snapshot;
    char from[PHIPFS_MAX_PATH];
    char to[PHIPFS_MAX_PATH];
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_open_file_state *state = &snapshot.file;
    const struct vfs_vnode_state *vnode = &snapshot.vnode;
    status = state->backend->publish_file == NULL ? PHIPFS_STATUS_ACCESS :
        resolve_metadata_path(vnode->volume, source, from);
    if (status == PHIPFS_STATUS_OK) status = resolve_metadata_path(vnode->volume, destination, to);
    if (status == PHIPFS_STATUS_OK) status = state->backend->publish_file(state->backend_handle, from, to);
    mount_release(vnode->volume);
    return status;
}

enum phipfs_status phipfs_unlink_held_file(phipfs_handle handle, const char *path)
{
    struct vfs_file_snapshot snapshot;
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_open_file_state *state = &snapshot.file;
    const struct vfs_vnode_state *vnode = &snapshot.vnode;
    status = state->backend->unlink_held_file == NULL ? PHIPFS_STATUS_ACCESS :
        resolve_metadata_path(vnode->volume, path, canonical);
    if (status == PHIPFS_STATUS_OK) status = state->backend->unlink_held_file(state->backend_handle, canonical);
    mount_release(vnode->volume);
    return status;
}

enum phipfs_status phipfs_fsync(phipfs_handle handle)
{
    struct vfs_file_snapshot snapshot;
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_open_file_state *state = &snapshot.file;
    const struct vfs_vnode_state *vnode = &snapshot.vnode;
    status = state->backend->fsync != NULL ? state->backend->fsync(state->backend_handle) :
        phipfs_sync(vnode->volume);
    mount_release(vnode->volume);
    return status;
}

enum phipfs_status phipfs_read(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    if (read_bytes != NULL) *read_bytes = 0U;
    struct vfs_file_snapshot snapshot;
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    status = snapshot.file.backend->read(snapshot.file.backend_handle, destination, capacity, read_bytes);
    mount_release(snapshot.vnode.volume);
    return status;
}

enum phipfs_status phipfs_pread(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
)
{
    if (read_bytes != NULL) *read_bytes = 0U;
    struct vfs_file_snapshot snapshot;
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    status = snapshot.file.backend->pread(snapshot.file.backend_handle, destination, capacity, offset, read_bytes);
    mount_release(snapshot.vnode.volume);
    return status;
}

enum phipfs_status phipfs_write(
    phipfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
)
{
    if (written_bytes != NULL) *written_bytes = 0U;
    struct vfs_file_snapshot snapshot;
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_open_file_state *state = &snapshot.file;

    if (status == PHIPFS_STATUS_OK && state->append) {
        if (written_bytes == NULL || (source_bytes != 0U && source == NULL)) {
            status = PHIPFS_STATUS_INVALID_ARGUMENT;
            goto finished;
        }
        *written_bytes = 0U;
        if (source_bytes == 0U) goto finished;
        if (state->backend->append != NULL) {
            status = state->backend->append(state->backend_handle, source, source_bytes, written_bytes);
            goto finished;
        }
        uint64_t position;
        status = state->backend->seek(state->backend_handle, 0, PHIPFS_SEEK_END, &position);
    }
    if (status == PHIPFS_STATUS_OK) status = state->backend->write(
        state->backend_handle, source, source_bytes, written_bytes);
finished:
    mount_release(snapshot.vnode.volume);
    return status;
}

enum phipfs_status phipfs_set_append(phipfs_handle handle, bool append)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    struct vfs_open_file_state *state;
    enum phipfs_status status = checked_open_file_state(handle, &state);

    if (status == PHIPFS_STATUS_OK) state->append = append;
    vnode_metadata_release(restore_interrupts);
    return status;
}

enum phipfs_status phipfs_seek(
    phipfs_handle handle,
    int64_t offset,
    enum phipfs_seek_origin origin,
    uint64_t *position
)
{
    if (position != NULL) *position = 0U;
    struct vfs_file_snapshot snapshot;
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    status = snapshot.file.backend->seek(snapshot.file.backend_handle, offset, origin, position);
    mount_release(snapshot.vnode.volume);
    return status;
}

enum phipfs_status phipfs_stat_path(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_stat *stat
)
{
    char canonical[PHIPFS_MAX_PATH];
    size_t vnode_index;
    enum phipfs_status status;

    if (stat == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(stat, sizeof(*stat));
    status = resolve_path(volume, path, canonical, &vnode_index);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    *stat = vnode_snapshot(vnode_index).stat;
    vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
    return PHIPFS_STATUS_OK;
}

static enum phipfs_status vfs_lstat_path_pinned(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *stat)
{
    char canonical[PHIPFS_MAX_PATH];
    if (stat == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    zero_bytes(stat, sizeof(*stat));
    const enum phipfs_status status = resolve_metadata_path(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_backend_ops *backend = mounts[volume].backend;
    // Backends without symlinks keep their ordinary path traversal checks.
    return backend->lstat_path != NULL ? backend->lstat_path(volume, canonical, stat) :
        phipfs_stat_path(volume, canonical, stat);
}

enum phipfs_status phipfs_list(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
)
{
    char canonical[PHIPFS_MAX_PATH];
    size_t vnode_index;
    enum phipfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (!vnode_snapshot(vnode_index).stat.directory) {
        vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
        return PHIPFS_STATUS_NOT_DIRECTORY;
    }
    status = mounts[volume].backend->list(volume, canonical, entries, capacity,
        entry_count);
    vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
    return status;
}

static enum phipfs_status vfs_directory_open_pinned(
    enum phipfs_volume volume,
    const char *path,
    phipfs_directory_handle *handle
)
{
    char canonical[PHIPFS_MAX_PATH];
    size_t vnode_index;
    size_t slot = VFS_MAX_DIRECTORY_ITERATORS;
    enum phipfs_status status;

    if (handle == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    status = resolve_path(volume, path, canonical, &vnode_index);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (!vnode_snapshot(vnode_index).stat.directory) {
        vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
        return PHIPFS_STATUS_NOT_DIRECTORY;
    }
    slot = phipia_slot_claim(directory_claims, VFS_MAX_DIRECTORY_ITERATORS);
    if (slot == VFS_MAX_DIRECTORY_ITERATORS) {
        vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
        return PHIPFS_STATUS_NO_HANDLES;
    }
    bool restore_interrupts = vnode_metadata_acquire();
    directories[slot].opening = true;
    directories[slot].backend = mounts[volume].backend;
    directories[slot].cursor = 0U;
    directories[slot].count = 0U;
    directories[slot].backend_handle = 0U;
    vnode_metadata_release(restore_interrupts);
    struct phipfs_stat opened_stat;
    const struct vfs_backend_ops *backend = mounts[volume].backend;
    const bool streaming =
        (backend->directory_open != NULL || backend->directory_open_with_stat != NULL) &&
        mounts[volume].backend->directory_read != NULL &&
        mounts[volume].backend->directory_close != NULL;
    phipfs_handle backend_handle = 0U;
    size_t count = 0U;
    if (streaming) {
        if (backend->directory_open_with_stat != NULL) {
            status = backend->directory_open_with_stat(volume, canonical,
                &backend_handle, &opened_stat);
        } else {
            status = backend->directory_open(volume, canonical,
                &backend_handle);
        }
    } else {
        status = mounts[volume].backend->list(volume, canonical,
            directories[slot].entries, PHIPFS_MAX_LIST_ENTRIES,
            &count);
    }
    if (status != PHIPFS_STATUS_OK) {
        vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
        restore_interrupts = vnode_metadata_acquire();
        directories[slot].opening = false;
        phipia_slot_release(directory_claims, slot);
        vnode_metadata_release(restore_interrupts);
        return status;
    }
    if (streaming && backend->directory_open_with_stat != NULL) {
        vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
        status = vnode_retain(volume, canonical, &opened_stat, &vnode_index);
        if (status != PHIPFS_STATUS_OK) {
            (void)backend->directory_close(backend_handle);
            restore_interrupts = vnode_metadata_acquire();
            directories[slot].opening = false;
            phipia_slot_release(directory_claims, slot);
            vnode_metadata_release(restore_interrupts);
            return status;
        }
    }
    restore_interrupts = vnode_metadata_acquire();
    directories[slot].streaming = streaming;
    directories[slot].backend_handle = backend_handle;
    directories[slot].count = count;
    directories[slot].generation = next_generation(
        &next_directory_generation, UINT64_MAX >> 8U);
    directories[slot].vnode_generation = vnodes[vnode_index].generation;
    directories[slot].vnode_index = (uint16_t)vnode_index;
    directories[slot].active = true;
    directories[slot].opening = false;
    *handle = encode_handle(slot, directories[slot].generation);
    vnode_metadata_release(restore_interrupts);
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_directory_read(
    phipfs_directory_handle handle,
    struct phipfs_list_entry *entry,
    bool *present
)
{
    struct vfs_directory_state *state;
    enum phipfs_status status;

    if (entry == NULL || present == NULL) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(entry, sizeof(*entry));
    *present = false;
    const bool restore_interrupts = vnode_metadata_acquire();
    status = directory_state(handle, &state);
    if (status != PHIPFS_STATUS_OK) {
        vnode_metadata_release(restore_interrupts);
        return status;
    }
    const struct vfs_vnode_state *vnode = &vnodes[state->vnode_index];
    if (!vnode->active || vnode->generation != state->vnode_generation ||
        !mounts[vnode->volume].active || vnode->mount_generation != mounts[vnode->volume].generation) {
        vnode_metadata_release(restore_interrupts);
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    if (state->streaming) {
        const enum phipfs_volume volume = vnode->volume;
        const struct vfs_backend_ops *backend = state->backend;
        const phipfs_handle backend_handle = state->backend_handle;
        const bool pinned = mount_retain(volume);
        vnode_metadata_release(restore_interrupts);
        if (!pinned) return PHIPFS_STATUS_BUSY;
        status = backend->directory_read(backend_handle, entry, present);
        mount_release(volume);
        return status;
    }
    if (state->cursor > state->count || state->count > PHIPFS_MAX_LIST_ENTRIES) {
        status = PHIPFS_STATUS_CORRUPT;
    } else if (state->cursor < state->count) {
        *entry = state->entries[state->cursor++];
        *present = true;
    }
    vnode_metadata_release(restore_interrupts);
    return status;
}

enum phipfs_status phipfs_directory_close(phipfs_directory_handle handle)
{
    const bool restore_interrupts = vnode_metadata_acquire();
    struct vfs_directory_state *state;
    const struct vfs_backend_ops *backend;
    phipfs_handle backend_handle;
    bool streaming;
    uint64_t vnode_generation;
    uint16_t vnode_index;
    enum phipfs_status status = directory_state(handle, &state);

    if (status != PHIPFS_STATUS_OK) {
        vnode_metadata_release(restore_interrupts);
        return status;
    }
    vnode_generation = state->vnode_generation;
    vnode_index = state->vnode_index;
    backend = state->backend;
    backend_handle = state->backend_handle;
    streaming = state->streaming;
    const enum phipfs_volume volume = vnodes[vnode_index].volume;
    const bool pin = streaming && vnodes[vnode_index].active &&
        vnodes[vnode_index].generation == vnode_generation && mounts[volume].active &&
        vnodes[vnode_index].mount_generation == mounts[volume].generation;
    if (pin && !mount_retain(volume)) {
        vnode_metadata_release(restore_interrupts);
        return PHIPFS_STATUS_BUSY;
    }
    state->active = false;
    state->backend_handle = 0U;
    vnode_release_locked(vnode_index, vnode_generation);
    phipia_slot_release(directory_claims, (size_t)(state - directories));
    vnode_metadata_release(restore_interrupts);
    if (streaming) status = backend->directory_close(backend_handle);
    if (pin) mount_release(volume);
    return status;
}

enum phipfs_status phipfs_create(enum phipfs_volume volume, const char *path)
{
    return phipfs_create_mode(volume, path, UINT16_C(0644));
}

static enum phipfs_status vfs_create_mode_pinned(enum phipfs_volume volume,
    const char *path, uint16_t mode)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, path, canonical);

    if ((mode & (uint16_t)~UINT16_C(07777)) != 0U) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    return status == PHIPFS_STATUS_OK ?
        mounts[volume].backend->create(volume, canonical, mode) : status;
}

enum phipfs_status phipfs_ftruncate(phipfs_handle handle, uint64_t size)
{
    struct vfs_file_snapshot snapshot;
    enum phipfs_status status = file_snapshot_pin(handle, &snapshot);
    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_open_file_state *state = &snapshot.file;
    const struct vfs_vnode_state *vnode = &snapshot.vnode;
    status = state->backend->ftruncate != NULL ? state->backend->ftruncate(state->backend_handle, size) :
        state->backend->truncate(vnode->volume, vnode->path, size);
    mount_release(vnode->volume);
    return status;
}

static enum phipfs_status vfs_truncate_pinned(
    enum phipfs_volume volume,
    const char *path,
    uint64_t size
)
{
    char canonical[PHIPFS_MAX_PATH];
    if (valid_volume(volume) && mounts[volume].active &&
        mounts[volume].backend->validates_mutation_paths) {
        enum phipfs_status result = resolve_parent(volume, path, canonical);
        return result == PHIPFS_STATUS_OK ?
            mounts[volume].backend->truncate(volume, canonical, size) : result;
    }
    size_t vnode_index;
    enum phipfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (vnode_snapshot(vnode_index).stat.directory) {
        status = PHIPFS_STATUS_IS_DIRECTORY;
    }
    vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
    return status == PHIPFS_STATUS_OK ?
        mounts[volume].backend->truncate(volume, canonical, size) : status;
}

enum phipfs_status phipfs_mkdir(enum phipfs_volume volume, const char *path)
{
    return phipfs_mkdir_mode(volume, path, 0755U);
}

static enum phipfs_status vfs_mkdir_mode_pinned(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    char canonical[PHIPFS_MAX_PATH];
    if ((mode & ~07777U) != 0U) return PHIPFS_STATUS_INVALID_ARGUMENT;
    enum phipfs_status status = resolve_parent(volume, path, canonical);

    if (status != PHIPFS_STATUS_OK) return status;
    const struct vfs_backend_ops *backend = mounts[volume].backend;
    return backend->mkdir_mode != NULL ? backend->mkdir_mode(volume, canonical, mode) :
        backend->mkdir(volume, canonical);
}

static enum phipfs_status vfs_rename_pinned(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
)
{
    char source_canonical[PHIPFS_MAX_PATH];
    char destination_canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, source, source_canonical);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = resolve_parent(volume, destination, destination_canonical);
    return status == PHIPFS_STATUS_OK ? mounts[volume].backend->rename(volume,
        source_canonical, destination_canonical) : status;
}

static enum phipfs_status vfs_unlink_pinned(enum phipfs_volume volume, const char *path)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, path, canonical);
    return status == PHIPFS_STATUS_OK ?
        mounts[volume].backend->unlink(volume, canonical) : status;
}

static enum phipfs_status vfs_remove_pinned(enum phipfs_volume volume, const char *path)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) return status;
    if (mounts[volume].backend->remove != NULL) {
        return mounts[volume].backend->remove(volume, canonical);
    }
    status = phipfs_unlink(volume, canonical);
    return status == PHIPFS_STATUS_IS_DIRECTORY ? phipfs_rmdir(volume, canonical) : status;
}

static enum phipfs_status vfs_rename_replace_pinned(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    char from[PHIPFS_MAX_PATH];
    char to[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, source, from);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = resolve_parent(volume, destination, to);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    return mounts[volume].backend->rename_replace == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->rename_replace(volume, from, to);
}

bool phipfs_has_atomic_replace(enum phipfs_volume volume)
{
    if (!valid_volume(volume)) return false;
    const bool restore_interrupts = vnode_metadata_acquire();
    const bool supported = mounts[volume].active &&
        mounts[volume].backend->rename_replace != NULL;
    vnode_metadata_release(restore_interrupts);
    return supported;
}

static enum phipfs_status vfs_rmdir_pinned(enum phipfs_volume volume, const char *path)
{
    char canonical[PHIPFS_MAX_PATH];
    if (valid_volume(volume) && mounts[volume].active &&
        mounts[volume].backend->validates_mutation_paths) {
        enum phipfs_status result = resolve_parent(volume, path, canonical);
        return result == PHIPFS_STATUS_OK ?
            mounts[volume].backend->rmdir(volume, canonical) : result;
    }
    size_t vnode_index;
    enum phipfs_status status = resolve_path(
        volume, path, canonical, &vnode_index);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    if (!vnode_snapshot(vnode_index).stat.directory) {
        status = PHIPFS_STATUS_NOT_DIRECTORY;
    } else if (vnode_snapshot(vnode_index).references != 1U) {
        status = PHIPFS_STATUS_BUSY;
    }
    vnode_release(vnode_index, vnode_snapshot(vnode_index).generation);
    return status == PHIPFS_STATUS_OK ?
        mounts[volume].backend->rmdir(volume, canonical) : status;
}

static enum phipfs_status vfs_link_pinned(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
)
{
    char source_canonical[PHIPFS_MAX_PATH];
    char destination_canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, source, source_canonical);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    status = resolve_parent(volume, destination, destination_canonical);
    return status == PHIPFS_STATUS_OK ? mounts[volume].backend->link(volume,
        source_canonical, destination_canonical) : status;
}

static enum phipfs_status vfs_set_times_pinned(enum phipfs_volume volume, const char *path,
    const struct phipfs_times *times)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_metadata_path(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) return status;
    return mounts[volume].backend->set_times == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->set_times(volume, canonical, times);
}

static enum phipfs_status vfs_chmod_pinned(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_metadata_path(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) return status;
    return mounts[volume].backend->chmod == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->chmod(volume, canonical, mode);
}

static enum phipfs_status vfs_set_xattr_pinned(enum phipfs_volume volume, const char *path,
    const char *name, const uint8_t *value, size_t length, bool remove)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_metadata_path(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) return status;
    return mounts[volume].backend->set_xattr == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->set_xattr(volume, canonical, name, value, length, remove);
}

static enum phipfs_status vfs_get_xattr_pinned(enum phipfs_volume volume, const char *path,
    const char *name, uint8_t *output, size_t capacity, size_t *length)
{
    char canonical[PHIPFS_MAX_PATH];
    if (length == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *length = 0U;
    enum phipfs_status status = resolve_metadata_path(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) return status;
    status = mounts[volume].backend->get_xattr == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->get_xattr(volume, canonical, name, output, capacity, length);
    if (status != PHIPFS_STATUS_OK) *length = 0U;
    return status;
}

static enum phipfs_status vfs_symlink_pinned(enum phipfs_volume volume,
    const char *path, const char *target)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status = resolve_parent(volume, path, canonical);

    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    return mounts[volume].backend->symlink == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->symlink(volume, canonical, target);
}

static enum phipfs_status vfs_readlink_pinned(enum phipfs_volume volume,
    const char *path, uint8_t *output, size_t capacity, size_t *read_bytes)
{
    char canonical[PHIPFS_MAX_PATH];
    enum phipfs_status status;

    if (read_bytes == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *read_bytes = 0U;
    if (output == NULL || capacity == 0U) {
        return PHIPFS_STATUS_INVALID_ARGUMENT;
    }
    /* Resolve only the parent so dangling and looping final links are readable. */
    status = resolve_parent(volume, path, canonical);
    if (status != PHIPFS_STATUS_OK) {
        return status;
    }
    return mounts[volume].backend->readlink == NULL ? PHIPFS_STATUS_ACCESS :
        mounts[volume].backend->readlink(volume, canonical, output, capacity, read_bytes);
}

/* Keep the mount identity stable through all path validation and backend
 * results. These wrappers own references, not a lock across storage calls. */
enum phipfs_status phipfs_lstat_path(enum phipfs_volume volume,
    const char *path, struct phipfs_stat *stat)
{
    if (stat == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    zero_bytes(stat, sizeof(*stat));
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_lstat_path_pinned(volume, path, stat);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_directory_open(
    enum phipfs_volume volume,
    const char *path,
    phipfs_directory_handle *handle
)
{
    if (handle == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *handle = 0U;
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_directory_open_pinned(volume, path, handle);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_create_mode(enum phipfs_volume volume,
    const char *path, uint16_t mode)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_create_mode_pinned(volume, path, mode);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_truncate(
    enum phipfs_volume volume,
    const char *path,
    uint64_t size
)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_truncate_pinned(volume, path, size);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_mkdir_mode(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    if ((mode & ~07777U) != 0U) return PHIPFS_STATUS_INVALID_ARGUMENT;
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_mkdir_mode_pinned(volume, path, mode);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_rename(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_rename_pinned(volume, source, destination);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_unlink(enum phipfs_volume volume, const char *path)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_unlink_pinned(volume, path);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_remove(enum phipfs_volume volume, const char *path)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_remove_pinned(volume, path);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_rename_replace(enum phipfs_volume volume,
    const char *source, const char *destination)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_rename_replace_pinned(volume, source, destination);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_rmdir(enum phipfs_volume volume, const char *path)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_rmdir_pinned(volume, path);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_link(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_link_pinned(volume, source, destination);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_set_times(enum phipfs_volume volume, const char *path,
    const struct phipfs_times *times)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_set_times_pinned(volume, path, times);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_chmod(enum phipfs_volume volume, const char *path, uint16_t mode)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_chmod_pinned(volume, path, mode);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_set_xattr(enum phipfs_volume volume, const char *path,
    const char *name, const uint8_t *value, size_t length, bool remove)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_set_xattr_pinned(volume, path, name, value, length, remove);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_get_xattr(enum phipfs_volume volume, const char *path,
    const char *name, uint8_t *output, size_t capacity, size_t *length)
{
    if (length == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *length = 0U;
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_get_xattr_pinned(volume, path, name, output, capacity, length);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_symlink(enum phipfs_volume volume,
    const char *path, const char *target)
{
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_symlink_pinned(volume, path, target);
    mount_release(volume);
    return status;
}

enum phipfs_status phipfs_readlink(enum phipfs_volume volume,
    const char *path, uint8_t *output, size_t capacity, size_t *read_bytes)
{
    if (read_bytes == NULL) return PHIPFS_STATUS_INVALID_ARGUMENT;
    *read_bytes = 0U;
    if (output == NULL || capacity == 0U) return PHIPFS_STATUS_INVALID_ARGUMENT;
    if (!valid_volume(volume)) return PHIPFS_STATUS_NOT_MOUNTED;
    const struct vfs_backend_ops *backend;
    enum phipfs_status status = mount_pin(volume, &backend);
    if (status != PHIPFS_STATUS_OK) return status;
    status = vfs_readlink_pinned(volume, path, output, capacity, read_bytes);
    mount_release(volume);
    return status;
}
