/* SPDX-License-Identifier: GPL-3.0-only */
int package_state_core_host_test_main(void);

#define main package_state_core_host_test_main
#include "package-state-host-test.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <phipia/fat32_fs.h>
#include <phipia/heap.h>
#include <phipia/package_service.h>

#define MOCK_MAX_NODES 96U
#define MOCK_MAX_FILE_BYTES 4096U
#define MOCK_MAX_HANDLES 16U
#define MOCK_MAX_EVENTS 128U
#define REMOVE_DATABASE_BYTES (PACKAGE_STATE_DATABASE_HEADER_BYTES + \
    PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES + \
    PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES)

enum mock_event {
    MOCK_EVENT_WRITE_AUTHORITY = 1,
    MOCK_EVENT_SYNC,
    MOCK_EVENT_RENAME_OLD,
    MOCK_EVENT_RENAME_AUTHORITY,
    MOCK_EVENT_UNLINK_JOURNAL
};

struct mock_node {
    bool active;
    bool directory;
    char path[PHIPFS_MAX_PATH];
    uint8_t bytes[MOCK_MAX_FILE_BYTES];
    size_t byte_count;
    uint16_t mode;
    uint64_t object_id;
};

struct mock_handle {
    bool active;
    size_t node;
    size_t offset;
};

static struct mock_node nodes[MOCK_MAX_NODES];
static struct mock_handle handles[MOCK_MAX_HANDLES];
static enum mock_event events[MOCK_MAX_EVENTS];
static size_t event_count;
static uint64_t next_object_id;
static bool fail_next_sync;
static uint32_t fail_sync_ordinal;
static uint32_t sync_attempts;

static void event(enum mock_event value)
{
    if (event_count < MOCK_MAX_EVENTS) {
        events[event_count++] = value;
    }
}

static size_t find_node(const char *path)
{
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        if (nodes[index].active && strcmp(nodes[index].path, path) == 0) {
            return index;
        }
    }
    return MOCK_MAX_NODES;
}

static size_t add_node(
    const char *path,
    bool directory,
    const uint8_t *bytes,
    size_t byte_count,
    uint16_t mode
)
{
    size_t existing = find_node(path);
    if (existing != MOCK_MAX_NODES) {
        nodes[existing].directory = directory;
        nodes[existing].byte_count = byte_count;
        nodes[existing].mode = mode;
        if (bytes != NULL && byte_count != 0U) {
            memcpy(nodes[existing].bytes, bytes, byte_count);
        }
        return existing;
    }
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        if (!nodes[index].active) {
            memset(&nodes[index], 0, sizeof(nodes[index]));
            nodes[index].active = true;
            nodes[index].directory = directory;
            nodes[index].byte_count = byte_count;
            nodes[index].mode = mode;
            nodes[index].object_id = next_object_id++;
            (void)snprintf(nodes[index].path, sizeof(nodes[index].path),
                "%s", path);
            if (bytes != NULL && byte_count != 0U) {
                memcpy(nodes[index].bytes, bytes, byte_count);
            }
            return index;
        }
    }
    return MOCK_MAX_NODES;
}

static void add_directory(const char *path)
{
    (void)add_node(path, true, NULL, 0U, UINT16_C(0555));
}

static void add_file(
    const char *path,
    const uint8_t *bytes,
    size_t byte_count,
    uint16_t mode
)
{
    (void)add_node(path, false, bytes, byte_count, mode);
}

static void reset_filesystem(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(handles, 0, sizeof(handles));
    memset(events, 0, sizeof(events));
    event_count = 0U;
    next_object_id = 1U;
    fail_next_sync = false;
    fail_sync_ordinal = 0U;
    sync_attempts = 0U;
    add_directory("pkgstate");
    add_directory("pkgstate/gen");
    add_directory("pkgstate/gen/00000000");
}

static void add_old_generation(const uint8_t old_database[OLD_DATABASE_BYTES])
{
    add_directory("pkgstate/gen/00000000/00000001");
    add_directory("pkgstate/gen/00000000/00000001/root");
    add_file("pkgstate/gen/00000000/00000001/state.db", old_database,
        OLD_DATABASE_BYTES, UINT16_C(0444));
}

static void add_new_generation(const uint8_t new_database[NEW_DATABASE_BYTES])
{
    add_directory("pkgstate/gen/00000000/00000002");
    add_directory("pkgstate/gen/00000000/00000002/root");
    add_directory("pkgstate/gen/00000000/00000002/root/bin");
    add_directory("pkgstate/gen/00000000/00000002/root/lib");
    add_file("pkgstate/gen/00000000/00000002/state.db", new_database,
        NEW_DATABASE_BYTES, UINT16_C(0444));
    add_file("pkgstate/gen/00000000/00000002/root/bin/app",
        (const uint8_t *)"app", 3U, UINT16_C(0555));
    add_file("pkgstate/gen/00000000/00000002/root/lib/libx.so.1",
        (const uint8_t *)"lib", 3U, UINT16_C(0444));
}

static void add_bootstrap_generation(
    const uint8_t database[NEW_DATABASE_BYTES]
)
{
    add_directory("pkgstate/gen/00000000/00000001");
    add_directory("pkgstate/gen/00000000/00000001/root");
    add_directory("pkgstate/gen/00000000/00000001/root/bin");
    add_directory("pkgstate/gen/00000000/00000001/root/lib");
    add_file("pkgstate/gen/00000000/00000001/state.db", database,
        NEW_DATABASE_BYTES, UINT16_C(0444));
    add_file("pkgstate/gen/00000000/00000001/root/bin/app",
        (const uint8_t *)"app", 3U, UINT16_C(0555));
    add_file("pkgstate/gen/00000000/00000001/root/lib/libx.so.1",
        (const uint8_t *)"lib", 3U, UINT16_C(0444));
}

struct phipfs_drive_info phipfs_drive(enum phipfs_volume volume)
{
    struct phipfs_drive_info info;
    memset(&info, 0, sizeof(info));
    info.volume = volume;
    info.present = true;
    info.mounted = true;
    info.healthy = true;
    info.total_bytes = UINT64_C(64) * 1024U * 1024U;
    info.free_bytes = UINT64_C(32) * 1024U * 1024U;
    return info;
}

enum phipfs_status phipfs_sync(enum phipfs_volume volume)
{
    (void)volume;
    event(MOCK_EVENT_SYNC);
    ++sync_attempts;
    if (fail_next_sync || sync_attempts == fail_sync_ordinal) {
        fail_next_sync = false;
        return PHIPFS_STATUS_WRITEBACK;
    }
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_stat_path(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_stat *stat
)
{
    (void)volume;
    size_t index = find_node(path);
    if (index == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    memset(stat, 0, sizeof(*stat));
    stat->size = nodes[index].byte_count;
    stat->object_id = nodes[index].object_id;
    stat->mode = nodes[index].mode;
    stat->links = 1U;
    stat->directory = nodes[index].directory;
    stat->read_only = (nodes[index].mode & UINT16_C(0222)) == 0U;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_open(
    enum phipfs_volume volume,
    const char *path,
    enum phipfs_access access,
    phipfs_handle *handle
)
{
    (void)volume;
    (void)access;
    size_t node = find_node(path);
    if (node == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (nodes[node].directory) {
        return PHIPFS_STATUS_IS_DIRECTORY;
    }
    for (size_t index = 0U; index < MOCK_MAX_HANDLES; ++index) {
        if (!handles[index].active) {
            handles[index].active = true;
            handles[index].node = node;
            handles[index].offset = 0U;
            *handle = index + 1U;
            return PHIPFS_STATUS_OK;
        }
    }
    return PHIPFS_STATUS_NO_HANDLES;
}

static struct mock_handle *mock_handle(phipfs_handle handle)
{
    if (handle == 0U || handle > MOCK_MAX_HANDLES ||
        !handles[handle - 1U].active) {
        return NULL;
    }
    return &handles[handle - 1U];
}

enum phipfs_status phipfs_close(phipfs_handle handle)
{
    struct mock_handle *state = mock_handle(handle);
    if (state == NULL) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    state->active = false;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_read(
    phipfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    struct mock_handle *state = mock_handle(handle);
    if (state == NULL) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    struct mock_node *node = &nodes[state->node];
    size_t available = node->byte_count - state->offset;
    size_t count = capacity < available ? capacity : available;
    if (count != 0U) {
        memcpy(destination, node->bytes + state->offset, count);
    }
    state->offset += count;
    *read_bytes = count;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_write(
    phipfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
)
{
    struct mock_handle *state = mock_handle(handle);
    if (state == NULL) {
        return PHIPFS_STATUS_STALE_HANDLE;
    }
    if (source_bytes > MOCK_MAX_FILE_BYTES - state->offset) {
        return PHIPFS_STATUS_FULL;
    }
    struct mock_node *node = &nodes[state->node];
    memcpy(node->bytes + state->offset, source, source_bytes);
    state->offset += source_bytes;
    if (state->offset > node->byte_count) {
        node->byte_count = state->offset;
    }
    *written_bytes = source_bytes;
    if (strcmp(node->path, PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == 0) {
        event(MOCK_EVENT_WRITE_AUTHORITY);
    }
    return PHIPFS_STATUS_OK;
}

static bool direct_child(
    const char *parent,
    const char *path,
    const char **name
)
{
    size_t length = strlen(parent);
    if (strncmp(parent, path, length) != 0 || path[length] != '/' ||
        path[length + 1U] == '\0') {
        return false;
    }
    const char *child = path + length + 1U;
    if (strchr(child, '/') != NULL) {
        return false;
    }
    *name = child;
    return true;
}

enum phipfs_status phipfs_list(
    enum phipfs_volume volume,
    const char *path,
    struct phipfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
)
{
    (void)volume;
    size_t parent = find_node(path);
    if (parent == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (!nodes[parent].directory) {
        return PHIPFS_STATUS_NOT_DIRECTORY;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        const char *name;
        if (!nodes[index].active || !direct_child(path, nodes[index].path,
                &name)) {
            continue;
        }
        if (count == capacity) {
            return PHIPFS_STATUS_DIRECTORY_FULL;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        (void)snprintf(entries[count].name, sizeof(entries[count].name),
            "%s", name);
        entries[count].size = nodes[index].byte_count;
        entries[count].object_id = nodes[index].object_id;
        entries[count].mode = nodes[index].mode;
        entries[count].directory = nodes[index].directory;
        ++count;
    }
    *entry_count = count;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_create(enum phipfs_volume volume, const char *path)
{
    (void)volume;
    if (find_node(path) != MOCK_MAX_NODES) {
        return PHIPFS_STATUS_EXISTS;
    }
    return add_node(path, false, NULL, 0U, 0U) ==
        MOCK_MAX_NODES ? PHIPFS_STATUS_FULL : PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_create_mode(enum phipfs_volume volume,
    const char *path, uint16_t mode)
{
    enum phipfs_status status = phipfs_create(volume, path);

    if (status == PHIPFS_STATUS_OK) {
        size_t index = find_node(path);

        if (index == MOCK_MAX_NODES) {
            return PHIPFS_STATUS_CORRUPT;
        }
        nodes[index].mode = mode;
    }
    return status;
}

enum phipfs_status phipfs_truncate(
    enum phipfs_volume volume,
    const char *path,
    uint64_t size
)
{
    (void)volume;
    size_t node = find_node(path);

    if (node == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (nodes[node].directory) {
        return PHIPFS_STATUS_IS_DIRECTORY;
    }
    if (size > SIZE_MAX) {
        return PHIPFS_STATUS_RANGE;
    }
    nodes[node].byte_count = (size_t)size;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_mkdir(enum phipfs_volume volume, const char *path)
{
    (void)volume;
    if (find_node(path) != MOCK_MAX_NODES) {
        return PHIPFS_STATUS_EXISTS;
    }
    return add_node(path, true, NULL, 0U, 0U) == MOCK_MAX_NODES ?
        PHIPFS_STATUS_FULL : PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_rename(
    enum phipfs_volume volume,
    const char *source,
    const char *destination
)
{
    (void)volume;
    size_t node = find_node(source);
    if (node == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (find_node(destination) != MOCK_MAX_NODES) {
        return PHIPFS_STATUS_EXISTS;
    }
    (void)snprintf(nodes[node].path, sizeof(nodes[node].path), "%s",
        destination);
    if (strcmp(destination, PACKAGE_SERVICE_AUTHORITY_OLD_PATH) == 0) {
        event(MOCK_EVENT_RENAME_OLD);
    } else if (strcmp(destination, PACKAGE_SERVICE_AUTHORITY_PATH) == 0) {
        event(MOCK_EVENT_RENAME_AUTHORITY);
    }
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_unlink(enum phipfs_volume volume, const char *path)
{
    (void)volume;
    size_t node = find_node(path);
    if (node == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (nodes[node].directory) {
        return PHIPFS_STATUS_IS_DIRECTORY;
    }
    if (strcmp(path, PACKAGE_SERVICE_JOURNAL_PATH) == 0) {
        event(MOCK_EVENT_UNLINK_JOURNAL);
    }
    nodes[node].active = false;
    return PHIPFS_STATUS_OK;
}

enum phipfs_status phipfs_rmdir(enum phipfs_volume volume, const char *path)
{
    (void)volume;
    size_t node = find_node(path);
    if (node == MOCK_MAX_NODES) {
        return PHIPFS_STATUS_NOT_FOUND;
    }
    if (!nodes[node].directory) {
        return PHIPFS_STATUS_NOT_DIRECTORY;
    }
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        const char *ignored;
        if (nodes[index].active && direct_child(path, nodes[index].path,
                &ignored)) {
            return PHIPFS_STATUS_NOT_EMPTY;
        }
    }
    nodes[node].active = false;
    return PHIPFS_STATUS_OK;
}

enum heap_status heap_allocate(uint64_t size, void **pointer)
{
    if (pointer == NULL || size == 0U || size > SIZE_MAX) {
        return HEAP_STATUS_ZERO_SIZE;
    }
    *pointer = malloc((size_t)size);
    return *pointer == NULL ? HEAP_STATUS_OUT_OF_MEMORY : HEAP_STATUS_OK;
}

enum heap_status heap_free(void *pointer)
{
    if (pointer == NULL) {
        return HEAP_STATUS_BAD_POINTER;
    }
    free(pointer);
    return HEAP_STATUS_OK;
}

bool heap_is_active(void)
{
    return true;
}

static bool report_clean(const struct package_service_report *report)
{
    return report->live_file_handles == 0U &&
        report->live_allocations == 0U &&
        report->peak_file_handles == 1U &&
        report->peak_allocations >= 2U;
}

static bool selected_authority_generation(uint64_t generation)
{
    struct package_state_authority_view authority;
    size_t selected = find_node(PACKAGE_SERVICE_AUTHORITY_PATH);

    return selected != MOCK_MAX_NODES &&
        package_state_authority_parse(nodes[selected].bytes,
            nodes[selected].byte_count, &authority) == PACKAGE_STATE_STATUS_OK &&
        authority.generation == generation;
}

static size_t first_event(enum mock_event wanted)
{
    for (size_t index = 0U; index < event_count; ++index) {
        if (events[index] == wanted) {
            return index;
        }
    }
    return MOCK_MAX_EVENTS;
}

static bool prepare_workspace(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    struct package_builder_workspace *workspace
)
{
    static const uint8_t target[] = "org.phipia.app";

    memset(workspace, 0, sizeof(*workspace));
    if (package_state_database_parse(old_database, OLD_DATABASE_BYTES,
            &workspace->installed) != PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    workspace->has_installed = true;
    workspace->verified_plan.operation = PACKAGE_MANAGER_PLAN_INSTALL;
    workspace->verified_plan.target = (struct package_manager_text){
        target, sizeof(target) - 1U
    };
    workspace->verified_plan.root = workspace->verified_plan.target;
    struct package_state_database_view target_view;
    if (package_state_database_parse(new_database, NEW_DATABASE_BYTES,
            &target_view) != PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    workspace->spec.generation = target_view.generation;
    workspace->spec.abi = target_view.abi;
    workspace->spec.package_count = target_view.package_count;
    workspace->spec.dependency_count = target_view.edge_count;
    workspace->spec.file_count = target_view.file_count;
    workspace->spec.packages = workspace->packages;
    workspace->spec.dependencies = workspace->dependencies;
    workspace->spec.files = workspace->files;
    for (uint32_t index = 0U; index < target_view.package_count; ++index) {
        struct package_state_package_view package;

        if (package_state_database_package(&target_view, index, &package) !=
                PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        workspace->packages[index] = (struct package_generation_package){
            package.identifier, package.version, package.package_sha256,
            package.publisher_key_id, package.explicit_root,
            package.dependency_start, package.dependency_count,
            package.file_count
        };
    }
    for (uint32_t index = 0U; index < target_view.edge_count; ++index) {
        struct package_state_dependency_view dependency;

        if (package_state_database_dependency(&target_view, index,
                &dependency) != PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        workspace->dependencies[index] =
            (struct package_generation_dependency){
                dependency.requested, dependency.constraint,
                dependency.provider
            };
    }
    for (uint32_t index = 0U; index < target_view.file_count; ++index) {
        struct package_state_file_view file;

        if (package_state_database_file(&target_view, index, &file) !=
                PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        workspace->files[index] = (struct package_generation_file){
            file.path, file.owner_index, file.kind, file.mode, file.length,
            file.sha256, file.soname
        };
        workspace->file_sources[index] =
            (struct package_builder_file_source){
                PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD, file.owner_index, index,
                (const uint8_t *)(index == 0U ? "app" : "lib"), 3U
            };
    }
    return true;
}

static bool bootstrap_workspace(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    struct package_builder_workspace *workspace
)
{
    if (!prepare_workspace(old_database, new_database, workspace)) {
        return false;
    }
    workspace->has_installed = false;
    memset(&workspace->installed, 0, sizeof(workspace->installed));
    workspace->spec.generation = 1U;
    return true;
}

static bool repair_workspace(
    const uint8_t installed_database[NEW_DATABASE_BYTES],
    struct package_builder_workspace *workspace,
    uint8_t target_database[NEW_DATABASE_BYTES]
)
{
    static const uint8_t payload[] = "app";

    memset(workspace, 0, sizeof(*workspace));
    if (package_state_database_parse(installed_database, NEW_DATABASE_BYTES,
            &workspace->installed) != PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    workspace->has_installed = true;
    workspace->verified_plan.operation = PACKAGE_MANAGER_PLAN_REPAIR;
    workspace->spec = (struct package_generation_spec){
        workspace->installed.generation + 1U, workspace->installed.abi,
        workspace->packages, workspace->installed.package_count,
        workspace->dependencies, workspace->installed.edge_count,
        workspace->files, workspace->installed.file_count
    };
    for (uint32_t index = 0U; index < workspace->installed.package_count;
        ++index) {
        struct package_state_package_view package;

        if (package_state_database_package(&workspace->installed, index,
                &package) != PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        workspace->packages[index] = (struct package_generation_package){
            package.identifier, package.version, package.package_sha256,
            package.publisher_key_id, package.explicit_root,
            package.dependency_start, package.dependency_count,
            package.file_count
        };
    }
    for (uint32_t index = 0U; index < workspace->installed.edge_count; ++index) {
        struct package_state_dependency_view dependency;

        if (package_state_database_dependency(&workspace->installed, index,
                &dependency) != PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        workspace->dependencies[index] =
            (struct package_generation_dependency){
                dependency.requested, dependency.constraint,
                dependency.provider
            };
    }
    for (uint32_t index = 0U; index < workspace->installed.file_count; ++index) {
        struct package_state_file_view file;

        if (package_state_database_file(&workspace->installed, index, &file) !=
                PACKAGE_STATE_STATUS_OK) {
            return false;
        }
        workspace->files[index] = (struct package_generation_file){
            file.path, file.owner_index, file.kind, file.mode, file.length,
            file.sha256, file.soname
        };
        workspace->file_sources[index] =
            (struct package_builder_file_source){
                PACKAGE_BUILDER_FILE_SOURCE_INSTALLED, file.owner_index, index,
                NULL, 0U
            };
    }
    workspace->file_sources[0] = (struct package_builder_file_source){
        PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD, 0U, 0U, payload,
        sizeof(payload) - 1U
    };
    struct package_state_database_view target;
    return package_generation_encode(&workspace->spec, target_database,
        NEW_DATABASE_BYTES, &target) == PACKAGE_STATE_STATUS_OK;
}

static void build_bootstrap_database(
    uint8_t database[NEW_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES]
)
{
    memcpy(database, new_database, NEW_DATABASE_BYTES);
    put_u64(database + 24U, 1U);
    finalize_database(database, NEW_DATABASE_BYTES);
}

static int test_prepare_is_recoverable_not_authoritative(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;
    struct package_service_report report;
    struct package_state_authority_view authority;

    CHECK(workspace != NULL && prepare_workspace(old_database, new_database,
        workspace), 170);
    request = (struct package_service_prepare_request){
        workspace, new_database, NEW_DATABASE_BYTES
    };
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.prepared &&
        report.journal_present && report.generation == 2U &&
        report.files_staged == 2U && report.files_verified == 2U &&
        report.sync_count == 3U && report.rename_count == 1U &&
        report.live_file_handles == 0U && report.live_allocations == 0U,
        171);
    CHECK(find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000002/root/bin/app") !=
            MOCK_MAX_NODES &&
        package_state_authority_parse(nodes[find_node(
            PACKAGE_SERVICE_AUTHORITY_PATH)].bytes,
            PACKAGE_STATE_AUTHORITY_BYTES, &authority) ==
                PACKAGE_STATE_STATUS_OK && authority.generation == 1U, 172);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.choice == PACKAGE_STATE_RECOVERY_OLD && report.generation == 1U &&
        report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES, 173);
    free(workspace);
    return 0;
}

static int test_authoritative_snapshot(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    uint8_t snapshot[OLD_DATABASE_BYTES];
    size_t snapshot_bytes = 99U;
    struct package_service_report report;

    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    memset(snapshot, UINT8_C(0xa5), sizeof(snapshot));
    CHECK(package_service_snapshot(snapshot, sizeof(snapshot), &snapshot_bytes,
            &report) == PACKAGE_SERVICE_STATUS_OK &&
        snapshot_bytes == sizeof(snapshot) &&
        memcmp(snapshot, old_database, sizeof(snapshot)) == 0 &&
        report.generation == 1U && report_clean(&report), 174);

    memset(snapshot, UINT8_C(0xa5), sizeof(snapshot));
    snapshot_bytes = 99U;
    CHECK(package_service_snapshot(snapshot, sizeof(snapshot) - 1U,
            &snapshot_bytes, &report) == PACKAGE_SERVICE_STATUS_RESOURCE &&
        snapshot_bytes == 0U && snapshot[0] == UINT8_C(0xa5) &&
        report.live_file_handles == 0U && report.live_allocations == 0U, 175);
    return 0;
}

static int test_prepare_sync_failure_cleans_unpublished_state(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;
    struct package_service_report report;

    CHECK(workspace != NULL && prepare_workspace(old_database, new_database,
        workspace), 180);
    request = (struct package_service_prepare_request){
        workspace, new_database, NEW_DATABASE_BYTES
    };
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    fail_next_sync = true;
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_DURABILITY && !report.prepared &&
        !report.journal_present && report.live_file_handles == 0U &&
        report.live_allocations == 0U &&
        find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES, 181);
    free(workspace);
    return 0;
}

static int test_prepare_then_commit_selects_target(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;
    struct package_service_report report;

    CHECK(workspace != NULL && prepare_workspace(old_database, new_database,
        workspace), 182);
    request = (struct package_service_prepare_request){
        workspace, new_database, NEW_DATABASE_BYTES
    };
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.prepared && !report.committed,
        183);
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.committed && report.choice == PACKAGE_STATE_RECOVERY_NEW &&
        report.generation == 2U && report.cleanup_complete &&
        selected_authority_generation(2U), 184);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.generation == 2U && report.cleanup_complete &&
        selected_authority_generation(2U), 197);
    CHECK(report_clean(&report), 198);
    free(workspace);
    return 0;
}

static int test_prepare_every_durability_boundary(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;

    CHECK(workspace != NULL && prepare_workspace(old_database, new_database,
        workspace), 185);
    request = (struct package_service_prepare_request){
        workspace, new_database, NEW_DATABASE_BYTES
    };
    for (uint32_t boundary = 1U; boundary <= 3U; ++boundary) {
        struct package_service_report report;

        reset_filesystem();
        add_old_generation(old_database);
        add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
            PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
        fail_sync_ordinal = boundary;
        CHECK(package_service_prepare(&request, &report) ==
                PACKAGE_SERVICE_STATUS_DURABILITY && !report.prepared &&
            report.live_file_handles == 0U && report.live_allocations == 0U,
            186);
        if (boundary < 3U) {
            CHECK(find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) ==
                    MOCK_MAX_NODES &&
                find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES &&
                find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES,
                187);
        } else {
            CHECK(find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) ==
                    MOCK_MAX_NODES &&
                find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES &&
                find_node("pkgstate/gen/00000000/00000002") != MOCK_MAX_NODES,
                188);
            CHECK(package_service_recover(&report) ==
                    PACKAGE_SERVICE_STATUS_OK &&
                report.choice == PACKAGE_STATE_RECOVERY_OLD &&
                report.generation == 1U && report.cleanup_complete, 189);
        }
    }
    free(workspace);
    return 0;
}

static int test_prepare_copies_unchanged_installed_file(
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    static const uint8_t target[] = "org.phipia.app";
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_state_database_view target_view;
    struct package_state_package_view library;
    struct package_state_file_view library_file;
    struct package_service_prepare_request request;
    struct package_service_report report;
    uint8_t database[REMOVE_DATABASE_BYTES];

    CHECK(workspace != NULL, 190);
    memset(workspace, 0, sizeof(*workspace));
    CHECK(package_state_database_parse(new_database, NEW_DATABASE_BYTES,
            &workspace->installed) == PACKAGE_STATE_STATUS_OK &&
        package_state_database_package(&workspace->installed, 1U, &library) ==
            PACKAGE_STATE_STATUS_OK &&
        package_state_database_file(&workspace->installed, 1U, &library_file) ==
            PACKAGE_STATE_STATUS_OK, 191);
    workspace->has_installed = true;
    workspace->verified_plan.operation = PACKAGE_MANAGER_PLAN_REMOVE;
    workspace->verified_plan.target = (struct package_manager_text){
        target, sizeof(target) - 1U
    };
    workspace->verified_plan.root = workspace->verified_plan.target;
    workspace->spec = (struct package_generation_spec){
        3U, 1U, workspace->packages, 1U, workspace->dependencies, 0U,
        workspace->files, 1U
    };
    workspace->packages[0] = (struct package_generation_package){
        library.identifier, library.version, library.package_sha256,
        library.publisher_key_id, true, 0U, 0U, 1U
    };
    workspace->files[0] = (struct package_generation_file){
        library_file.path, 0U, library_file.kind, library_file.mode,
        library_file.length, library_file.sha256, library_file.soname
    };
    workspace->file_sources[0] = (struct package_builder_file_source){
        PACKAGE_BUILDER_FILE_SOURCE_INSTALLED, 1U, 1U, NULL, 0U
    };
    CHECK(package_generation_encode(&workspace->spec, database,
            sizeof(database), &target_view) == PACKAGE_STATE_STATUS_OK, 192);
    request = (struct package_service_prepare_request){
        workspace, database, sizeof(database)
    };
    reset_filesystem();
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    size_t damaged = find_node(
        "pkgstate/gen/00000000/00000002/root/lib/libx.so.1");
    nodes[damaged].bytes[0] ^= UINT8_C(1);
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE && !report.prepared &&
        find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000003") == MOCK_MAX_NODES, 196);

    reset_filesystem();
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.prepared &&
        report.files_staged == 1U && report.files_verified == 3U &&
        find_node("pkgstate/gen/00000000/00000003/root/lib/libx.so.1") !=
            MOCK_MAX_NODES, 193);
    size_t copied = find_node(
        "pkgstate/gen/00000000/00000003/root/lib/libx.so.1");
    CHECK(nodes[copied].byte_count == 3U &&
        memcmp(nodes[copied].bytes, "lib", 3U) == 0, 194);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.choice == PACKAGE_STATE_RECOVERY_OLD && report.generation == 2U &&
        find_node("pkgstate/gen/00000000/00000003") == MOCK_MAX_NODES, 195);
    free(workspace);
    return 0;
}

static int test_bootstrap_creates_generation_one(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t bootstrap_database[NEW_DATABASE_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;
    struct package_service_report report;

    CHECK(workspace != NULL && bootstrap_workspace(old_database, new_database,
        workspace), 220);
    request = (struct package_service_prepare_request){
        workspace, bootstrap_database, NEW_DATABASE_BYTES
    };
    reset_filesystem();
    nodes[find_node("pkgstate/gen/00000000")].active = false;
    nodes[find_node("pkgstate/gen")].active = false;
    nodes[find_node("pkgstate")].active = false;
    CHECK(package_service_bootstrap(&request, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.prepared && report.committed &&
        report.authority_replaced && report.cleanup_complete &&
        report.generation == 1U && report.files_staged == 2U &&
        report.files_verified == 2U && report.rename_count == 1U &&
        report.sync_count == 3U && selected_authority_generation(1U), 221);
    CHECK(find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000001/root/bin/app") !=
            MOCK_MAX_NODES && report.live_file_handles == 0U &&
        report.live_allocations == 0U && report.peak_file_handles == 1U &&
        report.peak_allocations == 1U, 222);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.generation == 1U && report.cleanup_complete &&
        selected_authority_generation(1U) && report_clean(&report), 223);
    CHECK(package_service_bootstrap(&request, &report) ==
            PACKAGE_SERVICE_STATUS_STATE && !report.committed &&
        selected_authority_generation(1U), 224);

    reset_filesystem();
    workspace->file_sources[0].kind = PACKAGE_BUILDER_FILE_SOURCE_INSTALLED;
    workspace->file_sources[0].payload = NULL;
    workspace->file_sources[0].payload_bytes = 0U;
    CHECK(package_service_bootstrap(&request, &report) ==
            PACKAGE_SERVICE_STATUS_STATE && !report.committed &&
        find_node(PACKAGE_SERVICE_AUTHORITY_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000001") == MOCK_MAX_NODES, 234);
    free(workspace);
    return 0;
}

static int test_bootstrap_every_durability_boundary_recovers(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t bootstrap_database[NEW_DATABASE_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;

    CHECK(workspace != NULL && bootstrap_workspace(old_database, new_database,
        workspace), 225);
    request = (struct package_service_prepare_request){
        workspace, bootstrap_database, NEW_DATABASE_BYTES
    };
    for (uint32_t boundary = 1U; boundary <= 3U; ++boundary) {
        struct package_service_report report;

        reset_filesystem();
        fail_sync_ordinal = boundary;
        CHECK(package_service_bootstrap(&request, &report) ==
                PACKAGE_SERVICE_STATUS_DURABILITY && !report.committed &&
            report.live_file_handles == 0U && report.live_allocations == 0U,
            226);
        if (boundary <= 2U) {
            CHECK(find_node(PACKAGE_SERVICE_AUTHORITY_PATH) == MOCK_MAX_NODES &&
                find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) ==
                    MOCK_MAX_NODES &&
                find_node("pkgstate/gen/00000000/00000001") == MOCK_MAX_NODES &&
                package_service_recover(&report) ==
                    PACKAGE_SERVICE_STATUS_ABSENT, 227);
        } else {
            CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
                report.generation == 1U && report.cleanup_complete &&
                selected_authority_generation(1U), 228);
        }
        CHECK(report.live_file_handles == 0U &&
            report.live_allocations == 0U, 229);
    }
    free(workspace);
    return 0;
}

static int test_recovery_accepts_persisted_bootstrap_prefixes(
    const uint8_t bootstrap_database[NEW_DATABASE_BYTES],
    const uint8_t bootstrap_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_service_report report;

    reset_filesystem();
    add_directory("pkgstate/gen/00000000/00000001");
    add_directory("pkgstate/gen/00000000/00000001/root");
    add_file(PACKAGE_SERVICE_AUTHORITY_NEW_PATH, bootstrap_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_ABSENT &&
        report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000001") == MOCK_MAX_NODES, 230);

    reset_filesystem();
    add_bootstrap_generation(bootstrap_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_NEW_PATH, bootstrap_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.committed && report.authority_replaced &&
        report.generation == 1U && selected_authority_generation(1U) &&
        find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == MOCK_MAX_NODES, 231);

    reset_filesystem();
    add_bootstrap_generation(bootstrap_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, bootstrap_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.generation == 1U && selected_authority_generation(1U), 232);

    reset_filesystem();
    add_file(PACKAGE_SERVICE_AUTHORITY_NEW_PATH, (const uint8_t *)"bad", 3U,
        UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) != MOCK_MAX_NODES, 233);
    return 0;
}

static int test_repair_replaces_damaged_files(
    const uint8_t installed_database[NEW_DATABASE_BYTES],
    const uint8_t installed_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_builder_workspace *workspace = malloc(sizeof(*workspace));
    struct package_service_prepare_request request;
    struct package_service_report report;
    uint8_t snapshot[NEW_DATABASE_BYTES];
    uint8_t target_database[NEW_DATABASE_BYTES];
    size_t snapshot_bytes = 0U;

    CHECK(workspace != NULL && repair_workspace(installed_database, workspace,
        target_database), 240);
    request = (struct package_service_prepare_request){
        workspace, target_database, sizeof(target_database)
    };

    reset_filesystem();
    add_new_generation(installed_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, installed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    size_t damaged = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[damaged].bytes[0] ^= UINT8_C(1);
    CHECK(package_service_snapshot(snapshot, sizeof(snapshot),
            &snapshot_bytes, &report) == PACKAGE_SERVICE_STATUS_INCOMPLETE &&
        snapshot_bytes == 0U && !report.journal_present &&
        package_service_repair_snapshot(snapshot, sizeof(snapshot),
            &snapshot_bytes, &report) == PACKAGE_SERVICE_STATUS_OK &&
        snapshot_bytes == sizeof(snapshot) && report.generation == 2U &&
        memcmp(snapshot, installed_database, sizeof(snapshot)) == 0 &&
        report.live_file_handles == 0U && report.live_allocations == 0U, 249);
    workspace->file_sources[0] = (struct package_builder_file_source){
        PACKAGE_BUILDER_FILE_SOURCE_INSTALLED, 0U, 0U, NULL, 0U
    };
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE && !report.prepared &&
        find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000003") == MOCK_MAX_NODES &&
        selected_authority_generation(2U), 241);

    CHECK(repair_workspace(installed_database, workspace, target_database), 242);
    reset_filesystem();
    add_new_generation(installed_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, installed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    damaged = find_node("pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[damaged].bytes[0] ^= UINT8_C(1);
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.prepared &&
        report.generation == 3U && report.files_staged == 2U &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES, 243);
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.committed && report.choice == PACKAGE_STATE_RECOVERY_NEW &&
        report.generation == 3U && report.cleanup_complete &&
        selected_authority_generation(3U) &&
        find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES, 244);

    CHECK(repair_workspace(installed_database, workspace, target_database), 245);
    reset_filesystem();
    add_new_generation(installed_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, installed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    damaged = find_node("pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[damaged].bytes[0] ^= UINT8_C(1);
    CHECK(package_service_prepare(&request, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.prepared, 246);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.choice == PACKAGE_STATE_RECOVERY_NEW &&
        report.generation == 3U && report.authority_replaced &&
        report.cleanup_complete && selected_authority_generation(3U), 247);
    CHECK(report_clean(&report), 248);
    free(workspace);
    return 0;
}

static int test_commit_promotes_prepared_generation(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.committed && report.authority_replaced &&
        report.cleanup_complete && report.choice == PACKAGE_STATE_RECOVERY_NEW &&
        report.generation == 2U && report.files_verified == 2U &&
        report.rename_count == 2U && report.sync_count == 5U, 200);
    CHECK(selected_authority_generation(2U) &&
        find_node("pkgstate/gen/00000000/00000001") == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_AUTHORITY_OLD_PATH) == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES, 201);
    size_t write = first_event(MOCK_EVENT_WRITE_AUTHORITY);
    size_t old = first_event(MOCK_EVENT_RENAME_OLD);
    size_t current = first_event(MOCK_EVENT_RENAME_AUTHORITY);
    size_t journal_unlink = first_event(MOCK_EVENT_UNLINK_JOURNAL);
    CHECK(write < old && old < current && current < journal_unlink &&
        events[write + 1U] == MOCK_EVENT_SYNC &&
        events[old + 1U] == MOCK_EVENT_SYNC &&
        events[current + 1U] == MOCK_EVENT_SYNC &&
        events[journal_unlink - 1U] == MOCK_EVENT_SYNC &&
        events[journal_unlink + 1U] == MOCK_EVENT_SYNC, 202);
    CHECK(report_clean(&report), 203);
    return 0;
}

static int test_commit_refuses_unprepared_or_changed_state(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;

    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.committed && selected_authority_generation(1U), 204);

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_NEW_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.committed && selected_authority_generation(1U), 205);

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_AUTHORITY_NEW_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.committed && selected_authority_generation(1U), 213);

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_AUTHORITY_OLD_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.committed && selected_authority_generation(1U), 214);

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[0] ^= UINT8_C(1);
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_INCOMPLETE &&
        !report.committed && selected_authority_generation(1U) &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES, 206);

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_commit(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.committed && selected_authority_generation(2U) &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES, 207);
    return 0;
}

static int test_commit_every_durability_boundary_recovers(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    for (uint32_t boundary = 1U; boundary <= 5U; ++boundary) {
        struct package_service_report report;

        reset_filesystem();
        add_old_generation(old_database);
        add_new_generation(new_database);
        add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
            PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
        add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
            PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
        fail_sync_ordinal = boundary;
        enum package_service_status expected = boundary <= 3U ?
            PACKAGE_SERVICE_STATUS_DURABILITY : PACKAGE_SERVICE_STATUS_CLEANUP;
        enum package_state_recovery_choice expected_choice = boundary <= 3U ||
            boundary == 5U ? PACKAGE_STATE_RECOVERY_OLD :
                PACKAGE_STATE_RECOVERY_NEW;

        CHECK(package_service_commit(&report) == expected &&
            report.committed == (boundary >= 4U) &&
            report.live_file_handles == 0U && report.live_allocations == 0U,
            208);
        CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
            report.choice == expected_choice &&
            report.generation == (boundary <= 3U ? 1U : 2U) &&
            report.cleanup_complete &&
            selected_authority_generation(boundary <= 3U ? 1U : 2U) &&
            find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES, 209);
        CHECK(report_clean(&report), 210);
    }
    return 0;
}

static int test_recovery_accepts_persisted_commit_prefixes(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;

    for (uint32_t boundary = 1U; boundary <= 5U; ++boundary) {
        enum package_state_recovery_choice expected_choice = boundary <= 2U ||
            boundary == 5U ? PACKAGE_STATE_RECOVERY_OLD :
                PACKAGE_STATE_RECOVERY_NEW;

        reset_filesystem();
        if (boundary <= 3U) {
            add_old_generation(old_database);
        }
        add_new_generation(new_database);
        if (boundary == 1U) {
            add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
                PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
            add_file(PACKAGE_SERVICE_AUTHORITY_NEW_PATH, new_authority,
                PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
        } else if (boundary == 2U) {
            add_file(PACKAGE_SERVICE_AUTHORITY_OLD_PATH, old_authority,
                PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
            add_file(PACKAGE_SERVICE_AUTHORITY_NEW_PATH, new_authority,
                PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
        } else {
            add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
                PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
            if (boundary == 3U) {
                add_file(PACKAGE_SERVICE_AUTHORITY_OLD_PATH, old_authority,
                    PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
            }
        }
        if (boundary <= 4U) {
            add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
                PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
        }
        CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
            report.choice == expected_choice &&
            report.generation == (boundary <= 2U ? 1U : 2U) &&
            report.cleanup_complete &&
            selected_authority_generation(boundary <= 2U ? 1U : 2U) &&
            find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES &&
            find_node(PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == MOCK_MAX_NODES &&
            find_node(PACKAGE_SERVICE_AUTHORITY_OLD_PATH) == MOCK_MAX_NODES,
            211);
        CHECK(report_clean(&report), 212);
    }
    return 0;
}

static int test_absent_state_is_distinct(void)
{
    struct package_service_report report;
    reset_filesystem();
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_ABSENT,
        100);
    CHECK(report.live_file_handles == 0U &&
        report.live_allocations == 0U && !report.journal_present &&
        !report.cleanup_complete, 99);
    return 0;
}

static int test_selected_generation_without_journal(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 101);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.generation == 1U && !report.journal_present &&
        report.cleanup_complete && !report.authority_replaced, 102);
    CHECK(report_clean(&report), 103);
    return 0;
}

static int test_backup_authority_is_restored(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_OLD_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 104);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.authority_replaced && report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_AUTHORITY_PATH) != MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_AUTHORITY_OLD_PATH) == MOCK_MAX_NODES,
        105);
    CHECK(report_clean(&report), 106);
    return 0;
}

static int test_precommit_rolls_back(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 110);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.generation == 1U && report.files_verified == 2U &&
        report.cleanup_complete && !report.authority_replaced, 111);
    CHECK(find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES, 112);
    CHECK(report_clean(&report), 113);
    return 0;
}

static int test_unpublished_prepare_powercut_cleanup(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    static const uint8_t partial[] = "partial";
    struct package_service_report report;

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_NEW_PATH, partial,
        sizeof(partial) - 1U, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK &&
        report.choice == PACKAGE_STATE_RECOVERY_OLD && report.generation == 1U &&
        report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) == MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES, 114);

    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_NEW_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_STATE &&
        !report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_NEW_PATH) != MOCK_MAX_NODES, 115);
    return 0;
}

static int test_postcommit_completes(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 120);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_NEW &&
        report.generation == 2U && report.files_verified == 2U &&
        report.cleanup_complete && !report.authority_replaced, 121);
    CHECK(find_node("pkgstate/gen/00000000/00000001") == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES, 122);
    CHECK(report_clean(&report), 123);
    return 0;
}

static int test_tamper_repairs_authority_with_ordering(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    struct package_state_authority_view authority;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[0] ^= UINT8_C(0x20);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 130);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.authority_replaced && report.cleanup_complete &&
        report.rename_count == 2U && report.sync_count == 5U, 131);
    size_t selected = find_node(PACKAGE_SERVICE_AUTHORITY_PATH);
    CHECK(selected != MOCK_MAX_NODES &&
        package_state_authority_parse(nodes[selected].bytes,
            nodes[selected].byte_count, &authority) == PACKAGE_STATE_STATUS_OK &&
        authority.generation == 1U, 132);
    size_t write = first_event(MOCK_EVENT_WRITE_AUTHORITY);
    size_t old = first_event(MOCK_EVENT_RENAME_OLD);
    size_t current = first_event(MOCK_EVENT_RENAME_AUTHORITY);
    size_t journal_unlink = first_event(MOCK_EVENT_UNLINK_JOURNAL);
    CHECK(write < old && old < current && current < journal_unlink, 133);
    CHECK(events[write + 1U] == MOCK_EVENT_SYNC &&
        events[old + 1U] == MOCK_EVENT_SYNC &&
        events[current + 1U] == MOCK_EVENT_SYNC, 134);
    CHECK(report_clean(&report), 135);
    return 0;
}

static int test_extra_file_is_not_complete(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file("pkgstate/gen/00000000/00000002/root/extra",
        (const uint8_t *)"x", 1U, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 140);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.authority_replaced && report.cleanup_complete, 141);
    CHECK(report_clean(&report), 142);
    return 0;
}

static int test_no_complete_generation_refuses(
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[1] ^= UINT8_C(1);
    CHECK(package_service_recover(&report) ==
        PACKAGE_SERVICE_STATUS_INCOMPLETE, 150);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_NONE &&
        !report.authority_replaced && !report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES, 151);
    CHECK(report_clean(&report), 152);
    return 0;
}

static int test_sync_failure_keeps_journal(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[0] ^= UINT8_C(1);
    fail_next_sync = true;
    CHECK(package_service_recover(&report) ==
        PACKAGE_SERVICE_STATUS_DURABILITY, 160);
    CHECK(!report.authority_replaced && !report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000001") != MOCK_MAX_NODES,
        161);
    CHECK(report.live_file_handles == 0U &&
        report.live_allocations == 0U, 162);
    return 0;
}

static int test_repository_floor_is_durable_and_monotonic(void)
{
    uint8_t version_42[PACKAGE_SERVICE_REPOSITORY_FLOOR_BYTES];
    struct package_service_report report;
    uint64_t floor = UINT64_MAX;
    size_t current;
    size_t candidate;

    reset_filesystem();
    CHECK(package_service_repository_floor_read(&floor, &report) ==
            PACKAGE_SERVICE_STATUS_OK && floor == 0U &&
        report.repository_floor == 0U && report.live_file_handles == 0U &&
        report.live_allocations == 0U, 240);
    CHECK(package_service_repository_floor_advance(42U, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.repository_floor == 42U &&
        report.sync_count == 2U && report.rename_count == 1U &&
        package_service_repository_floor_read(&floor, &report) ==
            PACKAGE_SERVICE_STATUS_OK && floor == 42U, 241);
    current = find_node(PACKAGE_SERVICE_REPOSITORY_FLOOR_PATH);
    CHECK(current != MOCK_MAX_NODES &&
        nodes[current].byte_count == sizeof(version_42), 242);
    (void)memcpy(version_42, nodes[current].bytes, sizeof(version_42));

    sync_attempts = 0U;
    fail_sync_ordinal = 1U;
    CHECK(package_service_repository_floor_advance(43U, &report) ==
            PACKAGE_SERVICE_STATUS_DURABILITY &&
        find_node(PACKAGE_SERVICE_REPOSITORY_FLOOR_PATH) != MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH) !=
            MOCK_MAX_NODES &&
        package_service_repository_floor_read(&floor, &report) ==
            PACKAGE_SERVICE_STATUS_OK && floor == 43U, 243);
    fail_sync_ordinal = 0U;
    CHECK(package_service_repository_floor_advance(43U, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.repository_floor == 43U &&
        find_node(PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH) ==
            MOCK_MAX_NODES &&
        package_service_repository_floor_advance(42U, &report) ==
            PACKAGE_SERVICE_STATUS_STATE, 244);

    add_file(PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH, version_42,
        sizeof(version_42), UINT16_C(0444));
    CHECK(package_service_repository_floor_read(&floor, &report) ==
            PACKAGE_SERVICE_STATUS_OK && floor == 43U &&
        package_service_repository_floor_advance(43U, &report) ==
            PACKAGE_SERVICE_STATUS_OK &&
        find_node(PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH) ==
            MOCK_MAX_NODES, 245);

    sync_attempts = 0U;
    fail_sync_ordinal = 2U;
    CHECK(package_service_repository_floor_advance(44U, &report) ==
            PACKAGE_SERVICE_STATUS_DURABILITY &&
        package_service_repository_floor_read(&floor, &report) ==
            PACKAGE_SERVICE_STATUS_OK && floor == 44U, 246);
    fail_sync_ordinal = 0U;
    CHECK(package_service_repository_floor_advance(44U, &report) ==
            PACKAGE_SERVICE_STATUS_OK && report.repository_floor == 44U,
        247);

    add_file(PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH, version_42,
        sizeof(version_42), UINT16_C(0444));
    candidate = find_node(PACKAGE_SERVICE_REPOSITORY_FLOOR_NEW_PATH);
    CHECK(candidate != MOCK_MAX_NODES, 248);
    nodes[candidate].bytes[32U] ^= UINT8_C(1);
    CHECK(package_service_repository_floor_read(&floor, &report) ==
            PACKAGE_SERVICE_STATUS_STATE && floor == 0U &&
        report.state_status == PACKAGE_STATE_STATUS_MISMATCH &&
        report.live_file_handles == 0U && report.live_allocations == 0U, 249);
    return 0;
}

int main(void)
{
    static uint8_t old_database[OLD_DATABASE_BYTES];
    static uint8_t new_database[NEW_DATABASE_BYTES];
    static uint8_t bootstrap_database[NEW_DATABASE_BYTES];
    uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t bootstrap_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES];
    int result = package_state_core_host_test_main();

    if (result != 0) {
        return result;
    }
    build_old_database(old_database);
    build_new_database(new_database);
    build_bootstrap_database(bootstrap_database, new_database);
    build_authority(old_authority, old_database, OLD_DATABASE_BYTES);
    build_authority(new_authority, new_database, NEW_DATABASE_BYTES);
    build_authority(bootstrap_authority, bootstrap_database,
        NEW_DATABASE_BYTES);
    build_journal(journal, old_database, new_database);
    result = test_absent_state_is_distinct();
    if (result == 0) {
        result = test_selected_generation_without_journal(old_database,
            old_authority);
    }
    if (result == 0) {
        result = test_backup_authority_is_restored(old_database,
            old_authority);
    }
    if (result == 0) {
        result = test_precommit_rolls_back(old_database, new_database,
            old_authority, journal);
    }
    if (result == 0) {
        result = test_unpublished_prepare_powercut_cleanup(old_database,
            new_database, old_authority, journal);
    }
    if (result == 0) {
        result = test_postcommit_completes(old_database, new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_tamper_repairs_authority_with_ordering(old_database,
            new_database, new_authority, journal);
    }
    if (result == 0) {
        result = test_extra_file_is_not_complete(old_database, new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_no_complete_generation_refuses(new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_sync_failure_keeps_journal(old_database, new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_prepare_is_recoverable_not_authoritative(old_database,
            new_database, old_authority);
    }
    if (result == 0) {
        result = test_authoritative_snapshot(old_database, old_authority);
    }
    if (result == 0) {
        result = test_prepare_sync_failure_cleans_unpublished_state(
            old_database, new_database, old_authority);
    }
    if (result == 0) {
        result = test_prepare_then_commit_selects_target(old_database,
            new_database, old_authority);
    }
    if (result == 0) {
        result = test_prepare_every_durability_boundary(old_database,
            new_database, old_authority);
    }
    if (result == 0) {
        result = test_prepare_copies_unchanged_installed_file(new_database,
            new_authority);
    }
    if (result == 0) {
        result = test_bootstrap_creates_generation_one(old_database,
            new_database, bootstrap_database);
    }
    if (result == 0) {
        result = test_bootstrap_every_durability_boundary_recovers(old_database,
            new_database, bootstrap_database);
    }
    if (result == 0) {
        result = test_recovery_accepts_persisted_bootstrap_prefixes(
            bootstrap_database, bootstrap_authority);
    }
    if (result == 0) {
        result = test_repair_replaces_damaged_files(new_database,
            new_authority);
    }
    if (result == 0) {
        result = test_commit_promotes_prepared_generation(old_database,
            new_database, old_authority, journal);
    }
    if (result == 0) {
        result = test_commit_refuses_unprepared_or_changed_state(old_database,
            new_database, old_authority, new_authority, journal);
    }
    if (result == 0) {
        result = test_commit_every_durability_boundary_recovers(old_database,
            new_database, old_authority, journal);
    }
    if (result == 0) {
        result = test_recovery_accepts_persisted_commit_prefixes(old_database,
            new_database, old_authority, new_authority, journal);
    }
    if (result == 0) {
        result = test_repository_floor_is_durable_and_monotonic();
    }
    if (result != 0) {
        (void)fprintf(stderr, "package service host test failed: %d\n", result);
        return result;
    }
    (void)printf("package service host tests passed\n");
    return 0;
}
